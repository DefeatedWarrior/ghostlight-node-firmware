/* Host stub of the FreeRTOS API the tested components use. Single-threaded:
 * semaphores always succeed, tasks are never created, the tick count follows
 * the fake clock (fakes.h). */
#pragma once
#include "sdkconfig.h"
#include <stdint.h>
#include <stddef.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0
#define portMAX_DELAY ((TickType_t)0xFFFFFFFFu)
#define portTICK_PERIOD_MS (1000 / CONFIG_FREERTOS_HZ)
#define pdMS_TO_TICKS(ms) ((TickType_t)(((uint64_t)(ms) * CONFIG_FREERTOS_HZ) / 1000))
