/* Host stub: no task watchdog on the host */
#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static inline esp_err_t esp_task_wdt_add(TaskHandle_t task) { (void)task; return ESP_OK; }
static inline esp_err_t esp_task_wdt_delete(TaskHandle_t task) { (void)task; return ESP_OK; }
static inline esp_err_t esp_task_wdt_reset(void) { return ESP_OK; }
static inline esp_err_t esp_task_wdt_status(TaskHandle_t task) { (void)task; return ESP_OK; }
