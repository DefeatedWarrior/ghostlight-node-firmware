/* Host stub: set-up calls, compiled but never run */
#pragma once
#include "hal/uart_hal.h"
typedef void *intr_handle_t;
typedef int periph_module_t;
#define PERIPH_UART0_MODULE 0
#define uart_get_sclk_freq(s, f) ((void)(*(f) = 80000000u))
#define uart_set_pin(n, t, r, s, c) ((void)(n), (void)(t), (void)(r), (void)(s), ESP_OK)
#define esp_intr_alloc(i, f, h, a, o) ((void)(f), (void)(h), (void)(a), (void)(o), ESP_OK)
#define esp_intr_free(h) ((void)(h))
