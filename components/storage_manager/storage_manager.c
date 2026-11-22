#include "storage_manager.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static const char *TAG = "storage";
static const char *PARTITION = "littlefs";
static bool s_initialized = false;
static bool s_formatted = false;
static SemaphoreHandle_t s_lock;   /* recursive: one file operation at a time */

void storage_lock(void)
{
    if (s_lock) xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

void storage_unlock(void)
{
    if (s_lock) xSemaphoreGiveRecursive(s_lock);
}

/* True if the start of the partition was never written (all 0xFF): a board
 * that was just erased and installed, as opposed to a damaged file system.
 * LittleFS keeps its two superblocks in the first two blocks. */
static bool partition_blank(void)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                           ESP_PARTITION_SUBTYPE_ANY, PARTITION);
    if (!part) return false;
    enum { CHUNK = 256, SPAN = 2 * 4096 };
    uint8_t *buf = malloc(CHUNK);
    if (!buf) return false;
    bool blank = true;
    for (size_t off = 0; blank && off < SPAN && off + CHUNK <= part->size; off += CHUNK) {
        if (esp_partition_read(part, off, buf, CHUNK) != ESP_OK) {
            blank = false;
            break;
        }
        for (size_t i = 0; i < CHUNK; i++) {
            if (buf[i] != 0xFF) {
                blank = false;
                break;
            }
        }
    }
    free(buf);
    return blank;
}

esp_err_t storage_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateRecursiveMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }

    /* Format by hand rather than with format_if_mount_failed, so a damaged
     * file system is reported (storage_was_formatted()) instead of the
     * settings silently going back to factory defaults. */
    esp_vfs_littlefs_conf_t conf = {
        .base_path = STORAGE_BASE_PATH,
        .partition_label = PARTITION,
        .format_if_mount_failed = false,
        .dont_mount = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        bool blank = partition_blank();
        if (blank) {
            ESP_LOGI(TAG, "Storage is empty (first start), formatting");
        } else {
            ESP_LOGE(TAG, "Storage could not be mounted (%s): formatting it, the settings "
                     "and scenes on it are lost", esp_err_to_name(ret));
        }
        ret = esp_littlefs_format(PARTITION);
        if (ret == ESP_OK) {
            ret = esp_vfs_littlefs_register(&conf);
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to mount LittleFS: %s", esp_err_to_name(ret));
            return ret;
        }
        s_formatted = !blank;
    }

    size_t total = 0, used = 0;
    ret = esp_littlefs_info(PARTITION, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS: total=%d KB, used=%d KB", total/1024, used/1024);
    }

    s_initialized = true;
    return ESP_OK;
}

bool storage_was_formatted(void)
{
    return s_formatted;
}

esp_err_t storage_deinit(void)
{
    if (!s_initialized) return ESP_OK;

    storage_lock();
    esp_err_t ret = esp_vfs_littlefs_unregister(PARTITION);
    s_initialized = false;
    storage_unlock();
    return ret;
}

esp_err_t storage_read_file(const char *path, char *buffer, size_t *size)
{
    char full_path[128];
    snprintf(full_path, sizeof(full_path), "%s/%s", STORAGE_BASE_PATH, path);

    storage_lock();
    FILE *f = fopen(full_path, "r");
    if (!f) {
        storage_unlock();
        ESP_LOGE(TAG, "Failed to open file: %s", full_path);
        return ESP_FAIL;
    }

    size_t read_size = fread(buffer, 1, *size - 1, f);
    bool err = ferror(f) != 0;
    buffer[read_size] = '\0';
    *size = read_size;

    fclose(f);
    storage_unlock();
    if (err) {
        ESP_LOGE(TAG, "Read error in %s", path);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Read %d bytes from %s", read_size, path);
    return ESP_OK;
}

/* Write @p data to "<path>.tmp" and flush it to flash */
static esp_err_t write_tmp(const char *tmp_path, const char *path, const char *data, size_t size)
{
    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to create file: %s", tmp_path);
        return ESP_FAIL;
    }

    size_t written = fwrite(data, 1, size, f);
    bool ok = (written == size) && (fflush(f) == 0) && (fsync(fileno(f)) == 0);
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        ESP_LOGE(TAG, "Write failed for %s (%u of %u bytes)", path,
                 (unsigned)written, (unsigned)size);
        unlink(tmp_path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/*
 * Write to a temporary file and rename it over the target. LittleFS rename
 * replaces the target atomically, so a power cut mid-save leaves either the
 * old or the new file intact, never a truncated one. A failed rename leaves
 * the old file alone: deleting it first would lose the settings.
 */
static esp_err_t write_file(const char *path, const char *previous, const char *data, size_t size)
{
    char full_path[128];
    char tmp_path[136];
    char prev_path[136];
    snprintf(full_path, sizeof(full_path), "%s/%s", STORAGE_BASE_PATH, path);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", full_path);
    if (previous) {
        snprintf(prev_path, sizeof(prev_path), "%s/%s", STORAGE_BASE_PATH, previous);
    }

    storage_lock();
    esp_err_t err = write_tmp(tmp_path, path, data, size);
    if (err != ESP_OK) {
        storage_unlock();
        return err;
    }

    struct stat st;
    if (previous && stat(full_path, &st) == 0 && rename(full_path, prev_path) != 0) {
        /* Not fatal: the new file still replaces the old one below */
        ESP_LOGW(TAG, "Could not keep the previous %s", path);
    }
    if (rename(tmp_path, full_path) != 0) {
        ESP_LOGE(TAG, "Failed to rename %s", tmp_path);
        unlink(tmp_path);
        storage_unlock();
        return ESP_FAIL;
    }
    storage_unlock();

    ESP_LOGI(TAG, "Wrote %u bytes to %s", (unsigned)size, path);
    return ESP_OK;
}

esp_err_t storage_write_file(const char *path, const char *data, size_t size)
{
    return write_file(path, NULL, data, size);
}

esp_err_t storage_replace_file(const char *path, const char *previous, const char *data,
                               size_t size)
{
    return write_file(path, previous, data, size);
}

esp_err_t storage_rename_file(const char *from, const char *to)
{
    char from_path[128];
    char to_path[128];
    snprintf(from_path, sizeof(from_path), "%s/%s", STORAGE_BASE_PATH, from);
    snprintf(to_path, sizeof(to_path), "%s/%s", STORAGE_BASE_PATH, to);

    storage_lock();
    int r = rename(from_path, to_path);
    storage_unlock();
    if (r != 0) {
        ESP_LOGE(TAG, "Failed to rename %s to %s", from, to);
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool storage_file_exists(const char *path)
{
    char full_path[128];
    snprintf(full_path, sizeof(full_path), "%s/%s", STORAGE_BASE_PATH, path);

    struct stat st;
    storage_lock();
    bool found = stat(full_path, &st) == 0;
    storage_unlock();
    return found;
}

esp_err_t storage_delete_file(const char *path)
{
    char full_path[128];
    snprintf(full_path, sizeof(full_path), "%s/%s", STORAGE_BASE_PATH, path);

    storage_lock();
    int r = unlink(full_path);
    storage_unlock();
    if (r == 0) {
        ESP_LOGI(TAG, "Deleted file: %s", path);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to delete: %s", path);
    return ESP_FAIL;
}
