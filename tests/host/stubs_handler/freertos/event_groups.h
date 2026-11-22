/* Host stub */
#pragma once
#include "freertos/FreeRTOS.h"
typedef void *EventGroupHandle_t;
typedef uint32_t EventBits_t;
#define BIT0 1u
EventGroupHandle_t xEventGroupCreate(void);
void vEventGroupDelete(EventGroupHandle_t e);
EventBits_t xEventGroupSetBits(EventGroupHandle_t e, EventBits_t bits);
EventBits_t xEventGroupClearBits(EventGroupHandle_t e, EventBits_t bits);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t e, EventBits_t bits, BaseType_t clear,
                                BaseType_t all, TickType_t wait);
