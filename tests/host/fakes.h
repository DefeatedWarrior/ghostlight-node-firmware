/* Fakes behind the stub ESP-IDF headers (fakes.c) */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Fake clock in microseconds: esp_timer_get_time() and xTaskGetTickCount() */
extern int64_t host_now_us;
static inline void host_advance_ms(int64_t ms) { host_now_us += ms * 1000; }

/* Everything sent through sendto() since the last host_fakes_reset() */
#define HOST_SENT_MAX      16
#define HOST_SENT_MAX_LEN  1024
typedef struct {
    uint8_t data[HOST_SENT_MAX_LEN];
    size_t len;
    uint32_t dest_ip;          /* network byte order */
    uint16_t dest_port;        /* host byte order */
} host_sent_t;
extern host_sent_t host_sent[HOST_SENT_MAX];
extern int host_sent_count;    /* may exceed HOST_SENT_MAX; only the first are kept */

/* In-memory storage_manager: a few files */
extern int host_storage_writes;        /* storage_write_file() / storage_replace_file() calls */
extern bool host_storage_formatted;    /* what storage_was_formatted() says */
void host_storage_put(const char *name, const char *data, size_t len);
/* The file's contents (not terminated), NULL if there is no such file */
const char *host_storage_get(const char *name, size_t *len);

/* A receive buffer like the firmware's: @p bufsize bytes (the task's
 * malloc'd RX buffer) holding @p len received bytes. The bytes after @p len
 * are poisoned, so AddressSanitizer reports any read past the datagram. */
uint8_t *host_rx_buffer(const uint8_t *data, size_t len, size_t bufsize);
void host_rx_free(uint8_t *buf, size_t bufsize);

/* Clock back to 1 s, nothing sent, storage empty */
void host_fakes_reset(void);
