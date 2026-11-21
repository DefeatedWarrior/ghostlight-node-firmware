/**
 * @file dmx_tools.c
 * @brief Output test patterns and loopback self-test — see dmx_tools.h
 */
#include "dmx_tools.h"
#include "dmx_handler.h"
#include "config_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

static const char *TAG = "dmx_tools";

#define PORTS CONFIG_DMX_PORTS

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static dmx_test_t s_test[PORTS];

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ============================================================================
// Test patterns
// ============================================================================

static const char *const k_names[] = {"off", "static", "full", "chase", "ramp"};

const char *dmx_test_mode_name(dmx_test_mode_t mode)
{
    return (mode <= DMX_TEST_LAST) ? k_names[mode] : "off";
}

int dmx_test_mode_from_name(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i <= DMX_TEST_LAST; i++) {
        if (strcasecmp(name, k_names[i]) == 0) return i;
    }
    return -1;
}

esp_err_t dmx_test_start(uint8_t port, const dmx_test_t *test)
{
    if (port < 1 || port > PORTS || !test || test->mode > DMX_TEST_LAST) {
        return ESP_ERR_INVALID_ARG;
    }
    dmx_test_t t = *test;
    if (t.first < 1) t.first = 1;
    if (t.first > 512) t.first = 512;
    if (t.last < t.first) t.last = t.first;
    if (t.last > 512) t.last = 512;
    if (t.speed_ms < 20) t.speed_ms = 20;
    if (t.speed_ms > 60000) t.speed_ms = 60000;

    taskENTER_CRITICAL(&s_lock);
    s_test[port - 1] = t;
    taskEXIT_CRITICAL(&s_lock);
    if (t.mode == DMX_TEST_OFF) {
        ESP_LOGI(TAG, "Port %u: test stopped", port);
    } else {
        ESP_LOGI(TAG, "Port %u: test \"%s\" ch %u-%u level %u speed %u ms", port,
                 dmx_test_mode_name(t.mode), t.first, t.last, t.level, t.speed_ms);
    }
    return ESP_OK;
}

esp_err_t dmx_test_stop(uint8_t port)
{
    dmx_test_t off = {.mode = DMX_TEST_OFF, .first = 1, .last = 512};
    return dmx_test_start(port, &off);
}

bool dmx_test_get(uint8_t port, dmx_test_t *test)
{
    if (port < 1 || port > PORTS) return false;
    taskENTER_CRITICAL(&s_lock);
    dmx_test_t t = s_test[port - 1];
    taskEXIT_CRITICAL(&s_lock);
    if (test) *test = t;
    return t.mode != DMX_TEST_OFF;
}

// ---------------------------------------------------------------------------
// Self-test state (declared here: dmx_test_fill() uses the pattern)
// ---------------------------------------------------------------------------

static struct {
    SemaphoreHandle_t mutex;
    dmx_selftest_result_t res;
    volatile bool running;
    volatile uint8_t drive_port;      /* port the pattern goes out on, 0 = none */
    uint8_t pattern[512];
    dmx_selftest_restore_cb_t restore;
    void *restore_ctx;
} s_st;

uint8_t dmx_test_active_mask(void)
{
    uint8_t m = 0;
    for (int p = 0; p < PORTS; p++) {
        if (s_test[p].mode != DMX_TEST_OFF) m |= 1u << p;
    }
    if (s_st.running && s_st.drive_port) m |= 1u << (s_st.drive_port - 1);
    return m;
}

bool dmx_test_fill(uint8_t port, uint8_t data[512])
{
    if (port < 1 || port > PORTS) return false;

    if (s_st.running && s_st.drive_port == port && s_st.mutex) {
        xSemaphoreTake(s_st.mutex, portMAX_DELAY);
        memcpy(data, s_st.pattern, 512);
        xSemaphoreGive(s_st.mutex);
        return true;
    }

    taskENTER_CRITICAL(&s_lock);
    dmx_test_t t = s_test[port - 1];
    taskEXIT_CRITICAL(&s_lock);
    if (t.mode == DMX_TEST_OFF) return false;

    memset(data, 0, 512);
    uint16_t n = t.last - t.first + 1;
    uint8_t *r = &data[t.first - 1];
    uint32_t ms = now_ms();
    switch (t.mode) {
    case DMX_TEST_STATIC:
        memset(r, t.level, n);
        break;
    case DMX_TEST_FULL:
        memset(r, 255, n);
        break;
    case DMX_TEST_CHASE:
        r[(ms / t.speed_ms) % n] = t.level;
        break;
    case DMX_TEST_RAMP: {
        uint32_t period = 2u * t.speed_ms;
        uint32_t ph = ms % period;
        uint32_t v = ph < t.speed_ms ? ph * 255u / t.speed_ms
                                     : (period - ph) * 255u / t.speed_ms;
        memset(r, (int)(v > 255 ? 255 : v), n);
        break;
    }
    default:
        break;
    }
    return true;
}

// ============================================================================
// Loopback self-test
// ============================================================================

#define ST_SETTLE_MS     400     /* after the ports start */
#define ST_STEP_WAIT_MS  1500    /* per pattern: time for a matching frame */
#define ST_TIMING_S      10

static const char *const k_patterns[DMX_SELFTEST_PATTERNS] = {
    "All zero", "All full", "Channel number", "Alternating 55/AA", "Pseudo-random",
};

static void build_pattern(int k, uint8_t *d)
{
    uint32_t lfsr = 0xACE1u;
    for (int i = 0; i < 512; i++) {
        switch (k) {
        case 0: d[i] = 0x00; break;
        case 1: d[i] = 0xFF; break;
        case 2: d[i] = (uint8_t)(i + 1); break;
        case 3: d[i] = (i & 1) ? 0xAA : 0x55; break;
        default:
            /* 16-bit Galois LFSR: every bit pattern, no long runs */
            lfsr = (lfsr >> 1) ^ (-(lfsr & 1u) & 0xB400u);
            d[i] = (uint8_t)lfsr;
            break;
        }
    }
}

void dmx_selftest_set_restore_callback(dmx_selftest_restore_cb_t cb, void *user_data)
{
    s_st.restore = NULL;
    s_st.restore_ctx = user_data;
    s_st.restore = cb;
}

bool dmx_selftest_running(void)
{
    return s_st.running;
}

void dmx_selftest_get(dmx_selftest_result_t *out)
{
    if (!out) return;
    if (!s_st.mutex) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_st.mutex, portMAX_DELAY);
    *out = s_st.res;
    xSemaphoreGive(s_st.mutex);
}

static void st_message(const char *msg)
{
    xSemaphoreTake(s_st.mutex, portMAX_DELAY);
    snprintf(s_st.res.message, sizeof(s_st.res.message), "%s", msg);
    xSemaphoreGive(s_st.mutex);
}

static port_config_t st_port(dmx_mode_t mode)
{
    port_config_t pc;
    memset(&pc, 0, sizeof(pc));
    pc.mode = mode;
    pc.universe_secondary = -1;
    pc.sacn_universe = 1;
    pc.out_rate_hz = CONFIG_DMX_RATE_DEFAULT;
    pc.break_us = CONFIG_DMX_BREAK_DEFAULT;
    pc.mab_us = CONFIG_DMX_MAB_DEFAULT;
    pc.out_slots = CONFIG_DMX_SLOTS_MAX;
    return pc;
}

static void selftest_task(void *arg)
{
    (void)arg;
    uint8_t out = s_st.res.out_port, in = s_st.res.in_port;
    uint8_t *rx = malloc(512);
    uint8_t *expect = malloc(512);
    bool ok = rx && expect;

    ESP_LOGW(TAG, "Self-test: port %u out -> port %u in", out, in);

    if (ok) {
        port_config_t oc = st_port(DMX_MODE_OUTPUT);
        port_config_t ic = st_port(DMX_MODE_INPUT);
        xSemaphoreTake(s_st.mutex, portMAX_DELAY);
        build_pattern(0, s_st.pattern);
        xSemaphoreGive(s_st.mutex);
        s_st.drive_port = out;
        if (dmx_handler_configure_port(out, &oc) != ESP_OK || dmx_handler_start_port(out) != ESP_OK ||
            dmx_handler_configure_port(in, &ic) != ESP_OK || dmx_handler_start_port(in) != ESP_OK) {
            st_message("Could not start the ports");
            ok = false;
        }
    } else {
        st_message("Out of memory");
    }

    if (ok) {
        dmx_handler_measure_timing(in, ST_TIMING_S);
        vTaskDelay(pdMS_TO_TICKS(ST_SETTLE_MS));
        dmx_handler_reset_analysis(in);
    }

    for (int k = 0; ok && k < DMX_SELFTEST_PATTERNS; k++) {
        build_pattern(k, expect);
        xSemaphoreTake(s_st.mutex, portMAX_DELAY);
        memcpy(s_st.pattern, expect, 512);
        xSemaphoreGive(s_st.mutex);

        dmx_selftest_step_t step = {.name = k_patterns[k]};
        uint32_t t0 = now_ms();
        vTaskDelay(pdMS_TO_TICKS(60));   /* at least one frame with the new data */
        while (now_ms() - t0 < ST_STEP_WAIT_MS) {
            dmx_port_status_t ps;
            if (dmx_handler_read_dmx(in, rx, 100) == ESP_OK &&
                dmx_handler_get_port_status(in, &ps) == ESP_OK) {
                step.slots = ps.stats.last_slot_count;
                step.mismatches = 0;
                step.first_bad = 0;
                for (int i = 0; i < 512; i++) {
                    if (rx[i] != expect[i]) {
                        if (!step.first_bad) {
                            step.first_bad = (uint16_t)(i + 1);
                            step.sent = expect[i];
                            step.got = rx[i];
                        }
                        step.mismatches++;
                    }
                }
                if (step.mismatches == 0 && step.slots == 512) {
                    step.passed = true;
                    break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        xSemaphoreTake(s_st.mutex, portMAX_DELAY);
        s_st.res.steps[k] = step;
        s_st.res.steps_done = (uint8_t)(k + 1);
        xSemaphoreGive(s_st.mutex);
        ESP_LOGI(TAG, "Self-test %-18s %s (%u slots, %u differ)", step.name,
                 step.passed ? "pass" : "FAIL", step.slots, step.mismatches);
    }

    dmx_input_analysis_t an;
    memset(&an, 0, sizeof(an));
    if (ok) dmx_handler_get_analysis(in, &an);
    dmx_handler_measure_timing(in, 0);
    s_st.drive_port = 0;

    bool all = ok;
    for (int k = 0; k < DMX_SELFTEST_PATTERNS; k++) all = all && s_st.res.steps[k].passed;
    uint32_t errs = an.err_framing + an.err_overflow;

    xSemaphoreTake(s_st.mutex, portMAX_DELAY);
    s_st.res.refresh_hz = an.refresh_hz;
    s_st.res.break_us = an.break_us;
    s_st.res.mab_us = an.mab_us;
    s_st.res.errors = errs;
    if (ok) {
        if (all && errs == 0) {
            snprintf(s_st.res.message, sizeof(s_st.res.message),
                     "Passed: port %u -> port %u, %u frames/s", out, in, an.refresh_hz);
        } else if (s_st.res.steps[0].slots == 0 && !s_st.res.steps[0].passed) {
            snprintf(s_st.res.message, sizeof(s_st.res.message),
                     "No DMX received on port %u: is port %u connected to it?", in, out);
            all = false;
        } else {
            snprintf(s_st.res.message, sizeof(s_st.res.message),
                     "Failed: data errors between port %u and port %u", out, in);
            all = false;
        }
    }
    s_st.res.state = all ? DMX_SELFTEST_PASSED : DMX_SELFTEST_FAILED;
    xSemaphoreGive(s_st.mutex);
    ESP_LOGW(TAG, "Self-test %s: %s", all ? "PASSED" : "FAILED", s_st.res.message);

    free(rx);
    free(expect);
    s_st.running = false;

    /* Put the saved settings back */
    dmx_selftest_restore_cb_t cb = s_st.restore;
    if (cb) cb(s_st.restore_ctx);
    vTaskDelete(NULL);
}

esp_err_t dmx_selftest_start(uint8_t out_port, uint8_t in_port)
{
    if (out_port < 1 || out_port > PORTS || in_port < 1 || in_port > PORTS || out_port == in_port) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_st.mutex) {
        s_st.mutex = xSemaphoreCreateMutex();
        if (!s_st.mutex) return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_st.mutex, portMAX_DELAY);
    if (s_st.running) {
        xSemaphoreGive(s_st.mutex);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_st.res, 0, sizeof(s_st.res));
    s_st.res.state = DMX_SELFTEST_RUNNING;
    s_st.res.out_port = out_port;
    s_st.res.in_port = in_port;
    for (int k = 0; k < DMX_SELFTEST_PATTERNS; k++) s_st.res.steps[k].name = k_patterns[k];
    snprintf(s_st.res.message, sizeof(s_st.res.message), "Running...");
    s_st.running = true;
    xSemaphoreGive(s_st.mutex);

    /* Stop pattern tests on both ports: the self-test owns them now */
    dmx_test_stop(out_port);
    dmx_test_stop(in_port);

    if (xTaskCreate(selftest_task, "dmx_selftest", 4096, NULL, 5, NULL) != pdPASS) {
        s_st.running = false;
        xSemaphoreTake(s_st.mutex, portMAX_DELAY);
        s_st.res.state = DMX_SELFTEST_FAILED;
        snprintf(s_st.res.message, sizeof(s_st.res.message), "Out of memory");
        xSemaphoreGive(s_st.mutex);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
