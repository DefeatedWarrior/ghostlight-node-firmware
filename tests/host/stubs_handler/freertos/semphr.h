/* Host stub for test_dmx_handler: a count, and waiting on zero is a failure */
#pragma once
#include "freertos/FreeRTOS.h"
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
