/**
 * @file diag.c
 * @brief Remote diagnostics — see diag.h
 */
#include "diag.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_core_dump.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "diag";

#define LOG_RING_PSRAM     (32 * 1024)
#define LOG_RING_INTERNAL  (8 * 1024)
#define LOG_LINE_MAX       256

static struct {
    char *ring;
    size_t cap;
    uint32_t head;            /* total bytes ever written */
    SemaphoreHandle_t mutex;
    vprintf_like_t prev;
    char line[LOG_LINE_MAX];
    uint8_t esc;              /* ANSI escape parser state */
    esp_reset_reason_t reset;
} s_diag;

// ============================================================================
// Log ring
// ============================================================================

/* Caller holds the mutex. Colour escape sequences are dropped. */
static void ring_append(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (s_diag.esc == 1) {
            s_diag.esc = (c == '[') ? 2 : 0;
            continue;
        }
        if (s_diag.esc == 2) {
            if (c >= 0x40 && c <= 0x7E) s_diag.esc = 0;
            continue;
        }
        if (c == 0x1B) {
            s_diag.esc = 1;
            continue;
        }
        if (c == '\r') continue;
        s_diag.ring[s_diag.head % s_diag.cap] = c;
        s_diag.head++;
    }
}

static int diag_vprintf(const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    int r = s_diag.prev ? s_diag.prev(fmt, ap) : vprintf(fmt, ap);

    if (s_diag.ring && !xPortInIsrContext() &&
        xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
        xSemaphoreTake(s_diag.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        int n = vsnprintf(s_diag.line, sizeof(s_diag.line), fmt, copy);
        if (n > 0) {
            if (n >= (int)sizeof(s_diag.line)) {
                n = sizeof(s_diag.line) - 1;
                s_diag.line[n - 1] = '\n';   /* truncated line: keep line breaks */
            }
            ring_append(s_diag.line, (size_t)n);
        }
        xSemaphoreGive(s_diag.mutex);
    }
    va_end(copy);
    return r;
}

size_t diag_log_read(uint32_t *pos, char *buf, size_t max, bool *truncated)
{
    if (truncated) *truncated = false;
    if (!pos || !buf || max == 0 || !s_diag.ring) return 0;

    xSemaphoreTake(s_diag.mutex, portMAX_DELAY);
    uint32_t head = s_diag.head;
    uint32_t oldest = head > s_diag.cap ? head - (uint32_t)s_diag.cap : 0;
    uint32_t p = *pos;
    bool cut = false;
    bool lost = (p != 0 && p < oldest) || p > head;
    if (p > head) p = oldest;              /* position from before a reboot */
    if (p < oldest) {
        p = oldest;
        cut = oldest > 0;
    }
    if (p == oldest && oldest > 0) cut = true;   /* first byte may be mid-line */
    size_t n = head - p;
    if (n > max) n = max;
    for (size_t i = 0; i < n; i++) {
        buf[i] = s_diag.ring[(p + i) % s_diag.cap];
    }
    xSemaphoreGive(s_diag.mutex);

    size_t skip = 0;
    if (cut) {
        while (skip < n && buf[skip] != '\n') skip++;
        if (skip < n) skip++;
        memmove(buf, buf + skip, n - skip);
    }
    if (truncated) *truncated = lost;
    *pos = p + (uint32_t)n;
    return n - skip;
}

size_t diag_log_capacity(void)
{
    return s_diag.cap;
}

// ============================================================================
// Reset reason
// ============================================================================

const char *diag_reset_reason(void)
{
    switch (s_diag.reset) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external reset";
    case ESP_RST_SW:        return "software restart";
    case ESP_RST_PANIC:     return "crash (panic)";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "wake from deep sleep";
    case ESP_RST_BROWNOUT:  return "brown-out (supply voltage dropped)";
    case ESP_RST_SDIO:      return "SDIO reset";
    default:                return "other";
    }
}

bool diag_reset_was_crash(void)
{
    return s_diag.reset == ESP_RST_PANIC || s_diag.reset == ESP_RST_INT_WDT ||
           s_diag.reset == ESP_RST_TASK_WDT || s_diag.reset == ESP_RST_WDT ||
           s_diag.reset == ESP_RST_BROWNOUT;
}

// ============================================================================
// Core dump
// ============================================================================

esp_err_t diag_crash_get(diag_crash_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_check() != ESP_OK ||
        esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0) {
        return ESP_OK;   /* none stored */
    }
    out->present = true;
    out->size = (uint32_t)size;

    esp_core_dump_summary_t *sum = heap_caps_malloc(sizeof(*sum), MALLOC_CAP_DEFAULT);
    if (sum) {
        if (esp_core_dump_get_summary(sum) == ESP_OK) {
            snprintf(out->task, sizeof(out->task), "%.*s", (int)sizeof(sum->exc_task) - 1,
                     sum->exc_task);
            out->pc = sum->exc_pc;
            uint32_t d = sum->exc_bt_info.depth;
            if (d > 16) d = 16;
            memcpy(out->backtrace, sum->exc_bt_info.bt, d * sizeof(uint32_t));
            out->depth = (uint8_t)d;
            out->bt_corrupted = sum->exc_bt_info.corrupted;
            out->exc_cause = sum->ex_info.exc_cause;
            out->exc_vaddr = sum->ex_info.exc_vaddr;
            snprintf(out->app_sha, sizeof(out->app_sha), "%.16s", (const char *)sum->app_elf_sha256);
        }
        free(sum);
    }
    if (esp_core_dump_get_panic_reason(out->reason, sizeof(out->reason)) != ESP_OK) {
        out->reason[0] = '\0';
    }
    return ESP_OK;
}

esp_err_t diag_crash_read(uint32_t offset, void *buf, size_t len)
{
    size_t addr = 0, size = 0;
    if (!buf) return ESP_ERR_INVALID_ARG;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0) return ESP_ERR_NOT_FOUND;
    if (offset > size || len > size - offset) return ESP_ERR_INVALID_SIZE;
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                           ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!part || addr < part->address) return ESP_ERR_NOT_FOUND;
    return esp_partition_read(part, (addr - part->address) + offset, buf, len);
}

esp_err_t diag_crash_erase(void)
{
    esp_err_t err = esp_core_dump_image_erase();
    if (err == ESP_OK) ESP_LOGI(TAG, "Crash report deleted");
    return err;
}

// ============================================================================
// Init
// ============================================================================

esp_err_t diag_init(void)
{
    if (s_diag.mutex) return ESP_ERR_INVALID_STATE;
    s_diag.reset = esp_reset_reason();
    s_diag.mutex = xSemaphoreCreateMutex();
    if (!s_diag.mutex) return ESP_ERR_NO_MEM;

    s_diag.ring = heap_caps_malloc(LOG_RING_PSRAM, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_diag.cap = LOG_RING_PSRAM;
    if (!s_diag.ring) {
        s_diag.ring = heap_caps_malloc(LOG_RING_INTERNAL, MALLOC_CAP_8BIT);
        s_diag.cap = LOG_RING_INTERNAL;
    }
    if (!s_diag.ring) {
        s_diag.cap = 0;
        ESP_LOGW(TAG, "No memory for the web log");
    } else {
        s_diag.prev = esp_log_set_vprintf(diag_vprintf);
    }

    ESP_LOGI(TAG, "Last reset: %s", diag_reset_reason());
    diag_crash_t c;
    if (diag_crash_get(&c) == ESP_OK && c.present) {
        ESP_LOGW(TAG, "Crash report stored: task \"%s\", PC 0x%08lx%s%s", c.task,
                 (unsigned long)c.pc, c.reason[0] ? ", " : "", c.reason);
    }
    return ESP_OK;
}
