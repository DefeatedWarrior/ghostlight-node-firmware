#ifndef STORAGE_MANAGER_H
#define STORAGE_MANAGER_H

#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>

#define STORAGE_BASE_PATH "/littlefs"

/**
 * @brief Initialize LittleFS storage
 * @return ESP_OK on success
 */
esp_err_t storage_init(void);

/**
 * @brief Deinitialize storage
 * @return ESP_OK on success
 */
esp_err_t storage_deinit(void);

/**
 * @brief Read file from storage
 * @param path File path relative to base path
 * @param buffer Output buffer
 * @param size Buffer size (in), bytes read (out)
 * @return ESP_OK on success
 */
esp_err_t storage_read_file(const char *path, char *buffer, size_t *size);

/**
 * @brief Write file to storage
 * @param path File path relative to base path
 * @param data Data to write
 * @param size Data size
 * @return ESP_OK on success
 */
esp_err_t storage_write_file(const char *path, const char *data, size_t size);

/**
 * @brief Write a file like storage_write_file(), keeping the file it
 *        replaces as @p previous (replacing any older copy). A power cut
 *        during the save leaves @p path or, between the two renames,
 *        @p previous readable.
 */
esp_err_t storage_replace_file(const char *path, const char *previous, const char *data,
                               size_t size);

/** Rename a file (both paths relative to the base path); replaces @p to */
esp_err_t storage_rename_file(const char *from, const char *to);

/**
 * @brief True if the storage could not be mounted at boot and was formatted,
 *        losing what was on it (not set on the first start of an erased board)
 */
bool storage_was_formatted(void);

/**
 * @brief Hold off other file operations (recursive). The functions here take
 *        it themselves; code that reads files directly with fopen() takes it
 *        so a save can't rename a file it has open.
 */
void storage_lock(void);
void storage_unlock(void);

/**
 * @brief Check if file exists
 * @param path File path
 * @return true if exists
 */
bool storage_file_exists(const char *path);

/**
 * @brief Delete file
 * @param path File path
 * @return ESP_OK on success
 */
esp_err_t storage_delete_file(const char *path);

#endif
