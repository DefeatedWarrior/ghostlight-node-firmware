/* Host stub: here so it picks up this test's FreeRTOS, not stubs/'s */
#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#define esp_task_wdt_add(t) ((void)(t), ESP_OK)
#define esp_task_wdt_delete(t) ((void)(t), ESP_OK)
#define esp_task_wdt_reset() ((void)0)
