/**
 * @file board.h
 * @brief Hardware description of the board the firmware is built for
 *
 * Everything that depends on the PCB lives here: number of DMX ports, the
 * UART and pins of each port, optional per-port LEDs, the status LED and
 * the reset button. The rest of the firmware uses BOARD_DMX_PORTS and these
 * tables instead of hard-coded pins, so a new board (for example a 4-port
 * one) is a new block below plus `-DBOARD_xxx` in the top-level CMakeLists.
 *
 * DMX port tables are indexed by port - 1. UART numbers are esp_dmx port
 * numbers (DMX_NUM_1 = 1, DMX_NUM_2 = 2); UART0 stays the console.
 */

#ifndef BOARD_H
#define BOARD_H

#if !defined(BOARD_ESP_NODE_2RDM)
#define BOARD_ESP_NODE_2RDM 1      /* the only board so far */
#endif

#if BOARD_ESP_NODE_2RDM

#define BOARD_NAME              "ESP-NODE-2RDM"
#define BOARD_DMX_PORTS         2

/*                               port 1  port 2 */
#define BOARD_DMX_UART          { 1,      2  }
#define BOARD_DMX_TX_GPIO       { 17,     15 }
#define BOARD_DMX_RX_GPIO       { 16,     18 }
#define BOARD_DMX_DIR_GPIO      { 39,     40 }   /* RS-485 driver enable */

/* Optional single-colour LED per port (active high), -1 = not fitted */
#define BOARD_PORT_LED_GPIO     { 5,      6  }

/* Ghostlight Node extras (not used by the firmware yet) */
#define BOARD_DMX_BIAS_GPIO     { 1,      2  }   /* switchable E1.20 bias */
#define BOARD_W5500_RST_GPIO    4

#define BOARD_STATUS_LED_GPIO   48     /* WS2812 */
#define BOARD_RESET_BUTTON_GPIO 0      /* BOOT button, active low, -1 = none */

#endif

#endif /* BOARD_H */
