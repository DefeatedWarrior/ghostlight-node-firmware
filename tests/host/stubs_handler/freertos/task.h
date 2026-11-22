/* Host stub for test_dmx_handler: tasks are run by the test */
#pragma once
#include "freertos/FreeRTOS.h"
#define pdPASS 1
typedef void (*TaskFunction_t)(void *);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                   int priority, TaskHandle_t *out, int core);
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t task);
