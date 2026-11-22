/**
 * @file board_gpio.c
 * @brief The GPIO interrupt service, installed once (see board_gpio.h)
 */
#include "board_gpio.h"
#include <stdatomic.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum { NOT_YET, INSTALLING, INSTALLED };
static atomic_int s_state = NOT_YET;

esp_err_t board_gpio_isr_service(void)
{
    int expected = NOT_YET;
    if (atomic_compare_exchange_strong(&s_state, &expected, INSTALLING)) {
        esp_err_t err = gpio_install_isr_service(0);
        /* INVALID_STATE: something outside this component installed it */
        bool ok = err == ESP_OK || err == ESP_ERR_INVALID_STATE;
        atomic_store(&s_state, ok ? INSTALLED : NOT_YET);
        return ok ? ESP_OK : err;
    }
    while (atomic_load(&s_state) == INSTALLING) {
        vTaskDelay(1);      /* another task is installing it right now */
    }
    return atomic_load(&s_state) == INSTALLED ? ESP_OK : ESP_FAIL;
}
