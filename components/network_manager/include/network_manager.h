/**
 * @file network_manager.h
 * @brief Network interfaces for ESP-NODE-2RDM
 *
 * Owns the drivers: W5500 SPI Ethernet, WiFi station and WiFi access point
 * (station and AP can run together). Which of them are used, and when, is
 * decided by the supervisor (network_auto_fallback.c, network_supervisor_*).
 */

#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** IPv4 settings (dotted strings) */
typedef struct {
    bool use_dhcp;
    char ip[16];
    char netmask[16];
    char gateway[16];
    char dns1[16];
    char dns2[16];
} ip_config_t;

/** One WiFi network to join */
typedef struct {
    char ssid[33];
    char password[65];
    uint8_t priority;       /**< lower value = tried first */
    bool use_static_ip;
    ip_config_t static_ip;
} wifi_profile_t;

/** Primary interface, in order of preference Ethernet > WiFi > own AP */
typedef enum {
    NETWORK_DISCONNECTED = 0,
    NETWORK_ETHERNET_CONNECTED = 1,
    NETWORK_WIFI_STA_CONNECTED = 2,
    NETWORK_WIFI_AP_ACTIVE = 3,
    NETWORK_CONNECTING = 4,
    NETWORK_ERROR = 5
} network_state_t;

/** Legacy status of the primary interface */
typedef struct {
    network_state_t state;
    char ip_address[16];
    char netmask[16];
    char gateway[16];
    uint8_t mac[6];
    int rssi;               /**< WiFi station only */
    uint32_t packets_received;
    uint32_t packets_sent;
    uint64_t bytes_received;
    uint64_t bytes_sent;
} network_status_t;

/** Addressing of the primary (active) interface, all IPv4 in network byte order */
typedef struct {
    network_state_t state;   /**< which interface is primary */
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    bool dhcp;               /**< address obtained by DHCP */
    uint8_t mac[6];
} network_ip_info_t;

typedef enum {
    NET_STA_IDLE = 0,        /**< station off */
    NET_STA_CONNECTING,      /**< searching / associating */
    NET_STA_GETTING_IP,      /**< associated, waiting for DHCP */
    NET_STA_CONNECTED,       /**< has an address */
    NET_STA_FAILED,          /**< last attempt ended, see reason */
} network_sta_state_t;

typedef struct {
    bool present;            /**< W5500 found and driver installed */
    bool running;            /**< driver started */
    bool link;               /**< cable plugged in and link up */
    bool ready;              /**< link up and an address */
    bool dhcp;
    char ip[16];
    char netmask[16];
    char gateway[16];
    uint8_t mac[6];
    uint32_t restarts;       /**< times the W5500 was set up again (eth_watch.h) */
} network_eth_info_t;

typedef struct {
    network_sta_state_t state;
    char ssid[33];           /**< network being joined / joined */
    uint8_t reason;          /**< last disconnect reason (wifi_err_reason_t), 0 = none */
    uint32_t disconnects;
    bool dhcp;
    char ip[16];
    char netmask[16];
    char gateway[16];
    int8_t rssi;             /**< valid when connected */
    uint8_t channel;         /**< valid when connected */
    uint8_t mac[6];
} network_sta_info_t;

typedef struct {
    bool active;
    char ssid[33];
    char ip[16];
    uint8_t channel;
    uint8_t clients;
    uint8_t mac[6];
} network_ap_info_t;

#define NETWORK_AP_IP "192.168.4.1"

/* Attempt results that aren't 802.11 reasons (those stop at 212) */
#define NETWORK_REASON_NO_DHCP  250   /**< joined, but no DHCP lease */
#define NETWORK_REASON_TIMEOUT  251   /**< no answer within the attempt time */

typedef void (*network_state_callback_t)(network_state_t state, void *user_data);

// ============================================================================
// Initialization
// ============================================================================

/** Create the TCP/IP stack and event handlers. Call before anything else. */
esp_err_t network_init(void);

/** Start the supervisor task (asynchronous, see network_supervisor_*) */
esp_err_t network_start_with_fallback(void);
esp_err_t network_start(void);   /**< same as network_start_with_fallback() */

/** Stop every interface */
esp_err_t network_stop(void);

/** DHCP / mDNS hostname used for every interface (call before network start) */
void network_set_hostname(const char *hostname);
const char *network_get_hostname(void);

// ============================================================================
// Ethernet (W5500)
// ============================================================================

/** Install (first call) and start the W5500 driver. @p config NULL = DHCP */
esp_err_t network_ethernet_connect(const ip_config_t *config);
/** Stop the driver (it stays installed; network_ethernet_connect() restarts it) */
esp_err_t network_ethernet_disconnect(void);
/** Switch between DHCP and a static address while running */
esp_err_t network_ethernet_set_ip(const ip_config_t *config);
/**
 * Call regularly (the supervisor does, 4 times a second): sets the W5500 up
 * again when nothing has arrived for a long time with the link up
 * (eth_watch.h). Blocks for a moment when it does.
 */
void network_ethernet_watch(void);
/** Ethernet has a link and an address */
bool network_ethernet_is_link_up(void);
void network_get_eth_info(network_eth_info_t *info);

// ============================================================================
// WiFi station
// ============================================================================

/**
 * Join @p profile. An AP that is running stays up (AP+STA). The driver does
 * not reconnect by itself; the supervisor decides (network_wifi_sta_retry()).
 */
esp_err_t network_wifi_sta_connect(const wifi_profile_t *profile);
/** Try the current network again */
esp_err_t network_wifi_sta_retry(void);
/** Leave the network and switch the station off (AP untouched) */
esp_err_t network_wifi_sta_stop(void);
esp_err_t network_wifi_sta_disconnect(void);   /**< same as network_wifi_sta_stop() */
/** Station has an address */
bool network_wifi_sta_is_connected(void);
void network_get_sta_info(network_sta_info_t *info);

/** Human-readable text for a disconnect reason ("" for 0) */
const char *network_wifi_reason_text(uint8_t reason);
/** Retrying the same network won't help (wrong password, not found, ...) */
bool network_wifi_reason_is_final(uint8_t reason);

/**
 * Scan for networks. Uses the result of a scan younger than @p max_age_ms
 * when there is one (0 = always scan). While the station is busy joining a
 * network the last result is returned instead.
 * @return number of records written to @p out
 */
uint16_t network_wifi_scan_cached(wifi_ap_record_t *out, uint16_t max, uint32_t max_age_ms);
uint16_t network_wifi_scan(wifi_ap_record_t *scan_result, uint16_t max_aps);   /**< fresh scan */

// ============================================================================
// WiFi access point
// ============================================================================

/**
 * Start (or reconfigure) the node's own AP at 192.168.4.1. A station that
 * is running stays up; the AP then follows the station's channel.
 * @p password shorter than 8 characters makes an open AP.
 */
esp_err_t network_wifi_ap_start(const char *ssid, const char *password, uint8_t channel,
                                const ip_config_t *config);
/** Switch the AP off (station untouched) */
esp_err_t network_wifi_ap_stop(void);
uint16_t network_wifi_ap_get_stations(wifi_sta_info_t *stations, uint16_t max_stations);
void network_get_ap_info(network_ap_info_t *info);
/** The access point's address and netmask (network byte order); false while it is off */
bool network_ap_address(uint32_t *ip, uint32_t *netmask);

/** Switch WiFi off completely (station and AP) */
esp_err_t network_wifi_stop(void);

// ============================================================================
// Status
// ============================================================================

esp_err_t network_get_status(network_status_t *status);
/** Primary interface address, NULL when there is none */
const char *network_get_ip_address(void);
esp_err_t network_get_mac_address(uint8_t mac[6]);
/** Any interface usable (Ethernet, WiFi or own AP) */
bool network_is_connected(void);
/** Primary interface addressing (for ArtPollReply, directed broadcast, ...) */
esp_err_t network_get_ip_info(network_ip_info_t *info);

void network_register_state_callback(network_state_callback_t callback, void *user_data);

// ============================================================================
// Supervisor (network_auto_fallback.c): connection policy
// ============================================================================

typedef enum {
    NET_AP_OFF = 0,
    NET_AP_STANDALONE,       /**< mode "access point only" */
    NET_AP_ALWAYS,           /**< "keep access point on" */
    NET_AP_RESCUE,           /**< no other connection */
    NET_AP_LINGER,           /**< connected elsewhere; kept briefly for AP clients */
} network_ap_reason_t;

#define NETWORK_PROFILES_MAX 5

typedef struct {
    char ssid[33];
    bool visible;            /**< seen in the last scan */
    int8_t rssi;             /**< strongest signal in the last scan */
    uint8_t reason;          /**< result of the last attempt, 0 = none / OK */
    bool tried;
} network_profile_status_t;

typedef struct {
    uint8_t mode;            /**< net_mode_t */
    bool ap_always;

    bool eth_wanted;
    network_eth_info_t eth;

    bool sta_wanted;
    network_sta_info_t sta;
    uint8_t profile_count;
    network_profile_status_t profiles[NETWORK_PROFILES_MAX];
    uint32_t sta_retry_in_s; /**< time to the next attempt, 0 = trying now */
    bool sta_test;           /**< "connect now" from the web UI in progress */

    network_ap_info_t ap;
    network_ap_reason_t ap_reason;
    uint32_t ap_off_in_s;    /**< NET_AP_LINGER: seconds until the AP goes off */
} network_supervisor_status_t;

/** Settings changed: apply them without a restart */
void network_supervisor_reconfigure(void);
/** Try the saved network @p ssid now (skips back-off) */
esp_err_t network_supervisor_connect_now(const char *ssid);
/** Switch off an AP that is only lingering for its clients */
void network_supervisor_ap_dismiss(void);
void network_supervisor_get_status(network_supervisor_status_t *status);

#ifdef __cplusplus
}
#endif

#endif // NETWORK_MANAGER_H
