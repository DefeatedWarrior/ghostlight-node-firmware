/* Forced into test_dmx_handler: what the handler gets from ESP-IDF */
#pragma once
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "esp_err.h"
#include <stdatomic.h>
#ifndef ESP_ERR_INVALID_RESPONSE
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_INVALID_CRC 0x109
#endif
