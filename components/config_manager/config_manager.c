#include "config_manager.h"
#include "storage_manager.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "cJSON.h"
#include "mbedtls/pkcs5.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "config";
static const char *CONFIG_FILE = "config.json";
static const char *CONFIG_BACKUP = "config.bak";     /* the file before the last save */
static const char *CONFIG_DAMAGED = "config.bad";    /* a file that failed to load */

#define CONFIG_FILE_MAX 8192

/* PBKDF2 work factor for new passwords. Verification of a known-good
 * credential is cached by the web server, so only logins pay this. */
#define ADMIN_PBKDF2_ITERATIONS 4096

// Global configuration
static config_t g_config;

/* g_config is changed from the web server, the USB setup and the Art-Net task:
 * s_lock (recursive) guards every read-modify-write of it. Saves are ordered
 * by s_save_lock: the document is made under s_lock and numbered, then written
 * under s_save_lock alone, and an older document never overwrites a newer one.
 * Whoever holds s_save_lock never takes s_lock, so holding s_lock across a
 * config_save() is fine. */
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_save_lock;
static uint32_t s_save_gen;       /* last document made (under s_lock) */
static uint32_t s_written_gen;    /* last document written (under s_save_lock) */

static config_load_result_t s_load_result = CONFIG_LOAD_NEW;
static bool s_notice_cleared;

static esp_err_t parse_json(const char *json_str, bool from_file);

static config_change_callback_t s_change_cb;
static void *s_change_ctx;

// ============================================================================
// Helpers
// ============================================================================

/* Copy a string into a fixed buffer, always terminated */
static void copy_str(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    if (!src) src = "";
    size_t n = strnlen(src, dst_size - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Copy a JSON string member if present and of string type */
static bool json_get_str(const cJSON *obj, const char *key, char *dst, size_t dst_size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        copy_str(dst, dst_size, item->valuestring);
        return true;
    }
    return false;
}

/* Read a JSON number member if present, clamped to [min, max] */
static bool json_get_int(const cJSON *obj, const char *key, int min, int max, int *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        double v = item->valuedouble;
        if (v < min) v = min;
        if (v > max) v = max;
        *out = (int)v;
        return true;
    }
    return false;
}

/* A whole number 1..CONFIG_DMX_PORTS, else 0 (range checked before the
 * conversion, so huge numbers are safe) */
static int port_number(const cJSON *item)
{
    if (!cJSON_IsNumber(item)) return 0;
    double v = item->valuedouble;
    if (!(v >= 1 && v <= CONFIG_DMX_PORTS) || v != (int)v) return 0;
    return (int)v;
}

static bool json_get_bool(const cJSON *obj, const char *key, bool *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(item)) {
        *out = cJSON_IsTrue(item);
        return true;
    }
    if (cJSON_IsNumber(item)) {   /* accept 0/1 too */
        *out = item->valueint != 0;
        return true;
    }
    return false;
}

static void default_names(char *ssid, size_t ssid_size, char *short_name, size_t short_size)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (ssid) snprintf(ssid, ssid_size, "ESP-NODE-%02X%02X", mac[4], mac[5]);
    if (short_name) snprintf(short_name, short_size, "ESP-NODE-%02X%02X", mac[4], mac[5]);
}

static void port_defaults(port_config_t *p, uint16_t artnet, uint16_t sacn)
{
    p->mode = DMX_MODE_OUTPUT;
    p->universe_primary = artnet;
    p->universe_secondary = -1;
    p->universe_offset = 0;
    p->sacn_universe = sacn;
    p->protocol_mode = PROTOCOL_MERGE_BOTH;
    p->merge_mode = MERGE_MODE_HTP;
    p->rdm_enabled = true;
    p->out_rate_hz = CONFIG_DMX_RATE_DEFAULT;
    p->break_us = CONFIG_DMX_BREAK_DEFAULT;
    p->mab_us = CONFIG_DMX_MAB_DEFAULT;
    p->out_slots = CONFIG_DMX_SLOTS_MAX;
    p->startup_scene = 0;
    p->failsafe_scene = 0;
    p->loss_behavior = LOSS_AS_NODE;
    p->delta_output = false;
    memset(p->rules, 0, sizeof(p->rules));
    p->rule_count = 0;
    p->local_inputs = 0;
    p->local_only = false;
    p->send_to = SEND_TO_ALL;
    p->send_count = 0;
    memset(p->send_list, 0, sizeof(p->send_list));
}

/* Port n defaults: Art-Net n-1 (0:0:n-1), sACN n */
#define DEF_ARTNET(i) ((uint16_t)(i))
#define DEF_SACN(i)   ((uint16_t)((i) + 1))

static void tester_defaults(tester_port_config_t *t, int index)
{
    t->role = (index % 2 == 0) ? TESTER_ROLE_SEND : TESTER_ROLE_RECEIVE;
    t->rate_hz = CONFIG_DMX_RATE_DEFAULT;
    t->break_us = CONFIG_DMX_BREAK_DEFAULT;
    t->mab_us = CONFIG_DMX_MAB_DEFAULT;
    t->slots = CONFIG_DMX_SLOTS_MAX;
}

// Default configuration
static void config_set_defaults_into(config_t *c)
{
    memset(c, 0, sizeof(*c));

    // Network defaults
    c->network.mode = NET_MODE_AUTO;
    c->network.ap_always = false;
    c->network.eth_use_static_ip = false;
    copy_str(c->network.eth_static_ip, sizeof(c->network.eth_static_ip), "192.168.1.100");
    copy_str(c->network.eth_gateway, sizeof(c->network.eth_gateway), "192.168.1.1");
    copy_str(c->network.eth_netmask, sizeof(c->network.eth_netmask), "255.255.255.0");
    c->network.wifi_profile_count = 0;
    copy_str(c->network.ap_password, sizeof(c->network.ap_password), CONFIG_DEFAULT_AP_PASSWORD);
    c->network.ap_channel = 1;
    c->network.ap_captive = true;

    /* Common console convention: "Universe 1" = Art-Net 0:0:0 = sACN 1 */
    for (int i = 0; i < CONFIG_DMX_PORTS; i++) {
        port_defaults(&c->ports[i], DEF_ARTNET(i), DEF_SACN(i));
    }

    c->merge.timeout_seconds = 3;
    c->merge.loss_behavior = LOSS_HOLD_LAST_LOOK;

    default_names(c->network.ap_ssid, sizeof(c->network.ap_ssid),
                  c->node_info.short_name, sizeof(c->node_info.short_name));
    copy_str(c->node_info.long_name, sizeof(c->node_info.long_name),
             "ESP-NODE-2RDM Art-Net/sACN to DMX512/RDM Node");
    c->node_info.sacn_priority = 100;
    c->node_info.allow_ip_prog = true;

    /* Tester: odd ports send, even ports receive, so a cable from port 1
     * to port 2 is a loopback test straight away */
    c->tester.enabled = false;
    for (int i = 0; i < CONFIG_DMX_PORTS; i++) {
        tester_defaults(&c->tester.ports[i], i);
    }
}

#define CLAMP_FIELD(field, lo, hi, def) do {                              \
        if ((int)(field) < (int)(lo) || (int)(field) > (int)(hi)) {      \
            ESP_LOGW(TAG, #field " = %d out of range, using %d",          \
                     (int)(field), (int)(def));                           \
            (field) = (def);                                              \
            changed = true;                                               \
        }                                                                 \
    } while (0)

static const char *const RULE_NAMES[] = { [PORT_RULE_PATCH] = "patch",
                                           [PORT_RULE_LIMIT] = "limit",
                                           [PORT_RULE_PARK] = "park" };

const char *config_port_rule_name(uint8_t type)
{
    return (type >= PORT_RULE_FIRST && type <= PORT_RULE_LAST) ? RULE_NAMES[type] : NULL;
}

const char *config_port_rule_problem(const port_rule_t *r)
{
    if (!r || !config_port_rule_name(r->type)) return "Port rule: unknown type (use patch, limit or park)";
    if (r->channel < 1 || r->channel > 512) return "Port rule: the channel must be 1-512";
    if (r->count < 1 || r->channel + r->count - 1 > 512) return "Port rule: the channels must stay within 1-512";
    if (r->type == PORT_RULE_PATCH && (r->from < 1 || r->from + r->count - 1 > 512)) {
        return "Port rule: the channels a patch reads must stay within 1-512";
    }
    return NULL;
}

/* Drops unusable rules (keeping the order of the rest) and clears unused
 * slots, so equal rule lists compare equal byte for byte */
static bool sanitize_rules(port_config_t *p)
{
    bool changed = false;
    if (p->rule_count > CONFIG_PORT_RULES_MAX) {
        p->rule_count = CONFIG_PORT_RULES_MAX;
        changed = true;
    }
    uint8_t n = 0;
    for (uint8_t i = 0; i < p->rule_count; i++) {
        port_rule_t r = p->rules[i];
        if (r.type == PORT_RULE_PATCH) r.level = 0;   /* the JSON keeps only what each type uses */
        else r.from = 0;
        if (config_port_rule_problem(&r)) {
            changed = true;
            continue;
        }
        if (memcmp(&r, &p->rules[i], sizeof(r)) != 0) changed = true;
        p->rules[n++] = r;
    }
    if (n != p->rule_count) changed = true;
    p->rule_count = n;
    for (uint8_t i = n; i < CONFIG_PORT_RULES_MAX; i++) {
        static const port_rule_t zero;
        if (memcmp(&p->rules[i], &zero, sizeof(zero)) != 0) changed = true;
        memset(&p->rules[i], 0, sizeof(p->rules[i]));
    }
    return changed;
}

static const char *const k_send_to_names[] = {"all", "nodes", "list"};

const char *config_send_to_name(uint8_t send_to)
{
    return send_to <= SEND_TO_LAST ? k_send_to_names[send_to] : NULL;
}

static int send_to_from_name(const char *name)
{
    for (int i = 0; name && i <= SEND_TO_LAST; i++) {
        if (strcmp(name, k_send_to_names[i]) == 0) return i;
    }
    return -1;
}

bool config_parse_ipv4(const char *s, uint32_t *out)
{
    if (!s) return false;
    uint8_t b[4];
    for (int i = 0; i < 4; i++) {
        if (!isdigit((unsigned char)*s)) return false;
        int v = 0, digits = 0;
        while (isdigit((unsigned char)*s)) {
            v = v * 10 + (*s++ - '0');
            if (++digits > 3 || v > 255) return false;
        }
        b[i] = (uint8_t)v;
        if (i < 3 && *s++ != '.') return false;
    }
    if (*s) return false;
    memcpy(out, b, 4);   /* the bytes in wire order = network byte order */
    return true;
}

/* A usable unicast destination: not 0.0.0.0, not multicast or 255.255.255.255 */
static bool send_ip_ok(uint32_t ip)
{
    const uint8_t *b = (const uint8_t *)&ip;
    return ip != 0 && b[0] < 224;
}

static bool sanitize_send(port_config_t *p)
{
    bool changed = false;
    if (p->send_to > SEND_TO_LAST) {
        p->send_to = SEND_TO_ALL;
        changed = true;
    }
    if (p->send_count > CONFIG_SEND_LIST_MAX) {
        p->send_count = CONFIG_SEND_LIST_MAX;
        changed = true;
    }
    uint8_t n = 0;
    for (uint8_t i = 0; i < p->send_count; i++) {
        if (send_ip_ok(p->send_list[i])) p->send_list[n++] = p->send_list[i];
    }
    if (n != p->send_count) changed = true;
    p->send_count = n;
    for (uint8_t i = n; i < CONFIG_SEND_LIST_MAX; i++) {
        if (p->send_list[i]) changed = true;
        p->send_list[i] = 0;
    }
    if (p->send_to == SEND_TO_LIST && n == 0) {
        p->send_to = SEND_TO_ALL;   /* nowhere to send */
        changed = true;
    }
    return changed;
}

static bool sanitize_port(port_config_t *p, uint16_t def_artnet, uint16_t def_sacn)
{
    bool changed = sanitize_rules(p);
    changed |= sanitize_send(p);
    CLAMP_FIELD(p->mode, DMX_MODE_DISABLED, DMX_MODE_RDM_RESPONDER, DMX_MODE_OUTPUT);
    CLAMP_FIELD(p->universe_primary, 0, CONFIG_ARTNET_UNIVERSE_MAX, def_artnet);
    CLAMP_FIELD(p->universe_secondary, -1, CONFIG_ARTNET_UNIVERSE_MAX, -1);
    CLAMP_FIELD(p->sacn_universe, CONFIG_SACN_UNIVERSE_MIN, CONFIG_SACN_UNIVERSE_MAX, def_sacn);
    CLAMP_FIELD(p->protocol_mode, PROTOCOL_ARTNET_ONLY, PROTOCOL_MERGE_BOTH, PROTOCOL_MERGE_BOTH);
    CLAMP_FIELD(p->merge_mode, MERGE_MODE_HTP, MERGE_MODE_DISABLE, MERGE_MODE_HTP);
    CLAMP_FIELD(p->out_rate_hz, CONFIG_DMX_RATE_MIN, CONFIG_DMX_RATE_MAX, CONFIG_DMX_RATE_DEFAULT);
    CLAMP_FIELD(p->break_us, CONFIG_DMX_BREAK_MIN, CONFIG_DMX_BREAK_MAX, CONFIG_DMX_BREAK_DEFAULT);
    CLAMP_FIELD(p->mab_us, CONFIG_DMX_MAB_MIN, CONFIG_DMX_MAB_MAX, CONFIG_DMX_MAB_DEFAULT);
    CLAMP_FIELD(p->out_slots, CONFIG_DMX_SLOTS_MIN, CONFIG_DMX_SLOTS_MAX, CONFIG_DMX_SLOTS_MAX);
    CLAMP_FIELD(p->loss_behavior, LOSS_AS_NODE, LOSS_LAST, LOSS_AS_NODE);

    /* Settings for features this firmware was built without (menuconfig:
     * ESP-NODE > Features) fall back to what the node can do */
#if !CONFIG_NODE_RDM
    if (p->mode == DMX_MODE_RDM_MASTER)    { p->mode = DMX_MODE_OUTPUT; changed = true; }
    if (p->mode == DMX_MODE_RDM_RESPONDER) { p->mode = DMX_MODE_INPUT;  changed = true; }
    if (p->rdm_enabled)                    { p->rdm_enabled = false;    changed = true; }
#endif
#if !CONFIG_NODE_SCENES
    if (p->startup_scene || p->failsafe_scene) {
        p->startup_scene = p->failsafe_scene = 0;
        changed = true;
    }
    if (p->loss_behavior == LOSS_SCENE) {
        p->loss_behavior = LOSS_BLACKOUT;   /* no failsafe scene to show */
        changed = true;
    }
#endif
#if !CONFIG_NODE_SACN
    if (p->protocol_mode != PROTOCOL_ARTNET_ONLY) {
        p->protocol_mode = PROTOCOL_ARTNET_ONLY;
        changed = true;
    }
#endif
    return changed;
}

static bool sanitize_tester_port(tester_port_config_t *t)
{
    bool changed = false;
    CLAMP_FIELD(t->role, TESTER_ROLE_OFF, TESTER_ROLE_LAST, TESTER_ROLE_OFF);
    CLAMP_FIELD(t->rate_hz, CONFIG_DMX_RATE_MIN, CONFIG_DMX_RATE_MAX, CONFIG_DMX_RATE_DEFAULT);
    CLAMP_FIELD(t->break_us, CONFIG_DMX_BREAK_MIN, CONFIG_DMX_BREAK_MAX, CONFIG_DMX_BREAK_DEFAULT);
    CLAMP_FIELD(t->mab_us, CONFIG_DMX_MAB_MIN, CONFIG_DMX_MAB_MAX, CONFIG_DMX_MAB_DEFAULT);
    CLAMP_FIELD(t->slots, CONFIG_DMX_SLOTS_MIN, CONFIG_DMX_SLOTS_MAX, CONFIG_DMX_SLOTS_MAX);
#if !CONFIG_NODE_RDM
    if (t->role == TESTER_ROLE_RDM) { t->role = TESTER_ROLE_SEND; changed = true; }
#endif
    return changed;
}

static const char *const TESTER_ROLE_NAMES[] = {"off", "send", "receive", "rdm"};

const char *config_tester_role_name(uint8_t role)
{
    return role <= TESTER_ROLE_LAST ? TESTER_ROLE_NAMES[role] : "off";
}

int config_tester_role_from_name(const char *name)
{
    for (int i = 0; name && i <= TESTER_ROLE_LAST; i++) {
        if (strcmp(name, TESTER_ROLE_NAMES[i]) == 0) return i;
    }
    return -1;
}

void config_tester_port_settings(const config_t *c, int port, port_config_t *out)
{
    const tester_port_config_t *t = &c->tester.ports[port - 1];
    *out = c->ports[port - 1];
    switch (t->role) {
    case TESTER_ROLE_SEND:    out->mode = DMX_MODE_OUTPUT;     break;
    case TESTER_ROLE_RECEIVE: out->mode = DMX_MODE_INPUT;      break;
    case TESTER_ROLE_RDM:     out->mode = DMX_MODE_RDM_MASTER; break;
    default:                  out->mode = DMX_MODE_DISABLED;   break;
    }
    out->rdm_enabled = t->role == TESTER_ROLE_RDM;
    out->out_rate_hz = t->rate_hz;
    out->break_us = t->break_us;
    out->mab_us = t->mab_us;
    out->out_slots = t->slots;
    out->startup_scene = 0;
    out->failsafe_scene = 0;
    out->loss_behavior = LOSS_AS_NODE;
    out->delta_output = false;
    out->local_inputs = 0;
    out->local_only = false;
    out->send_to = SEND_TO_ALL;
    out->send_count = 0;
}

static void terminate(char *s, size_t size)
{
    s[size - 1] = '\0';
}

static bool is_hex(const char *s, size_t len)
{
    if (strlen(s) != len) return false;
    for (size_t i = 0; i < len; i++) {
        if (!isxdigit((unsigned char)s[i])) return false;
    }
    return true;
}

bool config_sanitize(config_t *c)
{
    bool changed = false;

    terminate(c->network.eth_static_ip, sizeof(c->network.eth_static_ip));
    terminate(c->network.eth_gateway, sizeof(c->network.eth_gateway));
    terminate(c->network.eth_netmask, sizeof(c->network.eth_netmask));
    terminate(c->network.ap_ssid, sizeof(c->network.ap_ssid));
    terminate(c->network.ap_password, sizeof(c->network.ap_password));
    terminate(c->node_info.short_name, sizeof(c->node_info.short_name));
    terminate(c->node_info.long_name, sizeof(c->node_info.long_name));

    if (c->network.wifi_profile_count > CONFIG_WIFI_PROFILES_MAX) {
        c->network.wifi_profile_count = CONFIG_WIFI_PROFILES_MAX;
        changed = true;
    }
    for (int i = 0; i < CONFIG_WIFI_PROFILES_MAX; i++) {
        config_wifi_profile_t *wp = &c->network.wifi_profiles[i];
        terminate(wp->ssid, sizeof(wp->ssid));
        terminate(wp->password, sizeof(wp->password));
        terminate(wp->static_ip, sizeof(wp->static_ip));
        terminate(wp->gateway, sizeof(wp->gateway));
        terminate(wp->netmask, sizeof(wp->netmask));
    }
    /* Drop profiles with an empty SSID */
    int n = 0;
    for (int i = 0; i < c->network.wifi_profile_count; i++) {
        if (c->network.wifi_profiles[i].ssid[0] != '\0') {
            if (n != i) c->network.wifi_profiles[n] = c->network.wifi_profiles[i];
            n++;
        } else {
            changed = true;
        }
    }
    c->network.wifi_profile_count = n;

    CLAMP_FIELD(c->network.mode, NET_MODE_AUTO, NET_MODE_LAST, NET_MODE_AUTO);
#if !CONFIG_NODE_ETHERNET
    if (c->network.mode == NET_MODE_ETHERNET) {
        c->network.mode = NET_MODE_AUTO;   /* no Ethernet built in: Wi-Fi, else own AP */
        changed = true;
    }
#endif
    CLAMP_FIELD(c->network.ap_channel, 1, 13, 1);
    if (c->network.ap_ssid[0] == '\0') {
        default_names(c->network.ap_ssid, sizeof(c->network.ap_ssid), NULL, 0);
        changed = true;
    }
    size_t pw = strlen(c->network.ap_password);
    if (pw > 0 && pw < 8) {
        ESP_LOGW(TAG, "AP password shorter than 8 characters (WPA2 minimum), using default");
        copy_str(c->network.ap_password, sizeof(c->network.ap_password), CONFIG_DEFAULT_AP_PASSWORD);
        changed = true;
    }

    for (int i = 0; i < CONFIG_DMX_PORTS; i++) {
        port_config_t *p = &c->ports[i];
        changed |= sanitize_port(p, DEF_ARTNET(i), DEF_SACN(i));
        changed |= sanitize_tester_port(&c->tester.ports[i]);
        /* Routing: other ports on the board, never the port itself, and
         * the network stays on while no input is routed */
        uint8_t inputs = p->local_inputs & CONFIG_PORT_BITS & (uint8_t)~(1u << i);
        if (inputs != p->local_inputs) {
            p->local_inputs = inputs;
            changed = true;
        }
        if (p->local_only && !p->local_inputs) {
            p->local_only = false;
            changed = true;
        }
    }
#if !CONFIG_NODE_TESTER
    if (c->tester.enabled) {
        c->tester.enabled = false;   /* built without the tester */
        changed = true;
    }
#endif

    CLAMP_FIELD(c->merge.timeout_seconds, 1, 60, 3);
    CLAMP_FIELD(c->merge.loss_behavior, LOSS_HOLD_LAST_LOOK, LOSS_LAST, LOSS_HOLD_LAST_LOOK);
#if !CONFIG_NODE_SCENES
    if (c->merge.loss_behavior == LOSS_SCENE) {
        c->merge.loss_behavior = LOSS_BLACKOUT;   /* no failsafe scene to show */
        changed = true;
    }
#endif
    CLAMP_FIELD(c->node_info.sacn_priority, 0, 200, 100);

    if (c->node_info.short_name[0] == '\0') {
        default_names(NULL, 0, c->node_info.short_name, sizeof(c->node_info.short_name));
        changed = true;
    }

    /* A damaged credential would lock everyone out: drop it, which puts the
     * web UI back into first-run setup. */
    terminate(c->security.admin_hash, sizeof(c->security.admin_hash));
    terminate(c->security.admin_salt, sizeof(c->security.admin_salt));
    if (c->security.admin_hash[0] != '\0' &&
        (!is_hex(c->security.admin_hash, CONFIG_ADMIN_HASH_HEX) ||
         !is_hex(c->security.admin_salt, CONFIG_ADMIN_SALT_HEX) ||
         c->security.admin_iterations < 1000 || c->security.admin_iterations > 100000)) {
        ESP_LOGW(TAG, "Stored admin credential is invalid, web setup required");
        memset(&c->security, 0, sizeof(c->security));
        changed = true;
    }
    if (c->security.admin_hash[0] != '\0' && c->security.no_login) {
        c->security.no_login = false;   /* a stored password wins */
        changed = true;
    }
    return changed;
}

void config_lock(void)
{
    if (s_lock) xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

void config_unlock(void)
{
    if (s_lock) xSemaphoreGiveRecursive(s_lock);
}

config_t *config_snapshot(void)
{
    config_t *c = malloc(sizeof(config_t));
    if (!c) return NULL;
    config_lock();
    config_sanitize(&g_config);
    memcpy(c, &g_config, sizeof(config_t));
    config_unlock();
    return c;
}

esp_err_t config_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_save_lock) s_save_lock = xSemaphoreCreateMutex();
    if (!s_lock || !s_save_lock) return ESP_ERR_NO_MEM;
    config_lock();
    config_set_defaults_into(&g_config);
    config_unlock();
    ESP_LOGI(TAG, "Configuration initialized with defaults");
    return ESP_OK;
}

/* Read and apply one settings file. ESP_ERR_NOT_FOUND if it isn't there. */
static esp_err_t load_file(const char *name)
{
    if (!storage_file_exists(name)) return ESP_ERR_NOT_FOUND;

    char *buffer = malloc(CONFIG_FILE_MAX);
    if (!buffer) return ESP_ERR_NO_MEM;
    size_t size = CONFIG_FILE_MAX;
    esp_err_t ret = storage_read_file(name, buffer, &size);
    if (ret == ESP_OK) {
        ret = parse_json(buffer, true);
    }
    free(buffer);
    return ret;
}

/*
 * config.json, or if that is damaged or missing the copy from before the last
 * save (config.bak), or factory settings. A damaged config.json is kept as
 * config.bad for a look later and never becomes the backup.
 */
esp_err_t config_load(void)
{
    config_lock();
    esp_err_t ret = load_file(CONFIG_FILE);
    if (ret == ESP_OK) {
        s_load_result = CONFIG_LOAD_OK;
        ESP_LOGI(TAG, "Config loaded from storage");
        config_unlock();
        return ESP_OK;
    }
    if (ret == ESP_ERR_NO_MEM) {
        config_unlock();
        return ret;    /* nothing wrong with the file: don't touch it */
    }

    bool damaged = ret != ESP_ERR_NOT_FOUND;
    if (damaged) {
        ESP_LOGE(TAG, "Config file is damaged, keeping it as %s", CONFIG_DAMAGED);
        config_set_defaults_into(&g_config);   /* parse_json() leaves it alone on failure */
        if (storage_rename_file(CONFIG_FILE, CONFIG_DAMAGED) != ESP_OK) {
            storage_delete_file(CONFIG_FILE);
        }
    }

    ret = load_file(CONFIG_BACKUP);
    if (ret == ESP_OK) {
        ESP_LOGW(TAG, "Config restored from the copy before the last save");
        s_load_result = CONFIG_LOAD_FROM_BACKUP;
    } else {
        config_set_defaults_into(&g_config);
        if (damaged || ret != ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "No readable settings, using factory settings");
            s_load_result = CONFIG_LOAD_RESET;
        } else {
            ESP_LOGW(TAG, "Config file not found, using defaults");
            s_load_result = CONFIG_LOAD_NEW;
        }
    }
    /* config.json is gone now, so this save leaves config.bak as it is */
    ret = config_save();
    config_unlock();
    return ret;
}

esp_err_t config_save(void)
{
    char *json_str = NULL;
    config_lock();
    esp_err_t ret = config_to_json(&json_str);
    uint32_t gen = ++s_save_gen;
    config_unlock();
    if (ret != ESP_OK || json_str == NULL) {
        free(json_str);
        ESP_LOGE(TAG, "Failed to serialize config");
        return ESP_FAIL;
    }

    xSemaphoreTake(s_save_lock, portMAX_DELAY);
    if ((int32_t)(gen - s_written_gen) <= 0) {
        ret = ESP_OK;    /* a later save already wrote these settings */
    } else {
        ret = storage_replace_file(CONFIG_FILE, CONFIG_BACKUP, json_str, strlen(json_str));
        if (ret == ESP_OK) s_written_gen = gen;
    }
    xSemaphoreGive(s_save_lock);
    free(json_str);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Config saved to storage");
    }
    return ret;
}

config_t* config_get(void)
{
    return &g_config;
}

esp_err_t config_reset_to_defaults(void)
{
    config_lock();
    config_set_defaults_into(&g_config);
    esp_err_t err = config_save();
    config_unlock();
    /* The old settings (passwords included) go too */
    if (storage_file_exists(CONFIG_BACKUP)) storage_delete_file(CONFIG_BACKUP);
    if (storage_file_exists(CONFIG_DAMAGED)) storage_delete_file(CONFIG_DAMAGED);
    return err;
}

config_load_result_t config_load_result(void)
{
    return s_load_result;
}

const char *config_boot_notice(void)
{
    if (s_notice_cleared) return NULL;
    if (storage_was_formatted()) {
        return "The settings storage could not be read at start-up and was erased: "
               "the node is on factory settings and its scenes are gone.";
    }
    switch (s_load_result) {
        case CONFIG_LOAD_FROM_BACKUP:
            return "The settings file was damaged or missing at start-up, so the copy from "
                   "before the last change was used. Check the last change you made.";
        case CONFIG_LOAD_RESET:
            return "The settings file was damaged at start-up and there was no good copy: "
                   "the node is on factory settings.";
        default:
            return NULL;
    }
}

void config_boot_notice_clear(void)
{
    s_notice_cleared = true;
}

// ============================================================================
// Admin password
// ============================================================================

static void to_hex(const uint8_t *in, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0x0F];
    }
    out[2 * len] = '\0';
}

static esp_err_t derive(const char *password, const char *salt_hex, uint32_t iterations,
                        char out_hex[CONFIG_ADMIN_HASH_HEX + 1])
{
    uint8_t key[CONFIG_ADMIN_HASH_HEX / 2];
    int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
                                           (const unsigned char *)password, strlen(password),
                                           (const unsigned char *)salt_hex, strlen(salt_hex),
                                           iterations, sizeof(key), key);
    if (rc != 0) {
        ESP_LOGE(TAG, "PBKDF2 failed: -0x%04x", (unsigned)-rc);
        return ESP_FAIL;
    }
    to_hex(key, sizeof(key), out_hex);
    memset(key, 0, sizeof(key));
    return ESP_OK;
}

bool config_admin_password_set(void)
{
    return g_config.security.admin_hash[0] != '\0';
}

bool config_setup_done(void)
{
    return config_admin_password_set() || g_config.security.no_login;
}

bool config_login_required(void)
{
    return config_admin_password_set() && !g_config.security.no_login;
}

esp_err_t config_set_no_login(void)
{
    config_lock();
    memset(&g_config.security, 0, sizeof(g_config.security));
    g_config.security.no_login = true;
    config_unlock();
    return config_save();
}

esp_err_t config_set_admin_password(const char *password)
{
    if (!password) return ESP_ERR_INVALID_ARG;
    size_t len = strlen(password);
    if (len < 8 || len > 63) return ESP_ERR_INVALID_ARG;

    uint8_t salt[CONFIG_ADMIN_SALT_HEX / 2];
    esp_fill_random(salt, sizeof(salt));
    char salt_hex[CONFIG_ADMIN_SALT_HEX + 1];
    to_hex(salt, sizeof(salt), salt_hex);

    char hash_hex[CONFIG_ADMIN_HASH_HEX + 1];
    esp_err_t err = derive(password, salt_hex, ADMIN_PBKDF2_ITERATIONS, hash_hex);
    if (err != ESP_OK) return err;

    config_lock();   /* after the slow part */
    copy_str(g_config.security.admin_salt, sizeof(g_config.security.admin_salt), salt_hex);
    copy_str(g_config.security.admin_hash, sizeof(g_config.security.admin_hash), hash_hex);
    g_config.security.admin_iterations = ADMIN_PBKDF2_ITERATIONS;
    g_config.security.no_login = false;
    config_unlock();
    return config_save();
}

bool config_check_admin_password(const char *password)
{
    if (!password) return false;
    /* A copy, so the slow derive runs without the lock */
    char salt[sizeof(g_config.security.admin_salt)];
    char stored[sizeof(g_config.security.admin_hash)];
    config_lock();
    bool set = config_admin_password_set();
    memcpy(salt, g_config.security.admin_salt, sizeof(salt));
    memcpy(stored, g_config.security.admin_hash, sizeof(stored));
    uint32_t iterations = g_config.security.admin_iterations;
    config_unlock();
    if (!set) return false;

    char hash_hex[CONFIG_ADMIN_HASH_HEX + 1];
    if (derive(password, salt, iterations, hash_hex) != ESP_OK) {
        return false;
    }
    uint8_t diff = 0;
    for (size_t i = 0; i < CONFIG_ADMIN_HASH_HEX; i++) {
        diff |= (uint8_t)(hash_hex[i] ^ stored[i]);
    }
    return diff == 0;
}

void config_register_change_callback(config_change_callback_t cb, void *user_data)
{
    s_change_ctx = user_data;
    s_change_cb = cb;
}

void config_notify_changed(void)
{
    config_change_callback_t cb = s_change_cb;
    if (cb) {
        cb(s_change_ctx);
    }
}

// ============================================================================
// JSON
// ============================================================================

static cJSON *port_to_json(const port_config_t *p)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddNumberToObject(o, "mode", p->mode);
    cJSON_AddNumberToObject(o, "universe_primary", p->universe_primary);
    cJSON_AddNumberToObject(o, "universe_secondary", p->universe_secondary);
    cJSON_AddNumberToObject(o, "universe_offset", p->universe_offset);
    cJSON_AddNumberToObject(o, "sacn_universe", p->sacn_universe);
    cJSON_AddNumberToObject(o, "protocol_mode", p->protocol_mode);
    cJSON_AddNumberToObject(o, "merge_mode", p->merge_mode);
    cJSON_AddBoolToObject(o, "rdm_enabled", p->rdm_enabled);
    cJSON_AddNumberToObject(o, "out_rate_hz", p->out_rate_hz);
    cJSON_AddNumberToObject(o, "break_us", p->break_us);
    cJSON_AddNumberToObject(o, "mab_us", p->mab_us);
    cJSON_AddNumberToObject(o, "out_slots", p->out_slots);
    cJSON_AddNumberToObject(o, "startup_scene", p->startup_scene);
    cJSON_AddNumberToObject(o, "failsafe_scene", p->failsafe_scene);
    cJSON_AddNumberToObject(o, "loss_behavior", p->loss_behavior);
    cJSON_AddBoolToObject(o, "delta_output", p->delta_output);
    cJSON *rules = cJSON_AddArrayToObject(o, "rules");
    for (uint8_t i = 0; rules && i < p->rule_count && i < CONFIG_PORT_RULES_MAX; i++) {
        const port_rule_t *r = &p->rules[i];
        const char *name = config_port_rule_name(r->type);
        cJSON *ro = name ? cJSON_CreateObject() : NULL;
        if (!ro) continue;
        cJSON_AddStringToObject(ro, "type", name);
        cJSON_AddNumberToObject(ro, "channel", r->channel);
        cJSON_AddNumberToObject(ro, "count", r->count);
        if (r->type == PORT_RULE_PATCH) {
            cJSON_AddNumberToObject(ro, "from", r->from);
        } else {
            cJSON_AddNumberToObject(ro, "level", r->level);
        }
        cJSON_AddItemToArray(rules, ro);
    }
    cJSON *inputs = cJSON_AddArrayToObject(o, "local_inputs");
    for (int k = 1; inputs && k <= CONFIG_DMX_PORTS; k++) {
        if (p->local_inputs & (1u << (k - 1))) cJSON_AddItemToArray(inputs, cJSON_CreateNumber(k));
    }
    cJSON_AddBoolToObject(o, "local_only", p->local_only);
    cJSON_AddStringToObject(o, "send_to", config_send_to_name(p->send_to) ? config_send_to_name(p->send_to) : "all");
    cJSON *list = cJSON_AddArrayToObject(o, "send_list");
    for (uint8_t i = 0; list && i < p->send_count && i < CONFIG_SEND_LIST_MAX; i++) {
        const uint8_t *b = (const uint8_t *)&p->send_list[i];
        char ip[16];
        snprintf(ip, sizeof(ip), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        cJSON_AddItemToArray(list, cJSON_CreateString(ip));
    }
    return o;
}

cJSON *config_port_to_json(const port_config_t *p)
{
    return port_to_json(p);
}

static esp_err_t to_json(char **json_str)
{
    *json_str = NULL;
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_FAIL;
    }

    cJSON_AddNumberToObject(root, "version", 2);

    // Network
    cJSON *network = cJSON_AddObjectToObject(root, "network");
    if (network) {
        cJSON_AddNumberToObject(network, "mode", g_config.network.mode);
        cJSON_AddBoolToObject(network, "ap_always", g_config.network.ap_always);
        cJSON_AddBoolToObject(network, "eth_use_static_ip", g_config.network.eth_use_static_ip);
        cJSON_AddStringToObject(network, "eth_static_ip", g_config.network.eth_static_ip);
        cJSON_AddStringToObject(network, "eth_gateway", g_config.network.eth_gateway);
        cJSON_AddStringToObject(network, "eth_netmask", g_config.network.eth_netmask);
        cJSON_AddStringToObject(network, "ap_ssid", g_config.network.ap_ssid);
        cJSON_AddStringToObject(network, "ap_password", g_config.network.ap_password);
        cJSON_AddNumberToObject(network, "ap_channel", g_config.network.ap_channel);
        cJSON_AddBoolToObject(network, "ap_captive", g_config.network.ap_captive);

        cJSON *profiles = cJSON_AddArrayToObject(network, "wifi_profiles");
        for (int i = 0; profiles && i < g_config.network.wifi_profile_count; i++) {
            const config_wifi_profile_t *wp = &g_config.network.wifi_profiles[i];
            cJSON *profile = cJSON_CreateObject();
            if (!profile) break;
            cJSON_AddStringToObject(profile, "ssid", wp->ssid);
            cJSON_AddStringToObject(profile, "password", wp->password);
            cJSON_AddNumberToObject(profile, "priority", wp->priority);
            cJSON_AddBoolToObject(profile, "use_static_ip", wp->use_static_ip);
            cJSON_AddStringToObject(profile, "static_ip", wp->static_ip);
            cJSON_AddStringToObject(profile, "gateway", wp->gateway);
            cJSON_AddStringToObject(profile, "netmask", wp->netmask);
            cJSON_AddItemToArray(profiles, profile);
        }
    }

    for (int i = 0; i < CONFIG_DMX_PORTS; i++) {
        char key[8];
        snprintf(key, sizeof(key), "port%d", i + 1);
        cJSON *po = port_to_json(&g_config.ports[i]);
        if (po) cJSON_AddItemToObject(root, key, po);
    }

    cJSON *merge = cJSON_AddObjectToObject(root, "merge");
    if (merge) {
        cJSON_AddNumberToObject(merge, "timeout_seconds", g_config.merge.timeout_seconds);
        cJSON_AddNumberToObject(merge, "loss_behavior", g_config.merge.loss_behavior);
    }

    cJSON *node_info = cJSON_AddObjectToObject(root, "node_info");
    if (node_info) {
        cJSON_AddStringToObject(node_info, "short_name", g_config.node_info.short_name);
        cJSON_AddStringToObject(node_info, "long_name", g_config.node_info.long_name);
        cJSON_AddNumberToObject(node_info, "sacn_priority", g_config.node_info.sacn_priority);
        cJSON_AddBoolToObject(node_info, "allow_ip_prog", g_config.node_info.allow_ip_prog);
    }

    cJSON *tester = cJSON_AddObjectToObject(root, "tester");
    if (tester) {
        cJSON_AddBoolToObject(tester, "enabled", g_config.tester.enabled);
        cJSON *tports = cJSON_AddArrayToObject(tester, "ports");
        for (int i = 0; tports && i < CONFIG_DMX_PORTS; i++) {
            const tester_port_config_t *t = &g_config.tester.ports[i];
            cJSON *o = cJSON_CreateObject();
            if (!o) break;
            cJSON_AddStringToObject(o, "role", config_tester_role_name(t->role));
            cJSON_AddNumberToObject(o, "rate_hz", t->rate_hz);
            cJSON_AddNumberToObject(o, "break_us", t->break_us);
            cJSON_AddNumberToObject(o, "mab_us", t->mab_us);
            cJSON_AddNumberToObject(o, "slots", t->slots);
            cJSON_AddItemToArray(tports, o);
        }
    }

    if (g_config.security.admin_hash[0] != '\0') {
        cJSON *security = cJSON_AddObjectToObject(root, "security");
        if (security) {
            cJSON_AddStringToObject(security, "admin_hash", g_config.security.admin_hash);
            cJSON_AddStringToObject(security, "admin_salt", g_config.security.admin_salt);
            cJSON_AddNumberToObject(security, "admin_iterations", g_config.security.admin_iterations);
        }
    } else if (g_config.security.no_login) {
        cJSON *security = cJSON_AddObjectToObject(root, "security");
        if (security) cJSON_AddBoolToObject(security, "no_login", true);
    }

    *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return (*json_str != NULL) ? ESP_OK : ESP_FAIL;
}

esp_err_t config_to_json(char **json_str)
{
    config_lock();
    esp_err_t err = to_json(json_str);
    config_unlock();
    return err;
}

/* One rule from JSON. false if a field is missing or of the wrong type; the
 * values themselves are checked by config_port_rule_problem() */
static bool rule_from_json(const cJSON *o, port_rule_t *r)
{
    memset(r, 0, sizeof(*r));
    if (!cJSON_IsObject(o)) return false;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(o, "type");
    if (!cJSON_IsString(type)) return false;
    for (uint8_t t = PORT_RULE_FIRST; t <= PORT_RULE_LAST; t++) {
        if (strcmp(type->valuestring, RULE_NAMES[t]) == 0) r->type = t;
    }
    if (!r->type) return false;
    const char *keys[3] = { "channel", "count", r->type == PORT_RULE_PATCH ? "from" : "level" };
    double v[3];
    for (int k = 0; k < 3; k++) {
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(o, keys[k]);
        if (!cJSON_IsNumber(n)) return false;
        v[k] = n->valuedouble;
        if (v[k] < 0 || v[k] > 65535 || v[k] != (double)(int)v[k]) return false;
    }
    if (r->type != PORT_RULE_PATCH && v[2] > 255) return false;
    r->channel = (uint16_t)v[0];
    r->count = (uint16_t)v[1];
    if (r->type == PORT_RULE_PATCH) r->from = (uint16_t)v[2];
    else r->level = (uint8_t)v[2];
    return true;
}

static void port_from_json(const cJSON *obj, port_config_t *p, bool *had_sacn)
{
    int v;
    if (!cJSON_IsObject(obj)) return;
    if (json_get_int(obj, "mode", DMX_MODE_DISABLED, DMX_MODE_RDM_RESPONDER, &v)) p->mode = v;
    if (json_get_int(obj, "universe_primary", 0, CONFIG_ARTNET_UNIVERSE_MAX, &v)) p->universe_primary = v;
    if (json_get_int(obj, "universe_secondary", -1, CONFIG_ARTNET_UNIVERSE_MAX, &v)) p->universe_secondary = v;
    if (json_get_int(obj, "universe_offset", -32768, 32767, &v)) p->universe_offset = v;
    if (json_get_int(obj, "sacn_universe", CONFIG_SACN_UNIVERSE_MIN, CONFIG_SACN_UNIVERSE_MAX, &v)) {
        p->sacn_universe = v;
        if (had_sacn) *had_sacn = true;
    }
    if (json_get_int(obj, "protocol_mode", PROTOCOL_ARTNET_ONLY, PROTOCOL_MERGE_BOTH, &v)) p->protocol_mode = v;
    if (json_get_int(obj, "merge_mode", MERGE_MODE_HTP, MERGE_MODE_DISABLE, &v)) p->merge_mode = v;
    json_get_bool(obj, "rdm_enabled", &p->rdm_enabled);
    if (json_get_int(obj, "out_rate_hz", CONFIG_DMX_RATE_MIN, CONFIG_DMX_RATE_MAX, &v)) p->out_rate_hz = v;
    if (json_get_int(obj, "break_us", CONFIG_DMX_BREAK_MIN, CONFIG_DMX_BREAK_MAX, &v)) p->break_us = v;
    if (json_get_int(obj, "mab_us", CONFIG_DMX_MAB_MIN, CONFIG_DMX_MAB_MAX, &v)) p->mab_us = v;
    if (json_get_int(obj, "out_slots", CONFIG_DMX_SLOTS_MIN, CONFIG_DMX_SLOTS_MAX, &v)) p->out_slots = v;
    if (json_get_int(obj, "startup_scene", 0, 255, &v)) p->startup_scene = v;
    if (json_get_int(obj, "failsafe_scene", 0, 255, &v)) p->failsafe_scene = v;
    if (json_get_int(obj, "loss_behavior", LOSS_AS_NODE, LOSS_LAST, &v)) p->loss_behavior = (int8_t)v;
    json_get_bool(obj, "delta_output", &p->delta_output);
    /* "rules" replaces the whole list; bad entries are dropped */
    const cJSON *rules = cJSON_GetObjectItemCaseSensitive(obj, "rules");
    if (cJSON_IsArray(rules)) {
        memset(p->rules, 0, sizeof(p->rules));
        p->rule_count = 0;
        const cJSON *ro;
        cJSON_ArrayForEach(ro, rules) {
            port_rule_t r;
            if (p->rule_count >= CONFIG_PORT_RULES_MAX) break;
            if (rule_from_json(ro, &r) && !config_port_rule_problem(&r)) {
                p->rules[p->rule_count++] = r;
            }
        }
    }
    /* "local_inputs" replaces the whole set; numbers that are no port are dropped */
    const cJSON *inputs = cJSON_GetObjectItemCaseSensitive(obj, "local_inputs");
    if (cJSON_IsArray(inputs)) {
        p->local_inputs = 0;
        const cJSON *in;
        cJSON_ArrayForEach(in, inputs) {
            int port = port_number(in);
            if (port) p->local_inputs |= (uint8_t)(1u << (port - 1));
        }
    }
    json_get_bool(obj, "local_only", &p->local_only);
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(obj, "send_to");
    if (cJSON_IsString(st)) {
        int k = send_to_from_name(st->valuestring);
        if (k >= 0) p->send_to = (uint8_t)k;
    }
    /* "send_list" replaces the whole list; anything that isn't an address is dropped */
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(obj, "send_list");
    if (cJSON_IsArray(list)) {
        memset(p->send_list, 0, sizeof(p->send_list));
        p->send_count = 0;
        const cJSON *e;
        cJSON_ArrayForEach(e, list) {
            uint32_t ip;
            if (p->send_count >= CONFIG_SEND_LIST_MAX) break;
            if (cJSON_IsString(e) && config_parse_ipv4(e->valuestring, &ip) && send_ip_ok(ip)) {
                p->send_list[p->send_count++] = ip;
            }
        }
    }
}

/*
 * Applies a (possibly partial) JSON document on top of the current
 * configuration. Parsing happens into a copy, which is sanitised and then
 * swapped in, so a bad document never leaves a half-applied configuration.
 */
static esp_err_t parse_json(const char *json_str, bool from_file)
{
    if (!json_str) return ESP_ERR_INVALID_ARG;
    cJSON *root = cJSON_Parse(json_str);
    if (!cJSON_IsObject(root)) {
        ESP_LOGE(TAG, "JSON parse error");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    config_t *c = malloc(sizeof(config_t));
    if (!c) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    /* Held until the result is swapped in: a change made meanwhile by
     * another task would otherwise be overwritten with the old value */
    config_lock();
    memcpy(c, &g_config, sizeof(config_t));
    int v;

    const cJSON *network = cJSON_GetObjectItemCaseSensitive(root, "network");
    if (cJSON_IsObject(network)) {
        if (json_get_int(network, "mode", NET_MODE_AUTO, NET_MODE_LAST, &v)) {
            c->network.mode = (uint8_t)v;
        } else {
            /* Files from before "mode": Ethernet switched off meant WiFi only */
            bool use_eth = true;
            if (json_get_bool(network, "use_ethernet", &use_eth) && !use_eth) {
                c->network.mode = NET_MODE_WIFI;
            }
        }
        json_get_bool(network, "ap_always", &c->network.ap_always);
        json_get_bool(network, "eth_use_static_ip", &c->network.eth_use_static_ip);
        json_get_str(network, "eth_static_ip", c->network.eth_static_ip, sizeof(c->network.eth_static_ip));
        json_get_str(network, "eth_gateway", c->network.eth_gateway, sizeof(c->network.eth_gateway));
        json_get_str(network, "eth_netmask", c->network.eth_netmask, sizeof(c->network.eth_netmask));
        json_get_str(network, "ap_ssid", c->network.ap_ssid, sizeof(c->network.ap_ssid));
        json_get_str(network, "ap_password", c->network.ap_password, sizeof(c->network.ap_password));
        if (json_get_int(network, "ap_channel", 1, 13, &v)) c->network.ap_channel = v;
        json_get_bool(network, "ap_captive", &c->network.ap_captive);

        const cJSON *profiles = cJSON_GetObjectItemCaseSensitive(network, "wifi_profiles");
        if (cJSON_IsArray(profiles)) {
            config_wifi_profile_t old[CONFIG_WIFI_PROFILES_MAX];
            int old_count = g_config.network.wifi_profile_count;
            if (old_count > CONFIG_WIFI_PROFILES_MAX) old_count = CONFIG_WIFI_PROFILES_MAX;
            memcpy(old, g_config.network.wifi_profiles, sizeof(old));

            memset(c->network.wifi_profiles, 0, sizeof(c->network.wifi_profiles));
            int count = 0;
            const cJSON *profile;
            cJSON_ArrayForEach(profile, profiles) {
                if (count >= CONFIG_WIFI_PROFILES_MAX) break;
                if (!cJSON_IsObject(profile)) continue;
                config_wifi_profile_t *wp = &c->network.wifi_profiles[count];
                if (!json_get_str(profile, "ssid", wp->ssid, sizeof(wp->ssid)) || wp->ssid[0] == '\0') {
                    memset(wp, 0, sizeof(*wp));
                    continue;
                }
                bool dup = false;   /* one entry per network name */
                for (int i = 0; i < count; i++) {
                    if (strcmp(c->network.wifi_profiles[i].ssid, wp->ssid) == 0) dup = true;
                }
                if (dup) {
                    memset(wp, 0, sizeof(*wp));
                    continue;
                }
                /* The web UI never receives stored passwords; a profile sent
                 * without one keeps the password saved for that SSID. */
                if (!json_get_str(profile, "password", wp->password, sizeof(wp->password))) {
                    for (int i = 0; i < old_count; i++) {
                        if (strcmp(old[i].ssid, wp->ssid) == 0) {
                            copy_str(wp->password, sizeof(wp->password), old[i].password);
                            break;
                        }
                    }
                }
                if (json_get_int(profile, "priority", 0, 255, &v)) wp->priority = v;
                json_get_bool(profile, "use_static_ip", &wp->use_static_ip);
                json_get_str(profile, "static_ip", wp->static_ip, sizeof(wp->static_ip));
                json_get_str(profile, "gateway", wp->gateway, sizeof(wp->gateway));
                json_get_str(profile, "netmask", wp->netmask, sizeof(wp->netmask));
                count++;
            }
            c->network.wifi_profile_count = count;
        }
    }

    bool had_sacn[CONFIG_DMX_PORTS] = {0};
    for (int i = 0; i < CONFIG_DMX_PORTS; i++) {
        char key[8];
        snprintf(key, sizeof(key), "port%d", i + 1);
        port_from_json(cJSON_GetObjectItemCaseSensitive(root, key), &c->ports[i], &had_sacn[i]);
    }

    /* Config files written before sacn_universe existed used universe_primary
     * for both protocols: carry that over so existing setups keep working. */
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    bool legacy = from_file && !cJSON_IsNumber(version);
    if (legacy) {
        for (int i = 0; i < CONFIG_DMX_PORTS; i++) {
            port_config_t *pc = &c->ports[i];
            if (!had_sacn[i]) pc->sacn_universe = pc->universe_primary >= 1 ? pc->universe_primary : DEF_SACN(i);
        }
        c->merge.loss_behavior = LOSS_HOLD_LAST_LOOK;
        c->node_info.sacn_priority = 100;
        ESP_LOGI(TAG, "Upgraded legacy config (sACN universes follow Art-Net)");
    }

    const cJSON *merge = cJSON_GetObjectItemCaseSensitive(root, "merge");
    if (cJSON_IsObject(merge)) {
        if (json_get_int(merge, "timeout_seconds", 1, 60, &v)) c->merge.timeout_seconds = v;
        if (json_get_int(merge, "loss_behavior", LOSS_HOLD_LAST_LOOK, LOSS_LAST, &v)) c->merge.loss_behavior = v;
    }

    const cJSON *node_info = cJSON_GetObjectItemCaseSensitive(root, "node_info");
    if (cJSON_IsObject(node_info)) {
        json_get_str(node_info, "short_name", c->node_info.short_name, sizeof(c->node_info.short_name));
        json_get_str(node_info, "long_name", c->node_info.long_name, sizeof(c->node_info.long_name));
        if (json_get_int(node_info, "sacn_priority", 0, 200, &v)) c->node_info.sacn_priority = v;
        json_get_bool(node_info, "allow_ip_prog", &c->node_info.allow_ip_prog);
    }

    const cJSON *tester = cJSON_GetObjectItemCaseSensitive(root, "tester");
    if (cJSON_IsObject(tester)) {
        json_get_bool(tester, "enabled", &c->tester.enabled);
        const cJSON *tports = cJSON_GetObjectItemCaseSensitive(tester, "ports");
        int i = 0;
        const cJSON *o;
        if (cJSON_IsArray(tports)) {
            cJSON_ArrayForEach(o, tports) {
                if (i >= CONFIG_DMX_PORTS) break;
                tester_port_config_t *t = &c->tester.ports[i++];
                if (!cJSON_IsObject(o)) continue;
                const cJSON *jr = cJSON_GetObjectItemCaseSensitive(o, "role");
                int role = cJSON_IsString(jr) ? config_tester_role_from_name(jr->valuestring) : -1;
                if (role >= 0) t->role = (uint8_t)role;
                if (json_get_int(o, "rate_hz", CONFIG_DMX_RATE_MIN, CONFIG_DMX_RATE_MAX, &v)) t->rate_hz = v;
                if (json_get_int(o, "break_us", CONFIG_DMX_BREAK_MIN, CONFIG_DMX_BREAK_MAX, &v)) t->break_us = v;
                if (json_get_int(o, "mab_us", CONFIG_DMX_MAB_MIN, CONFIG_DMX_MAB_MAX, &v)) t->mab_us = v;
                if (json_get_int(o, "slots", CONFIG_DMX_SLOTS_MIN, CONFIG_DMX_SLOTS_MAX, &v)) t->slots = v;
            }
        }
    }

    /* Credentials come from the config file only, never from the web API */
    const cJSON *security = cJSON_GetObjectItemCaseSensitive(root, "security");
    if (from_file && cJSON_IsObject(security)) {
        json_get_str(security, "admin_hash", c->security.admin_hash, sizeof(c->security.admin_hash));
        json_get_str(security, "admin_salt", c->security.admin_salt, sizeof(c->security.admin_salt));
        if (json_get_int(security, "admin_iterations", 0, 1000000, &v)) c->security.admin_iterations = v;
        json_get_bool(security, "no_login", &c->security.no_login);
    }

    cJSON_Delete(root);

    bool fixed = config_sanitize(c);
    memcpy(&g_config, c, sizeof(config_t));
    free(c);
    if (from_file && (legacy || fixed)) {
        ESP_LOGI(TAG, "Config upgraded/repaired, saving");
        config_save();
    }
    config_unlock();
    return ESP_OK;
}

esp_err_t config_from_json(const char *json_str)
{
    return parse_json(json_str, false);
}

const char *config_ap_password_problem(const char *pw)
{
    if (!pw) return "AP password missing";
    size_t n = strlen(pw);
    if (n == 0) return NULL;   /* no password: an open network */
    if (n < 8 || n > 63) return "AP password must be 8-63 characters (or empty for an open network)";
    if (strcmp(pw, CONFIG_DEFAULT_AP_PASSWORD) == 0) {
        return "Choose an AP password other than the factory default";
    }
    return NULL;
}

const char *config_update_problem(const cJSON *doc)
{
    if (!cJSON_IsObject(doc)) return "Expected a JSON object";
    const cJSON *net = cJSON_GetObjectItemCaseSensitive(doc, "network");
    /* A new AP password must be a valid WPA2 passphrase and not the factory
     * default, or empty for an open network. */
    const cJSON *ap = cJSON_GetObjectItemCaseSensitive(net, "ap_password");
    if (ap) {
        if (!cJSON_IsString(ap)) return "AP password must be a string";
        const char *p = config_ap_password_problem(ap->valuestring);
        if (p) return p;
    }
    const cJSON *profiles = cJSON_GetObjectItemCaseSensitive(net, "wifi_profiles");
    const cJSON *prof;
    cJSON_ArrayForEach(prof, profiles) {
        const cJSON *pw = cJSON_GetObjectItemCaseSensitive(prof, "password");
        if (cJSON_IsString(pw)) {
            size_t n = strlen(pw->valuestring);
            if (n != 0 && (n < 8 || n > 64)) {
                return "WiFi password must be 8-63 characters (or 64 hex digits)";
            }
        }
    }
    /* Port rules: rejected as a whole rather than silently dropped */
    for (int i = 1; i <= CONFIG_DMX_PORTS; i++) {
        char key[8];
        snprintf(key, sizeof(key), "port%d", i);
        const cJSON *port = cJSON_GetObjectItemCaseSensitive(doc, key);
        const cJSON *inputs = cJSON_GetObjectItemCaseSensitive(port, "local_inputs");
        if (inputs) {
            if (!cJSON_IsArray(inputs)) return "Routed inputs must be a list of port numbers";
            const cJSON *in;
            cJSON_ArrayForEach(in, inputs) {
                int port = port_number(in);
                if (!port) return "Routed inputs must be a list of port numbers";
                if (port == i) return "A port cannot take its own DMX input";
            }
        }
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(port, "send_to");
        if (st && (!cJSON_IsString(st) || send_to_from_name(st->valuestring) < 0)) {
            return "Send to must be \"all\", \"nodes\" or \"list\"";
        }
        const cJSON *list = cJSON_GetObjectItemCaseSensitive(port, "send_list");
        if (list) {
            if (!cJSON_IsArray(list)) return "Send-to addresses must be a list";
            if (cJSON_GetArraySize(list) > CONFIG_SEND_LIST_MAX) return "At most 4 send-to addresses per port";
            const cJSON *e;
            cJSON_ArrayForEach(e, list) {
                uint32_t ip;
                if (!cJSON_IsString(e) || !config_parse_ipv4(e->valuestring, &ip) || !send_ip_ok(ip)) {
                    return "Send-to addresses must be IPv4 addresses like 10.0.0.20 (not multicast or broadcast)";
                }
            }
        }
        if (cJSON_IsString(st) && strcmp(st->valuestring, "list") == 0 &&
            cJSON_IsArray(list) && cJSON_GetArraySize(list) == 0) {
            return "Give at least one address to send to";
        }
        const cJSON *rules = cJSON_GetObjectItemCaseSensitive(port, "rules");
        if (!rules) continue;
        if (!cJSON_IsArray(rules)) return "Port rules must be a list";
        if (cJSON_GetArraySize(rules) > CONFIG_PORT_RULES_MAX) return "At most 8 rules per port";
        const cJSON *ro;
        cJSON_ArrayForEach(ro, rules) {
            port_rule_t r;
            if (!rule_from_json(ro, &r)) {
                return "Each port rule needs a type (patch, limit or park), channel, count and from (patch) or level (0-255)";
            }
            const char *why = config_port_rule_problem(&r);
            if (why) return why;
        }
    }
    return NULL;
}

static cJSON *public_json(const config_t *c)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return NULL;

    cJSON *node = cJSON_AddObjectToObject(json, "node_info");
    cJSON_AddStringToObject(node, "short_name", c->node_info.short_name);
    cJSON_AddStringToObject(node, "long_name", c->node_info.long_name);
    cJSON_AddNumberToObject(node, "sacn_priority", c->node_info.sacn_priority);
    cJSON_AddBoolToObject(node, "allow_ip_prog", c->node_info.allow_ip_prog);

    cJSON *net = cJSON_AddObjectToObject(json, "network");
    cJSON_AddNumberToObject(net, "mode", c->network.mode);
    cJSON_AddBoolToObject(net, "ap_always", c->network.ap_always);
#if CONFIG_NODE_ETHERNET
    cJSON_AddBoolToObject(net, "eth_use_static_ip", c->network.eth_use_static_ip);
    cJSON_AddStringToObject(net, "eth_static_ip", c->network.eth_static_ip);
    cJSON_AddStringToObject(net, "eth_gateway", c->network.eth_gateway);
    cJSON_AddStringToObject(net, "eth_netmask", c->network.eth_netmask);
#endif
    cJSON_AddStringToObject(net, "ap_ssid", c->network.ap_ssid);
    cJSON_AddBoolToObject(net, "ap_open", c->network.ap_password[0] == '\0');
    cJSON_AddNumberToObject(net, "ap_channel", c->network.ap_channel);
    cJSON_AddBoolToObject(net, "ap_captive", c->network.ap_captive);
    cJSON_AddNumberToObject(net, "wifi_profile_count", c->network.wifi_profile_count);
    cJSON *profiles = cJSON_AddArrayToObject(net, "wifi_profiles");
    for (int i = 0; profiles && i < c->network.wifi_profile_count && i < CONFIG_WIFI_PROFILES_MAX; i++) {
        const config_wifi_profile_t *wp = &c->network.wifi_profiles[i];
        cJSON *p = cJSON_CreateObject();
        if (!p) break;
        cJSON_AddStringToObject(p, "ssid", wp->ssid);
        cJSON_AddNumberToObject(p, "priority", wp->priority);
        cJSON_AddBoolToObject(p, "has_password", wp->password[0] != '\0');
        cJSON_AddBoolToObject(p, "use_static_ip", wp->use_static_ip);
        cJSON_AddStringToObject(p, "static_ip", wp->static_ip);
        cJSON_AddStringToObject(p, "gateway", wp->gateway);
        cJSON_AddStringToObject(p, "netmask", wp->netmask);
        cJSON_AddItemToArray(profiles, p);
    }

    for (int p = 1; p <= CONFIG_DMX_PORTS; p++) {
        char key[8];
        snprintf(key, sizeof(key), "port%d", p);
        cJSON_AddItemToObject(json, key, config_port_to_json(&c->ports[p - 1]));
    }

    cJSON *merge = cJSON_AddObjectToObject(json, "merge");
    cJSON_AddNumberToObject(merge, "timeout_seconds", c->merge.timeout_seconds);
    cJSON_AddNumberToObject(merge, "loss_behavior", c->merge.loss_behavior);

    cJSON *tester = cJSON_AddObjectToObject(json, "tester");
    if (tester) {
        cJSON_AddBoolToObject(tester, "enabled", c->tester.enabled);
        cJSON *tports = cJSON_AddArrayToObject(tester, "ports");
        for (int p = 0; tports && p < CONFIG_DMX_PORTS; p++) {
            const tester_port_config_t *t = &c->tester.ports[p];
            cJSON *o = cJSON_CreateObject();
            if (!o) break;
            cJSON_AddStringToObject(o, "role", config_tester_role_name(t->role));
            cJSON_AddNumberToObject(o, "rate_hz", t->rate_hz);
            cJSON_AddNumberToObject(o, "break_us", t->break_us);
            cJSON_AddNumberToObject(o, "mab_us", t->mab_us);
            cJSON_AddNumberToObject(o, "slots", t->slots);
            cJSON_AddItemToArray(tports, o);
        }
    }
    return json;
}

cJSON *config_to_public_json(const config_t *c)
{
    if (!c) return NULL;
    config_lock();
    cJSON *json = public_json(c);
    config_unlock();
    return json;
}
