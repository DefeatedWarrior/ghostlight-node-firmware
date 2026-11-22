/**
 * @file diag.h
 * @brief Remote diagnostics: in-memory log, crash report, reset reason
 *
 * diag_init() hooks the ESP-IDF log output: every line still goes to the
 * USB/UART console and is also kept in a RAM ring buffer (PSRAM) that the web
 * UI reads. Crash information comes from the core dump the panic handler
 * writes to the "coredump" flash partition.
 */
#ifndef DIAG_H
#define DIAG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Hook the log output and record why the chip last reset (call first) */
esp_err_t diag_init(void);

/**
 * Copy log text written since position @p *pos (a running byte count; 0 for
 * everything still buffered). On return *pos is the position to ask for next
 * time. Returns the number of bytes copied (lines are never split at the
 * start). *truncated is set when older text was already overwritten.
 */
size_t diag_log_read(uint32_t *pos, char *buf, size_t max, bool *truncated);
/** Size of the log ring buffer in bytes */
size_t diag_log_capacity(void);

/** Last reset reason as a short text ("power-on", "panic", "task watchdog", ...) */
const char *diag_reset_reason(void);
/** true if the last reset was a crash or a watchdog */
bool diag_reset_was_crash(void);

typedef struct {
    bool present;            /**< a valid core dump is stored */
    uint32_t size;           /**< bytes */
    char task[16];           /**< task that crashed */
    uint32_t pc;             /**< program counter */
    uint32_t backtrace[16];
    uint8_t depth;
    bool bt_corrupted;
    uint32_t exc_cause;      /**< Xtensa EXCCAUSE */
    uint32_t exc_vaddr;
    char reason[128];        /**< panic reason text, if recorded */
    char app_sha[17];        /**< firmware ELF SHA-256 prefix */
} diag_crash_t;

/** Summary of the stored core dump; present = false if there is none */
esp_err_t diag_crash_get(diag_crash_t *out);
/** Read raw core dump bytes (for download / idf.py coredump-info) */
esp_err_t diag_crash_read(uint32_t offset, void *buf, size_t len);
/** Delete the stored core dump */
esp_err_t diag_crash_erase(void);

/* ---- Memory and stack headroom ------------------------------------------- */

#define DIAG_STACKS_MAX 40
#define DIAG_STACK_LOW_BYTES     512      /**< a task this close to its stack end is at risk */
#define DIAG_INTERNAL_LOW_BYTES  (16 * 1024)   /**< internal RAM low-water mark worth a warning */

typedef struct {
    char name[16];
    uint32_t stack_free;     /**< least free stack since the task started, bytes */
} diag_stack_t;

typedef struct {
    uint32_t internal_free;      /**< internal RAM (DMA-able, what drivers and Wi-Fi need) */
    uint32_t internal_min_free;  /**< ... lowest since boot */
    uint32_t internal_largest;   /**< ... largest free block */
    uint32_t psram_free;         /**< 0 without PSRAM */
    uint32_t psram_min_free;
    int stacks;                  /**< entries in stack[], least free first */
    diag_stack_t stack[DIAG_STACKS_MAX];
} diag_health_t;

/** Free memory and every known task's stack headroom (looked up by name) */
void diag_health(diag_health_t *out);

#ifdef __cplusplus
}
#endif

#endif // DIAG_H
