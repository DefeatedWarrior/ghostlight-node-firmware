/* Unit tests: Art-Net packet parsing in components/artnet_receiver.
 *
 * The component source is included so the tests can call its static packet
 * dispatcher (process_packet) directly, the way the receive task does. */
#include "../../components/artnet_receiver/artnet_receiver.c"
#include "fakes.h"
#include "packets.h"

static void reset(void);
#define TEST_SETUP() reset()
#include "test.h"

static __typeof__(s_an) s_an_pristine;

/* ---- recorded callbacks ------------------------------------------------- */

static struct {
    int calls;
    uint16_t universe;
    uint8_t data[600];
    uint16_t length;
    uint8_t sequence;
    uint32_t source_ip;
} dmx;

static void on_dmx(uint16_t universe, const uint8_t *data, uint16_t length,
                   uint8_t sequence, uint32_t source_ip, void *user_data)
{
    (void)user_data;
    dmx.calls++;
    dmx.universe = universe;
    memcpy(dmx.data, data, length);   /* ASan flags a length past the packet */
    dmx.length = length;
    dmx.sequence = sequence;
    dmx.source_ip = source_ip;
}

static struct {
    int calls;
    artnet_address_cmd_t cmd;
    char short_name[18];
    char long_name[64];
} addr;

static void on_address(const artnet_address_cmd_t *cmd, void *user_data)
{
    (void)user_data;
    addr.calls++;
    addr.cmd = *cmd;
    addr.short_name[0] = addr.long_name[0] = '\0';
    if (cmd->short_name) snprintf(addr.short_name, sizeof(addr.short_name), "%s", cmd->short_name);
    if (cmd->long_name) snprintf(addr.long_name, sizeof(addr.long_name), "%s", cmd->long_name);
}

static struct { int calls; uint32_t ip; } sync_rx;
static void on_sync(uint32_t source_ip, void *user_data)
{
    (void)user_data;
    sync_rx.calls++;
    sync_rx.ip = source_ip;
}

static struct { int calls; artnet_rdm_event_t ev; uint8_t rdm[700]; } rdm;
static void on_rdm(const artnet_rdm_event_t *ev, void *user_data)
{
    (void)user_data;
    rdm.calls++;
    rdm.ev = *ev;
    if (ev->rdm) memcpy(rdm.rdm, ev->rdm, ev->rdm_len);
}

static struct { int calls; artnet_poll_reply_t r; bool conflict; } peer;
static void on_peer(const artnet_poll_reply_t *r, bool ip_conflict, void *user_data)
{
    (void)user_data;
    peer.calls++;
    peer.r = *r;
    peer.conflict = ip_conflict;
}

static struct { int calls; artnet_ipprog_cmd_t cmd; } ipprog;
static void on_ipprog(const artnet_ipprog_cmd_t *cmd, artnet_ipprog_reply_t *reply, void *user_data)
{
    (void)user_data;
    ipprog.calls++;
    ipprog.cmd = *cmd;
    memcpy(reply->ip, (uint8_t[]){10, 0, 0, 5}, 4);
    memcpy(reply->mask, (uint8_t[]){255, 0, 0, 0}, 4);
    memcpy(reply->gateway, (uint8_t[]){10, 0, 0, 1}, 4);
    reply->dhcp = false;
}

/* Node: 2.0.0.10/8, port 1 = output 0:0:1, port 2 = input 1:2:3 */
static artnet_node_status_t node;
static void on_status(artnet_node_status_t *st, void *user_data)
{
    (void)user_data;
    *st = node;
}

static void reset(void)
{
    host_fakes_reset();
    s_an = s_an_pristine;
    memset(&dmx, 0, sizeof(dmx));
    memset(&addr, 0, sizeof(addr));
    memset(&sync_rx, 0, sizeof(sync_rx));
    memset(&rdm, 0, sizeof(rdm));
    memset(&ipprog, 0, sizeof(ipprog));
    memset(&peer, 0, sizeof(peer));

    memset(&node, 0, sizeof(node));
    snprintf(node.short_name, sizeof(node.short_name), "Test Node");
    snprintf(node.long_name, sizeof(node.long_name), "Host test node");
    memcpy(node.ip, (uint8_t[]){2, 0, 0, 10}, 4);
    memcpy(node.netmask, (uint8_t[]){255, 0, 0, 0}, 4);
    memcpy(node.mac, (uint8_t[]){0x24, 0x6F, 0x28, 1, 2, 3}, 6);
    node.firmware_version = 0x0102;
    node.ports[0].enabled = true;
    node.ports[0].port_address = 0x0001;
    node.ports[1].enabled = true;
    node.ports[1].is_input = true;
    node.ports[1].port_address = 0x0123;

    artnet_receiver_init();
    artnet_receiver_set_callback(on_dmx, NULL);
    artnet_receiver_set_address_callback(on_address, NULL);
    artnet_receiver_set_status_callback(on_status, NULL);
    artnet_receiver_set_sync_callback(on_sync, NULL);
    artnet_receiver_set_rdm_callback(on_rdm, NULL);
    artnet_receiver_set_poll_reply_callback(on_peer, NULL);
    s_an.sock = 42;    /* replies go to the fake sendto() */
}

#define IP(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

/* Hand @p len bytes to the parser in a receive buffer like the task's */
static void feed_from(const uint8_t *b, int len, uint32_t ip)
{
    uint8_t *copy = host_rx_buffer(b, (size_t)len, ARTNET_RX_BUF_SIZE);
    struct sockaddr_in from = { .sin_family = AF_INET, .sin_port = htons(6454) };
    from.sin_addr.s_addr = ip;
    s_an.stats.packets_received++;
    process_packet(copy, len, &from);
    host_rx_free(copy, ARTNET_RX_BUF_SIZE);
}
static void feed(const uint8_t *b, int len) { feed_from(b, len, IP(2, 0, 0, 99)); }

/* ---- ArtDmx ------------------------------------------------------------- */

static void test_artdmx_valid(void)
{
    uint8_t levels[512], b[600];
    for (int i = 0; i < 512; i++) levels[i] = (uint8_t)(i * 7);
    int n = pk_artdmx(b, 17, 0x0001, levels, 512);
    feed(b, n);
    CHECK_EQ(dmx.calls, 1);
    CHECK_EQ(dmx.universe, 0x0001);
    CHECK_EQ(dmx.length, 512);
    CHECK_EQ(dmx.sequence, 17);
    CHECK_EQ(dmx.source_ip, IP(2, 0, 0, 99));
    CHECK_MEM(dmx.data, levels, 512);
    CHECK_EQ(s_an.stats.dmx_packets, 1);
    CHECK_EQ(s_an.stats.invalid_packets, 0);
}

static void test_artdmx_port_address(void)
{
    uint8_t b[64], d[2] = {1, 2};
    int n = pk_artdmx(b, 0, 0x7ABC, d, 2);
    feed(b, n);
    CHECK_EQ(dmx.universe, 0x7ABC);
    /* bit 7 of Net is not part of the 15-bit Port-Address */
    b[15] |= 0x80;
    feed(b, n);
    CHECK_EQ(dmx.universe, 0x7ABC);
}

static void test_artdmx_short_frame(void)
{
    uint8_t b[64], d[2] = {9, 8};
    int n = pk_artdmx(b, 0, 0, d, 2);
    feed(b, n);
    CHECK_EQ(dmx.calls, 1);
    CHECK_EQ(dmx.length, 2);
    CHECK_EQ(dmx.data[0], 9);
    CHECK_EQ(dmx.data[1], 8);
}

static void test_artdmx_truncated_uses_what_arrived(void)
{
    uint8_t levels[512] = {0}, b[600];
    levels[99] = 0x55;
    pk_artdmx(b, 0, 0, levels, 512);
    feed(b, 18 + 100);             /* Length says 512, only 100 slots arrived */
    CHECK_EQ(dmx.calls, 1);
    CHECK_EQ(dmx.length, 100);
    CHECK_EQ(dmx.data[99], 0x55);
}

static void test_artdmx_bad_lengths(void)
{
    uint8_t levels[512] = {0}, b[600];
    int n = pk_artdmx(b, 0, 0, levels, 512);

    pk_be16(&b[16], 0);            /* Length 0 */
    feed(b, n);
    pk_be16(&b[16], 513);          /* Length > 512 */
    feed(b, n);
    pk_be16(&b[16], 0xFFFF);
    feed(b, n);
    pk_be16(&b[16], 512);
    feed(b, 18);                   /* header only, no slots */
    feed(b, 17);                   /* shorter than the ArtDmx header */
    CHECK_EQ(dmx.calls, 0);
    CHECK_EQ(s_an.stats.invalid_packets, 5);
    CHECK_EQ(s_an.stats.dmx_packets, 0);
}

static void test_artdmx_every_truncation(void)
{
    uint8_t levels[512], b[600];
    memset(levels, 0xAA, sizeof(levels));
    int n = pk_artdmx(b, 0, 0, levels, 512);
    for (int len = 0; len < n; len++) {
        dmx.calls = 0;
        feed(b, len);
        CHECK_EQ(dmx.calls, len > 18 ? 1 : 0);
        if (len > 18) CHECK_EQ(dmx.length, len - 18);
    }
}

/* ---- Header / OpCode ------------------------------------------------------ */

static void test_bad_id(void)
{
    uint8_t d[2] = {0}, b[64];
    int n = pk_artdmx(b, 0, 0, d, 2);
    b[7] = 'x';                    /* "Art-Net" must be followed by NUL */
    feed(b, n);
    memcpy(b, "Art-NeT", 7);
    b[7] = 0;
    feed(b, n);
    memcpy(b, "Art-Net\0", 8);
    feed(b, 9);                    /* too short for the OpCode */
    feed(b, 0);
    CHECK_EQ(dmx.calls, 0);
    CHECK_EQ(s_an.stats.invalid_packets, 4);
}

static void test_unknown_and_ignored_opcodes(void)
{
    uint8_t b[64] = {0};
    pk_art_header(b, 0x1234);
    feed(b, 20);
    pk_art_header(b, 0x0050);      /* ArtDmx with the OpCode byte-swapped */
    feed(b, 20);
    CHECK_EQ(s_an.stats.unsupported_packets, 2);
    pk_art_header(b, ARTNET_OP_POLL_REPLY);   /* other nodes: silently ignored */
    feed(b, 20);
    pk_art_header(b, ARTNET_OP_TOD_DATA);
    feed(b, 20);
    CHECK_EQ(s_an.stats.unsupported_packets, 2);
    CHECK_EQ(s_an.stats.invalid_packets, 0);
    CHECK_EQ(dmx.calls, 0);
    CHECK_EQ(host_sent_count, 0);
}

static void test_own_packets_ignored(void)
{
    uint8_t b[64], d[2] = {1, 2};
    int n = pk_artpoll(b, 0, 0, 0, 0);
    feed(b, n);                    /* learns the node's own IP from the status */
    n = pk_artdmx(b, 0, 0, d, 2);
    feed_from(b, n, IP(2, 0, 0, 10));
    CHECK_EQ(dmx.calls, 0);
    CHECK_EQ(s_an.stats.own_packets, 1);
}

/* ---- ArtPoll / ArtPollReply ----------------------------------------------- */

static void test_artpoll_replies_per_port(void)
{
    uint8_t b[64];
    int n = pk_artpoll(b, 0, 0, 0, 0);
    feed(b, n);
    CHECK_EQ(s_an.stats.poll_packets, 1);
    CHECK_EQ(host_sent_count, 2);   /* one ArtPollReply per enabled port */

    const host_sent_t *r = &host_sent[0];
    CHECK_EQ(r->len, ARTNET_REPLY_LEN);
    CHECK_EQ(r->dest_ip, IP(2, 255, 255, 255));   /* directed broadcast */
    CHECK_EQ(r->dest_port, 6454);
    CHECK_MEM(r->data, "Art-Net\0", 8);
    CHECK_EQ(r->data[8] | (r->data[9] << 8), ARTNET_OP_POLL_REPLY);
    CHECK_MEM(&r->data[10], node.ip, 4);
    CHECK_EQ(r->data[14] | (r->data[15] << 8), 6454);
    CHECK_EQ((r->data[16] << 8) | r->data[17], 0x0102);
    CHECK_STR((const char *)&r->data[26], "Test Node");
    CHECK_STR((const char *)&r->data[44], "Host test node");
    CHECK_EQ(r->data[173], 1);      /* NumPorts */
    CHECK_EQ(r->data[174], 0x80);   /* output */
    CHECK_EQ(r->data[190], 0x01);   /* SwOut */
    CHECK_EQ(r->data[211], 1);      /* BindIndex */
    CHECK_MEM(&r->data[201], node.mac, 6);

    r = &host_sent[1];
    CHECK_EQ(r->data[18], 0x01);    /* NetSwitch */
    CHECK_EQ(r->data[19], 0x02);    /* SubSwitch */
    CHECK_EQ(r->data[174], 0x40);   /* input */
    CHECK_EQ(r->data[186], 0x03);   /* SwIn */
    CHECK_EQ(r->data[211], 2);
}

static void test_artpoll_targeted(void)
{
    uint8_t b[64];
    int n = pk_artpoll(b, 0, 1, 0x0200, 0x0100);   /* only 0x100..0x200 */
    feed(b, n);
    CHECK_EQ(host_sent_count, 1);
    CHECK_EQ(host_sent[0].data[211], 2);

    host_sent_count = 0;
    n = pk_artpoll(b, 0, 1, 0x7FFF, 0x0500);
    feed(b, n);
    CHECK_EQ(host_sent_count, 0);   /* targeted and nothing in range: silent */

    host_sent_count = 0;
    feed(b, 17);                    /* targeted bit but no range: normal poll */
    CHECK_EQ(host_sent_count, 2);
}

static void test_artpoll_no_ports_still_announces(void)
{
    node.ports[0].enabled = node.ports[1].enabled = false;
    uint8_t b[64];
    feed(b, pk_artpoll(b, 0, 0, 0, 0));
    CHECK_EQ(host_sent_count, 1);
}

static void test_artpoll_other_subnet_unicast(void)
{
    uint8_t b[64];
    feed_from(b, pk_artpoll(b, 0, 0, 0, 0), IP(10, 0, 0, 7));
    CHECK_EQ(host_sent_count, 4);   /* broadcast + unicast per port */
    CHECK_EQ(host_sent[1].dest_ip, IP(10, 0, 0, 7));
}

static void test_artpoll_flags_and_errors(void)
{
    uint8_t b[64];
    int n = pk_artpoll(b, 0x02, 0, 0, 0);   /* TalkToMe: reply on change */
    feed(b, n);
    CHECK(s_an.reply_on_change);
    feed(b, 13);
    CHECK_EQ(s_an.stats.invalid_packets, 1);
    CHECK_EQ(s_an.stats.poll_packets, 1);

    host_sent_count = 0;
    artnet_receiver_enable_poll_reply(false);
    feed(b, n);
    CHECK_EQ(host_sent_count, 0);
}

static void test_artpoll_without_status_or_ip(void)
{
    uint8_t b[64];
    artnet_receiver_set_status_callback(NULL, NULL);
    feed(b, pk_artpoll(b, 0, 0, 0, 0));
    CHECK_EQ(host_sent_count, 0);
    artnet_receiver_set_status_callback(on_status, NULL);
    memset(node.ip, 0, 4);          /* no IP yet */
    feed(b, pk_artpoll(b, 0, 0, 0, 0));
    CHECK_EQ(host_sent_count, 0);
}

static void test_poll_reply_long_names_terminated(void)
{
    memset(node.short_name, 'S', sizeof(node.short_name));   /* no NUL */
    memset(node.long_name, 'L', sizeof(node.long_name));
    uint8_t b[64];
    feed(b, pk_artpoll(b, 0, 0, 0, 0));
    CHECK_EQ(host_sent_count, 2);
    CHECK_EQ(strnlen((const char *)&host_sent[0].data[26], 18), 17);
    CHECK_EQ(strnlen((const char *)&host_sent[0].data[44], 64), 63);
}

/* ---- ArtAddress ----------------------------------------------------------- */

static void test_artaddress_program(void)
{
    uint8_t b[128];
    pk_artaddress_t a = {
        .net = 0x80 | 0x05, .bind_index = 2, .short_name = "Stage L",
        .long_name = "Stage left dimmer rack", .swin = 0x7F, .swout = 0x80 | 0x0A,
        .sub = 0x80 | 0x03, .acn_priority = 150, .command = ARTNET_AC_MERGE_LTP0,
    };
    feed(b, pk_artaddress(b, &a));
    CHECK_EQ(addr.calls, 1);
    CHECK_EQ(addr.cmd.port, 2);
    CHECK_EQ(addr.cmd.net, 5);
    CHECK_EQ(addr.cmd.sub, 3);
    CHECK_EQ(addr.cmd.universe, 0x0A);
    CHECK(!addr.cmd.reset_address);
    CHECK_EQ(addr.cmd.command, ARTNET_AC_MERGE_LTP0);
    CHECK_EQ(addr.cmd.acn_priority, 150);
    CHECK_STR(addr.short_name, "Stage L");
    CHECK_STR(addr.long_name, "Stage left dimmer rack");
    CHECK_EQ(s_an.stats.address_packets, 1);
    CHECK_EQ(host_sent_count, 2);   /* answered with ArtPollReply */
}

static void test_artaddress_no_change_and_reset(void)
{
    uint8_t b[128];
    pk_artaddress_t a = { .net = 0x7F, .bind_index = 0, .swin = 0x7F, .swout = 0x7F, .sub = 0x7F };
    feed(b, pk_artaddress(b, &a));
    CHECK_EQ(addr.calls, 1);
    CHECK_EQ(addr.cmd.port, 1);     /* BindIndex 0 means the first port */
    CHECK_EQ(addr.cmd.net, -1);
    CHECK_EQ(addr.cmd.sub, -1);
    CHECK_EQ(addr.cmd.universe, -1);
    CHECK(!addr.cmd.reset_address);
    CHECK(addr.cmd.short_name == NULL);
    CHECK(addr.cmd.long_name == NULL);

    a.net = 0x00;                   /* 0 = reset to the default address */
    feed(b, pk_artaddress(b, &a));
    CHECK(addr.cmd.reset_address);
    a.net = 0x7F; a.sub = 0x00;
    feed(b, pk_artaddress(b, &a));
    CHECK(addr.cmd.reset_address);
    a.sub = 0x7F; a.swin = 0x80 | 0x04;   /* SwIn used when SwOut has no change */
    feed(b, pk_artaddress(b, &a));
    CHECK_EQ(addr.cmd.universe, 4);
}

static void test_artaddress_names_not_terminated(void)
{
    uint8_t b[128];
    pk_artaddress_t a = { .net = 0x7F, .swin = 0x7F, .swout = 0x7F, .sub = 0x7F };
    pk_artaddress(b, &a);
    memset(&b[14], 'A', 18);        /* ShortName without NUL */
    memset(&b[32], 'B', 64);        /* LongName without NUL */
    feed(b, 107);
    CHECK_EQ(addr.calls, 1);
    CHECK_EQ(strlen(addr.short_name), 17);
    CHECK_EQ(strlen(addr.long_name), 63);
}

static void test_artaddress_bad(void)
{
    uint8_t b[128];
    pk_artaddress_t a = { .net = 0x81, .bind_index = 3 };   /* this node has 2 ports */
    feed(b, pk_artaddress(b, &a));
    CHECK_EQ(addr.calls, 0);
    a.bind_index = 1;
    pk_artaddress(b, &a);
    feed(b, 106);                   /* one byte short */
    CHECK_EQ(addr.calls, 0);
    CHECK_EQ(s_an.stats.invalid_packets, 1);
}

/* ---- ArtSync -------------------------------------------------------------- */

static void test_artsync(void)
{
    uint8_t b[32];
    int n = pk_artsync(b);
    feed_from(b, n, IP(2, 0, 0, 50));
    CHECK_EQ(sync_rx.calls, 1);
    CHECK_EQ(sync_rx.ip, IP(2, 0, 0, 50));
    CHECK_EQ(s_an.stats.sync_packets, 1);
    feed(b, 13);
    CHECK_EQ(sync_rx.calls, 1);
    CHECK_EQ(s_an.stats.invalid_packets, 1);
}

/* ---- RDM: ArtTodRequest / ArtTodControl / ArtRdm -------------------------- */

static void test_tod_request(void)
{
    uint8_t b[128] = {0};
    pk_art_header(b, ARTNET_OP_TOD_REQUEST);
    b[21] = 0x81;                   /* Net (bit 7 ignored) */
    b[23] = 40;                     /* AddCount > 32 */
    for (int i = 0; i < 32; i++) b[24 + i] = (uint8_t)i;
    feed(b, 24 + 32);
    CHECK_EQ(rdm.calls, 1);
    CHECK_EQ(rdm.ev.type, ARTNET_RDM_EV_TOD_REQUEST);
    CHECK_EQ(rdm.ev.address_count, 32);
    CHECK_EQ(rdm.ev.addresses[5], 0x0105);

    b[23] = 10;                     /* AddCount 10, only 4 addresses sent */
    feed(b, 24 + 4);
    CHECK_EQ(rdm.ev.address_count, 4);
    feed(b, 23);
    CHECK_EQ(rdm.calls, 2);
    CHECK_EQ(s_an.stats.invalid_packets, 1);
}

static void test_tod_control_and_rdm(void)
{
    uint8_t b[700] = {0};
    pk_art_header(b, ARTNET_OP_TOD_CONTROL);
    b[21] = 0x02; b[22] = ARTNET_ATC_FLUSH; b[23] = 0x34;
    feed(b, 24);
    CHECK_EQ(rdm.ev.type, ARTNET_RDM_EV_TOD_CONTROL);
    CHECK_EQ(rdm.ev.port_address, 0x0234);
    CHECK_EQ(rdm.ev.command, ARTNET_ATC_FLUSH);
    feed(b, 23);
    CHECK_EQ(rdm.calls, 1);

    pk_art_header(b, ARTNET_OP_RDM);
    b[21] = 0; b[23] = 1;
    for (int i = 0; i < 24; i++) b[24 + i] = (uint8_t)(0x10 + i);
    feed(b, 24 + 24);
    CHECK_EQ(rdm.calls, 2);
    CHECK_EQ(rdm.ev.type, ARTNET_RDM_EV_RDM);
    CHECK_EQ(rdm.ev.rdm_len, 24);
    CHECK_EQ(rdm.rdm[0], 0x10);
    feed(b, 24 + 22);               /* shorter than any RDM message */
    CHECK_EQ(rdm.calls, 2);
    CHECK_EQ(s_an.stats.invalid_packets, 2);
}

/* ---- ArtIpProg ------------------------------------------------------------ */

static void test_ipprog(void)
{
    uint8_t b[64] = {0};
    pk_art_header(b, ARTNET_OP_IP_PROG);
    b[14] = 0x80 | 0x04 | 0x02 | 0x10;     /* program IP, mask, gateway */
    memcpy(&b[16], (uint8_t[]){10, 0, 0, 5}, 4);
    memcpy(&b[20], (uint8_t[]){255, 0, 0, 0}, 4);
    memcpy(&b[26], (uint8_t[]){10, 0, 0, 1}, 4);

    feed(b, 34);
    CHECK_EQ(ipprog.calls, 0);      /* no handler: ignored */
    CHECK_EQ(s_an.stats.unsupported_packets, 1);

    artnet_receiver_set_ipprog_callback(on_ipprog, NULL);
    feed_from(b, 34, IP(2, 0, 0, 77));
    CHECK_EQ(ipprog.calls, 1);
    CHECK(ipprog.cmd.program && ipprog.cmd.set_ip && ipprog.cmd.set_mask && ipprog.cmd.set_gateway);
    CHECK(!ipprog.cmd.dhcp && !ipprog.cmd.reset_default);
    CHECK_EQ(ipprog.cmd.ip[0], 10);
    CHECK_EQ(ipprog.cmd.gateway[3], 1);
    CHECK_EQ(host_sent_count, 1);
    CHECK_EQ(host_sent[0].len, 34);
    CHECK_EQ(host_sent[0].dest_ip, IP(2, 0, 0, 77));
    CHECK_EQ(host_sent[0].data[8] | (host_sent[0].data[9] << 8), ARTNET_OP_IP_PROG_REPLY);
    CHECK_EQ(host_sent[0].data[16], 10);

    feed(b, 29);                    /* no ProgDg: gateway not programmed */
    CHECK(!ipprog.cmd.set_gateway);
    feed(b, 23);
    CHECK_EQ(ipprog.calls, 2);
    CHECK_EQ(s_an.stats.invalid_packets, 1);
}

/* ---- Outgoing packets ----------------------------------------------------- */

static void test_send_tod_data_blocks(void)
{
    static uint8_t uids[450][6];
    for (int i = 0; i < 450; i++) uids[i][5] = (uint8_t)i;
    CHECK_EQ(artnet_receiver_send_tod_data(IP(2, 0, 0, 1), 1, 0x0001, false,
                                           (const uint8_t (*)[6])uids, 450), ESP_OK);
    CHECK_EQ(host_sent_count, 3);   /* 200 + 200 + 50 */
    CHECK_EQ(host_sent[2].data[26], 2);    /* BlockCount */
    CHECK_EQ(host_sent[2].data[27], 50);   /* UidCount */
    CHECK_EQ(host_sent[2].len, 28 + 50 * 6);

    host_sent_count = 0;
    CHECK_EQ(artnet_receiver_send_tod_data(IP(2, 0, 0, 1), 1, 0x0001, true, NULL, 0), ESP_OK);
    CHECK_EQ(host_sent_count, 1);
    CHECK_EQ(host_sent[0].data[22], ARTNET_TOD_NAK);
}

/* ---- other nodes (ArtPollReply in, ArtPoll out) --------------------------- */

/* Another node's reply for port @p idx, built the way this node builds its own */
static int peer_reply(uint8_t *b, const uint8_t ip[4], const uint8_t mac[6], int idx)
{
    artnet_node_status_t other = node;
    memcpy(other.ip, ip, 4);
    memcpy(other.mac, mac, 6);
    snprintf(other.short_name, sizeof(other.short_name), "Stage left");
    snprintf(other.long_name, sizeof(other.long_name), "Stage left rack, 2 ports");
    other.ports[0].port_address = 0x0205;     /* output 0:2:5 */
    other.ports[0].data_active = true;
    other.ports[1].port_address = 0x0123;     /* input 1:2:3, same as this node's */
    other.ports[1].data_active = true;
    build_reply(b, &other, idx);
    return ARTNET_REPLY_LEN;
}

static const uint8_t PEER_IP[4] = {2, 0, 0, 20};
static const uint8_t PEER_MAC[6] = {0x00, 0x50, 0xC2, 9, 8, 7};

static void test_peer_reply_parsed(void)
{
    uint8_t b[ARTNET_REPLY_LEN];
    feed_from(b, peer_reply(b, PEER_IP, PEER_MAC, 0), IP(2, 0, 0, 20));
    CHECK_EQ(peer.calls, 1);
    CHECK(!peer.conflict);
    CHECK_EQ(s_an.stats.peer_replies, 1);
    CHECK_EQ(host_sent_count, 0);              /* a reply is never answered */
    const artnet_poll_reply_t *r = &peer.r;
    CHECK_EQ(r->ip, IP(2, 0, 0, 20));
    CHECK_EQ(r->from_ip, IP(2, 0, 0, 20));
    CHECK_MEM(r->mac, PEER_MAC, 6);
    CHECK_STR(r->short_name, "Stage left");
    CHECK_STR(r->long_name, "Stage left rack, 2 ports");
    CHECK_EQ(r->firmware, 0x0102);
    CHECK_EQ(r->esta, ARTNET_ESTA_MAN);
    CHECK_EQ(r->bind_index, 1);
    CHECK_EQ(r->num_ports, 1);
    CHECK_EQ(r->ports[0].type, 0x80);
    CHECK_EQ(r->ports[0].out_address, 0x0205);
    CHECK(r->ports[0].good_output & 0x80);

    feed_from(b, peer_reply(b, PEER_IP, PEER_MAC, 1), IP(2, 0, 0, 20));
    CHECK_EQ(peer.calls, 2);
    CHECK_EQ(r->bind_index, 2);
    CHECK_EQ(r->ports[0].type, 0x40);
    CHECK_EQ(r->ports[0].in_address, 0x0123);
    CHECK(r->ports[0].good_input & 0x80);
}

static void test_peer_reply_names_terminated(void)
{
    uint8_t b[ARTNET_REPLY_LEN];
    peer_reply(b, PEER_IP, PEER_MAC, 0);
    memset(&b[26], 'S', 18);                   /* no NUL in either name */
    memset(&b[44], 'L', 64);
    b[173] = 9;                                /* more ports than a reply holds */
    feed_from(b, ARTNET_REPLY_LEN, IP(2, 0, 0, 20));
    CHECK_EQ(peer.calls, 1);
    CHECK_EQ(strlen(peer.r.short_name), 17);
    CHECK_EQ(strlen(peer.r.long_name), 63);
    CHECK_EQ(peer.r.num_ports, ARTNET_REPLY_PORTS);
}

static void test_peer_reply_every_truncation(void)
{
    uint8_t b[ARTNET_REPLY_LEN];
    peer_reply(b, PEER_IP, PEER_MAC, 0);
    int parsed = 0;
    for (int len = 0; len <= ARTNET_REPLY_LEN; len++) {
        peer.calls = 0;
        feed_from(b, len, IP(2, 0, 0, 20));    /* ASan: no read past len */
        parsed += peer.calls;
        if (len == 206) CHECK_EQ(peer.r.bind_index, 0);   /* not there yet */
    }
    CHECK_EQ(parsed, ARTNET_REPLY_LEN - 194 + 1);   /* from SwOut on */
    CHECK_EQ(host_sent_count, 0);
}

static void test_peer_ip_conflict(void)
{
    uint8_t b[ARTNET_REPLY_LEN];
    /* this node's own reply coming back: not a peer */
    feed_from(b, peer_reply(b, node.ip, node.mac, 0), IP(2, 0, 0, 10));
    CHECK_EQ(peer.calls, 0);
    CHECK_EQ(s_an.stats.own_packets, 1);
    /* a reply from this address without a MAC: can't tell, so it's ours */
    static const uint8_t no_mac[6];
    feed_from(b, peer_reply(b, node.ip, no_mac, 0), IP(2, 0, 0, 10));
    CHECK_EQ(peer.calls, 0);

    /* another device on this node's address */
    feed_from(b, peer_reply(b, node.ip, PEER_MAC, 0), IP(2, 0, 0, 10));
    CHECK_EQ(peer.calls, 1);
    CHECK(peer.conflict);
    /* ... or one that says it has it */
    feed_from(b, peer_reply(b, node.ip, PEER_MAC, 0), IP(2, 0, 0, 20));
    CHECK_EQ(peer.calls, 2);
    CHECK(peer.conflict);
    CHECK_EQ(peer.r.from_ip, IP(2, 0, 0, 20));
    CHECK_EQ(peer.r.ip, IP(2, 0, 0, 10));
}

static void test_self_poll(void)
{
    CHECK(!artnet_receiver_poll_network());    /* not running */
    s_an.running = true;
    CHECK(artnet_receiver_poll_network());
    CHECK(s_an.self_poll_pending);
    host_advance_ms(ARTNET_SELF_POLL_MS - 1);
    CHECK(!artnet_receiver_poll_network());    /* too soon */
    host_advance_ms(1);
    CHECK(artnet_receiver_poll_network());
    s_an.running = false;

    send_poll();
    CHECK_EQ(host_sent_count, 1);
    const host_sent_t *p = &host_sent[0];
    CHECK_EQ(p->len, 22);
    CHECK_EQ(p->dest_ip, IP(2, 255, 255, 255));
    CHECK_EQ(p->dest_port, 6454);
    CHECK_MEM(p->data, "Art-Net\0", 8);
    CHECK_EQ(p->data[8] | (p->data[9] << 8), ARTNET_OP_POLL);
    CHECK_EQ(p->data[11], ARTNET_PROTOCOL_VERSION);
    CHECK_EQ(p->data[12], 0);                  /* not targeted, no diagnostics */

    /* hearing its own poll: ignored, not answered */
    host_sent_count = 0;
    feed_from(host_sent[0].data, 22, IP(2, 0, 0, 10));
    CHECK_EQ(s_an.stats.poll_packets, 0);
    CHECK_EQ(host_sent_count, 0);
}

int main(void)
{
    s_an_pristine = s_an;
    RUN(test_artdmx_valid);
    RUN(test_artdmx_port_address);
    RUN(test_artdmx_short_frame);
    RUN(test_artdmx_truncated_uses_what_arrived);
    RUN(test_artdmx_bad_lengths);
    RUN(test_artdmx_every_truncation);
    RUN(test_bad_id);
    RUN(test_unknown_and_ignored_opcodes);
    RUN(test_own_packets_ignored);
    RUN(test_artpoll_replies_per_port);
    RUN(test_artpoll_targeted);
    RUN(test_artpoll_no_ports_still_announces);
    RUN(test_artpoll_other_subnet_unicast);
    RUN(test_artpoll_flags_and_errors);
    RUN(test_artpoll_without_status_or_ip);
    RUN(test_poll_reply_long_names_terminated);
    RUN(test_artaddress_program);
    RUN(test_artaddress_no_change_and_reset);
    RUN(test_artaddress_names_not_terminated);
    RUN(test_artaddress_bad);
    RUN(test_artsync);
    RUN(test_tod_request);
    RUN(test_tod_control_and_rdm);
    RUN(test_ipprog);
    RUN(test_send_tod_data_blocks);
    RUN(test_peer_reply_parsed);
    RUN(test_peer_reply_names_terminated);
    RUN(test_peer_reply_every_truncation);
    RUN(test_peer_ip_conflict);
    RUN(test_self_poll);
    return test_summary();
}
