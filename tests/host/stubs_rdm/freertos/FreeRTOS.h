/* Host stub for test_dmx_rdm: one task on a simulated clock. Waiting runs the
 * wire, the timer and the interrupts (see test_dmx_rdm.c). */
#pragma once
#include "sdkconfig.h"
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef int esp_err_t;
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
typedef struct { int64_t at; } TimeOut_t;
typedef struct { int unused; } spinlock_t;
typedef enum { eNoAction, eSetValueWithOverwrite } eNotifyAction;
#define ESP_OK 0
#define ESP_FAIL -1
#define IRAM_ATTR
#define configTICK_RATE_HZ CONFIG_FREERTOS_HZ
/* loud instead of undefined, so a test can see it being reached */
#define __unreachable() abort()
#undef ULONG_MAX
#define ULONG_MAX 0xFFFFFFFFu
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdTRUE 1
#define pdFALSE 0
#define portTICK_PERIOD_MS 1
#define portMUX_INITIALIZER_UNLOCKED { 0 }
#define portYIELD_FROM_ISR() ((void)0)
extern int host_critical;
#define taskENTER_CRITICAL(m) ((void)sizeof(m), host_critical++)
#define taskEXIT_CRITICAL(m) ((void)sizeof(m), host_critical--)
#define taskENTER_CRITICAL_ISR(m) ((void)sizeof(m), host_critical++)
#define taskEXIT_CRITICAL_ISR(m) ((void)sizeof(m), host_critical--)
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t wait);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s);
void vSemaphoreDelete(SemaphoreHandle_t s);
void vTaskSetTimeOutState(TimeOut_t *t);
BaseType_t xTaskCheckForTimeOut(TimeOut_t *t, TickType_t *wait);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
BaseType_t xTaskNotifyWait(uint32_t clr_in, uint32_t clr_out, uint32_t *value, TickType_t ticks);
BaseType_t xTaskNotifyStateClear(TaskHandle_t task);
BaseType_t xTaskNotifyFromISR(TaskHandle_t task, uint32_t value, eNotifyAction a, int *woken);

/* the bits of ESP-IDF that driver.c uses */
#define ESP_INTR_FLAG_LEVEL1 (1 << 1)
#define ESP_INTR_FLAG_LOWMED (7 << 1)
#define ESP_INTR_FLAG_IRAM (1 << 10)
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define heap_caps_malloc(n, caps) malloc(n)
#define heap_caps_free(p) free(p)
#define __XS_(x) #x
#define __XSTRING(x) __XS_(x)
