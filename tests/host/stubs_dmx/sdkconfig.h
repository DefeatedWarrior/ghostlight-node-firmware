/* Host stub for test_dmx_rx: the esp_dmx receive path on a 3-UART chip */
#pragma once
#define CONFIG_DMX_ISR_IN_IRAM 1
#define CONFIG_FREERTOS_HZ 1000
#define SOC_UART_NUM 3
#define ESP_IDF_VERSION_VAL(a, b, c) (((a) << 16) | ((b) << 8) | (c))
#define ESP_IDF_VERSION_MAJOR 5
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(5, 2, 6)
