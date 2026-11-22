/* Host stub: one thread, so the spinlocks have nothing to do */
#pragma once
#include "sdkconfig.h"
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef int esp_err_t;
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
typedef struct { int unused; } TimeOut_t;
typedef struct { int unused; } spinlock_t;
typedef enum { eNoAction, eSetValueWithOverwrite } eNotifyAction;
#define ESP_OK 0
#define IRAM_ATTR
#define configTICK_RATE_HZ CONFIG_FREERTOS_HZ
/* newlib's, and long is 32 bits on the chip */
#define __unreachable() __builtin_unreachable()
#undef ULONG_MAX
#define ULONG_MAX 0xFFFFFFFFu
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdTRUE 1
#define pdFALSE 0
#define portTICK_PERIOD_MS 1
#define portMUX_INITIALIZER_UNLOCKED { 0 }
#define portYIELD_FROM_ISR() ((void)0)
#define taskENTER_CRITICAL(m) ((void)sizeof(m))
#define taskEXIT_CRITICAL(m) ((void)sizeof(m))
#define taskENTER_CRITICAL_ISR(m) ((void)sizeof(m))
#define taskEXIT_CRITICAL_ISR(m) ((void)sizeof(m))
#define xSemaphoreTakeRecursive(s, t) ((void)(s), (void)(t), pdTRUE)
#define xSemaphoreGiveRecursive(s) ((void)(s))
#define vTaskSetTimeOutState(t) ((void)(t))
#define xTaskCheckForTimeOut(t, w) ((void)(t), (void)(w), pdFALSE)
TaskHandle_t xTaskGetCurrentTaskHandle(void);
BaseType_t xTaskNotifyWait(uint32_t clr_in, uint32_t clr_out, uint32_t *value, TickType_t ticks);
BaseType_t xTaskNotifyStateClear(TaskHandle_t task);
BaseType_t xTaskNotifyFromISR(TaskHandle_t task, uint32_t value, eNotifyAction a, int *woken);
