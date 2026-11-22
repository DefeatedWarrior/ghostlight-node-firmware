/**
 * @file web_tools.c
 * @brief Web API for diagnostics and DMX tools
 *
 *   GET  /api/diag/log?pos=N       log text since position N (JSON)
 *   GET  /api/diag/log.txt         whole log buffer (download)
 *   GET  /api/diag/crash           reset reason and crash summary
 *   GET  /api/diag/crash.bin       raw core dump (download, for idf.py coredump-info)
 *   POST /api/diag/crash/clear     delete the stored core dump
 *   GET  /api/tools/test           test pattern state of every port
 *   POST /api/tools/test           {"port":1,"mode":"chase","first":1,"last":512,"level":255,"speed_ms":500}
 *   POST /api/tools/analyzer       {"port":2,"action":"reset"} / {"port":2,"action":"timing","seconds":60}
 *   GET  /api/tools/selftest       loopback self-test result
 *   POST /api/tools/selftest       {"out_port":1,"in_port":2}
 *   GET  /api/system/identify      {"identify_left":s}
 *   POST /api/system/identify      {"seconds":30} (0 = stop)
 */
#include "web_internal.h"
#include "web_stream.h"
#include "diag.h"
#include "config_manager.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "web_tools";

#define LOG_CHUNK_MAX     6144    /* log text per JSON response */
#define DOWNLOAD_CHUNK    2048
#define IDENTIFY_MAX_S    600

// ============================================================================
// Helpers
// ============================================================================

static bool query_u32(httpd_req_t *req, const char *key, uint32_t *out)
{
    char q[64], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
    if (httpd_query_key_value(q, key, v, sizeof(v)) != ESP_OK) return false;
    char *end = NULL;
    unsigned long n = strtoul(v, &end, 10);
    if (!end || *end) return false;
    *out = (uint32_t)n;
    return true;
}

static int json_int(const cJSON *o, const char *key, int def)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(j) ? j->valueint : def;
}

static void send_ok(httpd_req_t *req, cJSON *json)
{
    web_send_json(req, json, 200);
    cJSON_Delete(json);
}

static void attachment(httpd_req_t *req, const char *type, const char *filename)
{
    static char disp[80];   /* httpd keeps the pointer until the response is sent */
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", filename);
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

static bool port_is_output(int port)
{
    dmx_port_status_t st;
    return dmx_handler_get_port_status(port, &st) == ESP_OK && st.is_active &&
           (st.mode == DMX_MODE_OUTPUT || st.mode == DMX_MODE_RDM_MASTER);
}

void web_test_to_json(cJSON *o, const dmx_test_t *t)
{
    if (!o || !t) return;
    cJSON_AddStringToObject(o, "mode", dmx_test_mode_name(t->mode));
    cJSON_AddNumberToObject(o, "first", t->first);
    cJSON_AddNumberToObject(o, "last", t->last);
    cJSON_AddNumberToObject(o, "level", t->level);
    cJSON_AddNumberToObject(o, "speed_ms", t->speed_ms);
}

void web_analysis_to_json(cJSON *o, const dmx_input_analysis_t *a)
{
    if (!o || !a) return;
    cJSON_AddNumberToObject(o, "refresh_hz", a->refresh_hz);
    cJSON_AddNumberToObject(o, "packets_per_s", a->packets_per_s);
    cJSON_AddNumberToObject(o, "slots", a->slots_last);
    cJSON_AddNumberToObject(o, "slots_min", a->slots_min);
    cJSON_AddNumberToObject(o, "slots_max", a->slots_max);
    cJSON_AddNumberToObject(o, "frames", a->frames);
    cJSON_AddNumberToObject(o, "err_framing", a->err_framing);
    cJSON_AddNumberToObject(o, "err_overflow", a->err_overflow);
    cJSON_AddNumberToObject(o, "err_short", a->err_short);
    cJSON_AddNumberToObject(o, "dropped_late", a->dropped_late);
    cJSON_AddNumberToObject(o, "alt_start_codes", a->alt_start_codes);
    cJSON_AddNumberToObject(o, "last_alt_start_code", a->last_alt_start_code);
    cJSON_AddNumberToObject(o, "rdm_packets", a->rdm_packets);
    cJSON_AddNumberToObject(o, "interval_min_ms", a->interval_min_ms);
    cJSON_AddNumberToObject(o, "interval_max_ms", a->interval_max_ms);
    cJSON_AddBoolToObject(o, "timing_on", a->timing_on);
    cJSON_AddNumberToObject(o, "timing_left_s", a->timing_left_s);
    cJSON_AddNumberToObject(o, "break_us", a->break_us);
    cJSON_AddNumberToObject(o, "mab_us", a->mab_us);
}

// ============================================================================
// Log
// ============================================================================

static esp_err_t log_get(httpd_req_t *req)
{
    if (!web_require_auth(req)) return ESP_OK;
    uint32_t pos = 0;
    query_u32(req, "pos", &pos);

    char *buf = malloc(LOG_CHUNK_MAX + 1);
    if (!buf) {
        web_send_error(req, 500, "Out of memory");
        return ESP_OK;
    }
    bool truncated = false;
    size_t n = diag_log_read(&pos, buf, LOG_CHUNK_MAX, &truncated);
    buf[n] = '\0';

    /* Not cJSON: it held the text three times over, 16 KB of internal RAM */
    web_stream_t *w = web_stream_begin(req);
    if (!w) {
        free(buf);
        return ESP_OK;
    }
    web_stream_printf(w, "{\"pos\":%lu,\"truncated\":%s,\"capacity\":%lu,\"text\":",
                      (unsigned long)pos, truncated ? "true" : "false",
                      (unsigned long)diag_log_capacity());
    web_stream_string(w, buf);
    free(buf);
    web_stream_printf(w, "}");
    return web_stream_end(w);
}

static esp_err_t log_download(httpd_req_t *req)
{
    if (!web_require_auth(req)) return ESP_OK;
    char *buf = malloc(DOWNLOAD_CHUNK);
    if (!buf) {
        web_send_error(req, 500, "Out of memory");
        return ESP_OK;
    }
    attachment(req, "text/plain; charset=utf-8", "esp-node-log.txt");
    uint32_t pos = 0;
    for (;;) {
        size_t n = diag_log_read(&pos, buf, DOWNLOAD_CHUNK, NULL);
        if (n == 0) break;
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) break;
    }
    free(buf);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// ============================================================================
// Crash report
// ============================================================================

static esp_err_t crash_get(httpd_req_t *req)
{
    if (!web_require_auth(req)) return ESP_OK;
    diag_crash_t *c = calloc(1, sizeof(*c));
    if (!c) {
        web_send_error(req, 500, "Out of memory");
        return ESP_OK;
    }
    diag_crash_get(c);

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "reset_reason", diag_reset_reason());
    cJSON_AddBoolToObject(json, "reset_was_crash", diag_reset_was_crash());
    cJSON_AddBoolToObject(json, "present", c->present);
    char sha[17] = "";
    esp_app_get_elf_sha256(sha, sizeof(sha));
    cJSON_AddStringToObject(json, "running_sha", sha);
    if (c->present) {
        char hex[12];
        cJSON_AddNumberToObject(json, "size", c->size);
        cJSON_AddStringToObject(json, "task", c->task);
        snprintf(hex, sizeof(hex), "0x%08lx", (unsigned long)c->pc);
        cJSON_AddStringToObject(json, "pc", hex);
        cJSON *bt = cJSON_AddArrayToObject(json, "backtrace");
        for (int i = 0; bt && i < c->depth; i++) {
            snprintf(hex, sizeof(hex), "0x%08lx", (unsigned long)c->backtrace[i]);
            cJSON_AddItemToArray(bt, cJSON_CreateString(hex));
        }
        cJSON_AddBoolToObject(json, "backtrace_corrupted", c->bt_corrupted);
        cJSON_AddNumberToObject(json, "exc_cause", c->exc_cause);
        snprintf(hex, sizeof(hex), "0x%08lx", (unsigned long)c->exc_vaddr);
        cJSON_AddStringToObject(json, "exc_vaddr", hex);
        cJSON_AddStringToObject(json, "reason", c->reason);
        cJSON_AddStringToObject(json, "app_sha", c->app_sha);
        /* Same firmware? Only then do the addresses match the current ELF */
        size_t k = strlen(c->app_sha) < strlen(sha) ? strlen(c->app_sha) : strlen(sha);
        cJSON_AddBoolToObject(json, "same_firmware", k > 0 && strncmp(c->app_sha, sha, k) == 0);
    }
    free(c);
    send_ok(req, json);
    return ESP_OK;
}

static esp_err_t crash_download(httpd_req_t *req)
{
    if (!web_require_auth(req)) return ESP_OK;
    diag_crash_t c;
    if (diag_crash_get(&c) != ESP_OK || !c.present) {
        web_send_error(req, 404, "No crash report stored");
        return ESP_OK;
    }
    char *buf = malloc(DOWNLOAD_CHUNK);
    if (!buf) {
        web_send_error(req, 500, "Out of memory");
        return ESP_OK;
    }
    attachment(req, "application/octet-stream", "esp-node-coredump.bin");
    for (uint32_t off = 0; off < c.size;) {
        size_t n = c.size - off > DOWNLOAD_CHUNK ? DOWNLOAD_CHUNK : c.size - off;
        if (diag_crash_read(off, buf, n) != ESP_OK) break;
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) break;
        off += n;
    }
    free(buf);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t crash_clear(httpd_req_t *req)
{
    if (!web_require_auth(req)) return ESP_OK;
    esp_err_t err = diag_crash_erase();
    if (err != ESP_OK) {
        web_send_error(req, 500, "Could not delete the crash report");
        return ESP_OK;
    }
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "status", "ok");
    send_ok(req, json);
    return ESP_OK;
}

/* GET /api/diag/health: free memory and each task's stack headroom */
static esp_err_t health_get(httpd_req_t *req)
{
    if (!web_require_auth(req)) return ESP_OK;
    diag_health_t *h = malloc(sizeof(*h));
    if (!h) {
        web_send_error(req, 500, "Out of memory");
        return ESP_OK;
    }
    diag_health(h);
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "internal_free", h->internal_free);
    cJSON_AddNumberToObject(json, "internal_min_free", h->internal_min_free);
    cJSON_AddNumberToObject(json, "internal_largest", h->internal_largest);
    cJSON_AddNumberToObject(json, "internal_low", DIAG_INTERNAL_LOW_BYTES);
    cJSON_AddNumberToObject(json, "psram_free", h->psram_free);
    cJSON_AddNumberToObject(json, "psram_min_free", h->psram_min_free);
    cJSON_AddNumberToObject(json, "stack_low", DIAG_STACK_LOW_BYTES);
    cJSON *stacks = cJSON_AddArrayToObject(json, "stacks");
    for (int i = 0; stacks && i < h->stacks; i++) {
        cJSON *o = cJSON_CreateObject();
        if (!o) break;
        cJSON_AddStringToObject(o, "task", h->stack[i].name);
        cJSON_AddNumberToObject(o, "free", h->stack[i].stack_free);
        cJSON_AddItemToArray(stacks, o);
    }
    free(h);
    send_ok(req, json);
    return ESP_OK;
}

// ============================================================================
// Test patterns
// ============================================================================

static cJSON *tests_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (int p = 1; p <= CONFIG_DMX_PORTS; p++) {
        dmx_test_t t;
        bool on = dmx_test_get(p, &t);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "port", p);
        cJSON_AddBoolToObject(o, "running", on);
        cJSON_AddBoolToObject(o, "output", port_is_output(p));
        if (on) web_test_to_json(o, &t);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

static esp_err_t test_get(httpd_req_t *req)
{
    if (!web_require_auth(req)) return ESP_OK;
    send_ok(req, tests_json());
    return ESP_OK;
}

static esp_err_t test_post(httpd_req_t *req)
{
    if (!web_require_auth(req)) return ESP_OK;
    cJSON *body = web_read_json(req);
    if (!body) return ESP_OK;

    int port = json_int(body, "port", 0);
    const cJSON *jm = cJSON_GetObjectItemCaseSensitive(body, "mode");
    int mode = cJSON_IsString(jm) ? dmx_test_mode_from_name(jm->valuestring) : -1;
    dmx_test_t t = {
        .mode = (dmx_test_mode_t)(mode < 0 ? 0 : mode),
        .first = (uint16_t)json_int(body, "first", 1),
        .last = (uint16_t)json_int(body, "last", 512),
        .level = (uint8_t)(json_int(body, "level", 255) & 0xFF),
        .speed_ms = (uint16_t)json_int(body, "speed_ms", 500),
    };
    int lvl = json_int(body, "level", 255);
    cJSON_Delete(body);

    if (port < 1 || port > CONFIG_DMX_PORTS || mode < 0 || lvl < 0 || lvl > 255) {
        web_send_error(req, 400, "port, mode (off/static/full/chase/ramp) and level 0-255 required");
        return ESP_OK;
    }
    if (t.mode != DMX_TEST_OFF && !port_is_output(port)) {
        web_send_error(req, 409, "Test patterns need a running output port");
        return ESP_OK;
    }
    if (t.mode != DMX_TEST_OFF && dmx_selftest_running()) {
        web_send_error(req, 409, "The self-test is running");
        return ESP_OK;
    }
    dmx_test_start(port, &t);
    send_ok(req, tests_json());
    return ESP_OK;
}

// ============================================================================
// Input analyser
// ============================================================================

static esp_err_t analyzer_post(httpd_req_t *req)
{
    if (!web_require_auth(req)) return ESP_OK;
    cJSON *body = web_read_json(req);
    if (!body) return ESP_OK;
    int port = json_int(body, "port", 0);
    int seconds = json_int(body, "seconds", 60);
    const cJSON *ja = cJSON_GetObjectItemCaseSensitive(body, "action");
    char action[12] = "";
    if (cJSON_IsString(ja)) snprintf(action, sizeof(action), "%s", ja->valuestring);
    cJSON_Delete(body);

    if (port < 1 || port > CONFIG_DMX_PORTS) {
        web_send_error(req, 400, "Invalid port");
        return ESP_OK;
    }
    esp_err_t err;
    if (strcmp(action, "reset") == 0) {
        err = dmx_handler_reset_analysis(port);
    } else if (strcmp(action, "timing") == 0) {
        err = dmx_handler_measure_timing(port, seconds < 0 ? 0 : (uint32_t)seconds);
    } else {
        web_send_error(req, 400, "action must be reset or timing");
        return ESP_OK;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        web_send_error(req, 409, "The analyser needs a running input port");
        return ESP_OK;
    }
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "status", err == ESP_OK ? "ok" : "error");
    dmx_input_analysis_t an;
    if (dmx_handler_get_analysis(port, &an) == ESP_OK) {
        web_analysis_to_json(cJSON_AddObjectToObject(json, "analysis"), &an);
    }
    send_ok(req, json);
    return ESP_OK;
}

// ============================================================================
// Self-test
// ============================================================================

static const char *const k_st_state[] = {"idle", "running", "passed", "failed"};

static esp_err_t selftest_get(httpd_req_t *req)
{
    if (!web_require_auth(req)) return ESP_OK;
    dmx_selftest_result_t r;
    dmx_selftest_get(&r);
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "state", r.state <= DMX_SELFTEST_FAILED ? k_st_state[r.state] : "idle");
    cJSON_AddNumberToObject(json, "out_port", r.out_port);
    cJSON_AddNumberToObject(json, "in_port", r.in_port);
    cJSON_AddStringToObject(json, "message", r.message);
    cJSON_AddNumberToObject(json, "refresh_hz", r.refresh_hz);
    cJSON_AddNumberToObject(json, "break_us", r.break_us);
    cJSON_AddNumberToObject(json, "mab_us", r.mab_us);
    cJSON_AddNumberToObject(json, "errors", r.errors);
    cJSON *steps = cJSON_AddArrayToObject(json, "steps");
    for (int k = 0; steps && k < DMX_SELFTEST_PATTERNS; k++) {
        const dmx_selftest_step_t *s = &r.steps[k];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", s->name ? s->name : "");
        cJSON_AddBoolToObject(o, "done", k < r.steps_done);
        cJSON_AddBoolToObject(o, "passed", s->passed);
        cJSON_AddNumberToObject(o, "slots", s->slots);
        cJSON_AddNumberToObject(o, "mismatches", s->mismatches);
        cJSON_AddNumberToObject(o, "first_bad", s->first_bad);
        cJSON_AddNumberToObject(o, "sent", s->sent);
        cJSON_AddNumberToObject(o, "got", s->got);
        cJSON_AddItemToArray(steps, o);
    }
    send_ok(req, json);
    return ESP_OK;
}

static esp_err_t selftest_post(httpd_req_t *req)
{
    if (!web_require_auth(req)) return ESP_OK;
    cJSON *body = web_read_json(req);
    if (!body) return ESP_OK;
    int out = json_int(body, "out_port", 1);
    int in = json_int(body, "in_port", 2);
    cJSON_Delete(body);

    esp_err_t err = dmx_selftest_start((uint8_t)out, (uint8_t)in);
    if (err == ESP_ERR_INVALID_ARG) {
        web_send_error(req, 400, "out_port and in_port must be two different ports");
        return ESP_OK;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        web_send_error(req, 409, "The self-test is already running");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        web_send_error(req, 500, "Could not start the self-test");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Self-test started from the web UI");
    return selftest_get(req);
}

// ============================================================================
// Identify
// ============================================================================

static esp_err_t identify_reply(httpd_req_t *req)
{
    const web_server_hooks_t *h = web_hooks();
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "identify_left", h && h->identify_left ? h->identify_left() : 0);
    send_ok(req, json);
    return ESP_OK;
}

static esp_err_t identify_get(httpd_req_t *req)
{
    if (!web_require_auth(req)) return ESP_OK;
    return identify_reply(req);
}

static esp_err_t identify_post(httpd_req_t *req)
{
    if (!web_require_auth(req)) return ESP_OK;
    cJSON *body = web_read_json(req);
    if (!body) return ESP_OK;
    int seconds = json_int(body, "seconds", 30);
    cJSON_Delete(body);
    if (seconds < 0) seconds = 0;
    if (seconds > IDENTIFY_MAX_S) seconds = IDENTIFY_MAX_S;
    const web_server_hooks_t *h = web_hooks();
    if (!h || !h->identify) {
        web_send_error(req, 503, "Identify not available");
        return ESP_OK;
    }
    h->identify((uint32_t)seconds);
    return identify_reply(req);
}

// ============================================================================
// Registration
// ============================================================================

void web_tools_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        { .uri = "/api/diag/log",          .method = HTTP_GET,  .handler = log_get },
        { .uri = "/api/diag/log.txt",      .method = HTTP_GET,  .handler = log_download },
        { .uri = "/api/diag/crash",        .method = HTTP_GET,  .handler = crash_get },
        { .uri = "/api/diag/crash.bin",    .method = HTTP_GET,  .handler = crash_download },
        { .uri = "/api/diag/crash/clear",  .method = HTTP_POST, .handler = crash_clear },
        { .uri = "/api/diag/health",       .method = HTTP_GET,  .handler = health_get },
        { .uri = "/api/tools/test",        .method = HTTP_GET,  .handler = test_get },
        { .uri = "/api/tools/test",        .method = HTTP_POST, .handler = test_post },
        { .uri = "/api/tools/analyzer",    .method = HTTP_POST, .handler = analyzer_post },
        { .uri = "/api/tools/selftest",    .method = HTTP_GET,  .handler = selftest_get },
        { .uri = "/api/tools/selftest",    .method = HTTP_POST, .handler = selftest_post },
        { .uri = "/api/system/identify",   .method = HTTP_GET,  .handler = identify_get },
        { .uri = "/api/system/identify",   .method = HTTP_POST, .handler = identify_post },
    };
    _Static_assert(sizeof(routes) / sizeof(routes[0]) == WEB_TOOLS_ROUTES, "update WEB_TOOLS_ROUTES");
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        if (httpd_register_uri_handler(server, &routes[i]) != ESP_OK) {
            ESP_LOGE(TAG, "Could not register %s", routes[i].uri);
        }
    }
}
