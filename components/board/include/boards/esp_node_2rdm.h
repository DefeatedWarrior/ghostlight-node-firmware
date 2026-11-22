/*
 * ESP-NODE-2RDM prototype: ESP32-S3 N16R8 dev board (16 MB flash, 8 MB
 * octal PSRAM), W5500 module, 2 RS-485 transceivers with DE and /RE tied
 * to DIR. Settings: boards/esp-node-2rdm/sdkconfig.defaults
 */
#pragma once

#define BOARD_NAME              "ESP-NODE-2RDM"
#define BOARD_ID                "esp-node-2rdm"   /* tools/build.sh name; firmware uploads must match */
#define BOARD_DMX_PORTS         2

/*                               port 1  port 2 */
#define BOARD_DMX_UART          { 1,      2  }
#define BOARD_DMX_TX_GPIO       { 17,     15 }
#define BOARD_DMX_RX_GPIO       { 16,     18 }
#define BOARD_DMX_DIR_GPIO      { 39,     40 }   /* high = transmit */

#define BOARD_ETH_SPI_HOST      SPI2_HOST
#define BOARD_ETH_SCK_GPIO      12
#define BOARD_ETH_MOSI_GPIO     11
#define BOARD_ETH_MISO_GPIO     13
#define BOARD_ETH_CS_GPIO       10
#define BOARD_ETH_INT_GPIO      9

#define BOARD_STATUS_LED_GPIO   48               /* WS2812 on the dev board */
#define BOARD_RESET_BUTTON_GPIO 0                /* BOOT */
