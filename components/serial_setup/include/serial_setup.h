#pragma once
/**
 * USB / serial setup (CONFIG_NODE_SERIAL_SETUP).
 *
 * Reads and changes the settings over the console port the node is flashed
 * through: UART0 (boards with a USB-serial chip) and, on the ESP32-S3, the
 * chip's own USB port (USB-Serial-JTAG). The browser flasher uses it to set a
 * node up straight after installing the firmware, and it gets a node whose
 * address or password is lost back onto the network.
 *
 * Protocol: one command per line, JSON after the prefix "#GL ":
 *
 *     #GL {"id":1,"cmd":"info"}
 *
 * Each reply is one line with the prefix "#GL> " and the same "id":
 *
 *     #GL> {"id":1,"ok":true,...}
 *     #GL> {"id":1,"ok":false,"error":"..."}
 *
 * Other input (typing in a serial monitor) is ignored. Replies go through
 * stdout like the log, so log lines may come between replies but never
 * inside one.
 *
 * Commands:
 *   info                    firmware, board, chip, features, MAC, whether the
 *                           admin password is set, tester mode, network status
 *   get                     the settings (config_to_public_json(): no passwords)
 *   set {"config":{...}}    apply settings: the document POST /api/config takes
 *   password {"password"}   set the web UI admin password (8-63 characters);
 *                           also replaces one that was forgotten
 *   scan                    Wi-Fi networks in range
 *   connect {"ssid"}        try a saved Wi-Fi network now
 *   restart                 restart in 0.5 s ("restarting": true)
 *
 * There is no sign-in: whoever holds the USB cable can reflash the node
 * anyway.
 */
#include "esp_err.h"

/** Start the task that listens on the console port(s). Call once, after
 *  the configuration and the network are up. */
esp_err_t serial_setup_start(void);
