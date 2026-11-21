/**
 * @file network_manager.c
 * @brief Network interface drivers: W5500 Ethernet, WiFi station, WiFi AP
 *
 * The supervisor (network_auto_fallback.c) decides which interfaces run;
 * this file owns the drivers, the events and the per-interface status.
 *
 * Notes:
 *  - Station and AP are independent: either can be switched on or off
 *    without touching the other (the WiFi mode becomes STA, AP or APSTA).
 *  - The station never reconnects by itself. Blind reconnect loops scan all
 *    channels every few seconds, which also knocks the AP off its channel;
 *    the supervisor retries with back-off instead.
 *  - Static addresses: lwIP brings a static address up together with the
 *    link but ESP-IDF posts no "got IP" event for it (and none at all
 *    without a gateway, which is normal on show networks). A static
 *    interface therefore counts as ready as soon as its link is up.
 *  - W5500: the IDF driver opens its MACRAW socket with MAC filtering on,
 *    which also drops multicast, so sACN would never arrive over Ethernet.
 *    IDF 5.2 has no "all multicast" ioctl for the W5500, so the filter is
 *    switched off with ETH_CMD_S_PROMISCUOUS; lwIP drops what isn't ours.
 */

#include "network_manager.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "network_mgr";

#define ETH_READY_BIT   BIT0   /* Ethernet link up and an address */
#define STA_READY_BIT   BIT1   /* station has an address */

/* W5500 wiring */
#define W5500_CS_PIN        GPIO_NUM_10
#define W5500_MOSI_PIN      GPIO_NUM_11
#define W5500_MISO_PIN      GPIO_NUM_13
#define W5500_SCK_PIN       GPIO_NUM_12
#define W5500_INT_PIN       GPIO_NUM_9

#define SCAN_CACHE_MAX      24

static bool s_initialized;
static EventGroupHandle_t s_bits;
static StaticEventGroup_t s_bits_buf;
static SemaphoreHandle_t s_wifi_lock;    /* recursive: all WiFi driver calls */
static SemaphoreHandle_t s_state_lock;   /* s_state / s_status */
static char s_hostname[32] = "esp-node";

static esp_netif_t *s_eth_netif;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;

/* Ethernet */
static esp_eth_handle_t s_eth_handle;
static volatile bool s_eth_running;
static volatile bool s_eth_link;
static volatile bool s_eth_dhcp = true;

/* WiFi */
static bool s_wifi_initialized;          /* esp_wifi_init() may only run once */
static bool s_wifi_started;
static bool s_sta_on;                    /* station wanted */
static bool s_ap_on;                     /* AP wanted */
static bool s_scan_sta;                  /* station enabled just for a scan */
static volatile bool s_connect_pending;  /* esp_wifi_connect() still to be called */
static volatile bool s_ap_active;
static volatile network_sta_state_t s_sta_state = NET_STA_IDLE;
static volatile uint8_t s_sta_reason;
static volatile uint32_t s_sta_disconnects;
static volatile bool s_sta_dhcp = true;
static char s_sta_ssid[33];
static wifi_config_t s_ap_cfg;           /* last AP configuration applied */

/* Scan cache */
static wifi_ap_record_t s_scan[SCAN_CACHE_MAX];
static uint16_t s_scan_count;
static int64_t s_scan_time_us;

/* Primary interface */
static network_state_t s_state = NETWORK_DISCONNECTED;
static network_status_t s_status;
static network_state_callback_t s_state_cb;
static void *s_state_cb_ctx;

static void network_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);

// ============================================================================
// Helpers
// ============================================================================

static void wifi_lock(void)   { xSemaphoreTakeRecursive(s_wifi_lock, portMAX_DELAY); }
static void wifi_unlock(void) { xSemaphoreGiveRecursive(s_wifi_lock); }

static void copy_bounded(char *dst, size_t dst_size, const char *src)
{
    size_t n = src ? strnlen(src, dst_size - 1) : 0;
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
}

static void ip_strings(esp_netif_t *netif, char *ip, char *mask, char *gw)
{
    esp_netif_ip_info_t info = {0};
    if (netif) esp_netif_get_ip_info(netif, &info);
    if (ip)   snprintf(ip, 16, IPSTR, IP2STR(&info.ip));
    if (mask) snprintf(mask, 16, IPSTR, IP2STR(&info.netmask));
    if (gw)   snprintf(gw, 16, IPSTR, IP2STR(&info.gw));
}

static bool bit_set(EventBits_t bit)
{
    return s_bits && (xEventGroupGetBits(s_bits) & bit) != 0;
}

/*
 * Work out the primary interface (Ethernet > station > AP) and tell the
 * callback when it, or its address (@p force), changed.
 */
static void refresh_state(bool force)
{
    if (!s_state_lock) return;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);

    network_state_t st;
    esp_netif_t *netif = NULL;
    if (bit_set(ETH_READY_BIT)) {
        st = NETWORK_ETHERNET_CONNECTED;
        netif = s_eth_netif;
    } else if (bit_set(STA_READY_BIT)) {
        st = NETWORK_WIFI_STA_CONNECTED;
        netif = s_sta_netif;
    } else if (s_ap_active) {
        st = NETWORK_WIFI_AP_ACTIVE;
        netif = s_ap_netif;
    } else if (s_eth_running || s_sta_on) {
        st = NETWORK_CONNECTING;
    } else {
        st = NETWORK_DISCONNECTED;
    }

    if (netif) {
        ip_strings(netif, s_status.ip_address, s_status.netmask, s_status.gateway);
        esp_netif_get_mac(netif, s_status.mac);
    } else {
        s_status.ip_address[0] = '\0';
        s_status.netmask[0] = '\0';
        s_status.gateway[0] = '\0';
    }

    bool changed = (st != s_state) || force;
    if (st != s_state) {
        ESP_LOGI(TAG, "Primary interface: %d -> %d", s_state, st);
    }
    s_state = st;
    s_status.state = st;
    xSemaphoreGive(s_state_lock);

    network_state_callback_t cb = s_state_cb;
    if (changed && cb) {
        cb(st, s_state_cb_ctx);
    }
}

static esp_err_t wifi_ensure_init(void)
{
    if (s_wifi_initialized) return ESP_OK;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    /* Settings live in our own config; keep the driver out of NVS */
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    s_wifi_initialized = true;
    return ESP_OK;
}

/* Put the driver in the mode the wanted interfaces need (lock held) */
static esp_err_t wifi_apply_mode(void)
{
    bool sta = s_sta_on || s_scan_sta;
    wifi_mode_t want = (sta && s_ap_on) ? WIFI_MODE_APSTA :
                       s_ap_on ? WIFI_MODE_AP :
                       sta ? WIFI_MODE_STA : WIFI_MODE_NULL;

    if (want == WIFI_MODE_NULL) {
        if (s_wifi_started) {
            ESP_LOGI(TAG, "WiFi off");
            esp_wifi_stop();
            s_wifi_started = false;
        }
        s_ap_active = false;
        return ESP_OK;
    }

    esp_err_t ret = wifi_ensure_init();
    if (ret != ESP_OK) return ret;

    wifi_mode_t cur = WIFI_MODE_NULL;
    esp_wifi_get_mode(&cur);
    if (cur != want) {
        ret = esp_wifi_set_mode(want);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_mode(%d) failed: %s", want, esp_err_to_name(ret));
            return ret;
        }
    }
    /* 20 MHz channels: far more reliable than 40 MHz in a crowded 2.4 GHz band */
    if (want != WIFI_MODE_AP) esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    if (want != WIFI_MODE_STA) esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    if (!s_ap_on) s_ap_active = false;
    return ESP_OK;
}

static esp_err_t wifi_start_if_needed(void)
{
    if (s_wifi_started) return ESP_OK;
    esp_err_t ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_wifi_started = true;
    /* Real-time UDP: no modem sleep (it adds 100 ms+ latency per packet) */
    esp_wifi_set_ps(WIFI_PS_NONE);
    return ESP_OK;
}

/* Call esp_wifi_connect() once: from here or from the STA_START event */
static void connect_if_pending(void)
{
    if (__atomic_exchange_n(&s_connect_pending, false, __ATOMIC_ACQ_REL)) {
        esp_err_t ret = esp_wifi_connect();
        if (ret != ESP_OK) {
            ESP_LOGD(TAG, "esp_wifi_connect: %s (waiting for STA start)", esp_err_to_name(ret));
            __atomic_store_n(&s_connect_pending, true, __ATOMIC_RELEASE);
        }
    }
}

// ============================================================================
// Events
// ============================================================================

static void eth_mark_ready(void)
{
    xEventGroupSetBits(s_bits, ETH_READY_BIT);
    refresh_state(true);
}

static void network_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == ETH_EVENT) {
        switch (id) {
        case ETHERNET_EVENT_START:
            s_eth_running = true;
            refresh_state(false);
            break;
        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "Ethernet stopped");
            s_eth_running = false;
            s_eth_link = false;
            xEventGroupClearBits(s_bits, ETH_READY_BIT);
            refresh_state(false);
            break;
        case ETHERNET_EVENT_CONNECTED:
            s_eth_link = true;
            if (!s_eth_dhcp) {
                char ip[16];
                ip_strings(s_eth_netif, ip, NULL, NULL);
                ESP_LOGI(TAG, "Ethernet link up, static address %s", ip);
                eth_mark_ready();
            } else {
                ESP_LOGI(TAG, "Ethernet link up, waiting for DHCP");
            }
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Ethernet link down");
            s_eth_link = false;
            xEventGroupClearBits(s_bits, ETH_READY_BIT);
            refresh_state(false);
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT) {
        switch (id) {
        case IP_EVENT_ETH_GOT_IP: {
            ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
            ESP_LOGI(TAG, "Ethernet address " IPSTR " mask " IPSTR " gw " IPSTR,
                     IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.netmask),
                     IP2STR(&ev->ip_info.gw));
            if (s_eth_link) eth_mark_ready();
            break;
        }
        case IP_EVENT_ETH_LOST_IP:
            if (s_eth_dhcp) {
                ESP_LOGW(TAG, "Ethernet DHCP lease lost");
                xEventGroupClearBits(s_bits, ETH_READY_BIT);
                refresh_state(false);
            }
            break;
        case IP_EVENT_STA_GOT_IP: {
            ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
            ESP_LOGI(TAG, "WiFi address " IPSTR " mask " IPSTR " gw " IPSTR,
                     IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.netmask),
                     IP2STR(&ev->ip_info.gw));
            if (s_sta_on) {
                s_sta_state = NET_STA_CONNECTED;
                s_sta_reason = 0;
                xEventGroupSetBits(s_bits, STA_READY_BIT);
                refresh_state(true);
            }
            break;
        }
        case IP_EVENT_STA_LOST_IP:
            if (s_sta_dhcp) {
                ESP_LOGW(TAG, "WiFi DHCP lease lost");
                xEventGroupClearBits(s_bits, STA_READY_BIT);
                if (s_sta_state == NET_STA_CONNECTED) s_sta_state = NET_STA_GETTING_IP;
                refresh_state(false);
            }
            break;
        default:
            break;
        }
    } else if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            connect_if_pending();
            break;
        case WIFI_EVENT_STA_CONNECTED: {
            wifi_event_sta_connected_t *ev = (wifi_event_sta_connected_t *)data;
            ESP_LOGI(TAG, "WiFi joined \"%.*s\" (channel %d)", ev ? ev->ssid_len : 0,
                     ev ? (const char *)ev->ssid : "", ev ? ev->channel : 0);
            if (!s_sta_on) break;
            if (!s_sta_dhcp) {
                s_sta_state = NET_STA_CONNECTED;
                s_sta_reason = 0;
                xEventGroupSetBits(s_bits, STA_READY_BIT);
                refresh_state(true);
            } else {
                s_sta_state = NET_STA_GETTING_IP;
            }
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
            uint8_t reason = ev ? ev->reason : 0;
            xEventGroupClearBits(s_bits, STA_READY_BIT);
            /* ASSOC_LEAVE is our own disconnect (new network, station off) */
            if (s_sta_on && reason != WIFI_REASON_ASSOC_LEAVE) {
                s_sta_disconnects++;
                s_sta_reason = reason;
                s_sta_state = NET_STA_FAILED;
                ESP_LOGW(TAG, "WiFi \"%s\": %s (reason %u)", s_sta_ssid,
                         network_wifi_reason_text(reason), reason);
            }
            refresh_state(false);
            break;
        }
        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "Access point up");
            s_ap_active = true;
            refresh_state(false);
            break;
        case WIFI_EVENT_AP_STOP:
            ESP_LOGI(TAG, "Access point down");
            s_ap_active = false;
            refresh_state(false);
            break;
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *ev = (wifi_event_ap_staconnected_t *)data;
            ESP_LOGI(TAG, "AP client " MACSTR " joined", MAC2STR(ev->mac));
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *ev = (wifi_event_ap_stadisconnected_t *)data;
            ESP_LOGI(TAG, "AP client " MACSTR " left", MAC2STR(ev->mac));
            break;
        }
        default:
            break;
        }
    }
}

// ============================================================================
// Initialization
// ============================================================================

esp_err_t network_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Initializing network manager...");

    s_bits = xEventGroupCreateStatic(&s_bits_buf);
    s_wifi_lock = xSemaphoreCreateRecursiveMutex();
    s_state_lock = xSemaphoreCreateMutex();
    if (!s_bits || !s_wifi_lock || !s_state_lock) {
        ESP_LOGE(TAG, "Failed to create sync objects");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK) return ret;
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, network_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, network_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event_handler, NULL));

    s_initialized = true;
    ESP_LOGI(TAG, "Network manager initialized");
    return ESP_OK;
}

void network_set_hostname(const char *hostname)
{
    if (!hostname || !hostname[0]) return;
    /* DNS label: letters, digits and '-' only */
    size_t j = 0;
    for (size_t i = 0; hostname[i] && j < sizeof(s_hostname) - 1; i++) {
        char c = hostname[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
            s_hostname[j++] = c;
        } else if (c >= 'A' && c <= 'Z') {
            s_hostname[j++] = c - 'A' + 'a';
        } else if (c == ' ' || c == '_') {
            s_hostname[j++] = '-';
        }
    }
    s_hostname[j] = '\0';
    if (j == 0) {
        snprintf(s_hostname, sizeof(s_hostname), "esp-node");
    }
    if (s_eth_netif) esp_netif_set_hostname(s_eth_netif, s_hostname);
    if (s_sta_netif) esp_netif_set_hostname(s_sta_netif, s_hostname);
}

const char *network_get_hostname(void)
{
    return s_hostname;
}

esp_err_t network_start(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    return network_start_with_fallback();
}

esp_err_t network_stop(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "Stopping network...");
    network_ethernet_disconnect();
    network_wifi_stop();
    return ESP_OK;
}

// ============================================================================
// Ethernet
// ============================================================================

/* Apply DHCP or a static address to the Ethernet netif */
static void eth_apply_ip(const ip_config_t *config)
{
    bool dhcp = !(config && !config->use_dhcp);
    esp_netif_ip_info_t info = {0};
    if (!dhcp) {
        if (esp_netif_str_to_ip4(config->ip, &info.ip) != ESP_OK ||
            esp_netif_str_to_ip4(config->netmask, &info.netmask) != ESP_OK ||
            info.ip.addr == 0) {
            ESP_LOGE(TAG, "Invalid static Ethernet address \"%s\"/\"%s\", using DHCP",
                     config->ip, config->netmask);
            dhcp = true;
        } else if (config->gateway[0]) {
            esp_netif_str_to_ip4(config->gateway, &info.gw);   /* optional */
        }
    }

    xEventGroupClearBits(s_bits, ETH_READY_BIT);
    if (dhcp) {
        s_eth_dhcp = true;
        esp_netif_dhcpc_start(s_eth_netif);
        ESP_LOGI(TAG, "Ethernet: DHCP");
    } else {
        s_eth_dhcp = false;
        esp_netif_dhcpc_stop(s_eth_netif);
        esp_netif_set_ip_info(s_eth_netif, &info);
        ESP_LOGI(TAG, "Ethernet: static " IPSTR " mask " IPSTR " gw " IPSTR,
                 IP2STR(&info.ip), IP2STR(&info.netmask), IP2STR(&info.gw));
        if (s_eth_link) eth_mark_ready();
    }
    refresh_state(false);
}

esp_err_t network_ethernet_connect(const ip_config_t *config)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    if (s_eth_handle) {
        /* Installed before: (re)start it with these settings */
        eth_apply_ip(config);
        if (!s_eth_running) {
            esp_err_t ret = esp_eth_start(s_eth_handle);
            if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
                ESP_LOGE(TAG, "esp_eth_start failed: %s", esp_err_to_name(ret));
                return ret;
            }
            s_eth_running = true;
        }
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initialising W5500 SPI Ethernet...");

    spi_bus_config_t buscfg = {
        .mosi_io_num   = W5500_MOSI_PIN,
        .miso_io_num   = W5500_MISO_PIN,
        .sclk_io_num   = W5500_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* The IDF 5.2 W5500 driver requires command_bits=16 / address_bits=8 */
    spi_device_interface_config_t devcfg = {
        .command_bits   = 16,
        .address_bits   = 8,
        .dummy_bits     = 0,
        .mode           = 0,
        .spics_io_num   = W5500_CS_PIN,
        .queue_size     = 20,
        .clock_speed_hz = 20 * 1000 * 1000,
    };

    esp_err_t isr_ret = gpio_install_isr_service(0);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "gpio_install_isr_service failed: %d", isr_ret);
    }

    eth_w5500_config_t w5500_cfg = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &devcfg);
    w5500_cfg.int_gpio_num = W5500_INT_PIN;

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.reset_gpio_num = -1;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_cfg, &mac_cfg);
    if (!mac) {
        ESP_LOGE(TAG, "esp_eth_mac_new_w5500 failed");
        return ESP_FAIL;
    }
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_cfg);
    if (!phy) {
        ESP_LOGE(TAG, "esp_eth_phy_new_w5500 failed");
        mac->del(mac);
        return ESP_FAIL;
    }

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t handle = NULL;
    ret = esp_eth_driver_install(&eth_cfg, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_driver_install failed: %s (is the W5500 fitted and wired?)",
                 esp_err_to_name(ret));
        phy->del(phy);
        mac->del(mac);
        return ret;
    }

    /* W5500 has no factory MAC; use the ESP32's Ethernet MAC */
    uint8_t mac_addr[6];
    esp_read_mac(mac_addr, ESP_MAC_ETH);
    esp_eth_ioctl(handle, ETH_CMD_S_MAC_ADDR, mac_addr);
    ESP_LOGI(TAG, "W5500 MAC " MACSTR, MAC2STR(mac_addr));

    /* Let multicast through (sACN), see file header */
    bool promiscuous = true;
    ret = esp_eth_ioctl(handle, ETH_CMD_S_PROMISCUOUS, &promiscuous);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "W5500 MAC filter could not be disabled (%s): sACN multicast "
                 "over Ethernet will not work", esp_err_to_name(ret));
    }

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);
    if (!s_eth_netif) {
        ESP_LOGE(TAG, "esp_netif_new failed");
        esp_eth_driver_uninstall(handle);
        return ESP_FAIL;
    }
    esp_netif_set_hostname(s_eth_netif, s_hostname);
    esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(handle));
    s_eth_handle = handle;

    eth_apply_ip(config);

    ret = esp_eth_start(s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_eth_running = true;
    refresh_state(false);
    ESP_LOGI(TAG, "W5500 Ethernet started, waiting for link");
    return ESP_OK;
}

esp_err_t network_ethernet_set_ip(const ip_config_t *config)
{
    if (!s_eth_handle) return ESP_ERR_INVALID_STATE;
    eth_apply_ip(config);
    return ESP_OK;
}

esp_err_t network_ethernet_disconnect(void)
{
    if (!s_eth_handle) return ESP_ERR_INVALID_STATE;
    if (s_eth_running) {
        ESP_LOGI(TAG, "Ethernet off");
        esp_eth_stop(s_eth_handle);
        s_eth_running = false;
        s_eth_link = false;
        xEventGroupClearBits(s_bits, ETH_READY_BIT);
        refresh_state(false);
    }
    return ESP_OK;
}

bool network_ethernet_is_link_up(void)
{
    return s_eth_handle && bit_set(ETH_READY_BIT);
}

void network_get_eth_info(network_eth_info_t *info)
{
    if (!info) return;
    memset(info, 0, sizeof(*info));
    info->present = s_eth_handle != NULL;
    info->running = s_eth_running;
    info->link = s_eth_link;
    info->ready = network_ethernet_is_link_up();
    info->dhcp = s_eth_dhcp;
    if (s_eth_netif) {
        if (info->ready) ip_strings(s_eth_netif, info->ip, info->netmask, info->gateway);
        esp_netif_get_mac(s_eth_netif, info->mac);
    } else {
        esp_read_mac(info->mac, ESP_MAC_ETH);
    }
}

// ============================================================================
// WiFi station
// ============================================================================

esp_err_t network_wifi_sta_connect(const wifi_profile_t *profile)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!profile || !profile->ssid[0]) return ESP_ERR_INVALID_ARG;

    wifi_lock();
    esp_err_t ret = wifi_ensure_init();
    if (ret != ESP_OK) {
        wifi_unlock();
        return ret;
    }
    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (!s_sta_netif) {
            wifi_unlock();
            return ESP_FAIL;
        }
        esp_netif_set_hostname(s_sta_netif, s_hostname);
    }

    /* Leave the current network first (reason ASSOC_LEAVE, ignored) */
    if (s_sta_on && s_wifi_started) {
        esp_wifi_disconnect();
    }
    xEventGroupClearBits(s_bits, STA_READY_BIT);

    /* Address: DHCP, or static (gateway optional) */
    bool dhcp = true;
    esp_netif_ip_info_t ip_info = {0};
    if (profile->use_static_ip) {
        if (esp_netif_str_to_ip4(profile->static_ip.ip, &ip_info.ip) == ESP_OK &&
            esp_netif_str_to_ip4(profile->static_ip.netmask, &ip_info.netmask) == ESP_OK &&
            ip_info.ip.addr != 0) {
            if (profile->static_ip.gateway[0]) {
                esp_netif_str_to_ip4(profile->static_ip.gateway, &ip_info.gw);
            }
            dhcp = false;
        } else {
            ESP_LOGW(TAG, "Invalid static WiFi address \"%s\", using DHCP", profile->static_ip.ip);
        }
    }
    esp_netif_dhcpc_stop(s_sta_netif);
    if (dhcp) {
        esp_netif_ip_info_t zero = {0};
        esp_netif_set_ip_info(s_sta_netif, &zero);
        esp_netif_dhcpc_start(s_sta_netif);
    } else {
        esp_netif_set_ip_info(s_sta_netif, &ip_info);
        ESP_LOGI(TAG, "WiFi static " IPSTR " mask " IPSTR " gw " IPSTR,
                 IP2STR(&ip_info.ip), IP2STR(&ip_info.netmask), IP2STR(&ip_info.gw));
    }
    s_sta_dhcp = dhcp;

    wifi_config_t wc = {0};
    size_t ssid_len = strnlen(profile->ssid, sizeof(wc.sta.ssid));
    memcpy(wc.sta.ssid, profile->ssid, ssid_len);
    /* 8-63 character passphrase, or a 64 hex digit key (not terminated) */
    size_t pw_len = strnlen(profile->password, sizeof(wc.sta.password));
    memcpy(wc.sta.password, profile->password, pw_len);
    /* Minimum security: any WPA/WPA2/WPA3 with a password, open without */
    wc.sta.threshold.authmode = pw_len ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;
    /* WPA3: both SAE methods (WiFi 6 routers often only offer hash-to-element) */
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    /* Mesh / multi-AP networks: search every channel, take the strongest */
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    copy_bounded(s_sta_ssid, sizeof(s_sta_ssid), profile->ssid);
    s_sta_on = true;
    s_sta_state = NET_STA_CONNECTING;
    s_sta_reason = 0;

    ret = wifi_apply_mode();
    if (ret == ESP_OK) ret = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (ret == ESP_OK) {
        __atomic_store_n(&s_connect_pending, true, __ATOMIC_RELEASE);
        bool was_started = s_wifi_started;
        ret = wifi_start_if_needed();
        if (ret == ESP_OK && was_started) {
            connect_if_pending();   /* no STA_START event when already running */
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi station start failed: %s", esp_err_to_name(ret));
        s_sta_on = false;
        s_sta_state = NET_STA_IDLE;
        __atomic_store_n(&s_connect_pending, false, __ATOMIC_RELEASE);
        wifi_apply_mode();
    } else {
        ESP_LOGI(TAG, "Joining WiFi \"%s\"%s", s_sta_ssid, dhcp ? "" : " (static address)");
    }
    wifi_unlock();
    refresh_state(false);
    return ret;
}

esp_err_t network_wifi_sta_retry(void)
{
    wifi_lock();
    if (!s_sta_on || !s_wifi_started) {
        wifi_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_sta_state = NET_STA_CONNECTING;
    esp_err_t ret = esp_wifi_connect();
    wifi_unlock();
    return ret;
}

esp_err_t network_wifi_sta_stop(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    wifi_lock();
    if (s_sta_on) {
        ESP_LOGI(TAG, "WiFi station off");
        s_sta_on = false;
        s_sta_state = NET_STA_IDLE;
        __atomic_store_n(&s_connect_pending, false, __ATOMIC_RELEASE);
        if (s_wifi_started) esp_wifi_disconnect();
        xEventGroupClearBits(s_bits, STA_READY_BIT);
        wifi_apply_mode();
    }
    wifi_unlock();
    refresh_state(false);
    return ESP_OK;
}

esp_err_t network_wifi_sta_disconnect(void)
{
    return network_wifi_sta_stop();
}

bool network_wifi_sta_is_connected(void)
{
    return bit_set(STA_READY_BIT);
}

void network_get_sta_info(network_sta_info_t *info)
{
    if (!info) return;
    memset(info, 0, sizeof(*info));
    info->state = s_sta_on ? s_sta_state : NET_STA_IDLE;
    copy_bounded(info->ssid, sizeof(info->ssid), s_sta_ssid);
    info->reason = s_sta_reason;
    info->disconnects = s_sta_disconnects;
    info->dhcp = s_sta_dhcp;
    if (bit_set(STA_READY_BIT) && s_sta_netif) {
        ip_strings(s_sta_netif, info->ip, info->netmask, info->gateway);
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            info->rssi = ap.rssi;
            info->channel = ap.primary;
        }
    }
    esp_read_mac(info->mac, ESP_MAC_WIFI_STA);
}

const char *network_wifi_reason_text(uint8_t reason)
{
    switch (reason) {
    case 0:
        return "";
    case WIFI_REASON_NO_AP_FOUND:
        return "Network not found (check the name; only 2.4 GHz networks work)";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "Security type not supported (open networks need an empty password)";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "Signal too weak";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return "Wrong password";
    case WIFI_REASON_AUTH_FAIL:
        return "Authentication failed (wrong password?)";
    case WIFI_REASON_802_1X_AUTH_FAILED:
        return "Enterprise (802.1X) networks are not supported";
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_ASSOC_EXPIRE:
        return "No answer from the access point (weak signal?)";
    case WIFI_REASON_ASSOC_TOOMANY:
        return "Access point is full";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "Signal lost";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_CONNECTION_FAIL:
        return "Connection failed";
    case WIFI_REASON_AUTH_LEAVE:
    case WIFI_REASON_AP_INITIATED:
        return "Disconnected by the access point";
    case NETWORK_REASON_NO_DHCP:
        return "Joined, but no address from DHCP (set a static address?)";
    case NETWORK_REASON_TIMEOUT:
        return "No answer (out of range?)";
    default:
        return "Disconnected";
    }
}

bool network_wifi_reason_is_final(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_802_1X_AUTH_FAILED:
        return true;
    default:
        return false;
    }
}

uint16_t network_wifi_scan(wifi_ap_record_t *out, uint16_t max)
{
    return network_wifi_scan_cached(out, max, 0);
}

uint16_t network_wifi_scan_cached(wifi_ap_record_t *out, uint16_t max, uint32_t max_age_ms)
{
    if (!s_initialized || !out || max == 0) return 0;

    wifi_lock();
    int64_t age_ms = (esp_timer_get_time() - s_scan_time_us) / 1000;
    bool fresh = s_scan_time_us != 0 && max_age_ms > 0 && age_ms <= (int64_t)max_age_ms;
    /* A scan would fail (and disturb) while the station is joining */
    bool busy = s_sta_on && (s_sta_state == NET_STA_CONNECTING ||
                             s_sta_state == NET_STA_GETTING_IP);

    if (!fresh && !busy && wifi_ensure_init() == ESP_OK) {
        bool temp = !s_sta_on;
        if (temp) {
            if (!s_sta_netif) {
                s_sta_netif = esp_netif_create_default_wifi_sta();
                if (s_sta_netif) esp_netif_set_hostname(s_sta_netif, s_hostname);
            }
            s_scan_sta = true;
        }
        if (wifi_apply_mode() == ESP_OK && wifi_start_if_needed() == ESP_OK) {
            wifi_scan_config_t sc = {
                .ssid = NULL,
                .bssid = NULL,
                .channel = 0,
                .show_hidden = false,
                .scan_type = WIFI_SCAN_TYPE_ACTIVE,
                .scan_time.active = {.min = 50, .max = 120},
            };
            esp_err_t ret = esp_wifi_scan_start(&sc, true);
            if (ret == ESP_OK) {
                uint16_t n = SCAN_CACHE_MAX;
                if (esp_wifi_scan_get_ap_records(&n, s_scan) == ESP_OK) {
                    s_scan_count = n;
                    s_scan_time_us = esp_timer_get_time();
                    ESP_LOGI(TAG, "WiFi scan: %u network(s)", n);
                }
            } else {
                ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(ret));
                esp_wifi_clear_ap_list();
            }
        }
        if (temp) {
            s_scan_sta = false;
            wifi_apply_mode();
        }
    }

    uint16_t n = s_scan_count < max ? s_scan_count : max;
    memcpy(out, s_scan, sizeof(wifi_ap_record_t) * n);
    wifi_unlock();
    return n;
}

// ============================================================================
// WiFi access point
// ============================================================================

esp_err_t network_wifi_ap_start(const char *ssid, const char *password, uint8_t channel,
                                const ip_config_t *config)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;

    wifi_config_t wc = {0};
    size_t ssid_len = strnlen(ssid, sizeof(wc.ap.ssid));
    memcpy(wc.ap.ssid, ssid, ssid_len);
    wc.ap.ssid_len = (uint8_t)ssid_len;
    wc.ap.channel = (channel >= 1 && channel <= 13) ? channel : 1;
    wc.ap.max_connection = 4;
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wc.ap.pmf_cfg.required = false;
    size_t pw_len = password ? strnlen(password, sizeof(wc.ap.password) - 1) : 0;
    if (pw_len >= 8) {
        memcpy(wc.ap.password, password, pw_len);
    } else {
        ESP_LOGW(TAG, "AP password missing or shorter than 8 characters: AP is OPEN");
        wc.ap.authmode = WIFI_AUTH_OPEN;
    }

    wifi_lock();
    esp_err_t ret = wifi_ensure_init();
    if (ret != ESP_OK) {
        wifi_unlock();
        return ret;
    }

    /* Same settings and already up: nothing to do (a new config would
     * restart the AP and drop its clients) */
    if (s_ap_on && s_ap_active && memcmp(&wc, &s_ap_cfg, sizeof(wc)) == 0) {
        wifi_unlock();
        return ESP_OK;
    }

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) {
            wifi_unlock();
            return ESP_FAIL;
        }
        esp_netif_ip_info_t ip_info = {0};
        const char *ip = (config && config->ip[0]) ? config->ip : NETWORK_AP_IP;
        const char *mask = (config && config->netmask[0]) ? config->netmask : "255.255.255.0";
        esp_netif_str_to_ip4(ip, &ip_info.ip);
        esp_netif_str_to_ip4(mask, &ip_info.netmask);
        ip_info.gw = ip_info.ip;
        esp_netif_dhcps_stop(s_ap_netif);
        esp_netif_set_ip_info(s_ap_netif, &ip_info);
        esp_netif_dhcps_start(s_ap_netif);
    }

    s_ap_on = true;
    ret = wifi_apply_mode();
    if (ret == ESP_OK) ret = esp_wifi_set_config(WIFI_IF_AP, &wc);
    if (ret == ESP_OK) {
        bool was_started = s_wifi_started;
        ret = wifi_start_if_needed();
        if (ret == ESP_OK && was_started) {
            s_ap_active = true;   /* mode change on a running driver */
        }
    }
    if (ret == ESP_OK) {
        s_ap_cfg = wc;
        ESP_LOGI(TAG, "Access point \"%s\" on channel %d (%s)", ssid, wc.ap.channel,
                 wc.ap.authmode == WIFI_AUTH_OPEN ? "open" : "WPA2");
    } else {
        ESP_LOGE(TAG, "Access point start failed: %s", esp_err_to_name(ret));
        s_ap_on = false;
        wifi_apply_mode();
    }
    wifi_unlock();
    refresh_state(false);
    return ret;
}

esp_err_t network_wifi_ap_stop(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    wifi_lock();
    if (s_ap_on) {
        ESP_LOGI(TAG, "Access point off");
        s_ap_on = false;
        wifi_apply_mode();
        s_ap_active = false;
    }
    wifi_unlock();
    refresh_state(false);
    return ESP_OK;
}

uint16_t network_wifi_ap_get_stations(wifi_sta_info_t *stations, uint16_t max_stations)
{
    if (!stations || max_stations == 0 || !s_ap_active) return 0;
    wifi_sta_list_t list;
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) return 0;
    uint16_t n = (list.num < max_stations) ? list.num : max_stations;
    for (int i = 0; i < n; i++) {
        stations[i] = list.sta[i];
    }
    return n;
}

void network_get_ap_info(network_ap_info_t *info)
{
    if (!info) return;
    memset(info, 0, sizeof(*info));
    info->active = s_ap_active;
    size_t n = strnlen((const char *)s_ap_cfg.ap.ssid, sizeof(s_ap_cfg.ap.ssid));
    memcpy(info->ssid, s_ap_cfg.ap.ssid, n);
    info->ssid[n] = '\0';
    esp_read_mac(info->mac, ESP_MAC_WIFI_SOFTAP);
    if (!s_ap_active) return;
    ip_strings(s_ap_netif, info->ip, NULL, NULL);
    uint8_t primary = 0;
    wifi_second_chan_t second;
    if (esp_wifi_get_channel(&primary, &second) == ESP_OK) info->channel = primary;
    wifi_sta_list_t list;
    if (esp_wifi_ap_get_sta_list(&list) == ESP_OK) info->clients = (uint8_t)list.num;
}

esp_err_t network_wifi_stop(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    wifi_lock();
    s_sta_on = false;
    s_ap_on = false;
    s_sta_state = NET_STA_IDLE;
    __atomic_store_n(&s_connect_pending, false, __ATOMIC_RELEASE);
    xEventGroupClearBits(s_bits, STA_READY_BIT);
    wifi_apply_mode();
    wifi_unlock();
    refresh_state(false);
    return ESP_OK;
}

// ============================================================================
// Status
// ============================================================================

esp_err_t network_get_status(network_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    memcpy(status, &s_status, sizeof(*status));
    status->state = s_state;
    xSemaphoreGive(s_state_lock);
    if (status->state == NETWORK_WIFI_STA_CONNECTED) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) status->rssi = ap.rssi;
    }
    return ESP_OK;
}

static esp_netif_t *primary_netif(void)
{
    switch (s_state) {
    case NETWORK_ETHERNET_CONNECTED: return s_eth_netif;
    case NETWORK_WIFI_STA_CONNECTED: return s_sta_netif;
    case NETWORK_WIFI_AP_ACTIVE:     return s_ap_netif;
    default:                         return NULL;
    }
}

esp_err_t network_get_ip_info(network_ip_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));
    info->state = s_state;
    esp_netif_t *netif = primary_netif();
    if (!netif) {
        esp_read_mac(info->mac, ESP_MAC_WIFI_STA);
        return ESP_ERR_INVALID_STATE;
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        info->ip = ip.ip.addr;
        info->netmask = ip.netmask.addr;
        info->gateway = ip.gw.addr;
    }
    esp_netif_get_mac(netif, info->mac);
    info->dhcp = (netif == s_eth_netif) ? s_eth_dhcp :
                 (netif == s_sta_netif) ? s_sta_dhcp : false;
    return ESP_OK;
}

const char *network_get_ip_address(void)
{
    return s_status.ip_address[0] ? s_status.ip_address : NULL;
}

esp_err_t network_get_mac_address(uint8_t mac[6])
{
    if (!mac) return ESP_ERR_INVALID_ARG;
    esp_netif_t *netif = primary_netif();
    return netif ? esp_netif_get_mac(netif, mac) : esp_read_mac(mac, ESP_MAC_WIFI_STA);
}

bool network_is_connected(void)
{
    return s_state == NETWORK_ETHERNET_CONNECTED ||
           s_state == NETWORK_WIFI_STA_CONNECTED ||
           s_state == NETWORK_WIFI_AP_ACTIVE;
}

void network_register_state_callback(network_state_callback_t callback, void *user_data)
{
    s_state_cb_ctx = user_data;
    s_state_cb = callback;
}
