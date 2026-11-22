/**
 * @file board_gpio.h
 * @brief The GPIO interrupt service, shared by the components that need it
 */
#ifndef BOARD_GPIO_H
#define BOARD_GPIO_H

#include "esp_err.h"

/**
 * Install ESP-IDF's GPIO interrupt service if nobody has yet. ESP-IDF logs an
 * error for every install after the first, so everything (the W5500 and the
 * break/MAB measurement) installs it through this.
 * @return ESP_OK once it is installed
 */
esp_err_t board_gpio_isr_service(void);

#endif // BOARD_GPIO_H
