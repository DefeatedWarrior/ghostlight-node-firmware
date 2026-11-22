/* Host fakes for the ESP-IDF, FreeRTOS, lwIP, mbedTLS and storage_manager
 * functions the tested components call. See stubs/ for the headers. */
#include "fakes.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "mbedtls/pkcs5.h"
#include "storage_manager.h"
#include <stdlib.h>
#include <string.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define HOST_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define HOST_ASAN 1
#endif
#ifdef HOST_ASAN
#include <sanitizer/asan_interface.h>
#endif

int host_log_enabled;
int64_t host_now_us = 1000000;
host_sent_t host_sent[HOST_SENT_MAX];
int host_sent_count;
int host_storage_writes;
bool host_storage_formatted;

#define HOST_FILES 8
static struct {
    char name[64];
    char *data;
    size_t len;
} s_files[HOST_FILES];

__attribute__((constructor)) static void host_log_init(void)
{
    const char *e = getenv("HOST_LOG");
    host_log_enabled = e && e[0] == '1';
}

void host_fakes_reset(void)
{
    host_now_us = 1000000;
    host_sent_count = 0;
    memset(host_sent, 0, sizeof(host_sent));
    for (int i = 0; i < HOST_FILES; i++) {
        free(s_files[i].data);
        memset(&s_files[i], 0, sizeof(s_files[i]));
    }
    host_storage_writes = 0;
    host_storage_formatted = false;
}

uint8_t *host_rx_buffer(const uint8_t *data, size_t len, size_t bufsize)
{
    if (len > bufsize) len = bufsize;   /* recvfrom() truncates */
    uint8_t *b = malloc(bufsize);
    memcpy(b, data, len);
#ifdef HOST_ASAN
    ASAN_POISON_MEMORY_REGION(b + len, bufsize - len);
#endif
    return b;
}

void host_rx_free(uint8_t *buf, size_t bufsize)
{
#ifdef HOST_ASAN
    ASAN_UNPOISON_MEMORY_REGION(buf, bufsize);
#else
    (void)bufsize;
#endif
    free(buf);
}

/* ---- esp_timer / esp_mac / esp_random ---------------------------------- */

int64_t esp_timer_get_time(void) { return host_now_us; }

esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type)
{
    static const uint8_t m[6] = {0x24, 0x6F, 0x28, 0xAB, 0xCD, 0xEF};
    memcpy(mac, m, 6);
    mac[5] += (uint8_t)type;
    return ESP_OK;
}

static uint32_t s_rand = 0x12345678;
uint32_t esp_random(void)
{
    s_rand ^= s_rand << 13;
    s_rand ^= s_rand >> 17;
    s_rand ^= s_rand << 5;
    return s_rand;
}

void esp_fill_random(void *buf, size_t len)
{
    uint8_t *b = buf;
    for (size_t i = 0; i < len; i++) b[i] = (uint8_t)esp_random();
}

/* ---- FreeRTOS ----------------------------------------------------------- */

TickType_t xTaskGetTickCount(void)
{
    return (TickType_t)(host_now_us / 1000 / portTICK_PERIOD_MS);
}
void vTaskDelay(TickType_t ticks) { host_now_us += (int64_t)ticks * portTICK_PERIOD_MS * 1000; }
void vTaskDelete(TaskHandle_t task) { (void)task; }
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack,
                                   void *arg, UBaseType_t prio, TaskHandle_t *handle,
                                   BaseType_t core)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)prio; (void)handle; (void)core;
    return pdFAIL;   /* the tests call the packet handlers directly */
}

static int s_sem_dummy;
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &s_sem_dummy; }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return &s_sem_dummy; }
void vSemaphoreDelete(SemaphoreHandle_t s) { (void)s; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait) { (void)s; (void)wait; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return &s_sem_dummy; }
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t wait) { (void)s; (void)wait; return pdTRUE; }
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }

/* ---- lwIP --------------------------------------------------------------- */

ssize_t host_fake_sendto(int sock, const void *buf, size_t len, int flags,
                         const struct sockaddr *to, socklen_t tolen)
{
    (void)sock; (void)flags; (void)tolen;
    if (host_sent_count < HOST_SENT_MAX) {
        host_sent_t *s = &host_sent[host_sent_count];
        s->len = len < HOST_SENT_MAX_LEN ? len : HOST_SENT_MAX_LEN;
        memcpy(s->data, buf, s->len);
        const struct sockaddr_in *in = (const struct sockaddr_in *)to;
        s->dest_ip = in->sin_addr.s_addr;
        s->dest_port = ntohs(in->sin_port);
    }
    host_sent_count++;
    return (ssize_t)len;
}

/* ---- mbedTLS (not a real KDF, see stubs/mbedtls/pkcs5.h) ---------------- */

int mbedtls_pkcs5_pbkdf2_hmac_ext(mbedtls_md_type_t md_type,
                                  const unsigned char *password, size_t plen,
                                  const unsigned char *salt, size_t slen,
                                  unsigned int iteration_count,
                                  uint32_t key_length, unsigned char *output)
{
    (void)md_type;
    uint64_t h = 1469598103934665603ull ^ iteration_count;
    for (size_t i = 0; i < plen; i++) h = (h ^ password[i]) * 1099511628211ull;
    h = (h ^ 0xFF) * 1099511628211ull;
    for (size_t i = 0; i < slen; i++) h = (h ^ salt[i]) * 1099511628211ull;
    for (uint32_t i = 0; i < key_length; i++) {
        h = (h ^ i) * 1099511628211ull;
        output[i] = (unsigned char)(h >> 56);
    }
    return 0;
}

/* ---- storage_manager (same contract as storage_manager.c) --------------- */

static int find_file(const char *name)
{
    for (int i = 0; i < HOST_FILES; i++) {
        if (s_files[i].data && strcmp(s_files[i].name, name) == 0) return i;
    }
    return -1;
}

static void drop_file(int i)
{
    free(s_files[i].data);
    memset(&s_files[i], 0, sizeof(s_files[i]));
}

void host_storage_put(const char *name, const char *data, size_t len)
{
    int i = find_file(name);
    if (i < 0) {
        for (i = 0; i < HOST_FILES && s_files[i].data; i++) {}
        if (i == HOST_FILES) abort();   /* more files than a test needs */
    } else {
        free(s_files[i].data);
    }
    s_files[i].data = malloc(len ? len : 1);
    if (len) memcpy(s_files[i].data, data, len);
    s_files[i].len = len;
    snprintf(s_files[i].name, sizeof(s_files[i].name), "%s", name);
}

const char *host_storage_get(const char *name, size_t *len)
{
    int i = find_file(name);
    if (i < 0) return NULL;
    if (len) *len = s_files[i].len;
    return s_files[i].data;
}

esp_err_t storage_init(void) { return ESP_OK; }
esp_err_t storage_deinit(void) { return ESP_OK; }
bool storage_was_formatted(void) { return host_storage_formatted; }
void storage_lock(void) {}
void storage_unlock(void) {}

bool storage_file_exists(const char *path)
{
    return find_file(path) >= 0;
}

/* Like the real one: reads at most *size - 1 bytes and NUL-terminates */
esp_err_t storage_read_file(const char *path, char *buffer, size_t *size)
{
    int i = find_file(path);
    if (i < 0 || *size == 0) return ESP_FAIL;
    size_t n = s_files[i].len < *size - 1 ? s_files[i].len : *size - 1;
    memcpy(buffer, s_files[i].data, n);
    buffer[n] = '\0';
    *size = n;
    return ESP_OK;
}

esp_err_t storage_write_file(const char *path, const char *data, size_t size)
{
    host_storage_put(path, data, size);
    host_storage_writes++;
    return ESP_OK;
}

esp_err_t storage_rename_file(const char *from, const char *to)
{
    int i = find_file(from);
    if (i < 0) return ESP_FAIL;
    int j = find_file(to);
    if (j >= 0) drop_file(j);
    snprintf(s_files[i].name, sizeof(s_files[i].name), "%s", to);
    return ESP_OK;
}

esp_err_t storage_replace_file(const char *path, const char *previous, const char *data,
                               size_t size)
{
    if (find_file(path) >= 0) storage_rename_file(path, previous);
    return storage_write_file(path, data, size);
}

esp_err_t storage_delete_file(const char *path)
{
    int i = find_file(path);
    if (i < 0) return ESP_FAIL;
    drop_file(i);
    return ESP_OK;
}
