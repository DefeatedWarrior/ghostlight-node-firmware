/*
 * Captive portal DNS task (see captive_dns.h)
 */
#include <errno.h>
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "config_manager.h"
#include "network_manager.h"
#include "captive_dns.h"

static const char *TAG = "captive_dns";
static TaskHandle_t s_task;

static int open_socket(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return -1;
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&a, sizeof(a)) < 0) {
        ESP_LOGW(TAG, "Cannot listen on port 53 (errno %d)", errno);
        close(sock);
        return -1;
    }
    return sock;
}

static void dns_task(void *arg)
{
    static uint8_t query[512], reply[DNS_REPLY_MAX];
    int sock = -1;
    for (;;) {
        if (sock < 0) {
            sock = open_socket();
            if (sock < 0) {
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
            ESP_LOGI(TAG, "Answering DNS for devices on the node's own Wi-Fi");
        }
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, query, sizeof(query), 0, (struct sockaddr *)&from, &from_len);
        if (n < 0) {
            if (errno != EINTR && errno != EAGAIN) {
                ESP_LOGW(TAG, "recvfrom failed (errno %d), reopening", errno);
                close(sock);
                sock = -1;
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            continue;
        }
        uint32_t ap_ip, ap_mask;
        if (!network_ap_address(&ap_ip, &ap_mask)) continue;            /* AP off */
        if (((from.sin_addr.s_addr ^ ap_ip) & ap_mask) != 0) continue;   /* not an AP client */
        const config_t *cfg = config_get();
        size_t len = dns_captive_reply(query, (size_t)n, reply, sizeof(reply), ap_ip,
                                       cfg && cfg->network.ap_captive);
        if (len) sendto(sock, reply, len, 0, (struct sockaddr *)&from, from_len);
    }
}

void captive_dns_start(void)
{
    if (s_task) return;
    if (xTaskCreatePinnedToCore(dns_task, "captive_dns", 3072, NULL, 2, &s_task, 0) != pdPASS) {
        s_task = NULL;
        ESP_LOGW(TAG, "DNS task not started");
    }
}
