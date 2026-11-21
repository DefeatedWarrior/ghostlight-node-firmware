/*
 * Ghostlight Node rev1 (KiCad project: hardware/kicad/ at the top of the
 * ghostlight-node repository): ESP32-S3-WROOM-1-N8
 * (8 MB flash, no PSRAM), W5500 with PoE magjack, 2 SN65HVD72 ports with
 * switchable bias, port LEDs, WS2812 status LED.
 * Settings: boards/ghostlight-rev1/sdkconfig.defaults
 *
 * Not used yet: the third-port signals on J5 (DIR3 / BIAS3 / LED3, UART0
 * shared with the J7 debug console).
 */
#pragma once

#define BOARD_NAME              "Ghostlight Node rev1"
#define BOARD_DMX_PORTS         2

/*                               port 1  port 2 */
#define BOARD_DMX_UART          { 1,      2  }
#define BOARD_DMX_TX_GPIO       { 17,     15 }
#define BOARD_DMX_RX_GPIO       { 16,     18 }
#define BOARD_DMX_DIR_GPIO      { 39,     40 }   /* high = transmit */
#define BOARD_DMX_BIAS_GPIO     { 1,      2  }   /* bias switch, active high */
#define BOARD_PORT_LED_GPIO     { 5,      6  }

#define BOARD_ETH_SPI_HOST      SPI2_HOST
#define BOARD_ETH_SCK_GPIO      12
#define BOARD_ETH_MOSI_GPIO     11
#define BOARD_ETH_MISO_GPIO     13
#define BOARD_ETH_CS_GPIO       10
#define BOARD_ETH_INT_GPIO      9
#define BOARD_ETH_RST_GPIO      4                /* 10k pull-up on the board */

#define BOARD_STATUS_LED_GPIO   48               /* WS2812B */
#define BOARD_RESET_BUTTON_GPIO 0                /* BOOT */
