#include "sacn_transmitter.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

static const char *TAG = "sacn_tx";

#define SACN_PORT              5568
#define SACN_MULTICAST_TTL     8
#define SACN_TX_UNIVERSES      8
#define SACN_DISCOVERY_UNIVERSE 64214
#define SACN_DISCOVERY_INTERVAL_US (10 * 1000000LL)
#define SACN_ACTIVE_WINDOW_US      (10 * 1000000LL)

#define VECTOR_ROOT_E131_DATA       0x00000004
#define VECTOR_ROOT_E131_EXTENDED   0x00000008
#define VECTOR_E131_DATA_PACKET     0x00000002
#define VECTOR_E131_EXTENDED_DISCOVERY 0x00000002
#define VECTOR_UNIVERSE_DISCOVERY_UNIVERSE_LIST 0x00000001

#define OPT_STREAM_TERMINATED  0x40

typedef struct __attribute__((packed)) {
    /* Root layer */
    uint16_t preamble_size;
    uint16_t postamble_size;
    uint8_t  acn_pid[12];
    uint16_t root_flags_length;
    uint32_t root_vector;
    uint8_t  cid[16];
    /* Framing layer */
    uint16_t frame_flags_length;
    uint32_t frame_vector;
    char     source_name[64];
    uint8_t  priority;
    uint16_t sync_address;
    uint8_t  sequence_number;
    uint8_t  options;
    uint16_t universe;
    /* DMP layer */
    uint16_t dmp_flags_length;
    uint8_t  dmp_vector;
    uint8_t  address_type;
    uint16_t first_address;
    uint16_t address_increment;
    uint16_t property_count;
    uint8_t  start_code;
    uint8_t  data[512];
} sacn_packet_t;

typedef struct __attribute__((packed)) {
    uint16_t preamble_size;
    uint16_t postamble_size;
    uint8_t  acn_pid[12];
    uint16_t root_flags_length;
    uint32_t root_vector;
    uint8_t  cid[16];
    uint16_t frame_flags_length;
    uint32_t frame_vector;
    char     source_name[64];
    uint32_t reserved;
    uint16_t disc_flags_length;
    uint32_t disc_vector;
    uint8_t  page;
    uint8_t  last_page;
    uint16_t universes[SACN_TX_UNIVERSES];
} sacn_discovery_packet_t;

static const uint8_t SACN_ACN_PID[12] = {
    0x41, 0x53, 0x43, 0x2D, 0x45, 0x31, 0x2E, 0x31, 0x37, 0x00, 0x00, 0x00
};

typedef struct {
    uint16_t universe;      /* 0 = free slot */
    uint8_t  seq;
    uint8_t  priority;
    uint16_t length;
    int64_t  last_tx_us;
    uint8_t  last_data[512];
} tx_universe_t;

static struct {
    bool     initialized;
    int      sock;
    uint8_t  cid[16];
    char     source_name[64];
    SemaphoreHandle_t lock;
    tx_universe_t uni[SACN_TX_UNIVERSES];
    int64_t  last_discovery_us;
    sacn_tx_stats_t stats;
} s_tx = {
    .initialized = false,
    .sock = -1,
};

/* Returns network-byte-order IPv4 for universe multicast group 239.255.hi.lo */
static uint32_t universe_to_mcast(uint16_t universe)
{
    uint8_t hi = (uint8_t)(universe >> 8);
    uint8_t lo = (uint8_t)(universe & 0xFF);
    return htonl((239u << 24) | (255u << 16) | ((uint32_t)hi << 8) | (uint32_t)lo);
}

static void copy_name(char dst[64], const char *src)
{
    memset(dst, 0, 64);
    if (!src || !src[0]) src = "ESP-NODE-2RDM";
    memcpy(dst, src, strnlen(src, 63));
}

/* Stable per-device CID: UUID (version 4 layout) built from the base MAC */
static void make_cid(uint8_t cid[16])
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    static const uint8_t ns[10] = {'E', 'S', 'P', 'N', 'O', 'D', 'E', '2', 'R', 'D'};
    memcpy(cid, ns, 10);
    memcpy(cid + 10, mac, 6);
    /* mix the MAC into the namespace bytes so CIDs differ everywhere */
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; i++) {
        h ^= mac[i];
        h *= 16777619u;
    }
    for (int i = 0; i < 10; i++) {
        cid[i] ^= (uint8_t)(h >> ((i % 4) * 8));
    }
    cid[6] = (cid[6] & 0x0F) | 0x40;   /* version 4 */
    cid[8] = (cid[8] & 0x3F) | 0x80;   /* RFC 4122 variant */
}

static tx_universe_t *find_universe(uint16_t universe, bool create)
{
    tx_universe_t *free_slot = NULL;
    tx_universe_t *oldest = NULL;
    for (int i = 0; i < SACN_TX_UNIVERSES; i++) {
        tx_universe_t *u = &s_tx.uni[i];
        if (u->universe == universe) return u;
        if (u->universe == 0 && !free_slot) free_slot = u;
        if (!oldest || u->last_tx_us < oldest->last_tx_us) oldest = u;
    }
    if (!create) return NULL;
    tx_universe_t *u = free_slot ? free_slot : oldest;
    memset(u, 0, sizeof(*u));
    u->universe = universe;
    return u;
}

esp_err_t sacn_transmitter_init(const char *source_name)
{
    if (s_tx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_tx.lock = xSemaphoreCreateMutex();
    if (!s_tx.lock) {
        return ESP_ERR_NO_MEM;
    }

    make_cid(s_tx.cid);
    copy_name(s_tx.source_name, source_name);

    s_tx.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_tx.sock < 0) {
        ESP_LOGE(TAG, "socket() failed: %d", errno);
        vSemaphoreDelete(s_tx.lock);
        s_tx.lock = NULL;
        return ESP_FAIL;
    }

    uint8_t ttl = SACN_MULTICAST_TTL;
    if (setsockopt(s_tx.sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
        ESP_LOGW(TAG, "IP_MULTICAST_TTL failed: %d", errno);
    }

    uint8_t loop = 0;
    setsockopt(s_tx.sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    memset(s_tx.uni, 0, sizeof(s_tx.uni));
    memset(&s_tx.stats, 0, sizeof(s_tx.stats));
    s_tx.last_discovery_us = 0;
    s_tx.initialized = true;

    ESP_LOGI(TAG, "sACN transmitter initialized, source \"%s\", CID "
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             s_tx.source_name,
             s_tx.cid[0], s_tx.cid[1], s_tx.cid[2], s_tx.cid[3], s_tx.cid[4], s_tx.cid[5],
             s_tx.cid[6], s_tx.cid[7], s_tx.cid[8], s_tx.cid[9], s_tx.cid[10], s_tx.cid[11],
             s_tx.cid[12], s_tx.cid[13], s_tx.cid[14], s_tx.cid[15]);
    return ESP_OK;
}

esp_err_t sacn_transmitter_deinit(void)
{
    if (!s_tx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    for (int i = 0; i < SACN_TX_UNIVERSES; i++) {
        if (s_tx.uni[i].universe) {
            sacn_transmitter_terminate(s_tx.uni[i].universe);
        }
    }
    s_tx.initialized = false;
    if (s_tx.sock >= 0) {
        close(s_tx.sock);
        s_tx.sock = -1;
    }
    vSemaphoreDelete(s_tx.lock);
    s_tx.lock = NULL;
    return ESP_OK;
}

void sacn_transmitter_set_source_name(const char *source_name)
{
    if (!s_tx.initialized) return;
    xSemaphoreTake(s_tx.lock, portMAX_DELAY);
    copy_name(s_tx.source_name, source_name);
    xSemaphoreGive(s_tx.lock);
}

void sacn_transmitter_set_interface(uint32_t ip_addr)
{
    if (!s_tx.initialized) return;
    struct in_addr ifaddr = {.s_addr = ip_addr};
    if (setsockopt(s_tx.sock, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof(ifaddr)) < 0) {
        ESP_LOGW(TAG, "IP_MULTICAST_IF failed: %d", errno);
    }
}

const uint8_t *sacn_transmitter_get_cid(void)
{
    return s_tx.cid;
}

/* Caller holds the lock */
static esp_err_t send_packet(tx_universe_t *u, const uint8_t *data, uint16_t length,
                             uint8_t priority, uint8_t options)
{
    /* PDU length fields include the flags_length word itself */
    uint16_t dmp_pdu_len   = 11 + length;         /* 10 bytes DMP header + start_code + data */
    uint16_t frame_pdu_len = 77 + dmp_pdu_len;    /* 77 bytes framing header + DMP PDU */
    uint16_t root_pdu_len  = 22 + frame_pdu_len;  /* 22 bytes root header + framing PDU */
    uint16_t total_len     = 16 + root_pdu_len;   /* 16 byte preamble + root PDU */

    static sacn_packet_t pkt;   /* protected by s_tx.lock; keeps 640 B off the caller's stack */
    memset(&pkt, 0, sizeof(pkt));

    pkt.preamble_size      = htons(0x0010);
    pkt.postamble_size     = 0;
    memcpy(pkt.acn_pid, SACN_ACN_PID, 12);
    pkt.root_flags_length  = htons(0x7000 | root_pdu_len);
    pkt.root_vector        = htonl(VECTOR_ROOT_E131_DATA);
    memcpy(pkt.cid, s_tx.cid, 16);

    pkt.frame_flags_length = htons(0x7000 | frame_pdu_len);
    pkt.frame_vector       = htonl(VECTOR_E131_DATA_PACKET);
    memcpy(pkt.source_name, s_tx.source_name, 64);
    pkt.priority           = priority > 200 ? 200 : priority;
    pkt.sync_address       = 0;
    pkt.sequence_number    = u->seq++;
    pkt.options            = options;
    pkt.universe           = htons(u->universe);

    pkt.dmp_flags_length   = htons(0x7000 | dmp_pdu_len);
    pkt.dmp_vector         = 0x02;
    pkt.address_type       = 0xA1;
    pkt.first_address      = 0;
    pkt.address_increment  = htons(0x0001);
    pkt.property_count     = htons((uint16_t)(length + 1)); /* +1 for start code */
    pkt.start_code         = 0x00;
    memcpy(pkt.data, data, length);

    struct sockaddr_in dest = {
        .sin_family      = AF_INET,
        .sin_port        = htons(SACN_PORT),
        .sin_addr.s_addr = universe_to_mcast(u->universe),
    };

    ssize_t sent = sendto(s_tx.sock, &pkt, total_len, 0,
                          (struct sockaddr *)&dest, sizeof(dest));
    if (sent < 0) {
        s_tx.stats.send_errors++;
        if (s_tx.stats.send_errors == 1 || (s_tx.stats.send_errors % 100) == 0) {
            ESP_LOGW(TAG, "sendto universe %u failed: %d (total errors: %" PRIu32 ")",
                     u->universe, errno, s_tx.stats.send_errors);
        }
        return ESP_FAIL;
    }
    s_tx.stats.packets_sent++;
    return ESP_OK;
}

esp_err_t sacn_transmitter_send(uint16_t universe, const uint8_t *data,
                                uint16_t length, uint8_t priority)
{
    if (!s_tx.initialized || !data) {
        return ESP_ERR_INVALID_STATE;
    }
    if (length == 0 || length > 512 || universe < 1 || universe > 63999) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_tx.lock, portMAX_DELAY);
    tx_universe_t *u = find_universe(universe, true);
    esp_err_t ret = send_packet(u, data, length, priority, 0);
    u->priority = priority;
    u->length = length;
    u->last_tx_us = esp_timer_get_time();
    memcpy(u->last_data, data, length);
    xSemaphoreGive(s_tx.lock);
    return ret;
}

esp_err_t sacn_transmitter_terminate(uint16_t universe)
{
    if (!s_tx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_tx.lock, portMAX_DELAY);
    tx_universe_t *u = find_universe(universe, false);
    if (!u) {
        xSemaphoreGive(s_tx.lock);
        return ESP_ERR_NOT_FOUND;
    }
    /* E1.31 6.2.6: three packets with Stream_Terminated set */
    for (int i = 0; i < 3; i++) {
        send_packet(u, u->last_data, u->length ? u->length : 512, u->priority,
                    OPT_STREAM_TERMINATED);
    }
    s_tx.stats.terminations_sent++;
    memset(u, 0, sizeof(*u));
    xSemaphoreGive(s_tx.lock);
    ESP_LOGI(TAG, "Universe %u: stream terminated", universe);
    return ESP_OK;
}

static int cmp_u16(const void *a, const void *b)
{
    return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}

void sacn_transmitter_tick(void)
{
    if (!s_tx.initialized) return;
    int64_t now = esp_timer_get_time();
    if (now - s_tx.last_discovery_us < SACN_DISCOVERY_INTERVAL_US) return;

    xSemaphoreTake(s_tx.lock, portMAX_DELAY);
    s_tx.last_discovery_us = now;

    uint16_t list[SACN_TX_UNIVERSES];
    int n = 0;
    for (int i = 0; i < SACN_TX_UNIVERSES; i++) {
        const tx_universe_t *u = &s_tx.uni[i];
        if (u->universe && (now - u->last_tx_us) < SACN_ACTIVE_WINDOW_US) {
            list[n++] = u->universe;
        }
    }
    if (n == 0) {
        xSemaphoreGive(s_tx.lock);
        return;   /* nothing being sourced */
    }
    qsort(list, n, sizeof(list[0]), cmp_u16);

    sacn_discovery_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    uint16_t disc_len  = 8 + 2 * n;
    uint16_t frame_len = 74 + disc_len;
    uint16_t root_len  = 22 + frame_len;
    uint16_t total     = 16 + root_len;

    pkt.preamble_size      = htons(0x0010);
    memcpy(pkt.acn_pid, SACN_ACN_PID, 12);
    pkt.root_flags_length  = htons(0x7000 | root_len);
    pkt.root_vector        = htonl(VECTOR_ROOT_E131_EXTENDED);
    memcpy(pkt.cid, s_tx.cid, 16);
    pkt.frame_flags_length = htons(0x7000 | frame_len);
    pkt.frame_vector       = htonl(VECTOR_E131_EXTENDED_DISCOVERY);
    memcpy(pkt.source_name, s_tx.source_name, 64);
    pkt.disc_flags_length  = htons(0x7000 | disc_len);
    pkt.disc_vector        = htonl(VECTOR_UNIVERSE_DISCOVERY_UNIVERSE_LIST);
    pkt.page = 0;
    pkt.last_page = 0;
    for (int i = 0; i < n; i++) {
        pkt.universes[i] = htons(list[i]);
    }

    struct sockaddr_in dest = {
        .sin_family      = AF_INET,
        .sin_port        = htons(SACN_PORT),
        .sin_addr.s_addr = universe_to_mcast(SACN_DISCOVERY_UNIVERSE),
    };
    if (sendto(s_tx.sock, &pkt, total, 0, (struct sockaddr *)&dest, sizeof(dest)) >= 0) {
        s_tx.stats.discovery_sent++;
    }
    xSemaphoreGive(s_tx.lock);
}

esp_err_t sacn_transmitter_get_stats(sacn_tx_stats_t *stats)
{
    if (!stats) return ESP_ERR_INVALID_ARG;
    *stats = s_tx.stats;
    return ESP_OK;
}
