/**
 * @file artnet_receiver.c
 * @brief Art-Net 4 node (ArtDmx / ArtPoll / ArtPollReply / ArtAddress)
 *
 * Wire format notes (Art-Net 4):
 *  - OpCode and the ArtPollReply UDP port field are little-endian.
 *  - ArtDmx Length, ArtPollReply VersInfo, Oem, NumPorts, RefreshRate and the
 *    ArtPoll TargetPortAddress fields are big-endian (Hi byte first).
 *  - EstaMan is little-endian (Lo byte first).
 *  - Port-Address = Net(7 bits) : Sub-Net(4 bits) : Universe(4 bits).
 */

#include "artnet_receiver.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

static const char *TAG = "artnet_receiver";

#define ARTNET_TASK_STACK_SIZE   6144
#define ARTNET_TASK_PRIORITY     6
#define ARTNET_TASK_CORE         0
#define ARTNET_RX_BUF_SIZE       640
#define ARTNET_RX_TIMEOUT_MS     200
#define ARTNET_REPLY_LEN         239
#define ARTNET_CHANGE_CHECK_US   1000000

#define ARTNET_OEM               0x00FF   /* OemUnknown */
#define ARTNET_ESTA_MAN          0x7FF0   /* ESTA prototype / experimental range */
#define ARTNET_REFRESH_HZ        44

static struct {
    bool initialized;
    volatile bool running;
    int sock;
    TaskHandle_t task;
    SemaphoreHandle_t task_done;

    artnet_dmx_callback_t dmx_cb;
    void *dmx_ctx;
    artnet_status_callback_t status_cb;
    void *status_ctx;
    artnet_address_callback_t addr_cb;
    void *addr_ctx;
    artnet_rdm_callback_t rdm_cb;
    void *rdm_ctx;
    artnet_sync_callback_t sync_cb;
    void *sync_ctx;
    artnet_ipprog_callback_t ipprog_cb;
    void *ipprog_ctx;
    artnet_poll_reply_callback_t peer_cb;
    void *peer_ctx;

    bool poll_reply_enabled;
    volatile bool reply_pending;
    volatile bool reply_force;
    bool reply_on_change;
    uint32_t last_signature;
    int64_t last_change_check_us;

    uint16_t report_code;
    char report_text[48];
    uint32_t reply_counter;

    uint32_t own_ip;            /* network byte order, 0 = unknown */
    uint8_t own_mac[6];         /* the MAC address in this node's replies */
    volatile bool self_poll_pending;
    int64_t last_self_poll_us;
    artnet_stats_t stats;
} s_an = {
    .sock = -1,
    .poll_reply_enabled = true,
    .report_code = ARTNET_RC_POWER_OK,
    .report_text = "Power On Tests successful",
};

static inline uint32_t ip_from_bytes(const uint8_t b[4])
{
    uint32_t v;
    memcpy(&v, b, 4);           /* bytes are already in network order */
    return v;
}

// ============================================================================
// ArtPollReply
// ============================================================================

static void build_reply(uint8_t *r, const artnet_node_status_t *st, int port_idx)
{
    memset(r, 0, ARTNET_REPLY_LEN);
    memcpy(r, ARTNET_HEADER, 8);
    r[8]  = ARTNET_OP_POLL_REPLY & 0xFF;
    r[9]  = ARTNET_OP_POLL_REPLY >> 8;
    memcpy(&r[10], st->ip, 4);
    r[14] = ARTNET_PORT & 0xFF;               /* Port: Lo first */
    r[15] = ARTNET_PORT >> 8;
    r[16] = st->firmware_version >> 8;        /* VersInfoH */
    r[17] = st->firmware_version & 0xFF;      /* VersInfoL */

    const artnet_port_status_t *p = (port_idx >= 0) ? &st->ports[port_idx] : NULL;
    uint16_t pa = p ? (p->port_address & 0x7FFF) : 0;
    r[18] = (pa >> 8) & 0x7F;                 /* NetSwitch */
    r[19] = (pa >> 4) & 0x0F;                 /* SubSwitch */
    r[20] = ARTNET_OEM >> 8;                  /* OemHi */
    r[21] = ARTNET_OEM & 0xFF;                /* Oem */
    r[22] = 0;                                /* UBEA */

    bool any_rdm = false;
    for (int i = 0; i < ARTNET_NODE_PORTS; i++) {
        if (st->ports[i].enabled && st->ports[i].rdm_capable) any_rdm = true;
    }
    uint8_t indicator = (st->indicator == 1) ? 0x40 : (st->indicator == 2) ? 0x80 : 0xC0;
    r[23] = indicator | 0x20 | (any_rdm ? 0x02 : 0x00);   /* Status1: addr set by network */
    r[24] = ARTNET_ESTA_MAN & 0xFF;           /* EstaManLo */
    r[25] = ARTNET_ESTA_MAN >> 8;             /* EstaManHi */

    /* ShortName 18 / LongName 64 bytes, always NUL-terminated */
    memcpy(&r[26], st->short_name, strnlen(st->short_name, 17));
    memcpy(&r[44], st->long_name, strnlen(st->long_name, 63));
    snprintf((char *)&r[108], 64, "#%04X [%04" PRIu32 "] %s",
             s_an.report_code, s_an.reply_counter % 10000, s_an.report_text);

    if (p) {
        r[172] = 0;                           /* NumPortsHi */
        r[173] = 1;                           /* NumPortsLo */
        if (p->is_input) {
            r[174] = 0x40;                    /* PortTypes: DMX512 input */
            uint8_t gi = 0;
            if (p->data_active)  gi |= 0x80;
            if (p->input_errors) gi |= 0x04;
            if (p->sacn)         gi |= 0x01;
            r[178] = gi;
            r[186] = pa & 0x0F;               /* SwIn */
        } else {
            r[174] = 0x80;                    /* PortTypes: DMX512 output */
            uint8_t go = 0;
            if (p->data_active) go |= 0x80;
            if (p->merging)     go |= 0x08;
            if (p->ltp)         go |= 0x02;
            if (p->sacn)        go |= 0x01;
            r[182] = go;
            r[190] = pa & 0x0F;               /* SwOut */
        }
        uint8_t gob = 0x40;                   /* continuous output */
        if (!p->rdm_enabled)     gob |= 0x80;
        if (!p->rdm_discovering) gob |= 0x20;
        if (!p->rdm_enabled || !p->rdm_background) gob |= 0x10;
        r[213] = gob;                          /* GoodOutputB */
        memcpy(&r[218], p->responder_uid, 6); /* DefaultRespUID */
    }
    r[194] = st->sacn_priority;                /* AcnPriority */
    r[200] = 0x00;                             /* Style: StNode */
    memcpy(&r[201], st->mac, 6);
    memcpy(&r[207], st->ip, 4);                /* BindIp */
    r[211] = (port_idx >= 0) ? (uint8_t)(port_idx + 1) : 1;   /* BindIndex */
    r[212] = 0x01 | (st->dhcp ? 0x02 : 0x00) | 0x04 | 0x08;   /* Status2 */
    /* Status3: failsafe state (7-6), programmable failsafe (5), port
     * direction switchable (3) */
    r[217] = (uint8_t)(((st->failsafe & 0x03) << 6) | 0x20 | 0x08);
    r[226] = 0;                                /* RefreshRateHi */
    r[227] = ARTNET_REFRESH_HZ;                /* RefreshRateLo */
}

static uint32_t fnv1a(uint32_t h, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        h ^= d[i];
        h *= 16777619u;
    }
    return h;
}

static bool get_status(artnet_node_status_t *st)
{
    memset(st, 0, sizeof(*st));
    st->sacn_priority = 100;    /* unless the application says otherwise */
    if (!s_an.status_cb) {
        return false;
    }
    s_an.status_cb(st, s_an.status_ctx);
    s_an.own_ip = ip_from_bytes(st->ip);
    memcpy(s_an.own_mac, st->mac, 6);
    return s_an.own_ip != 0;
}

/* The directed broadcast address of the node's network */
static struct sockaddr_in broadcast_to(const artnet_node_status_t *st)
{
    uint32_t ip = ip_from_bytes(st->ip);
    uint32_t mask = ip_from_bytes(st->netmask);
    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_port = htons(ARTNET_PORT),
        .sin_addr.s_addr = mask ? ((ip & mask) | ~mask) : htonl(INADDR_BROADCAST),
    };
    return to;
}

/* ArtPoll so the other nodes announce themselves (artnet_receiver_poll_network) */
static void send_poll(void)
{
    artnet_node_status_t st;
    if (!get_status(&st) || s_an.sock < 0) {
        return;
    }
    struct sockaddr_in to = broadcast_to(&st);
    uint8_t poll[22] = {0};
    memcpy(poll, ARTNET_HEADER, 8);
    poll[8] = ARTNET_OP_POLL & 0xFF;
    poll[9] = ARTNET_OP_POLL >> 8;
    poll[11] = ARTNET_PROTOCOL_VERSION;
    /* Flags 0: reply to this poll only, no diagnostics, not targeted */
    sendto(s_an.sock, poll, sizeof(poll), 0, (struct sockaddr *)&to, sizeof(to));
}

/**
 * Send the node's ArtPollReply set.
 * @param poller     controller that polled (NULL for unsolicited replies)
 * @param targeted   ArtPoll targeted mode: only report ports in [bottom, top]
 */
static void send_replies(const struct sockaddr_in *poller, bool targeted,
                         uint16_t bottom, uint16_t top)
{
    artnet_node_status_t st;
    if (!get_status(&st) || s_an.sock < 0) {
        return;
    }

    uint32_t ip = ip_from_bytes(st.ip);
    uint32_t mask = ip_from_bytes(st.netmask);
    struct sockaddr_in bcast = broadcast_to(&st);
    bool unicast_too = poller && mask &&
                       ((poller->sin_addr.s_addr & mask) != (ip & mask));

    uint8_t reply[ARTNET_REPLY_LEN];
    int sent = 0;
    for (int p = 0; p < ARTNET_NODE_PORTS; p++) {
        if (!st.ports[p].enabled) continue;
        uint16_t pa = st.ports[p].port_address & 0x7FFF;
        if (targeted && (pa < bottom || pa > top)) continue;
        build_reply(reply, &st, p);
        if (sendto(s_an.sock, reply, sizeof(reply), 0,
                   (struct sockaddr *)&bcast, sizeof(bcast)) >= 0) {
            s_an.stats.poll_replies_sent++;
        }
        if (unicast_too) {
            struct sockaddr_in to = *poller;
            to.sin_port = htons(ARTNET_PORT);
            sendto(s_an.sock, reply, sizeof(reply), 0, (struct sockaddr *)&to, sizeof(to));
        }
        sent++;
    }
    if (sent == 0 && !targeted) {
        /* No active ports: still announce the node itself */
        build_reply(reply, &st, -1);
        if (sendto(s_an.sock, reply, sizeof(reply), 0,
                   (struct sockaddr *)&bcast, sizeof(bcast)) >= 0) {
            s_an.stats.poll_replies_sent++;
        }
    }
    s_an.reply_counter++;
}

/* Signature of everything a controller would see change (NodeReport excluded) */
static uint32_t status_signature(void)
{
    artnet_node_status_t st;
    if (!get_status(&st)) {
        return 0;
    }
    uint8_t reply[ARTNET_REPLY_LEN];
    uint32_t h = 2166136261u;
    for (int p = 0; p < ARTNET_NODE_PORTS; p++) {
        build_reply(reply, &st, st.ports[p].enabled ? p : -1);
        h = fnv1a(h, reply, 108);
        h = fnv1a(h, reply + 172, ARTNET_REPLY_LEN - 172);
    }
    return h;
}

// ============================================================================
// Packet handlers
// ============================================================================

static void handle_dmx(const uint8_t *b, int len, const struct sockaddr_in *from)
{
    if (len < 18) {
        s_an.stats.invalid_packets++;
        return;
    }
    uint16_t length = ((uint16_t)b[16] << 8) | b[17];
    if (length == 0 || length > 512) {
        s_an.stats.invalid_packets++;
        return;
    }
    int avail = len - 18;
    if (avail <= 0) {
        s_an.stats.invalid_packets++;
        return;
    }
    if (length > avail) {
        length = (uint16_t)avail;   /* truncated packet: use what arrived */
    }
    uint16_t port_address = b[14] | ((uint16_t)(b[15] & 0x7F) << 8);
    s_an.stats.dmx_packets++;

    artnet_dmx_callback_t cb = s_an.dmx_cb;
    if (cb) {
        cb(port_address, &b[18], length, b[12], from->sin_addr.s_addr, s_an.dmx_ctx);
    }
}

static void handle_poll(const uint8_t *b, int len, const struct sockaddr_in *from)
{
    if (len < 14) {
        s_an.stats.invalid_packets++;
        return;
    }
    s_an.stats.poll_packets++;
    uint8_t flags = b[12];
    s_an.reply_on_change = (flags & 0x02) != 0;

    bool targeted = (flags & 0x20) && len >= 18;
    uint16_t top = 0x7FFF, bottom = 0;
    if (targeted) {
        top    = (((uint16_t)b[14] << 8) | b[15]) & 0x7FFF;
        bottom = (((uint16_t)b[16] << 8) | b[17]) & 0x7FFF;
    }
    if (s_an.poll_reply_enabled) {
        send_replies(from, targeted, bottom, top);
    }
}

/* NetSwitch, SubSwitch, SwIn, SwOut: bit 7 programs, 0x00 resets that field
 * and no other, anything else changes nothing */
static int16_t address_field(uint8_t v, uint8_t mask)
{
    if (v & 0x80) return v & mask;
    return v == 0x00 ? ARTNET_FIELD_RESET : ARTNET_FIELD_KEEP;
}

static void handle_address(const uint8_t *b, int len, const struct sockaddr_in *from)
{
    if (len < 107) {
        s_an.stats.invalid_packets++;
        return;
    }
    s_an.stats.address_packets++;

    uint8_t bind = b[13];
    artnet_address_cmd_t cmd = {
        .port = (bind <= 1) ? 1 : bind,
        .net = address_field(b[12], 0x7F),
        .sub = address_field(b[104], 0x0F),
        .universe_in = address_field(b[96], 0x0F),
        .universe_out = address_field(b[100], 0x0F),
        .command = b[106],
        .acn_priority = b[105],
        .source_ip = from->sin_addr.s_addr,
    };
    if (cmd.port > ARTNET_NODE_PORTS) {
        return;   /* addressed to a bind index we do not have */
    }

    char short_name[18], long_name[64];
    if (b[14]) {
        memcpy(short_name, &b[14], 17);
        short_name[17] = '\0';
        cmd.short_name = short_name;
    }
    if (b[32]) {
        memcpy(long_name, &b[32], 63);
        long_name[63] = '\0';
        cmd.long_name = long_name;
    }

    ESP_LOGI(TAG, "ArtAddress port %u: net %d sub %d in %d out %d cmd 0x%02X%s%s",
             cmd.port, cmd.net, cmd.sub, cmd.universe_in, cmd.universe_out, cmd.command,
             cmd.short_name ? " short-name" : "", cmd.long_name ? " long-name" : "");

    artnet_address_callback_t cb = s_an.addr_cb;
    if (cb) {
        cb(&cmd, s_an.addr_ctx);
    }
    /* The node replies to ArtAddress with an ArtPollReply */
    send_replies(from, false, 0, 0x7FFF);
}

/* ArtTodRequest: Net 21, Command 22, AddCount 23, Address[32] 24.. */
static void handle_tod_request(const uint8_t *b, int len, const struct sockaddr_in *from)
{
    if (len < 24) {
        s_an.stats.invalid_packets++;
        return;
    }
    artnet_rdm_event_t ev = {
        .type = ARTNET_RDM_EV_TOD_REQUEST,
        .source_ip = from->sin_addr.s_addr,
        .net = b[21] & 0x7F,
        .command = b[22],
    };
    int count = b[23] > 32 ? 32 : b[23];
    if (count > len - 24) count = len - 24;
    for (int i = 0; i < count; i++) {
        ev.addresses[i] = ((uint16_t)ev.net << 8) | b[24 + i];
    }
    ev.address_count = (uint8_t)count;
    artnet_rdm_callback_t cb = s_an.rdm_cb;
    if (cb) cb(&ev, s_an.rdm_ctx);
}

/* ArtTodControl: Net 21, Command 22, Address 23 */
static void handle_tod_control(const uint8_t *b, int len, const struct sockaddr_in *from)
{
    if (len < 24) {
        s_an.stats.invalid_packets++;
        return;
    }
    artnet_rdm_event_t ev = {
        .type = ARTNET_RDM_EV_TOD_CONTROL,
        .source_ip = from->sin_addr.s_addr,
        .net = b[21] & 0x7F,
        .command = b[22],
        .port_address = ((uint16_t)(b[21] & 0x7F) << 8) | b[23],
    };
    artnet_rdm_callback_t cb = s_an.rdm_cb;
    if (cb) cb(&ev, s_an.rdm_ctx);
}

/* ArtRdm: RdmVer 12, FifoAvail 19, FifoMax 20, Net 21, Command 22,
 * Address 23, RdmPacket 24.. */
static void handle_rdm(const uint8_t *b, int len, const struct sockaddr_in *from)
{
    if (len < 24 + 23) {   /* shortest RDM message without start code: 23 bytes */
        s_an.stats.invalid_packets++;
        return;
    }
    artnet_rdm_event_t ev = {
        .type = ARTNET_RDM_EV_RDM,
        .source_ip = from->sin_addr.s_addr,
        .net = b[21] & 0x7F,
        .command = b[22],
        .port_address = ((uint16_t)(b[21] & 0x7F) << 8) | b[23],
        .rdm = &b[24],
        .rdm_len = (uint16_t)(len - 24),
    };
    artnet_rdm_callback_t cb = s_an.rdm_cb;
    if (cb) cb(&ev, s_an.rdm_ctx);
}

static void handle_sync(const uint8_t *b, int len, const struct sockaddr_in *from)
{
    (void)b;
    s_an.stats.sync_packets++;
    if (len < 14) {
        s_an.stats.invalid_packets++;
        return;
    }
    artnet_sync_callback_t cb = s_an.sync_cb;
    if (cb) cb(from->sin_addr.s_addr, s_an.sync_ctx);
}

static void put_header(uint8_t *b, uint16_t opcode);

/* ArtIpProg: Command 14, ProgIp 16-19, ProgSm 20-23, ProgPort 24-25,
 * ProgDg 26-29 (Art-Net 4). Reply: ProgIp 16, ProgSm 20, ProgPort 24,
 * Status 26 (bit 6 = DHCP), ProgDg 28, 34 bytes, unicast to the sender. */
static void handle_ip_prog(const uint8_t *b, int len, const struct sockaddr_in *from)
{
    if (len < 24) {
        s_an.stats.invalid_packets++;
        return;
    }
    artnet_ipprog_callback_t cb = s_an.ipprog_cb;
    if (!cb) {
        s_an.stats.unsupported_packets++;
        return;
    }
    uint8_t c = b[14];
    artnet_ipprog_cmd_t cmd = {
        .program = (c & 0x80) != 0,
        .dhcp = (c & 0xC0) == 0xC0,
        .set_gateway = (c & 0x90) == 0x90 && len >= 30,
        .reset_default = (c & 0x88) == 0x88,
        .set_ip = (c & 0x84) == 0x84,
        .set_mask = (c & 0x82) == 0x82,
        .source_ip = from->sin_addr.s_addr,
    };
    memcpy(cmd.ip, &b[16], 4);
    memcpy(cmd.mask, &b[20], 4);
    if (len >= 30) memcpy(cmd.gateway, &b[26], 4);

    ESP_LOGI(TAG, "ArtIpProg command 0x%02X%s", c, cmd.program ? "" : " (query)");
    artnet_ipprog_reply_t rep;
    memset(&rep, 0, sizeof(rep));
    cb(&cmd, &rep, s_an.ipprog_ctx);

    uint8_t r[34];
    memset(r, 0, sizeof(r));
    put_header(r, ARTNET_OP_IP_PROG_REPLY);
    memcpy(&r[16], rep.ip, 4);
    memcpy(&r[20], rep.mask, 4);
    r[24] = ARTNET_PORT >> 8;
    r[25] = ARTNET_PORT & 0xFF;
    r[26] = rep.dhcp ? 0x40 : 0x00;
    memcpy(&r[28], rep.gateway, 4);
    struct sockaddr_in to = *from;
    if (s_an.sock >= 0) {
        sendto(s_an.sock, r, sizeof(r), 0, (struct sockaddr *)&to, sizeof(to));
    }
}

/* NUL-terminated copy of a fixed-size name field */
static void copy_field(char *dst, const uint8_t *src, size_t size)
{
    size_t n = strnlen((const char *)src, size - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

bool artnet_parse_poll_reply(const uint8_t *b, int len, artnet_poll_reply_t *out)
{
    /* Everything up to SwOut (byte 193) is needed; older nodes stop there
     * or a little later, so the rest is read only if it arrived */
    if (!b || !out || len < 194 || memcmp(b, ARTNET_HEADER, 8) != 0 ||
        (b[8] | ((uint16_t)b[9] << 8)) != ARTNET_OP_POLL_REPLY) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->ip = ip_from_bytes(&b[10]);
    out->firmware = (uint16_t)((b[16] << 8) | b[17]);
    uint16_t base = (uint16_t)(((b[18] & 0x7F) << 8) | ((b[19] & 0x0F) << 4));
    out->oem = (uint16_t)((b[20] << 8) | b[21]);
    out->esta = (uint16_t)(b[24] | (b[25] << 8));
    copy_field(out->short_name, &b[26], sizeof(out->short_name));
    copy_field(out->long_name, &b[44], sizeof(out->long_name));
    out->num_ports = b[173] > ARTNET_REPLY_PORTS ? ARTNET_REPLY_PORTS : b[173];
    for (int i = 0; i < out->num_ports; i++) {
        out->ports[i].type = b[174 + i];
        out->ports[i].good_input = b[178 + i];
        out->ports[i].good_output = b[182 + i];
        out->ports[i].in_address = base | (b[186 + i] & 0x0F);
        out->ports[i].out_address = base | (b[190 + i] & 0x0F);
    }
    if (len > 200) out->style = b[200];
    if (len >= 207) memcpy(out->mac, &b[201], 6);
    if (len > 211) out->bind_index = b[211];
    if (len > 212) out->status2 = b[212];
    return true;
}

/*
 * Another node announcing itself. Checked before the own-address filter: a
 * reply from this node's IP address with another MAC address is a second
 * device on the same address.
 */
static void handle_poll_reply(const uint8_t *b, int len, const struct sockaddr_in *from)
{
    artnet_poll_reply_t r;
    if (!artnet_parse_poll_reply(b, len, &r)) {
        return;   /* too short to say anything useful */
    }
    r.from_ip = from->sin_addr.s_addr;
    if (!s_an.own_ip) {
        artnet_node_status_t st;
        get_status(&st);
    }
    static const uint8_t no_mac[6];
    bool same_ip = s_an.own_ip && (r.from_ip == s_an.own_ip || r.ip == s_an.own_ip);
    if (same_ip && (memcmp(r.mac, no_mac, 6) == 0 || memcmp(r.mac, s_an.own_mac, 6) == 0)) {
        s_an.stats.own_packets++;   /* this node's own reply */
        return;
    }
    s_an.stats.peer_replies++;
    if (s_an.peer_cb) {
        s_an.peer_cb(&r, same_ip, s_an.peer_ctx);
    }
}

static void process_packet(const uint8_t *b, int len, const struct sockaddr_in *from)
{
    if (len < 10 || memcmp(b, ARTNET_HEADER, 8) != 0) {
        s_an.stats.invalid_packets++;
        return;
    }
    uint16_t opcode = b[8] | ((uint16_t)b[9] << 8);
    if (opcode == ARTNET_OP_POLL_REPLY) {
        handle_poll_reply(b, len, from);
        return;
    }
    if (s_an.own_ip && from->sin_addr.s_addr == s_an.own_ip) {
        s_an.stats.own_packets++;
        return;
    }
    switch (opcode) {
        case ARTNET_OP_DMX:        handle_dmx(b, len, from);     break;
        case ARTNET_OP_POLL:       handle_poll(b, len, from);    break;
        case ARTNET_OP_ADDRESS:    handle_address(b, len, from); break;
        case ARTNET_OP_SYNC:        handle_sync(b, len, from);        break;
        case ARTNET_OP_TOD_REQUEST: handle_tod_request(b, len, from); break;
        case ARTNET_OP_TOD_CONTROL: handle_tod_control(b, len, from); break;
        case ARTNET_OP_RDM:         handle_rdm(b, len, from);         break;
        case ARTNET_OP_IP_PROG:     handle_ip_prog(b, len, from);     break;
        case ARTNET_OP_IP_PROG_REPLY: break;
        case ARTNET_OP_TOD_DATA:    break;   /* from other gateways */
        default:                   s_an.stats.unsupported_packets++; break;
    }
}

static void artnet_receive_task(void *arg)
{
    uint8_t *buf = malloc(ARTNET_RX_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "rx buffer alloc failed");
        s_an.running = false;
    }
    ESP_LOGI(TAG, "Art-Net receiver task started");

    /* Task watchdog: the loop wakes at least every ARTNET_RX_TIMEOUT_MS, so a
     * receiver stuck for 10 s restarts the node instead of leaving it deaf */
    esp_task_wdt_add(NULL);
    while (s_an.running) {
        esp_task_wdt_reset();
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(s_an.sock, buf, ARTNET_RX_BUF_SIZE, 0,
                           (struct sockaddr *)&from, &from_len);
        if (len > 0) {
            s_an.stats.packets_received++;
            process_packet(buf, len, &from);
        } else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK && s_an.running) {
            ESP_LOGW(TAG, "recvfrom error %d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (s_an.self_poll_pending) {
            s_an.self_poll_pending = false;
            send_poll();
        }

        if (s_an.reply_pending) {
            bool force = s_an.reply_force;
            s_an.reply_pending = false;
            s_an.reply_force = false;
            if (force || s_an.reply_on_change) {
                send_replies(NULL, false, 0, 0x7FFF);
            }
            s_an.last_signature = status_signature();
        }

        int64_t now = esp_timer_get_time();
        if (s_an.reply_on_change && now - s_an.last_change_check_us > ARTNET_CHANGE_CHECK_US) {
            s_an.last_change_check_us = now;
            uint32_t sig = status_signature();
            if (sig != s_an.last_signature) {
                s_an.last_signature = sig;
                send_replies(NULL, false, 0, 0x7FFF);
            }
        }
    }

    esp_task_wdt_delete(NULL);
    free(buf);
    ESP_LOGI(TAG, "Art-Net receiver task stopped");
    xSemaphoreGive(s_an.task_done);
    vTaskDelete(NULL);
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t artnet_receiver_init(void)
{
    if (s_an.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_an.task_done = xSemaphoreCreateBinary();
    if (!s_an.task_done) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_an.stats, 0, sizeof(s_an.stats));
    s_an.initialized = true;
    ESP_LOGI(TAG, "Art-Net receiver initialized");
    return ESP_OK;
}

esp_err_t artnet_receiver_deinit(void)
{
    if (!s_an.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    artnet_receiver_stop();
    vSemaphoreDelete(s_an.task_done);
    s_an.task_done = NULL;
    s_an.initialized = false;
    return ESP_OK;
}

esp_err_t artnet_receiver_start(void)
{
    if (!s_an.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_an.running) {
        return ESP_OK;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: %d", errno);
        return ESP_FAIL;
    }
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    struct timeval tv = { .tv_sec = 0, .tv_usec = ARTNET_RX_TIMEOUT_MS * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(ARTNET_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind(%d) failed: %d", ARTNET_PORT, errno);
        close(sock);
        return ESP_FAIL;
    }

    s_an.sock = sock;
    s_an.running = true;
    xSemaphoreTake(s_an.task_done, 0);
    if (xTaskCreatePinnedToCore(artnet_receive_task, "artnet_rx", ARTNET_TASK_STACK_SIZE,
                                NULL, ARTNET_TASK_PRIORITY, &s_an.task,
                                ARTNET_TASK_CORE) != pdPASS) {
        s_an.running = false;
        close(sock);
        s_an.sock = -1;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Art-Net node listening on UDP %d", ARTNET_PORT);
    return ESP_OK;
}

esp_err_t artnet_receiver_stop(void)
{
    if (!s_an.initialized || !s_an.running) {
        return ESP_OK;
    }
    s_an.running = false;
    if (xSemaphoreTake(s_an.task_done, pdMS_TO_TICKS(ARTNET_RX_TIMEOUT_MS * 5)) != pdTRUE) {
        ESP_LOGW(TAG, "rx task did not stop in time");
    }
    s_an.task = NULL;
    if (s_an.sock >= 0) {
        close(s_an.sock);
        s_an.sock = -1;
    }
    return ESP_OK;
}

int artnet_receiver_get_socket(void)
{
    return s_an.running ? s_an.sock : -1;
}

esp_err_t artnet_receiver_set_callback(artnet_dmx_callback_t callback, void *user_data)
{
    if (!s_an.initialized) return ESP_ERR_INVALID_STATE;
    s_an.dmx_ctx = user_data;
    s_an.dmx_cb = callback;
    return ESP_OK;
}

esp_err_t artnet_receiver_set_status_callback(artnet_status_callback_t callback, void *user_data)
{
    if (!s_an.initialized) return ESP_ERR_INVALID_STATE;
    s_an.status_ctx = user_data;
    s_an.status_cb = callback;
    return ESP_OK;
}

esp_err_t artnet_receiver_set_address_callback(artnet_address_callback_t callback, void *user_data)
{
    if (!s_an.initialized) return ESP_ERR_INVALID_STATE;
    s_an.addr_ctx = user_data;
    s_an.addr_cb = callback;
    return ESP_OK;
}

esp_err_t artnet_receiver_set_ipprog_callback(artnet_ipprog_callback_t callback, void *user_data)
{
    s_an.ipprog_cb = NULL;
    s_an.ipprog_ctx = user_data;
    s_an.ipprog_cb = callback;
    return ESP_OK;
}

esp_err_t artnet_receiver_set_poll_reply_callback(artnet_poll_reply_callback_t callback, void *user_data)
{
    s_an.peer_cb = NULL;
    s_an.peer_ctx = user_data;
    s_an.peer_cb = callback;
    return ESP_OK;
}

bool artnet_receiver_poll_network(void)
{
    if (!s_an.running) {
        return false;
    }
    int64_t now = esp_timer_get_time();
    if (s_an.last_self_poll_us && now - s_an.last_self_poll_us < (int64_t)ARTNET_SELF_POLL_MS * 1000) {
        return false;
    }
    s_an.last_self_poll_us = now;
    s_an.self_poll_pending = true;   /* sent by the receive task */
    return true;
}

esp_err_t artnet_receiver_set_rdm_callback(artnet_rdm_callback_t callback, void *user_data)
{
    s_an.rdm_cb = NULL;
    s_an.rdm_ctx = user_data;
    s_an.rdm_cb = callback;
    return ESP_OK;
}

esp_err_t artnet_receiver_set_sync_callback(artnet_sync_callback_t callback, void *user_data)
{
    s_an.sync_cb = NULL;
    s_an.sync_ctx = user_data;
    s_an.sync_cb = callback;
    return ESP_OK;
}

static void put_header(uint8_t *b, uint16_t opcode)
{
    memcpy(b, ARTNET_HEADER, 8);
    b[8] = opcode & 0xFF;
    b[9] = opcode >> 8;
    b[10] = 0;
    b[11] = ARTNET_PROTOCOL_VERSION;
}

static esp_err_t send_to(uint32_t dest_ip, const uint8_t *b, size_t len)
{
    if (s_an.sock < 0 || dest_ip == 0) return ESP_ERR_INVALID_STATE;
    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_port = htons(ARTNET_PORT),
        .sin_addr.s_addr = dest_ip,
    };
    return sendto(s_an.sock, b, len, 0, (struct sockaddr *)&to, sizeof(to)) >= 0
               ? ESP_OK : ESP_FAIL;
}

esp_err_t artnet_receiver_send_tod_data(uint32_t dest_ip, uint8_t bind_index,
                                        uint16_t port_address, bool nak,
                                        const uint8_t (*uids)[6], uint16_t total)
{
    if (nak || !uids) total = 0;
    uint8_t *b = malloc(28 + ARTNET_TOD_BLOCK_UIDS * 6);
    if (!b) return ESP_ERR_NO_MEM;
    esp_err_t ret = ESP_OK;
    uint16_t done = 0;
    uint8_t block = 0;
    do {
        uint16_t n = total - done;
        if (n > ARTNET_TOD_BLOCK_UIDS) n = ARTNET_TOD_BLOCK_UIDS;
        memset(b, 0, 28);
        put_header(b, ARTNET_OP_TOD_DATA);
        b[12] = 0x01;                          /* RdmVer: E1.20 */
        b[13] = 1;                             /* Port: one port per BindIndex */
        b[20] = bind_index;
        b[21] = (port_address >> 8) & 0x7F;    /* Net */
        b[22] = nak ? ARTNET_TOD_NAK : ARTNET_TOD_FULL;
        b[23] = port_address & 0xFF;           /* Address (Sub-Net:Universe) */
        b[24] = total >> 8;                    /* UidTotalHi */
        b[25] = total & 0xFF;                  /* UidTotalLo */
        b[26] = block;                         /* BlockCount */
        b[27] = (uint8_t)n;                    /* UidCount */
        if (n) memcpy(&b[28], uids[done], (size_t)n * 6);
        if (send_to(dest_ip, b, 28 + (size_t)n * 6) != ESP_OK) ret = ESP_FAIL;
        done += n;
        block++;
    } while (done < total);
    free(b);
    return ret;
}

esp_err_t artnet_receiver_send_rdm(uint32_t dest_ip, uint8_t bind_index, uint16_t port_address,
                                   const uint8_t *rdm, uint16_t len)
{
    (void)bind_index;
    if (!rdm || len == 0 || len > 512) return ESP_ERR_INVALID_ARG;
    uint8_t b[24 + 260];
    if (len > sizeof(b) - 24) return ESP_ERR_INVALID_SIZE;
    memset(b, 0, 24);
    put_header(b, ARTNET_OP_RDM);
    b[12] = 0x01;                              /* RdmVer */
    b[21] = (port_address >> 8) & 0x7F;        /* Net */
    b[22] = 0x00;                              /* ArProcess */
    b[23] = port_address & 0xFF;               /* Address */
    memcpy(&b[24], rdm, len);
    return send_to(dest_ip, b, 24 + (size_t)len);
}

void artnet_receiver_set_node_report(artnet_report_code_t code, const char *text)
{
    s_an.report_code = (uint16_t)code;
    strncpy(s_an.report_text, text ? text : "", sizeof(s_an.report_text) - 1);
    s_an.report_text[sizeof(s_an.report_text) - 1] = '\0';
}

void artnet_receiver_notify_change(bool force)
{
    if (force) {
        s_an.reply_force = true;
    }
    s_an.reply_pending = true;
}

esp_err_t artnet_receiver_get_stats(artnet_stats_t *stats)
{
    if (!s_an.initialized) return ESP_ERR_INVALID_STATE;
    if (!stats) return ESP_ERR_INVALID_ARG;
    *stats = s_an.stats;
    return ESP_OK;
}

esp_err_t artnet_receiver_enable_poll_reply(bool enable)
{
    if (!s_an.initialized) return ESP_ERR_INVALID_STATE;
    s_an.poll_reply_enabled = enable;
    return ESP_OK;
}

bool artnet_receiver_is_running(void)
{
    return s_an.running;
}
