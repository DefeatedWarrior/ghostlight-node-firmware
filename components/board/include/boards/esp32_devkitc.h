/*
 * Original ESP32: ESP32-DevKitC with a WROOM-32 module (4 MB flash, no
 * PSRAM), a W5500 module and two RS-485 transceivers (DE and /RE tied to
 * DIR, high = transmit). Settings: boards/esp32-devkitc/sdkconfig.defaults
 *
 * Wiring (every pin is a general-purpose I/O on the WROOM-32; GPIO6-11 are
 * the flash, GPIO34-39 are input-only, GPIO2/12/15 are boot straps):
 *
 *   DMX port 1   UART1  TX 17  RX 16  DIR 21  LED 32
 *   DMX port 2   UART2  TX 25  RX 26  DIR 27  LED 33
 *   W5500        VSPI   SCK 18  MISO 19  MOSI 23  CS 5  INT 4  RST 22
 *   Status LED   WS2812 on GPIO 13 (optional)
 *   BOOT button  GPIO 0 (on the DevKitC), hold 10 s to factory reset
 *   Console      UART0 on the DevKitC's USB-serial chip
 *
 * A WROVER module uses GPIO16/17 for its PSRAM: move port 1 if you use one.
 */
#pragma once

#define BOARD_NAME              "ESP32 DevKitC"
#define BOARD_DMX_PORTS         2

/*                               port 1  port 2 */
#define BOARD_DMX_UART          { 1,      2  }
#define BOARD_DMX_TX_GPIO       { 17,     25 }
#define BOARD_DMX_RX_GPIO       { 16,     26 }
#define BOARD_DMX_DIR_GPIO      { 21,     27 }
#define BOARD_PORT_LED_GPIO     { 32,     33 }

/* VSPI's own pins (IO_MUX) */
#define BOARD_ETH_SPI_HOST      SPI3_HOST
#define BOARD_ETH_SCK_GPIO      18
#define BOARD_ETH_MOSI_GPIO     23
#define BOARD_ETH_MISO_GPIO     19
#define BOARD_ETH_CS_GPIO       5
#define BOARD_ETH_INT_GPIO      4
#define BOARD_ETH_RST_GPIO      22

#define BOARD_STATUS_LED_GPIO   13
#define BOARD_RESET_BUTTON_GPIO 0
