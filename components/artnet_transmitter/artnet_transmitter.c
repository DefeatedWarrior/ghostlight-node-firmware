#include "artnet_transmitter.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string.h>
#include <inttypes.h>

static const char *TAG = "artnet_tx";

#define ARTNET_PORT 6454
#define ARTNET_HEADER_LEN 18   /* id(8)+opcode(2)+ver(2)+seq(1)+phys(1)+uni(2)+len(2) */
#define ARTNET_TX_UNIVERSES 8

typedef struct __attribute__((packed)) {
    uint8_t  id[8];
    uint8_t  opcode_lo;
    uint8_t  opcode_hi;
    uint8_t  prot_ver_hi;
    uint8_t  prot_ver_lo;
    uint8_t  sequence;
    uint8_t  physical;
    uint8_t  sub_uni;      /* Port-Address bits 7-0 */
    uint8_t  net;          /* Port-Address bits 14-8 */
    uint8_t  length_hi;    /* big-endian 16-bit length */
    uint8_t  length_lo;
    uint8_t  data[512];
} artnet_dmx_packet_t;

typedef struct {
    bool used;
    uint16_t universe;
    uint8_t seq;
} tx_seq_t;

static struct {
    bool   initialized;
    int    own_sock;
    volatile int shared_sock;
    volatile uint32_t dest_ip;
    SemaphoreHandle_t lock;
    tx_seq_t seq[ARTNET_TX_UNIVERSES];
    artnet_tx_stats_t stats;
} s_tx = {
    .initialized = false,
    .own_sock = -1,
    .shared_sock = -1,
};

esp_err_t artnet_transmitter_init(void)
{
    if (s_tx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_tx.lock = xSemaphoreCreateMutex();
    if (!s_tx.lock) {
        return ESP_ERR_NO_MEM;
    }

    s_tx.own_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_tx.own_sock < 0) {
        ESP_LOGE(TAG, "socket() failed: %d", errno);
        vSemaphoreDelete(s_tx.lock);
        s_tx.lock = NULL;
        return ESP_FAIL;
    }

    int bcast = 1;
    if (setsockopt(s_tx.own_sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast)) < 0) {
        ESP_LOGW(TAG, "SO_BROADCAST failed: %d", errno);
    }

    memset(s_tx.seq, 0, sizeof(s_tx.seq));
    memset(&s_tx.stats, 0, sizeof(s_tx.stats));
    s_tx.dest_ip = 0;
    s_tx.initialized = true;

    ESP_LOGI(TAG, "Art-Net transmitter initialized");
    return ESP_OK;
}

esp_err_t artnet_transmitter_deinit(void)
{
    if (!s_tx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_tx.initialized = false;
    if (s_tx.own_sock >= 0) {
        close(s_tx.own_sock);
        s_tx.own_sock = -1;
    }
    s_tx.shared_sock = -1;
    vSemaphoreDelete(s_tx.lock);
    s_tx.lock = NULL;
    return ESP_OK;
}

void artnet_transmitter_set_destination(uint32_t ip_addr)
{
    s_tx.dest_ip = ip_addr;
    struct in_addr a = {.s_addr = ip_addr ? ip_addr : htonl(INADDR_BROADCAST)};
    ESP_LOGI(TAG, "ArtDmx destination %s", inet_ntoa(a));
}

void artnet_transmitter_use_socket(int sock)
{
    s_tx.shared_sock = sock;
}

static uint8_t next_seq(uint16_t universe)
{
    tx_seq_t *slot = NULL;
    for (int i = 0; i < ARTNET_TX_UNIVERSES; i++) {
        if (s_tx.seq[i].used && s_tx.seq[i].universe == universe) {
            slot = &s_tx.seq[i];
            break;
        }
    }
    if (!slot) {
        for (int i = 0; i < ARTNET_TX_UNIVERSES; i++) {
            if (!s_tx.seq[i].used) {
                slot = &s_tx.seq[i];
                break;
            }
        }
        if (!slot) slot = &s_tx.seq[universe % ARTNET_TX_UNIVERSES];
        slot->used = true;
        slot->universe = universe;
        slot->seq = 0;
    }
    if (++slot->seq == 0) slot->seq = 1;   /* 0 = sequencing disabled */
    return slot->seq;
}

esp_err_t artnet_transmitter_send_port(uint16_t universe, uint8_t physical,
                                       const uint8_t *data, uint16_t length)
{
    if (!s_tx.initialized || !data) {
        return ESP_ERR_INVALID_STATE;
    }
    if (length == 0 || length > 512 || universe > 0x7FFF) {
        return ESP_ERR_INVALID_ARG;
    }

    /* ArtDmx length must be even (2-512) */
    uint16_t wire_len = (length + 1) & ~1u;
    if (wire_len < 2) wire_len = 2;

    xSemaphoreTake(s_tx.lock, portMAX_DELAY);

    static artnet_dmx_packet_t pkt;   /* protected by s_tx.lock */
    memcpy(pkt.id, "Art-Net\0", 8);
    pkt.opcode_lo   = 0x00;           /* OpDmx 0x5000, little-endian */
    pkt.opcode_hi   = 0x50;
    pkt.prot_ver_hi = 0;
    pkt.prot_ver_lo = 14;
    pkt.sequence    = next_seq(universe);
    pkt.physical    = physical;
    pkt.sub_uni     = (uint8_t)(universe & 0xFF);
    pkt.net         = (uint8_t)((universe >> 8) & 0x7F);
    pkt.length_hi   = (uint8_t)(wire_len >> 8);
    pkt.length_lo   = (uint8_t)(wire_len & 0xFF);
    memcpy(pkt.data, data, length);
    if (wire_len > length) pkt.data[length] = 0;

    struct sockaddr_in dest = {
        .sin_family      = AF_INET,
        .sin_port        = htons(ARTNET_PORT),
        .sin_addr.s_addr = s_tx.dest_ip ? s_tx.dest_ip : htonl(INADDR_BROADCAST),
    };

    int sock = s_tx.shared_sock >= 0 ? s_tx.shared_sock : s_tx.own_sock;
    ssize_t sent = sendto(sock, &pkt, ARTNET_HEADER_LEN + wire_len, 0,
                          (struct sockaddr *)&dest, sizeof(dest));
    esp_err_t ret = ESP_OK;
    if (sent < 0) {
        s_tx.stats.send_errors++;
        /* Rate-limit: log only the first error and every 100th thereafter
         * to avoid flooding the log at 40 Hz when the network is down. */
        if (s_tx.stats.send_errors == 1 || (s_tx.stats.send_errors % 100) == 0) {
            ESP_LOGW(TAG, "sendto failed: %d (total errors: %" PRIu32 ")",
                     errno, s_tx.stats.send_errors);
        }
        ret = ESP_FAIL;
    } else {
        s_tx.stats.packets_sent++;
    }
    xSemaphoreGive(s_tx.lock);
    return ret;
}

esp_err_t artnet_transmitter_send(uint16_t universe, const uint8_t *data, uint16_t length)
{
    return artnet_transmitter_send_port(universe, 0, data, length);
}

esp_err_t artnet_transmitter_get_stats(artnet_tx_stats_t *stats)
{
    if (!stats) return ESP_ERR_INVALID_ARG;
    *stats = s_tx.stats;
    return ESP_OK;
}
