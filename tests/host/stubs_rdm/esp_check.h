/* Host stub */
#pragma once
#include "freertos/FreeRTOS.h"
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGD(tag, ...) ((void)(tag))
#define ESP_RETURN_ON_FALSE(a, err, tag, ...) do { if (!(a)) { (void)(tag); return (err); } } while (0)
