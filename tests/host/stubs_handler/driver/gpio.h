/* Host stub */
#pragma once
#include "esp_err.h"
typedef int gpio_num_t;
#define GPIO_IS_VALID_GPIO(n) ((n) >= 0)
#define GPIO_IS_VALID_OUTPUT_GPIO(n) ((n) >= 0)
#define GPIO_MODE_OUTPUT 2
int gpio_reset_pin(int pin);
int gpio_set_direction(int pin, int mode);
int gpio_set_level(int pin, uint32_t level);
int gpio_install_isr_service(int flags);
