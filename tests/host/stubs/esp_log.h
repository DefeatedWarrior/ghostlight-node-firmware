/* Host stub of esp_log.h: printed to stderr only when HOST_LOG=1 is set in
 * the environment, so the format strings are still type-checked. */
#pragma once
#include <stdio.h>
extern int host_log_enabled;
#define HOST_LOG_(lvl, tag, fmt, ...) do {                                  \
        if (host_log_enabled) fprintf(stderr, lvl " (%s) " fmt "\n", tag, ##__VA_ARGS__); \
    } while (0)
#define ESP_LOGE(tag, fmt, ...) HOST_LOG_("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) HOST_LOG_("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) HOST_LOG_("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) HOST_LOG_("D", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) HOST_LOG_("V", tag, fmt, ##__VA_ARGS__)
