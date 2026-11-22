/**
 * @file board.h
 * @brief Hardware description of the board the firmware is built for
 *
 * Everything that depends on the PCB lives in one header per board under
 * boards/, picked by the "ESP-NODE > Board" menuconfig choice. The rest of
 * the firmware only uses the BOARD_* names below, never pin numbers.
 *
 * Adding a board: copy one of the headers in boards/, add it to the choice in
 * main/Kconfig.projbuild and to the list below, and add
 * boards/<name>/sdkconfig.defaults at the top of the repository
 * (docs/DEVELOPING.md walks through it).
 *
 * A board header must define:
 *   BOARD_NAME, BOARD_ID (the boards/<name> folder name: web firmware
 *     uploads must carry the same one), BOARD_DMX_PORTS,
 *   BOARD_DMX_UART, BOARD_DMX_TX_GPIO, BOARD_DMX_RX_GPIO, BOARD_DMX_DIR_GPIO
 *     (tables indexed by port - 1; UART numbers are esp_dmx port numbers)
 * and if it has a W5500 (CONFIG_BOARD_HAS_W5500):
 *   BOARD_ETH_SPI_HOST, BOARD_ETH_SCK_GPIO, BOARD_ETH_MOSI_GPIO,
 *   BOARD_ETH_MISO_GPIO, BOARD_ETH_CS_GPIO, BOARD_ETH_INT_GPIO
 * A board without a W5500 leaves all BOARD_ETH_* out and builds Wi-Fi only.
 * Everything else is optional; -1 means "not fitted".
 */

#ifndef BOARD_H
#define BOARD_H

#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if CONFIG_BOARD_ESP_NODE_2RDM
#include "boards/esp_node_2rdm.h"
#elif CONFIG_BOARD_GHOSTLIGHT_REV1
#include "boards/ghostlight_rev1.h"
#elif CONFIG_BOARD_ESP32_DEVKITC
#include "boards/esp32_devkitc.h"
#elif CONFIG_BOARD_ESP32S3_DEVKITC
#include "boards/esp32s3_devkitc.h"
#else
#error "No board selected: idf.py menuconfig > ESP-NODE > Board (or use tools/build.sh <board>)"
#endif

#if BOARD_DMX_PORTS < 1 || BOARD_DMX_PORTS > 4
#error "BOARD_DMX_PORTS must be 1..4"
#endif

#if !defined(BOARD_ID)
#error "The board header needs a BOARD_ID: its boards/<name> folder name"
#endif

#if CONFIG_BOARD_HAS_W5500 && !defined(BOARD_ETH_CS_GPIO)
#error "The board selects BOARD_HAS_W5500 but its header has no BOARD_ETH_* pins"
#endif
#if !CONFIG_BOARD_HAS_W5500 && defined(BOARD_ETH_CS_GPIO)
#error "The board header has BOARD_ETH_* pins: add select BOARD_HAS_W5500 to its Kconfig entry"
#endif

/* ---- Optional fields: defaults ---------------------------------------- */

#if BOARD_DMX_PORTS == 1
#define BOARD_NONE_PER_PORT     { -1 }
#elif BOARD_DMX_PORTS == 2
#define BOARD_NONE_PER_PORT     { -1, -1 }
#elif BOARD_DMX_PORTS == 3
#define BOARD_NONE_PER_PORT     { -1, -1, -1 }
#else
#define BOARD_NONE_PER_PORT     { -1, -1, -1, -1 }
#endif

/* Per-port LED (active high) */
#ifndef BOARD_PORT_LED_GPIO
#define BOARD_PORT_LED_GPIO     BOARD_NONE_PER_PORT
#endif
/* Per-port RS-485 bias switch (active high, low = no bias). The firmware
 * turns it on while the port drives the bus (output / RDM controller). */
#ifndef BOARD_DMX_BIAS_GPIO
#define BOARD_DMX_BIAS_GPIO     BOARD_NONE_PER_PORT
#endif
/* WS2812 status LED */
#ifndef BOARD_STATUS_LED_GPIO
#define BOARD_STATUS_LED_GPIO   -1
#endif
/* Factory-reset button (active low, hold 10 s) */
#ifndef BOARD_RESET_BUTTON_GPIO
#define BOARD_RESET_BUTTON_GPIO -1
#endif
/* W5500 hardware reset (active low) */
#ifndef BOARD_ETH_RST_GPIO
#define BOARD_ETH_RST_GPIO      -1
#endif
#ifndef BOARD_ETH_SPI_MHZ
#define BOARD_ETH_SPI_MHZ       20
#endif

/* ---- CPU cores ---------------------------------------------------------- */

/* DMX tasks get their own core on dual-core chips; the network side
 * (Wi-Fi, lwIP, Art-Net/sACN receive, web server) stays on core 0. */
#if CONFIG_FREERTOS_UNICORE || SOC_CPU_CORES_NUM < 2
#define BOARD_CORE_DMX          0
#else
#define BOARD_CORE_DMX          1
#endif
#define BOARD_CORE_NET          0

#endif /* BOARD_H */
