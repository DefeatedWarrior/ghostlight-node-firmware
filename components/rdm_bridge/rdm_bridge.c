/**
 * @file rdm_bridge.c
 * @brief RDM over Art-Net — see rdm_bridge.h
 *
 * Everything runs in one task. The Art-Net receive task and the discovery
 * tasks only post messages to it, so RDM transactions (which hold the DMX
 * bus for up to ~30 ms) never stall Art-Net reception.
 *
 * Messages are queued at their own size, so a console's burst of requests
 * (most are about 26 bytes) fits: about 36 in the 2 KB queue, about 140 in
 * 8 KB of PSRAM.
 */

#include "rdm_bridge.h"
#include "dmx_handler.h"
#include "artnet_receiver.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "rdm_bridge";

#define BRIDGE_TASK_STACK     4096
#define BRIDGE_TASK_PRIORITY  5
#if CONFIG_SPIRAM
#define BRIDGE_QUEUE_BYTES    8192                  /* in PSRAM */
#else
#define BRIDGE_QUEUE_BYTES    2048
#endif
#define REQUEST_MAX_AGE_US    (3LL * 1000000)       /* the controller gave up on it by then */
#define RDM_GET_ATTEMPTS      3                     /* a GET whose reply was lost, and two more */
#define BRIDGE_PORTS          DMX_PORT_MAX
#define SUBSCRIBERS_MAX       8
#define SUBSCRIBER_TTL_US     (15LL * 60 * 1000000)  /* forget controllers after 15 min */
#define BACKGROUND_PERIOD_US  (30LL * 1000000)       /* incremental discovery interval */
#define RETRY_FULL_US         (5LL * 1000000)        /* retry a missing TOD this often */
#define RDM_MAX_PACKET        257                   /* SC + 255 + checksum */

typedef enum {
    MSG_ARTNET = 0,
    MSG_DISCOVERY_DONE,
} msg_type_t;

/* What artnet_rdm_event_t carries, without its fixed-size arrays */
typedef struct {
    uint8_t type;                /* msg_type_t */
    uint8_t ev_type;             /* artnet_rdm_event_type_t */
    uint8_t port;                /* DISCOVERY_DONE */
    uint8_t net;
    uint8_t command;
    uint8_t address_count;
    uint16_t port_address;
    uint32_t source_ip;
    uint16_t len;                /* of data: the addresses (TOD_REQUEST) or the RDM packet */
    int64_t queued_us;
    uint8_t data[];
} bridge_msg_t;

typedef struct {
    volatile uint16_t port_address;
    bool available;              /* RDM usable on the port right now */
    bool background;             /* incremental discovery on (AtcIncOn/Off) */
    bool tod_announced;          /* subscribers have this TOD */
    bool had_tod;                /* TOD was valid at the last check */
    int64_t last_discovery_us;
    uint16_t uid_count;
    uint8_t uids[DMX_MAX_DEVICES][6];
} bridge_port_t;

static struct {
    RingbufHandle_t queue;
    TaskHandle_t task;
    bridge_port_t ports[BRIDGE_PORTS];
    /* refresh_tod() scratch, bridge task only: too big for its stack */
    rdm_uid_t tod_uids[DMX_MAX_DEVICES];
    uint8_t tod_bytes[DMX_MAX_DEVICES][6];
    struct {
        uint32_t ip;
        int64_t last_us;
    } subs[SUBSCRIBERS_MAX];
    rdm_bridge_stats_t stats;
} s_br;

// ============================================================================
// Helpers
// ============================================================================

static void uid_to_bytes(const rdm_uid_t *u, uint8_t out[6])
{
    out[0] = u->man_id >> 8;
    out[1] = u->man_id & 0xFF;
    out[2] = u->dev_id >> 24;
    out[3] = (u->dev_id >> 16) & 0xFF;
    out[4] = (u->dev_id >> 8) & 0xFF;
    out[5] = u->dev_id & 0xFF;
}

/* Port (1-based) whose Port-Address is @p pa and that can do RDM, else 0 */
static uint8_t port_for_address(uint16_t pa)
{
    for (int i = 0; i < BRIDGE_PORTS; i++) {
        if (s_br.ports[i].available && (s_br.ports[i].port_address & 0x7FFF) == (pa & 0x7FFF)) {
            return (uint8_t)(i + 1);
        }
    }
    return 0;
}

static void add_subscriber(uint32_t ip)
{
    int64_t now = esp_timer_get_time();
    int slot = -1, oldest = 0;
    for (int i = 0; i < SUBSCRIBERS_MAX; i++) {
        if (s_br.subs[i].ip == ip) {
            slot = i;
            break;
        }
        if (s_br.subs[i].ip == 0 && slot < 0) slot = i;
        if (s_br.subs[i].last_us < s_br.subs[oldest].last_us) oldest = i;
    }
    if (slot < 0) slot = oldest;
    s_br.subs[slot].ip = ip;
    s_br.subs[slot].last_us = now;
}

/* Re-read the port's TOD from the DMX driver; true if it differs */
static bool refresh_tod(int idx)
{
    bridge_port_t *bp = &s_br.ports[idx];
    rdm_uid_t *uids = s_br.tod_uids;
    size_t n = dmx_handler_get_rdm_uids((uint8_t)(idx + 1), uids, DMX_MAX_DEVICES);
    uint8_t (*bytes)[6] = s_br.tod_bytes;
    for (size_t i = 0; i < n; i++) {
        uid_to_bytes(&uids[i], bytes[i]);
    }
    bool changed = n != bp->uid_count || memcmp(bytes, bp->uids, n * 6) != 0;
    memcpy(bp->uids, bytes, n * 6);
    bp->uid_count = (uint16_t)n;
    return changed;
}

static void send_tod(uint32_t ip, int idx)
{
    bridge_port_t *bp = &s_br.ports[idx];
    bool nak = !dmx_handler_rdm_tod_valid((uint8_t)(idx + 1));
    if (!nak) refresh_tod(idx);
    if (artnet_receiver_send_tod_data(ip, (uint8_t)(idx + 1), bp->port_address, nak,
                                      (const uint8_t (*)[6])bp->uids, bp->uid_count) == ESP_OK) {
        s_br.stats.tod_data_sent++;
    }
}

static void announce_tod(int idx)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < SUBSCRIBERS_MAX; i++) {
        if (s_br.subs[i].ip == 0) continue;
        if (now - s_br.subs[i].last_us > SUBSCRIBER_TTL_US) {
            s_br.subs[i].ip = 0;
            continue;
        }
        send_tod(s_br.subs[i].ip, idx);
    }
    s_br.ports[idx].tod_announced = true;
}

static void start_discovery(int idx, dmx_disc_mode_t mode)
{
    esp_err_t err = dmx_handler_rdm_discover_mode((uint8_t)(idx + 1), mode);
    if (err == ESP_OK) {
        s_br.ports[idx].last_discovery_us = esp_timer_get_time();
    }
}

// ============================================================================
// Message handlers (bridge task)
// ============================================================================

static void on_tod_request(const artnet_rdm_event_t *ev)
{
    s_br.stats.tod_requests++;
    add_subscriber(ev->source_ip);
    for (int i = 0; i < BRIDGE_PORTS; i++) {
        if (!s_br.ports[i].available) continue;
        uint16_t pa = s_br.ports[i].port_address & 0x7FFF;
        for (int a = 0; a < ev->address_count; a++) {
            if (ev->addresses[a] == pa) {
                send_tod(ev->source_ip, i);
                break;
            }
        }
    }
}

static void on_tod_control(const artnet_rdm_event_t *ev)
{
    uint8_t port = port_for_address(ev->port_address);
    if (!port) return;
    int idx = port - 1;
    add_subscriber(ev->source_ip);
    switch (ev->command) {
    case ARTNET_ATC_FLUSH:
        ESP_LOGI(TAG, "Port %u: TOD flush requested", port);
        if (dmx_handler_rdm_is_discovering(port)) {
            dmx_handler_rdm_abort_discovery(port);
            dmx_handler_rdm_wait_discovery(port, 3000);
        }
        start_discovery(idx, DMX_DISC_FLUSH);
        s_br.ports[idx].tod_announced = false;
        break;
    case ARTNET_ATC_END:
        dmx_handler_rdm_abort_discovery(port);
        break;
    case ARTNET_ATC_INC_ON:
        s_br.ports[idx].background = true;
        ESP_LOGI(TAG, "Port %u: background discovery on", port);
        break;
    case ARTNET_ATC_INC_OFF:
        s_br.ports[idx].background = false;
        ESP_LOGI(TAG, "Port %u: background discovery off", port);
        break;
    default:
        break;
    }
    artnet_receiver_notify_change(true);   /* GoodOutputB discovery bits */
    send_tod(ev->source_ip, idx);
}

/* Lost (no reply) or garbled: worth asking again */
static bool reply_lost(esp_err_t err)
{
    return err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_CRC;
}

static void on_rdm(const artnet_rdm_event_t *ev, int64_t queued_us)
{
    uint8_t port = port_for_address(ev->port_address);
    if (!port || ev->command != 0x00) return;   /* only ArProcess */
    s_br.stats.rdm_requests++;
    if (esp_timer_get_time() - queued_us > REQUEST_MAX_AGE_US) {
        s_br.stats.rdm_expired++;   /* the controller has asked again or given up */
        return;
    }

    /* RdmPacket starts at the sub-start code; message length counts the
     * start code, so the packet must hold at least ML-1 bytes */
    const uint8_t *r = ev->rdm;
    uint16_t len = ev->rdm_len;
    if (len < 23 || r[0] != RDM_SUB_SC || r[1] < 24 || len < r[1] - 1) {
        s_br.stats.rdm_rejected++;
        return;
    }
    uint8_t ml = r[1];
    uint8_t cc = r[19];
    uint16_t pid = (uint16_t)(r[20] << 8 | r[21]);
    if (cc != RDM_CC_GET_COMMAND && cc != RDM_CC_SET_COMMAND) {
        s_br.stats.rdm_rejected++;   /* discovery is proxied by the node */
        return;
    }

    uint8_t req[RDM_MAX_PACKET];
    req[0] = RDM_SC;
    memcpy(&req[1], r, ml - 1);
    uint16_t sum = 0;
    for (int i = 0; i < ml; i++) sum += req[i];
    req[ml] = sum >> 8;
    req[ml + 1] = sum & 0xFF;

    /* A GET whose reply was lost is asked again. Not a SET: it may have been
     * done, and only its reply lost. Nor QUEUED_MESSAGE or STATUS_MESSAGES:
     * each reply hands a message over once, so asking again would skip one. */
    int attempts = (cc == RDM_CC_GET_COMMAND && pid != RDM_PID_QUEUED_MESSAGE &&
                    pid != RDM_PID_STATUS_MESSAGE) ? RDM_GET_ATTEMPTS : 1;
    uint8_t resp[RDM_MAX_PACKET];
    size_t resp_len = 0;
    esp_err_t err = ESP_FAIL;
    for (int a = 0; a < attempts; a++) {
        if (a) s_br.stats.rdm_retries++;
        resp_len = 0;
        err = dmx_handler_rdm_transaction(port, req, (size_t)ml + 2, resp, sizeof(resp),
                                          &resp_len);
        if (!reply_lost(err)) break;
    }
    if (err == ESP_OK && resp_len > 1) {
        s_br.stats.rdm_responses++;
        artnet_receiver_send_rdm(ev->source_ip, port, s_br.ports[port - 1].port_address,
                                 &resp[1], (uint16_t)(resp_len - 1));
    } else if (err == ESP_ERR_TIMEOUT) {
        s_br.stats.rdm_timeouts++;
    } else if (err != ESP_OK) {
        ESP_LOGD(TAG, "Port %u: ArtRdm PID 0x%04x failed: %s", port, pid, esp_err_to_name(err));
    }
}

static void on_discovery_done(uint8_t port)
{
    int idx = port - 1;
    if (idx < 0 || idx >= BRIDGE_PORTS || !s_br.ports[idx].available) return;
    if (!dmx_handler_rdm_tod_valid(port)) return;
    bool changed = refresh_tod(idx);
    if (changed || !s_br.ports[idx].tod_announced) {
        ESP_LOGI(TAG, "Port %u: TOD %s (%u device(s)), informing controllers", port,
                 changed ? "changed" : "ready", s_br.ports[idx].uid_count);
        announce_tod(idx);
    }
    artnet_receiver_notify_change(false);
}

/*
 * Once a second: notice ports that started/stopped, make sure every RDM port
 * has a TOD (full discovery after power-on or a port restart, which also
 * clears the TOD) and run background discovery.
 */
static void supervise(void)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < BRIDGE_PORTS; i++) {
        bridge_port_t *bp = &s_br.ports[i];
        uint8_t port = (uint8_t)(i + 1);
        bool avail = dmx_handler_rdm_available(port);
        if (avail != bp->available) {
            bp->available = avail;
            ESP_LOGI(TAG, "Port %u: RDM gateway %s (Port-Address %u)", port,
                     avail ? "active" : "inactive", bp->port_address);
        }
        bool tod = avail && dmx_handler_rdm_tod_valid(port);
        if (!tod && bp->had_tod) {
            /* Port restarted or TOD flushed: the next TOD goes to everyone */
            bp->tod_announced = false;
            bp->uid_count = 0;
        }
        bp->had_tod = tod;
        if (!avail || dmx_handler_rdm_is_discovering(port)) {
            continue;
        }
        if (!tod) {
            if (bp->last_discovery_us == 0 || now - bp->last_discovery_us > RETRY_FULL_US) {
                start_discovery(i, DMX_DISC_FULL);
            }
        } else if (bp->background && now - bp->last_discovery_us > BACKGROUND_PERIOD_US) {
            start_discovery(i, DMX_DISC_INCREMENTAL);
        }
    }
}

/* Handle one queued message, waiting up to @p wait for it */
static void bridge_step(TickType_t wait)
{
    size_t size = 0;
    bridge_msg_t *m = xRingbufferReceive(s_br.queue, &size, wait);
    if (!m) return;
    if (m->type == MSG_DISCOVERY_DONE) {
        on_discovery_done(m->port);
    } else {
        artnet_rdm_event_t ev = {
            .type = (artnet_rdm_event_type_t)m->ev_type,
            .source_ip = m->source_ip,
            .net = m->net,
            .command = m->command,
            .port_address = m->port_address,
        };
        switch (ev.type) {
        case ARTNET_RDM_EV_TOD_REQUEST:
            ev.address_count = m->address_count;
            memcpy(ev.addresses, m->data, (size_t)m->address_count * sizeof(uint16_t));
            on_tod_request(&ev);
            break;
        case ARTNET_RDM_EV_TOD_CONTROL:
            on_tod_control(&ev);
            break;
        case ARTNET_RDM_EV_RDM:
            ev.rdm = m->data;
            ev.rdm_len = m->len;
            on_rdm(&ev, m->queued_us);
            break;
        }
    }
    vRingbufferReturnItem(s_br.queue, m);
}

static void bridge_task(void *arg)
{
    (void)arg;
    int64_t last_supervise = 0;
    /* Task watchdog: the queue wait is 500 ms and each RDM exchange has its
     * own timeouts, so 10 s without a pass means the bridge is stuck */
    esp_task_wdt_add(NULL);
    for (;;) {
        esp_task_wdt_reset();
        bridge_step(pdMS_TO_TICKS(500));
        int64_t now = esp_timer_get_time();
        if (now - last_supervise > 1000000) {
            last_supervise = now;
            supervise();
        }
    }
}

// ============================================================================
// Callbacks (other tasks) -> queue
// ============================================================================

static void artnet_rdm_cb(const artnet_rdm_event_t *ev, void *user_data)
{
    (void)user_data;
    /* only the Art-Net receive task calls this */
    static union {
        bridge_msg_t m;
        uint8_t bytes[sizeof(bridge_msg_t) + RDM_MAX_PACKET];
    } u;
    bridge_msg_t *m = &u.m;
    memset(m, 0, sizeof(*m));
    m->type = MSG_ARTNET;
    m->ev_type = (uint8_t)ev->type;
    m->net = ev->net;
    m->command = ev->command;
    m->port_address = ev->port_address;
    m->source_ip = ev->source_ip;
    m->queued_us = esp_timer_get_time();
    if (ev->type == ARTNET_RDM_EV_TOD_REQUEST) {
        uint8_t n = ev->address_count < 32 ? ev->address_count : 32;
        m->address_count = n;
        m->len = (uint16_t)(n * sizeof(uint16_t));
        memcpy(m->data, ev->addresses, m->len);
    } else if (ev->type == ARTNET_RDM_EV_RDM && ev->rdm) {
        m->len = ev->rdm_len > RDM_MAX_PACKET ? RDM_MAX_PACKET : ev->rdm_len;
        memcpy(m->data, ev->rdm, m->len);
    }
    if (xRingbufferSend(s_br.queue, m, sizeof(*m) + m->len, 0) != pdTRUE) {
        s_br.stats.rdm_queue_full++;   /* the controller will ask again */
    }
}

static void discovery_cb(uint8_t port, uint8_t count, bool aborted, void *user_data)
{
    (void)count;
    (void)aborted;
    (void)user_data;
    bridge_msg_t m = {.type = MSG_DISCOVERY_DONE, .port = port};
    xRingbufferSend(s_br.queue, &m, sizeof(m), pdMS_TO_TICKS(100));
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t rdm_bridge_start(void)
{
    if (s_br.queue) return ESP_OK;
    for (int i = 0; i < BRIDGE_PORTS; i++) {
        s_br.ports[i].background = true;
    }
    bool in_psram = false;
#if CONFIG_SPIRAM
    s_br.queue = xRingbufferCreateWithCaps(BRIDGE_QUEUE_BYTES, RINGBUF_TYPE_NOSPLIT,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    in_psram = s_br.queue != NULL;
#endif
    if (!s_br.queue) {
        s_br.queue = xRingbufferCreate(BRIDGE_QUEUE_BYTES, RINGBUF_TYPE_NOSPLIT);
    }
    if (!s_br.queue) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(bridge_task, "rdm_bridge", BRIDGE_TASK_STACK, NULL,
                                BRIDGE_TASK_PRIORITY, &s_br.task, 0) != pdPASS) {
        if (in_psram) {
            vRingbufferDeleteWithCaps(s_br.queue);
        } else {
            vRingbufferDelete(s_br.queue);
        }
        s_br.queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    for (int p = 1; p <= BRIDGE_PORTS; p++) {
        dmx_handler_register_discovery_callback(p, discovery_cb, NULL);
    }
    artnet_receiver_set_rdm_callback(artnet_rdm_cb, NULL);
    ESP_LOGI(TAG, "RDM over Art-Net ready");
    return ESP_OK;
}

void rdm_bridge_set_port_address(uint8_t port, uint16_t port_address)
{
    if (port < 1 || port > BRIDGE_PORTS) return;
    s_br.ports[port - 1].port_address = port_address & 0x7FFF;
}

bool rdm_bridge_background_enabled(uint8_t port)
{
    if (port < 1 || port > BRIDGE_PORTS) return false;
    return s_br.ports[port - 1].background;
}

void rdm_bridge_get_stats(rdm_bridge_stats_t *stats)
{
    if (stats) *stats = s_br.stats;
}
