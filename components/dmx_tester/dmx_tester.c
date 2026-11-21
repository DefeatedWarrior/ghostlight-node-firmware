/*
 * DMX tester mode: levels for send ports, statistics for receive ports.
 * See dmx_tester.h.
 *
 * dmx_tester_on_frame() runs on the port's input task for every frame;
 * the web server reads the statistics from its own task. A mutex per port
 * keeps the two apart (held for one pass over 512 slots).
 */
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "config_manager.h"
#include "dmx_tester.h"

static const char *TAG = "dmx_tester";

#define NPORTS            CONFIG_DMX_PORTS
#define GAP_US            (1000 * 1000)   /* intervals longer than this are gaps */
#define WINDOW_US         (1000 * 1000)   /* flicker watch window */

const uint16_t DMX_TESTER_HIST_EDGES_MS[DMX_TESTER_HIST_BUCKETS - 1] = {
    20, 24, 28, 35, 50, 100, 250, 1000
};

typedef struct {
    SemaphoreHandle_t lock;
    uint8_t out[512];              /* send: levels */

    /* receive */
    uint8_t last[512];
    uint8_t min[512];
    uint8_t max[512];
    uint32_t changes[512];
    uint8_t win_cur[64];           /* changed in the running window */
    uint8_t win_prev[64];          /* changed in the window before */
    int64_t win_start_us;
    int64_t reset_us;
    int64_t last_frame_us;
    int64_t last_loss_us;
    uint32_t frames;
    uint32_t losses;
    uint64_t sum_us;               /* intervals up to GAP_US */
    uint64_t sumsq_us;
    uint32_t n_intervals;
    uint32_t min_us, max_us;
    uint32_t hist[DMX_TESTER_HIST_BUCKETS];
} tport_t;

static tport_t *s_ports[NPORTS];
static bool s_enabled;

static tport_t *get(uint8_t port)
{
    return (port >= 1 && port <= NPORTS) ? s_ports[port - 1] : NULL;
}

static void reset_rx(tport_t *t)
{
    memset(t->last, 0, sizeof(t->last));
    memset(t->min, 0xFF, sizeof(t->min));
    memset(t->max, 0, sizeof(t->max));
    memset(t->changes, 0, sizeof(t->changes));
    memset(t->win_cur, 0, sizeof(t->win_cur));
    memset(t->win_prev, 0, sizeof(t->win_prev));
    memset(t->hist, 0, sizeof(t->hist));
    int64_t now = esp_timer_get_time();
    t->win_start_us = now;
    t->reset_us = now;
    t->last_frame_us = 0;
    t->last_loss_us = 0;
    t->frames = 0;
    t->losses = 0;
    t->sum_us = 0;
    t->sumsq_us = 0;
    t->n_intervals = 0;
    t->min_us = 0;
    t->max_us = 0;
}

esp_err_t dmx_tester_init(bool enabled)
{
    s_enabled = enabled;
    if (!enabled) return ESP_OK;
    for (int i = 0; i < NPORTS; i++) {
        if (s_ports[i]) continue;
        tport_t *t = calloc(1, sizeof(tport_t));
        if (!t) return ESP_ERR_NO_MEM;
        t->lock = xSemaphoreCreateMutex();
        if (!t->lock) {
            free(t);
            return ESP_ERR_NO_MEM;
        }
        reset_rx(t);
        s_ports[i] = t;
    }
    ESP_LOGI(TAG, "Tester buffers ready (%u bytes per port)", (unsigned)sizeof(tport_t));
    return ESP_OK;
}

bool dmx_tester_enabled(void)
{
    return s_enabled;
}

// ============================================================================
// Send
// ============================================================================

esp_err_t dmx_tester_set_levels(uint8_t port, uint16_t start, const uint8_t *values, uint16_t count)
{
    tport_t *t = get(port);
    if (!t) return s_enabled ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    if (!values || start < 1 || start > 512) return ESP_ERR_INVALID_ARG;
    if (count > 513 - start) count = 513 - start;
    memcpy(&t->out[start - 1], values, count);   /* single bytes: no tearing worth a lock */
    return ESP_OK;
}

esp_err_t dmx_tester_fill(uint8_t port, uint16_t first, uint16_t last, uint8_t value)
{
    tport_t *t = get(port);
    if (!t) return s_enabled ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    if (first < 1 || last > 512 || first > last) return ESP_ERR_INVALID_ARG;
    memset(&t->out[first - 1], value, last - first + 1);
    return ESP_OK;
}

void dmx_tester_get_levels(uint8_t port, uint8_t data[512])
{
    tport_t *t = get(port);
    if (t) {
        memcpy(data, t->out, 512);
    } else {
        memset(data, 0, 512);
    }
}

// ============================================================================
// Receive
// ============================================================================

static int hist_bucket(uint32_t us)
{
    uint32_t ms = us / 1000;
    for (int k = 0; k < DMX_TESTER_HIST_BUCKETS - 1; k++) {
        if (ms < DMX_TESTER_HIST_EDGES_MS[k]) return k;
    }
    return DMX_TESTER_HIST_BUCKETS - 1;
}

void dmx_tester_on_frame(uint8_t port, const uint8_t data[512])
{
    tport_t *t = get(port);
    if (!t) return;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(t->lock, portMAX_DELAY);

    if (now - t->win_start_us >= WINDOW_US) {
        if (now - t->win_start_us >= 2 * WINDOW_US) {
            memset(t->win_prev, 0, 64);   /* no frames for a while */
        } else {
            memcpy(t->win_prev, t->win_cur, 64);
        }
        memset(t->win_cur, 0, 64);
        t->win_start_us = now;
    }

    bool first = t->frames == 0;
    for (int c = 0; c < 512; c++) {
        uint8_t v = data[c];
        if (v < t->min[c]) t->min[c] = v;
        if (v > t->max[c]) t->max[c] = v;
        if (!first && v != t->last[c]) {
            t->changes[c]++;
            t->win_cur[c >> 3] |= (uint8_t)(1u << (c & 7));
        }
        t->last[c] = v;
    }

    if (t->last_frame_us) {
        uint32_t iv = (uint32_t)(now - t->last_frame_us);
        t->hist[hist_bucket(iv)]++;
        if (t->min_us == 0 || iv < t->min_us) t->min_us = iv;
        if (iv > t->max_us) t->max_us = iv;
        if (iv <= GAP_US) {
            t->sum_us += iv;
            t->sumsq_us += (uint64_t)iv * iv;
            t->n_intervals++;
        }
    }
    t->last_frame_us = now;
    t->frames++;
    xSemaphoreGive(t->lock);
}

void dmx_tester_on_loss(uint8_t port)
{
    tport_t *t = get(port);
    if (!t) return;
    xSemaphoreTake(t->lock, portMAX_DELAY);
    t->losses++;
    t->last_loss_us = esp_timer_get_time();
    t->last_frame_us = 0;   /* the gap is a loss, not a long frame interval */
    xSemaphoreGive(t->lock);
}

esp_err_t dmx_tester_reset(uint8_t port)
{
    tport_t *t = get(port);
    if (!t) return s_enabled ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    xSemaphoreTake(t->lock, portMAX_DELAY);
    reset_rx(t);
    xSemaphoreGive(t->lock);
    return ESP_OK;
}

/* Channels that changed in the last full second (the window before the
 * running one, or nothing if no frame arrived for a while) */
static const uint8_t *recent_mask(const tport_t *t, int64_t now)
{
    static const uint8_t none[64];
    int64_t age = now - t->win_start_us;
    if (age >= 2 * WINDOW_US) return none;
    return age >= WINDOW_US ? t->win_cur : t->win_prev;
}

esp_err_t dmx_tester_get_stats(uint8_t port, dmx_tester_stats_t *out)
{
    tport_t *t = get(port);
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!t) return s_enabled ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    memset(out, 0, sizeof(*out));
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(t->lock, portMAX_DELAY);
    out->frames = t->frames;
    out->seconds = (uint32_t)((now - t->reset_us) / 1000000);
    out->losses = t->losses;
    out->last_loss_s = t->last_loss_us ? (int32_t)((now - t->last_loss_us) / 1000000) : -1;
    if (t->n_intervals) {
        double mean = (double)t->sum_us / t->n_intervals;
        double var = (double)t->sumsq_us / t->n_intervals - mean * mean;
        out->interval_avg_us = (uint32_t)(mean + 0.5);
        out->interval_jitter_us = var > 0 ? (uint32_t)(sqrt(var) + 0.5) : 0;
    }
    out->interval_min_us = t->min_us;
    out->interval_max_us = t->max_us;
    memcpy(out->hist, t->hist, sizeof(out->hist));
    const uint8_t *mask = recent_mask(t, now);
    uint16_t changing = 0, changed = 0;
    for (int c = 0; c < 512; c++) {
        if (mask[c >> 3] & (1u << (c & 7))) changing++;
        if (t->changes[c]) changed++;
    }
    out->changing = changing;
    out->changed = changed;
    xSemaphoreGive(t->lock);
    return ESP_OK;
}

esp_err_t dmx_tester_get_channels(uint8_t port, uint8_t min[512], uint8_t max[512],
                                  uint32_t changes[512], uint8_t changing[64])
{
    tport_t *t = get(port);
    if (!t) return s_enabled ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(t->lock, portMAX_DELAY);
    bool any = t->frames > 0;
    if (min) {
        if (any) memcpy(min, t->min, 512);
        else memset(min, 0, 512);
    }
    if (max) memcpy(max, t->max, 512);
    if (changes) memcpy(changes, t->changes, sizeof(t->changes));
    if (changing) memcpy(changing, recent_mask(t, now), 64);
    xSemaphoreGive(t->lock);
    return ESP_OK;
}
