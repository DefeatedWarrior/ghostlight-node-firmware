/*
 * ESP32-S3 DevKitC-1 (any N8 / N8R8 / N16R8 module) with 2 RS-485
 * transceivers and no Ethernet chip: Wi-Fi only. Same DMX pins as the
 * ESP-NODE-2RDM prototype. Settings: boards/esp32s3-devkitc/sdkconfig.defaults
 *
 *   RS-485 transceiver (MAX485, SP3485, ...)   DevKitC-1
 *   port 1  DI  <- GPIO17   RO -> GPIO16   DE + /RE <- GPIO39
 *   port 2  DI  <- GPIO15   RO -> GPIO18   DE + /RE <- GPIO40
 *
 * The RGB LED is on GPIO48 on DevKitC-1 v1.0 and on GPIO38 on v1.1:
 * change BOARD_STATUS_LED_GPIO if yours is a v1.1.
 */
#pragma once

#define BOARD_NAME              "ESP32-S3 DevKitC-1"
#define BOARD_DMX_PORTS         2

/*                               port 1  port 2 */
#define BOARD_DMX_UART          { 1,      2  }
#define BOARD_DMX_TX_GPIO       { 17,     15 }
#define BOARD_DMX_RX_GPIO       { 16,     18 }
#define BOARD_DMX_DIR_GPIO      { 39,     40 }   /* high = transmit */

/* No W5500: no BOARD_ETH_* pins (Kconfig does not select BOARD_HAS_W5500) */

#define BOARD_STATUS_LED_GPIO   48               /* v1.1 boards: 38 */
#define BOARD_RESET_BUTTON_GPIO 0                /* BOOT */
