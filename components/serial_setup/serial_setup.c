/*
 * USB / serial setup: the protocol is described in serial_setup.h.
 *
 * The console port(s) are switched to their interrupt drivers so they can be
 * read; the log keeps going to them as before. On the ESP32-S3 the chip's own
 * USB port is the secondary console (output only) unless this reads it too.
 */
#include "serial_setup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_idf_version.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_wifi_types.h"
#include "cJSON.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#include "board.h"
#include "config_manager.h"
#include "network_manager.h"
#include "dmx_tester.h"

#if CONFIG_ESP_CONSOLE_UART
#define SETUP_UART 1
#define SETUP_UART_NUM CONFIG_ESP_CONSOLE_UART_NUM
#include "driver/uart.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
#include "driver/uart_vfs.h"
#define SETUP_UART_USE_DRIVER(n) uart_vfs_dev_use_driver(n)
#else
#include "esp_vfs_dev.h"
#define SETUP_UART_USE_DRIVER(n) esp_vfs_dev_uart_use_driver(n)
#endif
#else
#define SETUP_UART 0
#endif

#if SOC_USB_SERIAL_JTAG_SUPPORTED && \
    (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG)
#define SETUP_USB 1
#include "driver/usb_serial_jtag.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
#include "driver/usb_serial_jtag_vfs.h"
#define SETUP_USB_USE_DRIVER() usb_serial_jtag_vfs_use_driver()
#else
#include "esp_vfs_dev.h"   /* esp_vfs_usb_serial_jtag_use_driver() in 5.2 */
#define SETUP_USB_USE_DRIVER() esp_vfs_usb_serial_jtag_use_driver()
#endif
#else
#define SETUP_USB 0
#endif

/* 1 if the Kconfig bool @p x is set (a switched-off bool is not defined) */
#define SETUP_HAS_(x) (#x[0] == '1' && #x[1] == '\0')
#define SETUP_HAS(x)  SETUP_HAS_(x)

#if CONFIG_IDF_TARGET_ESP32S3
#define SETUP_CHIP "ESP32-S3"
#elif CONFIG_IDF_TARGET_ESP32
#define SETUP_CHIP "ESP32"
#else
#define SETUP_CHIP CONFIG_IDF_TARGET
#endif

#define PREFIX      "#GL "
#define SETUP_LINE_MAX    4096
#define SCAN_MAX    24

static const char *TAG = "setup";

typedef struct {
    char *buf;
    size_t len;
    bool overflow;
} line_t;

static bool s_started;
static bool s_uart_ok;   /* console UART driver installed: read it */
static bool s_usb_ok;    /* USB-Serial-JTAG driver installed: read it */

// ---------------------------------------------------------------------------
// Replies
// ---------------------------------------------------------------------------

/** Print @p reply as one "#GL> " line (stdout: every console port) */
static void send_reply(cJSON *reply)
{
    char *s = reply ? cJSON_PrintUnformatted(reply) : NULL;
    if (s) {
        /* One printf holds the stdout lock, so log lines can't land inside */
        printf("\n#GL> %s\n", s);
        fflush(stdout);
        cJSON_free(s);
    } else {
        printf("\n#GL> {\"ok\":false,\"error\":\"Out of memory\"}\n");
        fflush(stdout);
    }
}

static cJSON *error_reply(const char *msg)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", false);
    cJSON_AddStringToObject(r, "error", msg);
    return r;
}

static cJSON *ok_reply(void)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    return r;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

static void add_network(cJSON *r)
{
    static const char *net_modes[] = {"auto", "ethernet", "wifi", "ap"};
    static const char *sta_states[] = {"off", "connecting", "getting_ip", "connected", "failed"};
    static const char *ap_reasons[] = {"off", "standalone", "always", "rescue", "linger"};

    network_status_t st = {0};
    network_get_status(&st);
    network_supervisor_status_t sup;
    memset(&sup, 0, sizeof(sup));
    network_supervisor_get_status(&sup);

    cJSON *n = cJSON_AddObjectToObject(r, "network");
    if (!n) return;
    const char *primary = "none";
    switch (st.state) {
        case NETWORK_ETHERNET_CONNECTED: primary = "ethernet"; break;
        case NETWORK_WIFI_STA_CONNECTED: primary = "wifi";     break;
        case NETWORK_WIFI_AP_ACTIVE:     primary = "ap";       break;
        default: break;
    }
    cJSON_AddStringToObject(n, "primary", primary);
    cJSON_AddStringToObject(n, "ip_address", primary[0] != 'n' ? st.ip_address : "");
    cJSON_AddStringToObject(n, "mode", sup.mode <= NET_MODE_LAST ? net_modes[sup.mode] : "auto");
    cJSON_AddStringToObject(n, "hostname", network_get_hostname());
#if CONFIG_NODE_ETHERNET
    cJSON *eth = cJSON_AddObjectToObject(n, "ethernet");
    cJSON_AddBoolToObject(eth, "present", sup.eth.present);
    cJSON_AddBoolToObject(eth, "link", sup.eth.link);
    cJSON_AddBoolToObject(eth, "ready", sup.eth.ready);
    cJSON_AddStringToObject(eth, "ip", sup.eth.ip);
#endif
    cJSON *w = cJSON_AddObjectToObject(n, "wifi");
    cJSON_AddStringToObject(w, "state", sup.sta.state <= NET_STA_FAILED ? sta_states[sup.sta.state] : "off");
    cJSON_AddStringToObject(w, "ssid", sup.sta.ssid);
    cJSON_AddStringToObject(w, "ip", sup.sta.ip);
    cJSON_AddNumberToObject(w, "rssi", sup.sta.rssi);
    cJSON_AddNumberToObject(w, "reason", sup.sta.reason);
    if (sup.sta.reason) {
        cJSON_AddStringToObject(w, "reason_text", network_wifi_reason_text(sup.sta.reason));
    }
    cJSON_AddNumberToObject(w, "retry_in", sup.sta_retry_in_s);
    cJSON *ap = cJSON_AddObjectToObject(n, "ap");
    cJSON_AddBoolToObject(ap, "active", sup.ap.active);
    cJSON_AddStringToObject(ap, "ssid", sup.ap.ssid);
    cJSON_AddStringToObject(ap, "ip", sup.ap.ip);
    cJSON_AddStringToObject(ap, "reason", sup.ap_reason <= NET_AP_LINGER ? ap_reasons[sup.ap_reason] : "off");
}

static cJSON *cmd_info(void)
{
    cJSON *r = ok_reply();
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON_AddStringToObject(r, "firmware_version", app ? app->version : "");
    cJSON_AddStringToObject(r, "board", BOARD_NAME);
    cJSON_AddStringToObject(r, "chip", SETUP_CHIP);
    cJSON_AddNumberToObject(r, "ports", CONFIG_DMX_PORTS);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macs[18];
    snprintf(macs, sizeof(macs), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(r, "mac", macs);
    cJSON_AddBoolToObject(r, "password_set", config_admin_password_set());
    cJSON_AddBoolToObject(r, "setup_done", config_setup_done());
    cJSON_AddBoolToObject(r, "login_required", config_login_required());

    cJSON *f = cJSON_AddObjectToObject(r, "features");
    cJSON_AddBoolToObject(f, "ethernet", SETUP_HAS(CONFIG_NODE_ETHERNET));
    cJSON_AddBoolToObject(f, "sacn", SETUP_HAS(CONFIG_NODE_SACN));
    cJSON_AddBoolToObject(f, "rdm", SETUP_HAS(CONFIG_NODE_RDM));
    cJSON_AddBoolToObject(f, "scenes", SETUP_HAS(CONFIG_NODE_SCENES));
    cJSON_AddBoolToObject(f, "dmx_tools", SETUP_HAS(CONFIG_NODE_DMX_TOOLS));
    cJSON_AddBoolToObject(f, "tester", SETUP_HAS(CONFIG_NODE_TESTER));
    cJSON_AddBoolToObject(f, "web_log", SETUP_HAS(CONFIG_NODE_WEB_LOG));
    cJSON_AddBoolToObject(f, "mdns", SETUP_HAS(CONFIG_NODE_MDNS));

    const config_t *c = config_get();
    cJSON *t = cJSON_AddObjectToObject(r, "tester");
    cJSON_AddBoolToObject(t, "enabled", dmx_tester_enabled());
    cJSON_AddBoolToObject(t, "next_boot", c && c->tester.enabled);

    add_network(r);
    return r;
}

static cJSON *cmd_get(void)
{
    cJSON *cfg = config_to_public_json(config_get());
    if (!cfg) return error_reply("Out of memory");
    cJSON *r = ok_reply();
    cJSON_AddItemToObject(r, "config", cfg);
    return r;
}

static cJSON *cmd_set(const cJSON *req)
{
    const cJSON *doc = cJSON_GetObjectItemCaseSensitive(req, "config");
    if (!cJSON_IsObject(doc)) return error_reply("\"config\" must be an object");
    const char *problem = config_update_problem(doc);
    if (problem) return error_reply(problem);

    char *s = cJSON_PrintUnformatted(doc);
    if (!s) return error_reply("Out of memory");
    esp_err_t err = config_from_json(s);
    cJSON_free(s);
    if (err != ESP_OK) return error_reply("Invalid settings");
    if (config_save() != ESP_OK) return error_reply("Could not save the settings");

    config_notify_changed();
    if (cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(doc, "network"))) {
        network_supervisor_reconfigure();
    }
    ESP_LOGI(TAG, "Settings changed over USB");

    const config_t *c = config_get();
    cJSON *r = ok_reply();
    /* Switching tester mode on or off takes a restart */
    cJSON_AddBoolToObject(r, "restart_needed", c && c->tester.enabled != dmx_tester_enabled());
    return r;
}

/* {"password":"..."} sets the admin password (sign-in on);
 * {"no_password":true} turns sign-in off */
static cJSON *cmd_password(const cJSON *req)
{
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "no_password"))) {
        if (config_set_no_login() != ESP_OK) return error_reply("Could not save the setting");
        ESP_LOGW(TAG, "Web UI sign-in turned off over USB");
        return ok_reply();
    }
    const cJSON *pw = cJSON_GetObjectItemCaseSensitive(req, "password");
    if (!cJSON_IsString(pw)) return error_reply("\"password\" missing");
    size_t n = strlen(pw->valuestring);
    if (n < 8 || n > 63) return error_reply("The admin password must be 8-63 characters");
    if (config_set_admin_password(pw->valuestring) != ESP_OK) {
        return error_reply("Could not save the password");
    }
    ESP_LOGI(TAG, "Admin password set over USB");
    return ok_reply();
}

static cJSON *cmd_scan(void)
{
    wifi_ap_record_t *recs = calloc(SCAN_MAX, sizeof(*recs));
    if (!recs) return error_reply("Out of memory");
    uint16_t n = network_wifi_scan_cached(recs, SCAN_MAX, 8000);
    cJSON *r = ok_reply();
    cJSON *list = cJSON_AddArrayToObject(r, "networks");
    for (uint16_t i = 0; list && i < n; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid[0]) continue;
        bool seen = false;   /* strongest entry per name only (sorted by the scan) */
        for (uint16_t j = 0; j < i && !seen; j++) {
            seen = strcmp((const char *)recs[j].ssid, ssid) == 0;
        }
        if (seen) continue;
        wifi_auth_mode_t auth = recs[i].authmode;
        cJSON *o = cJSON_CreateObject();
        if (!o) break;
        cJSON_AddStringToObject(o, "ssid", ssid);
        cJSON_AddNumberToObject(o, "rssi", recs[i].rssi);
        cJSON_AddNumberToObject(o, "channel", recs[i].primary);
        cJSON_AddBoolToObject(o, "secure", auth != WIFI_AUTH_OPEN && auth != WIFI_AUTH_OWE);
        cJSON_AddBoolToObject(o, "enterprise", auth == WIFI_AUTH_ENTERPRISE ||
                                               auth == WIFI_AUTH_WPA3_ENT_192 ||
                                               auth == WIFI_AUTH_WPA_ENTERPRISE);
        cJSON_AddBoolToObject(o, "wep", auth == WIFI_AUTH_WEP);
        cJSON_AddItemToArray(list, o);
    }
    free(recs);
    return r;
}

static cJSON *cmd_connect(const cJSON *req)
{
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(req, "ssid");
    if (!cJSON_IsString(ssid) || !ssid->valuestring[0]) return error_reply("\"ssid\" missing");
    const config_t *c = config_get();
    bool saved = false;
    for (int i = 0; c && i < c->network.wifi_profile_count && i < CONFIG_WIFI_PROFILES_MAX; i++) {
        saved |= strcmp(c->network.wifi_profiles[i].ssid, ssid->valuestring) == 0;
    }
    if (!saved) return error_reply("That network is not saved");
    esp_err_t err = network_supervisor_connect_now(ssid->valuestring);
    if (err != ESP_OK) return error_reply(esp_err_to_name(err));
    return ok_reply();
}

static void handle_line(const char *line)
{
    if (strncmp(line, PREFIX, strlen(PREFIX)) != 0) return;
    cJSON *req = cJSON_Parse(line + strlen(PREFIX));
    cJSON *reply;
    bool restart = false;
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(req, "cmd");
    if (!req) {
        reply = error_reply("Invalid JSON");
    } else if (!cJSON_IsString(cmd)) {
        reply = error_reply("\"cmd\" missing");
    } else if (strcmp(cmd->valuestring, "info") == 0) {
        reply = cmd_info();
    } else if (strcmp(cmd->valuestring, "get") == 0) {
        reply = cmd_get();
    } else if (strcmp(cmd->valuestring, "set") == 0) {
        reply = cmd_set(req);
    } else if (strcmp(cmd->valuestring, "password") == 0) {
        reply = cmd_password(req);
    } else if (strcmp(cmd->valuestring, "scan") == 0) {
        reply = cmd_scan();
    } else if (strcmp(cmd->valuestring, "connect") == 0) {
        reply = cmd_connect(req);
    } else if (strcmp(cmd->valuestring, "restart") == 0) {
        reply = ok_reply();
        cJSON_AddBoolToObject(reply, "restarting", true);
        restart = true;
    } else {
        reply = error_reply("Unknown command");
    }

    const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
    if (reply && cJSON_IsNumber(id)) cJSON_AddNumberToObject(reply, "id", id->valuedouble);
    send_reply(reply);
    cJSON_Delete(reply);
    cJSON_Delete(req);

    if (restart) {
        ESP_LOGW(TAG, "Restart requested over USB");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

static void feed(line_t *l, const uint8_t *data, int n)
{
    for (int i = 0; i < n; i++) {
        char c = (char)data[i];
        if (c == '\n' || c == '\r') {
            if (l->len && !l->overflow) {
                l->buf[l->len] = '\0';
                handle_line(l->buf);
            }
            l->len = 0;
            l->overflow = false;
        } else if (l->len < SETUP_LINE_MAX - 1) {
            l->buf[l->len++] = c;
        } else {
            l->overflow = true;   /* drop the rest of an over-long line */
        }
    }
}

static void setup_task(void *arg)
{
    (void)arg;
    uint8_t chunk[128];
    (void)chunk;   /* unused if the console is on neither port */
    line_t uart_line = { .buf = s_uart_ok ? malloc(SETUP_LINE_MAX) : NULL };
    line_t usb_line = { .buf = s_usb_ok ? malloc(SETUP_LINE_MAX) : NULL };
    if (!uart_line.buf && !usb_line.buf) {
        ESP_LOGW(TAG, "Out of memory");
        vTaskDelete(NULL);
        return;
    }
    /* Wait this long for input on each port (shorter when there are two) */
    const TickType_t wait = pdMS_TO_TICKS((uart_line.buf && usb_line.buf) ? 20 : 100);

    for (;;) {
#if SETUP_UART
        if (uart_line.buf) {
            int n = uart_read_bytes(SETUP_UART_NUM, chunk, sizeof(chunk), wait);
            if (n > 0) feed(&uart_line, chunk, n);
        }
#endif
#if SETUP_USB
        if (usb_line.buf) {
            int n = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), wait);
            if (n > 0) feed(&usb_line, chunk, n);
        }
#endif
    }
}

esp_err_t serial_setup_start(void)
{
    if (s_started) return ESP_OK;

#if SETUP_UART
    if (!uart_is_driver_installed(SETUP_UART_NUM)) {
        /* No TX buffer: writes block like the default console does */
        esp_err_t err = uart_driver_install(SETUP_UART_NUM, 1024, 0, 0, NULL, 0);
        if (err == ESP_OK) {
            SETUP_UART_USE_DRIVER(SETUP_UART_NUM);
        } else {
            ESP_LOGW(TAG, "UART%d: %s", SETUP_UART_NUM, esp_err_to_name(err));
        }
    }
    s_uart_ok = uart_is_driver_installed(SETUP_UART_NUM);
#endif

#if SETUP_USB
    usb_serial_jtag_driver_config_t ucfg = {
        .tx_buffer_size = 1024,
        .rx_buffer_size = 1024,
    };
    esp_err_t uerr = usb_serial_jtag_driver_install(&ucfg);
    if (uerr == ESP_OK) {
        /* The log keeps going out; with no computer reading, output is dropped
         * after one 50 ms wait instead of blocking */
        SETUP_USB_USE_DRIVER();
        s_usb_ok = true;
    } else {
        ESP_LOGW(TAG, "USB serial: %s", esp_err_to_name(uerr));
    }
#endif

    if (!s_uart_ok && !s_usb_ok) {
        ESP_LOGW(TAG, "No console port to listen on");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (xTaskCreate(setup_task, "serial_setup", 6144, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    ESP_LOGI(TAG, "USB setup ready (%s%s%s)", s_uart_ok ? "UART" : "",
             (s_uart_ok && s_usb_ok) ? " and " : "", s_usb_ok ? "USB" : "");
    return ESP_OK;
}
