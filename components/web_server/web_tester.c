/**
 * @file web_tester.c
 * @brief Web API for DMX tester mode (CONFIG_NODE_TESTER, dmx_tester.h)
 *
 *   GET  /api/tester                   mode, and every port's job, timing and statistics
 *   GET  /api/tester/channels?port=N   per-channel levels, lowest, highest, changes,
 *                                      and the channels that changed in the last second
 *   POST /api/tester/mode              {"enabled":true} saves and restarts the node
 *   POST /api/tester/port              {"port":1,"role":"send","rate_hz":44,"break_us":176,
 *                                       "mab_us":12,"slots":512} (any subset; applies live)
 *   POST /api/tester/levels            {"port":1,"start":1,"values":[255,0,...]} or
 *                                      {"port":1,"first":1,"last":512,"value":0}
 *   POST /api/tester/reset             {"port":2} (0 = every port): statistics start again
 *
 * Tester mode itself is switched at start-up only (main.c), so the mode
 * route saves the setting and restarts. Everything else needs the node to
 * be running in tester mode. RDM, test patterns and the loopback self-test
 * use their usual routes (under /api/rdm/ and /api/tools/).
 */
#include "web_internal.h"
#include "web_stream.h"
#include "config_manager.h"
#include "dmx_tester.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "web_tester";

#define RESTART_DELAY_MS 800

// ============================================================================
// Helpers
// ============================================================================

static void send_ok(httpd_req_t *req, cJSON *json)
{
    web_send_json(req, json, 200);
    cJSON_Delete(json);
}

static void send_status_ok(httpd_req_t *req)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "status", "ok");
    send_ok(req, json);
}

/* Auth plus "is the node running as a tester"; sends the error itself */
static bool tester_ready(httpd_req_t *req)
{
    if (!web_require_auth(req)) return false;
    if (!dmx_tester_enabled()) {
        web_send_error(req, 409, "The node is not in DMX tester mode");
        return false;
    }
    return true;
}

static int body_port(const cJSON *body)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(body, "port");
    return cJSON_IsNumber(j) ? j->valueint : -1;
}

static bool get_int(const cJSON *o, const char *key, int lo, int hi, int *out)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(j)) return false;
    int v = j->valueint;
    *out = v < lo ? lo : v > hi ? hi : v;
    return true;
}

static cJSON *rx_stats_json(uint8_t port)
{
    dmx_tester_stats_t s;
    if (dmx_tester_get_stats(port, &s) != ESP_OK) return NULL;
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddNumberToObject(o, "frames", s.frames);
    cJSON_AddNumberToObject(o, "seconds", s.seconds);
    cJSON_AddNumberToObject(o, "losses", s.losses);
    cJSON_AddNumberToObject(o, "last_loss_s", s.last_loss_s);
    cJSON_AddNumberToObject(o, "interval_avg_us", s.interval_avg_us);
    cJSON_AddNumberToObject(o, "interval_jitter_us", s.interval_jitter_us);
    cJSON_AddNumberToObject(o, "interval_min_us", s.interval_min_us);
    cJSON_AddNumberToObject(o, "interval_max_us", s.interval_max_us);
    cJSON *h = cJSON_AddArrayToObject(o, "hist");
    for (int k = 0; h && k < DMX_TESTER_HIST_BUCKETS; k++) {
        cJSON_AddItemToArray(h, cJSON_CreateNumber(s.hist[k]));
    }
    cJSON_AddNumberToObject(o, "changing", s.changing);
    cJSON_AddNumberToObject(o, "changed", s.changed);
    return o;
}

// ============================================================================
// GET /api/tester
// ============================================================================

static esp_err_t tester_get(httpd_req_t *req)
{
    if (!web_require_auth(req)) return ESP_OK;
    const config_t *c = config_get();
    bool on = dmx_tester_enabled();

    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "available", WEB_HAS(CONFIG_NODE_TESTER));
    cJSON_AddBoolToObject(json, "enabled", on);
    cJSON_AddBoolToObject(json, "next_boot", c->tester.enabled);
    cJSON *edges = cJSON_AddArrayToObject(json, "hist_edges_ms");
    for (int k = 0; edges && k < DMX_TESTER_HIST_BUCKETS - 1; k++) {
        cJSON_AddItemToArray(edges, cJSON_CreateNumber(DMX_TESTER_HIST_EDGES_MS[k]));
    }

    cJSON *ports = cJSON_AddArrayToObject(json, "ports");
    for (uint8_t p = 1; ports && p <= CONFIG_DMX_PORTS; p++) {
        const tester_port_config_t *t = &c->tester.ports[p - 1];
        cJSON *o = cJSON_CreateObject();
        if (!o) break;
        cJSON_AddNumberToObject(o, "port", p);
        cJSON_AddStringToObject(o, "role", config_tester_role_name(t->role));
        cJSON_AddNumberToObject(o, "rate_hz", t->rate_hz);
        cJSON_AddNumberToObject(o, "break_us", t->break_us);
        cJSON_AddNumberToObject(o, "mab_us", t->mab_us);
        cJSON_AddNumberToObject(o, "slots", t->slots);

        dmx_port_status_t st;
        if (dmx_handler_get_port_status(p, &st) == ESP_OK) {
            cJSON_AddBoolToObject(o, "active", st.is_active);
            cJSON_AddNumberToObject(o, "mode", st.mode);
            cJSON_AddBoolToObject(o, "signal", st.stats.signal_present);
            cJSON_AddNumberToObject(o, "frames_sent", st.stats.frames_sent);
            cJSON_AddNumberToObject(o, "frames_received", st.stats.frames_received);
            cJSON_AddNumberToObject(o, "errors", st.stats.error_count);
            cJSON_AddNumberToObject(o, "rdm_devices", st.rdm_device_count);
            cJSON_AddBoolToObject(o, "rdm_discovering", st.rdm_discovering);
            cJSON_AddBoolToObject(o, "rdm_tod_valid", st.rdm_tod_valid);
        }
        dmx_output_timing_t tm;
        if (dmx_handler_get_output_timing(p, &tm) == ESP_OK) {
            cJSON *j = cJSON_AddObjectToObject(o, "timing");
            cJSON_AddNumberToObject(j, "rate_hz", tm.rate_hz);
            cJSON_AddNumberToObject(j, "break_us", tm.break_us);
            cJSON_AddNumberToObject(j, "mab_us", tm.mab_us);
            cJSON_AddNumberToObject(j, "slots", tm.slots);
        }
        dmx_test_t test;
        if (dmx_test_get(p, &test)) {
            web_test_to_json(cJSON_AddObjectToObject(o, "test"), &test);
        }
        dmx_input_analysis_t an;
        if (dmx_handler_get_analysis(p, &an) == ESP_OK) {
            web_analysis_to_json(cJSON_AddObjectToObject(o, "analysis"), &an);
        }
        if (on) {
            cJSON *rx = rx_stats_json(p);
            if (rx) cJSON_AddItemToObject(o, "rx", rx);
        }
        cJSON_AddItemToArray(ports, o);
    }
    send_ok(req, json);
    return ESP_OK;
}

// ============================================================================
// GET /api/tester/channels?port=N
// ============================================================================

static esp_err_t channels_get(httpd_req_t *req)
{
    if (!tester_ready(req)) return ESP_OK;
    char q[32], v[8];
    int port = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "port", v, sizeof(v)) == ESP_OK) {
        port = atoi(v);
    }
    if (port < 1 || port > CONFIG_DMX_PORTS) {
        web_send_error(req, 400, "port=1..N required");
        return ESP_OK;
    }

    struct {
        uint8_t level[512], min[512], max[512], changing[64];
        uint32_t changes[512];
    } *b = malloc(sizeof(*b));
    if (!b) {
        web_send_error(req, 500, "Out of memory");
        return ESP_OK;
    }
    dmx_handler_get_levels((uint8_t)port, b->level);
    dmx_tester_get_channels((uint8_t)port, b->min, b->max, b->changes, b->changing);

    /* Not cJSON: over 2,000 numbers, about 80 KB of internal RAM */
    web_stream_t *w = web_stream_begin(req);
    if (!w) {
        free(b);
        return ESP_OK;
    }
    web_stream_printf(w, "{\"port\":%d,\"level\":", port);
    web_stream_u8_array(w, b->level, 512);
    web_stream_printf(w, ",\"min\":");
    web_stream_u8_array(w, b->min, 512);
    web_stream_printf(w, ",\"max\":");
    web_stream_u8_array(w, b->max, 512);
    web_stream_printf(w, ",\"changes\":");
    web_stream_u32_array(w, b->changes, 512);
    web_stream_printf(w, ",\"changing\":[");   /* channel numbers */
    const char *sep = "";
    for (int i = 0; i < 512; i++) {
        if (b->changing[i >> 3] & (1u << (i & 7))) {
            web_stream_printf(w, "%s%d", sep, i + 1);
            sep = ",";
        }
    }
    web_stream_printf(w, "]}");
    free(b);
    return web_stream_end(w);
}

// ============================================================================
// POST /api/tester/mode
// ============================================================================

static esp_err_t mode_post(httpd_req_t *req)
{
    if (!web_require_auth(req)) return ESP_OK;
    cJSON *body = web_read_json(req);
    if (!body) return ESP_OK;
    const cJSON *je = cJSON_GetObjectItemCaseSensitive(body, "enabled");
    if (!cJSON_IsBool(je)) {
        cJSON_Delete(body);
        web_send_error(req, 400, "enabled (true / false) required");
        return ESP_OK;
    }
    bool enable = cJSON_IsTrue(je);
    cJSON_Delete(body);
#if !CONFIG_NODE_TESTER
    if (enable) {
        web_send_error(req, 400, "DMX tester mode is not built into this firmware");
        return ESP_OK;
    }
#endif
    config_t *c = config_get();
    config_lock();
    c->tester.enabled = enable;
    config_unlock();
    if (config_save() != ESP_OK) {
        web_send_error(req, 500, "Failed to save the setting");
        return ESP_OK;
    }
    bool restart = enable != dmx_tester_enabled();
    ESP_LOGW(TAG, "DMX tester mode %s%s", enable ? "on" : "off", restart ? ", restarting" : "");

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "status", "ok");
    cJSON_AddBoolToObject(json, "enabled", enable);
    cJSON_AddBoolToObject(json, "restarting", restart);
    send_ok(req, json);
    if (restart) web_restart_later(RESTART_DELAY_MS);
    return ESP_OK;
}

// ============================================================================
// POST /api/tester/port
// ============================================================================

static esp_err_t port_post(httpd_req_t *req)
{
    if (!web_require_auth(req)) return ESP_OK;   /* settings work in either mode */
    cJSON *body = web_read_json(req);
    if (!body) return ESP_OK;
    int port = body_port(body);
    if (port < 1 || port > CONFIG_DMX_PORTS) {
        cJSON_Delete(body);
        web_send_error(req, 400, "port 1..N required");
        return ESP_OK;
    }
    config_t *c = config_get();
    int role = -2;   /* not given */
    const cJSON *jr = cJSON_GetObjectItemCaseSensitive(body, "role");
    if (jr) {
        role = cJSON_IsString(jr) ? config_tester_role_from_name(jr->valuestring) : -1;
#if !CONFIG_NODE_RDM
        if (role == TESTER_ROLE_RDM) role = -1;
#endif
        if (role < 0) {
            cJSON_Delete(body);
            web_send_error(req, 400, "role must be off, send, receive or rdm");
            return ESP_OK;
        }
    }
    int rate = -1, brk = -1, mab = -1, slots = -1, v;
    if (get_int(body, "rate_hz", CONFIG_DMX_RATE_MIN, CONFIG_DMX_RATE_MAX, &v)) rate = v;
    if (get_int(body, "break_us", CONFIG_DMX_BREAK_MIN, CONFIG_DMX_BREAK_MAX, &v)) brk = v;
    if (get_int(body, "mab_us", CONFIG_DMX_MAB_MIN, CONFIG_DMX_MAB_MAX, &v)) mab = v;
    if (get_int(body, "slots", CONFIG_DMX_SLOTS_MIN, CONFIG_DMX_SLOTS_MAX, &v)) slots = v;
    cJSON_Delete(body);

    config_lock();   /* read-modify-write of the live settings */
    tester_port_config_t *t = &c->tester.ports[port - 1];
    if (role >= 0) t->role = (uint8_t)role;
    if (rate >= 0) t->rate_hz = rate;
    if (brk >= 0) t->break_us = brk;
    if (mab >= 0) t->mab_us = mab;
    if (slots >= 0) t->slots = slots;
    config_unlock();
    if (config_save() != ESP_OK) {
        web_send_error(req, 500, "Failed to save the setting");
        return ESP_OK;
    }
    config_notify_changed();   /* the main task re-applies the ports */
    send_status_ok(req);
    return ESP_OK;
}

// ============================================================================
// POST /api/tester/levels
// ============================================================================

static esp_err_t levels_post(httpd_req_t *req)
{
    if (!tester_ready(req)) return ESP_OK;
    cJSON *body = web_read_json(req);
    if (!body) return ESP_OK;
    int port = body_port(body);
    esp_err_t err = ESP_ERR_INVALID_ARG;
    const cJSON *jv = cJSON_GetObjectItemCaseSensitive(body, "values");
    int start = 1, first = 1, last = 512, value = 0;
    if (port < 1 || port > CONFIG_DMX_PORTS) {
        err = ESP_ERR_INVALID_ARG;
    } else if (cJSON_IsArray(jv) && get_int(body, "start", 1, 512, &start)) {
        uint8_t vals[512];
        int n = 0;
        const cJSON *it;
        cJSON_ArrayForEach(it, jv) {
            if (n >= 513 - start) break;
            int x = cJSON_IsNumber(it) ? it->valueint : 0;
            vals[n++] = (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
        }
        err = dmx_tester_set_levels((uint8_t)port, (uint16_t)start, vals, (uint16_t)n);
    } else if (get_int(body, "value", 0, 255, &value)) {
        if (!get_int(body, "first", 1, 512, &first)) first = 1;
        if (!get_int(body, "last", 1, 512, &last)) last = 512;
        err = dmx_tester_fill((uint8_t)port, (uint16_t)first, (uint16_t)last, (uint8_t)value);
    }
    cJSON_Delete(body);
    if (err != ESP_OK) {
        web_send_error(req, 400, "port with start + values[], or value (+ first, last) required");
        return ESP_OK;
    }
    send_status_ok(req);
    return ESP_OK;
}

// ============================================================================
// POST /api/tester/reset
// ============================================================================

static esp_err_t reset_post(httpd_req_t *req)
{
    if (!tester_ready(req)) return ESP_OK;
    cJSON *body = web_read_json(req);
    if (!body) return ESP_OK;
    int port = body_port(body);
    cJSON_Delete(body);
    if (port < 0 || port > CONFIG_DMX_PORTS) {
        web_send_error(req, 400, "port 0..N required (0 = every port)");
        return ESP_OK;
    }
    for (int p = 1; p <= CONFIG_DMX_PORTS; p++) {
        if (port != 0 && p != port) continue;
        dmx_tester_reset((uint8_t)p);
        dmx_handler_reset_analysis((uint8_t)p);
    }
    send_status_ok(req);
    return ESP_OK;
}

// ============================================================================
// Registration
// ============================================================================

void web_tester_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        { .uri = "/api/tester",           .method = HTTP_GET,  .handler = tester_get },
        { .uri = "/api/tester/channels",  .method = HTTP_GET,  .handler = channels_get },
        { .uri = "/api/tester/mode",      .method = HTTP_POST, .handler = mode_post },
        { .uri = "/api/tester/port",      .method = HTTP_POST, .handler = port_post },
        { .uri = "/api/tester/levels",    .method = HTTP_POST, .handler = levels_post },
        { .uri = "/api/tester/reset",     .method = HTTP_POST, .handler = reset_post },
    };
    _Static_assert(sizeof(routes) / sizeof(routes[0]) == WEB_TESTER_ROUTES, "update WEB_TESTER_ROUTES");
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        if (httpd_register_uri_handler(server, &routes[i]) != ESP_OK) {
            ESP_LOGE(TAG, "Could not register %s", routes[i].uri);
        }
    }
}
