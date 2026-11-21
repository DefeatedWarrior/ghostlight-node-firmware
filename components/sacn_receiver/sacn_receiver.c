/**
 * @file sacn_receiver.c
 * @brief sACN (E1.31) receiver — see sacn_receiver.h
 */

#include "sacn_receiver.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

static const char *TAG = "sacn_receiver";

#define SACN_TASK_STACK_SIZE   6144
#define SACN_TASK_PRIORITY     6
#define SACN_TASK_CORE         0
#define SACN_RX_BUF_SIZE       700
#define SACN_RECEIVE_TIMEOUT_MS 500

/* Bytes before dmp.start_code */
#define SACN_HDR_LEN  (offsetof(sacn_packet_t, dmp) + offsetof(sacn_dmp_layer_t, start_code))

typedef struct {
    uint16_t universe;   /* 0 = free slot */
    bool joined;
} subscription_t;

static struct {
    bool initialized;
    volatile bool running;
    int sock;
    TaskHandle_t task;
    SemaphoreHandle_t task_done;
    SemaphoreHandle_t mutex;

    sacn_dmx_callback_t cb;
    void *cb_data;
    sacn_sync_callback_t sync_cb;
    void *sync_data;

    /* Sequence tracking for synchronization packets (per source + universe) */
    struct {
        uint16_t universe;
        uint8_t cid[16];
        uint8_t seq;
        TickType_t seen;
    } sync_seq[4];

    subscription_t subs[SACN_MAX_UNIVERSES];
    bool have_local_cid;
    uint8_t local_cid[16];

    sacn_stats_t stats;
} s_sacn = {
    .sock = -1,
};

static struct in_addr universe_group(uint16_t universe)
{
    struct in_addr a;
    a.s_addr = htonl(0xEFFF0000u | universe);
    return a;
}

/* Caller holds the mutex */
static bool join_group(subscription_t *sub, bool rejoin)
{
    if (s_sacn.sock < 0 || sub->universe == 0) {
        return false;
    }
    struct ip_mreq mreq = {
        .imr_multiaddr = universe_group(sub->universe),
        .imr_interface.s_addr = htonl(INADDR_ANY),
    };
    if (rejoin || sub->joined) {
        /* Leave first so lwIP re-joins on every interface that exists now */
        setsockopt(s_sacn.sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
        sub->joined = false;
    }
    if (setsockopt(s_sacn.sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        ESP_LOGW(TAG, "Join universe %u (%s) failed: errno %d — will retry on link up",
                 sub->universe, inet_ntoa(mreq.imr_multiaddr), errno);
        return false;
    }
    sub->joined = true;
    ESP_LOGI(TAG, "Joined universe %u (%s)", sub->universe, inet_ntoa(mreq.imr_multiaddr));
    return true;
}

/* Caller holds the mutex */
static void leave_group(subscription_t *sub)
{
    if (s_sacn.sock >= 0 && sub->joined) {
        struct ip_mreq mreq = {
            .imr_multiaddr = universe_group(sub->universe),
            .imr_interface.s_addr = htonl(INADDR_ANY),
        };
        setsockopt(s_sacn.sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
    }
    sub->joined = false;
}

// ============================================================================
// Packet parsing
// ============================================================================

/* Sync sequence check (E1.31 6.7.2 applied per source and sync universe) */
static bool sync_sequence_ok(uint16_t universe, const uint8_t *cid, uint8_t seq)
{
    TickType_t now = xTaskGetTickCount();
    int slot = -1, oldest = 0;
    for (int i = 0; i < 4; i++) {
        if (s_sacn.sync_seq[i].universe == universe &&
            memcmp(s_sacn.sync_seq[i].cid, cid, 16) == 0) {
            slot = i;
            break;
        }
        if (s_sacn.sync_seq[i].seen < s_sacn.sync_seq[oldest].seen) oldest = i;
    }
    if (slot >= 0 && (now - s_sacn.sync_seq[slot].seen) < pdMS_TO_TICKS(2500)) {
        int8_t diff = (int8_t)(seq - s_sacn.sync_seq[slot].seq);
        if (diff <= 0 && diff > -20) {
            return false;
        }
    }
    if (slot < 0) {
        slot = oldest;
        s_sacn.sync_seq[slot].universe = universe;
        memcpy(s_sacn.sync_seq[slot].cid, cid, 16);
    }
    s_sacn.sync_seq[slot].seq = seq;
    s_sacn.sync_seq[slot].seen = now;
    return true;
}

/*
 * Extended packets: synchronization (root vector 0x8, framing vector 0x1)
 *   38 flags/len, 40 vector, 44 sequence, 45-46 sync address, 47-48 reserved
 * Universe discovery (framing vector 0x2) is ignored.
 */
static void process_extended(const uint8_t *buf, int len, const struct sockaddr_in *from)
{
    const sacn_packet_t *p = (const sacn_packet_t *)buf;
    if (len < 44) {
        s_sacn.stats.invalid_packets++;
        return;
    }
    uint32_t vec = ((uint32_t)buf[40] << 24) | ((uint32_t)buf[41] << 16) |
                   ((uint32_t)buf[42] << 8) | buf[43];
    if (vec != SACN_EXT_VECTOR_SYNC) {
        s_sacn.stats.extended_packets++;
        return;
    }
    if (len < SACN_SYNC_PACKET_LEN) {
        s_sacn.stats.invalid_packets++;
        return;
    }
    if (s_sacn.have_local_cid && memcmp(p->root.cid, s_sacn.local_cid, 16) == 0) {
        s_sacn.stats.own_packets++;
        return;
    }
    uint16_t universe = ((uint16_t)buf[45] << 8) | buf[46];
    if (universe < 1 || universe > 63999) {
        s_sacn.stats.invalid_packets++;
        return;
    }
    if (!sync_sequence_ok(universe, p->root.cid, buf[44])) {
        return;
    }
    s_sacn.stats.sync_packets++;
    sacn_sync_callback_t cb = s_sacn.sync_cb;
    if (cb) {
        cb(universe, p->root.cid, from->sin_addr.s_addr, s_sacn.sync_data);
    }
}

static void process_packet(const uint8_t *buf, int len, const struct sockaddr_in *from)
{
    if (len < (int)offsetof(sacn_packet_t, framing)) {
        s_sacn.stats.invalid_packets++;
        return;
    }
    const sacn_packet_t *p = (const sacn_packet_t *)buf;

    if (ntohs(p->root.preamble_size) != 0x0010 ||
        ntohs(p->root.postamble_size) != 0x0000 ||
        memcmp(p->root.acn_pid, SACN_PACKET_IDENTIFIER, 12) != 0) {
        s_sacn.stats.invalid_packets++;
        return;
    }

    uint32_t root_vector = ntohl(p->root.vector);
    if (root_vector == SACN_ROOT_VECTOR_EXTENDED) {
        process_extended(buf, len, from);
        return;
    }
    if (root_vector != SACN_ROOT_VECTOR_DATA || len < (int)SACN_HDR_LEN + 1) {
        s_sacn.stats.invalid_packets++;
        return;
    }
    if (ntohl(p->framing.vector) != SACN_FRAME_VECTOR_DATA ||
        p->dmp.vector != SACN_DMP_VECTOR ||
        p->dmp.address_type != 0xA1 ||
        ntohs(p->dmp.first_address) != 0 ||
        ntohs(p->dmp.address_increment) != 1) {
        s_sacn.stats.invalid_packets++;
        return;
    }

    uint16_t prop_count = ntohs(p->dmp.property_count);
    if (prop_count < 1 || prop_count > 513 ||
        len < (int)(SACN_HDR_LEN + prop_count)) {
        s_sacn.stats.invalid_packets++;
        return;
    }

    uint16_t universe = ntohs(p->framing.universe);
    if (universe < 1 || universe > 63999) {
        s_sacn.stats.invalid_packets++;
        return;
    }

    if (s_sacn.have_local_cid && memcmp(p->root.cid, s_sacn.local_cid, 16) == 0) {
        s_sacn.stats.own_packets++;
        return;
    }

    uint8_t start_code = p->dmp.start_code;
    if (start_code != 0x00 && start_code != 0xDD) {
        return;   /* alternate start codes (e.g. 0xCC RDM, 0x17 text) are ignored */
    }

    uint8_t options = p->framing.options;
    char name[65];
    memcpy(name, p->framing.source_name, 64);
    name[64] = '\0';

    sacn_packet_info_t info = {
        .universe     = universe,
        .cid          = p->root.cid,
        .source_name  = name,
        .priority     = p->framing.priority > 200 ? 200 : p->framing.priority,
        .sequence     = p->framing.sequence_number,
        .options      = options,
        .preview      = (options & SACN_OPT_PREVIEW) != 0,
        .terminated   = (options & SACN_OPT_STREAM_TERM) != 0,
        .sync_address = ntohs(p->framing.sync_address),
        .start_code   = start_code,
        .data         = p->dmp.data,
        .length       = (uint16_t)(prop_count - 1),
        .source_ip    = from->sin_addr.s_addr,
    };

    if (info.preview)    s_sacn.stats.preview_packets++;
    if (info.terminated) s_sacn.stats.terminated_packets++;
    if (start_code == 0xDD) s_sacn.stats.pap_packets++;
    else                    s_sacn.stats.data_packets++;

    sacn_dmx_callback_t cb = s_sacn.cb;
    if (cb) {
        cb(&info, s_sacn.cb_data);
    }
}

static void sacn_receive_task(void *arg)
{
    uint8_t *buf = malloc(SACN_RX_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "rx buffer alloc failed");
        s_sacn.running = false;
    }
    ESP_LOGI(TAG, "sACN receiver task started");

    while (s_sacn.running) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(s_sacn.sock, buf, SACN_RX_BUF_SIZE, 0,
                           (struct sockaddr *)&from, &from_len);
        if (len < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && s_sacn.running) {
                ESP_LOGW(TAG, "recvfrom error %d", errno);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            continue;
        }
        s_sacn.stats.packets_received++;
        process_packet(buf, len, &from);
    }

    free(buf);
    ESP_LOGI(TAG, "sACN receiver task stopped");
    xSemaphoreGive(s_sacn.task_done);
    vTaskDelete(NULL);
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t sacn_receiver_init(void)
{
    if (s_sacn.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_sacn.mutex = xSemaphoreCreateMutex();
    s_sacn.task_done = xSemaphoreCreateBinary();
    if (!s_sacn.mutex || !s_sacn.task_done) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_sacn.stats, 0, sizeof(s_sacn.stats));
    memset(s_sacn.subs, 0, sizeof(s_sacn.subs));
    s_sacn.initialized = true;
    ESP_LOGI(TAG, "sACN receiver initialized");
    return ESP_OK;
}

esp_err_t sacn_receiver_deinit(void)
{
    if (!s_sacn.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    sacn_receiver_stop();
    vSemaphoreDelete(s_sacn.mutex);
    vSemaphoreDelete(s_sacn.task_done);
    s_sacn.mutex = NULL;
    s_sacn.task_done = NULL;
    s_sacn.initialized = false;
    return ESP_OK;
}

esp_err_t sacn_receiver_start(void)
{
    if (!s_sacn.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_sacn.running) {
        return ESP_OK;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: %d", errno);
        return ESP_FAIL;
    }
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct timeval tv = { .tv_sec = 0, .tv_usec = SACN_RECEIVE_TIMEOUT_MS * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(SACN_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind(%d) failed: %d", SACN_PORT, errno);
        close(sock);
        return ESP_FAIL;
    }

    xSemaphoreTake(s_sacn.mutex, portMAX_DELAY);
    s_sacn.sock = sock;
    for (int i = 0; i < SACN_MAX_UNIVERSES; i++) {
        s_sacn.subs[i].joined = false;
        if (s_sacn.subs[i].universe) {
            join_group(&s_sacn.subs[i], false);
        }
    }
    xSemaphoreGive(s_sacn.mutex);

    s_sacn.running = true;
    xSemaphoreTake(s_sacn.task_done, 0);
    if (xTaskCreatePinnedToCore(sacn_receive_task, "sacn_rx", SACN_TASK_STACK_SIZE,
                                NULL, SACN_TASK_PRIORITY, &s_sacn.task,
                                SACN_TASK_CORE) != pdPASS) {
        s_sacn.running = false;
        close(sock);
        s_sacn.sock = -1;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "sACN receiver listening on UDP %d", SACN_PORT);
    return ESP_OK;
}

esp_err_t sacn_receiver_stop(void)
{
    if (!s_sacn.initialized || !s_sacn.running) {
        return ESP_OK;
    }
    s_sacn.running = false;
    /* The task exits within one receive timeout */
    if (xSemaphoreTake(s_sacn.task_done, pdMS_TO_TICKS(SACN_RECEIVE_TIMEOUT_MS * 4)) != pdTRUE) {
        ESP_LOGW(TAG, "rx task did not stop in time");
    }
    s_sacn.task = NULL;

    xSemaphoreTake(s_sacn.mutex, portMAX_DELAY);
    for (int i = 0; i < SACN_MAX_UNIVERSES; i++) {
        leave_group(&s_sacn.subs[i]);
    }
    if (s_sacn.sock >= 0) {
        close(s_sacn.sock);
        s_sacn.sock = -1;
    }
    xSemaphoreGive(s_sacn.mutex);
    return ESP_OK;
}

esp_err_t sacn_receiver_subscribe_universe(uint16_t universe)
{
    if (!s_sacn.initialized) return ESP_ERR_INVALID_STATE;
    if (universe < 1 || universe > 63999) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    xSemaphoreTake(s_sacn.mutex, portMAX_DELAY);
    subscription_t *slot = NULL;
    for (int i = 0; i < SACN_MAX_UNIVERSES; i++) {
        if (s_sacn.subs[i].universe == universe) {
            slot = &s_sacn.subs[i];
            break;
        }
    }
    if (!slot) {
        for (int i = 0; i < SACN_MAX_UNIVERSES; i++) {
            if (s_sacn.subs[i].universe == 0) {
                slot = &s_sacn.subs[i];
                slot->universe = universe;
                slot->joined = false;
                break;
            }
        }
    }
    if (!slot) {
        ret = ESP_ERR_NO_MEM;
    } else if (!slot->joined && s_sacn.sock >= 0) {
        ret = join_group(slot, false) ? ESP_OK : ESP_FAIL;
    }
    xSemaphoreGive(s_sacn.mutex);
    return ret;
}

esp_err_t sacn_receiver_unsubscribe_universe(uint16_t universe)
{
    if (!s_sacn.initialized) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    xSemaphoreTake(s_sacn.mutex, portMAX_DELAY);
    for (int i = 0; i < SACN_MAX_UNIVERSES; i++) {
        if (s_sacn.subs[i].universe == universe) {
            leave_group(&s_sacn.subs[i]);
            s_sacn.subs[i].universe = 0;
            ESP_LOGI(TAG, "Left universe %u", universe);
            ret = ESP_OK;
        }
    }
    xSemaphoreGive(s_sacn.mutex);
    return ret;
}

esp_err_t sacn_receiver_set_universes(const uint16_t *universes, uint8_t count)
{
    if (!s_sacn.initialized) return ESP_ERR_INVALID_STATE;

    /* Leave groups that are no longer wanted */
    uint16_t current[SACN_MAX_UNIVERSES];
    xSemaphoreTake(s_sacn.mutex, portMAX_DELAY);
    for (int i = 0; i < SACN_MAX_UNIVERSES; i++) current[i] = s_sacn.subs[i].universe;
    xSemaphoreGive(s_sacn.mutex);

    for (int i = 0; i < SACN_MAX_UNIVERSES; i++) {
        if (!current[i]) continue;
        bool keep = false;
        for (uint8_t k = 0; k < count; k++) {
            if (universes[k] == current[i]) keep = true;
        }
        if (!keep) sacn_receiver_unsubscribe_universe(current[i]);
    }

    esp_err_t ret = ESP_OK;
    for (uint8_t k = 0; k < count; k++) {
        if (universes[k] >= 1 && universes[k] <= 63999) {
            esp_err_t e = sacn_receiver_subscribe_universe(universes[k]);
            if (e != ESP_OK) ret = e;
        }
    }
    return ret;
}

esp_err_t sacn_receiver_refresh_subscriptions(void)
{
    if (!s_sacn.initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_sacn.mutex, portMAX_DELAY);
    for (int i = 0; i < SACN_MAX_UNIVERSES; i++) {
        if (s_sacn.subs[i].universe) {
            join_group(&s_sacn.subs[i], true);
        }
    }
    xSemaphoreGive(s_sacn.mutex);
    return ESP_OK;
}

void sacn_receiver_set_local_cid(const uint8_t cid[16])
{
    if (cid) {
        memcpy(s_sacn.local_cid, cid, 16);
        s_sacn.have_local_cid = true;
    } else {
        s_sacn.have_local_cid = false;
    }
}

esp_err_t sacn_receiver_set_callback(sacn_dmx_callback_t callback, void *user_data)
{
    if (!s_sacn.initialized) return ESP_ERR_INVALID_STATE;
    s_sacn.cb_data = user_data;
    s_sacn.cb = callback;
    return ESP_OK;
}

esp_err_t sacn_receiver_set_sync_callback(sacn_sync_callback_t callback, void *user_data)
{
    if (!s_sacn.initialized) return ESP_ERR_INVALID_STATE;
    s_sacn.sync_cb = NULL;
    s_sacn.sync_data = user_data;
    s_sacn.sync_cb = callback;
    return ESP_OK;
}

esp_err_t sacn_receiver_get_stats(sacn_stats_t *stats)
{
    if (!s_sacn.initialized) return ESP_ERR_INVALID_STATE;
    if (!stats) return ESP_ERR_INVALID_ARG;
    *stats = s_sacn.stats;   /* 32-bit counters, written by one task only */
    return ESP_OK;
}

bool sacn_receiver_is_running(void)
{
    return s_sacn.running;
}

uint8_t sacn_receiver_get_subscription_count(void)
{
    if (!s_sacn.initialized) return 0;
    uint8_t n = 0;
    xSemaphoreTake(s_sacn.mutex, portMAX_DELAY);
    for (int i = 0; i < SACN_MAX_UNIVERSES; i++) {
        if (s_sacn.subs[i].universe && s_sacn.subs[i].joined) n++;
    }
    xSemaphoreGive(s_sacn.mutex);
    return n;
}
