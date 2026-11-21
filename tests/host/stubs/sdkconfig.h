/* Host test build: the sdkconfig options the tested components read.
 * Board: ESP-NODE-2RDM (2 DMX ports), every feature switch on. */
#pragma once
#define CONFIG_IDF_TARGET_ESP32S3 1
#define CONFIG_FREERTOS_HZ 1000
#define CONFIG_BOARD_ESP_NODE_2RDM 1
#define CONFIG_BOARD_HAS_W5500 1
#define CONFIG_NODE_ETHERNET 1
#define CONFIG_NODE_SACN 1
#define CONFIG_NODE_RDM 1
#define CONFIG_NODE_SCENES 1
#define CONFIG_NODE_DMX_TOOLS 1
#define CONFIG_NODE_WEB_LOG 1
#define CONFIG_NODE_WEB_LOG_KB 32
#define CONFIG_NODE_MDNS 1
