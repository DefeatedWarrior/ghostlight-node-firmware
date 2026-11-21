# FIRMWARE DEVELOPMENT PLAN
**Project: ESP-NODE-2RDM - Art-Net/sACN to DMX512/RDM Converter**

**Platform:** ESP32-S3-WROOM-1-N16R8 (16MB Flash, 8MB PSRAM)  
**Framework:** ESP-IDF v5.2.6  
**Document version:** 1.0  
**Date:** 25/12/2025

---

## CONTENTS
1. [Project overview](#1-project-overview)
2. [Development strategy](#2-development-strategy)
3. [Project structure](#3-project-structure)
4. [Libraries used](#4-libraries-used)
5. [Development roadmap](#5-development-roadmap)
6. [References](#6-references)

---

## 1. PROJECT OVERVIEW

### 1.1. Goal
Build complete firmware for a device that converts Art-Net/sACN to DMX512/RDM on 2 independent ports, using existing libraries as much as possible to shorten development time and improve reliability.

### 1.2. Main requirements
- ✅ Art-Net v4 and sACN (E1.31) support
- ✅ 2 independent DMX512/RDM ports (configurable)
- ✅ W5500 Ethernet connection (primary) + WiFi fallback
- ✅ Merge engine with several modes (HTP/LTP/LAST/BACKUP)
- ✅ Web interface for configuration and monitoring
- ✅ RDM Master/Responder support
- ✅ WS2812 LED status indicator
- ✅ JSON configuration stored on LittleFS

### 1.3. Design priorities
1. **Use existing libraries** instead of writing code from scratch
2. **Reuse** the official ESP-IDF components
3. **Optimise** performance with the dual-core ESP32-S3
4. **No security** - the whole config can be downloaded as JSON
5. **Modular** code that is easy to maintain and test

---

## 2. DEVELOPMENT STRATEGY

### 2.1. Approach
**Bottom-up Development**: build from the most basic modules (Storage, LED) up to the complex ones (protocol handlers, merge engine).

### 2.2. Principles
1. **Library-first**: always look for an existing library before writing code
2. **Test each module**: every module is tested on its own before integration
3. **Incremental**: develop small features one at a time and commit often
4. **Documentation**: every module needs a README and API documentation

### 2.3. Tools and environment
- **IDE**: VS Code with the ESP-IDF extension
- **Build system**: CMake (ESP-IDF standard)
- **Version control**: Git with a branching strategy
- **Testing**: Physical hardware testing (theo TESTING_GUIDE.md)
- **Debug**: JTAG + ESP-IDF monitor + GDB

---

## 3. PROJECT STRUCTURE

### 3.1. Directory structure

```
ESP-NODE-2RDM/
├── CMakeLists.txt                      # Root CMake file
├── sdkconfig.defaults                  # Default SDK configuration
├── partitions.csv                      # Partition table
├── main/
│   ├── CMakeLists.txt
│   ├── main.c                          # Entry point
│   ├── Kconfig.projbuild               # Project configuration
│   └── idf_component.yml               # Component dependencies
├── components/                         # Custom components
│   ├── config_manager/                 # Configuration module
│   │   ├── CMakeLists.txt
│   │   ├── include/
│   │   │   └── config_manager.h
│   │   └── config_manager.c
│   ├── network_manager/                # Network module
│   │   ├── CMakeLists.txt
│   │   ├── include/
│   │   │   └── network_manager.h
│   │   └── network_manager.c
│   ├── led_manager/                    # LED status module
│   │   ├── CMakeLists.txt
│   │   ├── include/
│   │   │   └── led_manager.h
│   │   └── led_manager.c
│   ├── dmx_handler/                    # DMX/RDM handler
│   │   ├── CMakeLists.txt
│   │   ├── include/
│   │   │   └── dmx_handler.h
│   │   └── dmx_handler.c
│   ├── merge_engine/                   # Data merging logic
│   │   ├── CMakeLists.txt
│   │   ├── include/
│   │   │   └── merge_engine.h
│   │   └── merge_engine.c
│   ├── artnet_receiver/                # Art-Net protocol
│   │   ├── CMakeLists.txt
│   │   ├── include/
│   │   │   └── artnet_receiver.h
│   │   └── artnet_receiver.c
│   ├── sacn_receiver/                  # sACN protocol
│   │   ├── CMakeLists.txt
│   │   ├── include/
│   │   │   └── sacn_receiver.h
│   │   └── sacn_receiver.c
│   ├── web_server/                     # HTTP + WebSocket
│   │   ├── CMakeLists.txt
│   │   ├── include/
│   │   │   └── web_server.h
│   │   ├── web_server.c
│   │   └── www/                        # Web files
│   │       ├── index.html
│   │       ├── style.css
│   │       └── script.js
│   └── storage_manager/                # LittleFS wrapper
│       ├── CMakeLists.txt
│       ├── include/
│       │   └── storage_manager.h
│       └── storage_manager.c
├── external_components/                # External libraries
│   ├── esp-dmx/                        # DMX512/RDM library
│   ├── libe131/                        # sACN library
│   └── artnet/                         # Art-Net library
├── docs/                               # Documentation
│   ├── FIRMWARE_DEVELOPMENT_PLAN.md    # This file
│   ├── LIBRARY_INTEGRATION_GUIDE.md    # Library integration details
│   ├── IMPLEMENTATION_ROADMAP.md       # Phase-by-phase roadmap
│   ├── API_REFERENCE.md                # API documentation
│   ├── CODING_STANDARDS.md             # Coding standards
│   ├── TESTING_GUIDE.md                # Testing guide
│   └── modules/                        # Module design docs
└── tools/                              # Development tools
    ├── test_scripts/                   # Test scripts
    └── config_generator/               # Config file generator
```

### 3.2. Partition table

```csv
# Name,     Type, SubType,  Offset,   Size,     Flags
nvs,        data, nvs,      0x9000,   0x4000,
otadata,    data, ota,      0xd000,   0x2000,
phy_init,   data, phy,      0xf000,   0x1000,
factory,    app,  factory,  0x10000,  0x200000,
littlefs,   data, spiffs,   0x210000, 0x100000,
```

**Explanation:**
- **nvs** (16KB): NVS storage for basic information
- **factory** (2MB): Application firmware
- **littlefs** (1MB): file system for config.json and the web files
- **OTA** (optional): a partition for OTA updates can be added later

---

## 4. LIBRARIES USED

### 4.1. Official ESP-IDF libraries (built in)

| Library | Purpose | CMake component |
|----------|----------|----------------------|
| `esp_eth` | W5500 Ethernet driver | `esp_eth` |
| `esp_wifi` | WiFi STA + AP | `esp_wifi` |
| `esp_http_server` | HTTP server + WebSocket | `esp_http_server` |
| `led_strip` | WS2812 LED control via RMT | `led_strip` |
| `lwip` | TCP/IP stack, UDP sockets | `lwip` |
| `nvs_flash` | NVS storage | `nvs_flash` |
| `esp_timer` | High-resolution timers | `esp_timer` |
| `cJSON` | JSON parsing/generation | `json` |
| `esp_littlefs` | LittleFS file system | `esp_littlefs` |

### 4.2. Third-party libraries (must be added to the project)

#### 4.2.1. esp-dmx
**Repository:** https://github.com/someweisguy/esp-dmx  
**Version:** Latest stable  
**Purpose:** DMX512 + RDM support  
**Why this choice:**
- The best DMX/RDM library for ESP-IDF
- Full RDM discovery, get/set support
- Well maintained, with complete documentation
- Supports multiple ports

**How to add it to the project:**
```bash
cd components
git clone https://github.com/someweisguy/esp-dmx.git esp-dmx
```

**CMakeLists.txt:**
```cmake
idf_component_register(
    SRCS "dmx_handler.c"
    INCLUDE_DIRS "include"
    REQUIRES esp-dmx
)
```

#### 4.2.2. libe131
**Repository:** https://github.com/hhromic/libe131  
**Version:** Latest stable  
**Purpose:** sACN (E1.31) receiver  
**Why this choice:**
- Lightweight, only ~500 lines of code
- Supports multicast join/leave
- Easy to port to ESP-IDF (only needs lwip sockets)

**How to add it to the project:**
```bash
cd components
git clone https://github.com/hhromic/libe131.git libe131
```

**Note:** This library may need a CMakeLists.txt wrapper.

#### 4.2.3. Art-Net Library
**Option 1:** Fork https://github.com/hideakitai/ArtNet  
**Option 2:** Implement a minimal Art-Net ourselves (recommended)

**Why implement it ourselves:**
- The Art-Net protocol is simpler than sACN
- Only ArtDmx, ArtPoll, ArtPollReply and ArtAddress need handling
- Arduino libraries may not be optimal for ESP-IDF

**If an existing library is used:**
```bash
cd components
git clone https://github.com/hideakitai/ArtNet.git artnet
# Needs porting to lwip sockets instead of Arduino UDP
```

### 4.3. Component dependencies map

```
main.c
├── config_manager
│   ├── cJSON
│   ├── nvs_flash
│   └── storage_manager (LittleFS)
├── network_manager
│   ├── esp_eth (W5500)
│   ├── esp_wifi
│   └── lwip
├── led_manager
│   └── led_strip (RMT)
├── dmx_handler
│   └── esp-dmx
├── artnet_receiver
│   └── lwip (UDP)
├── sacn_receiver
│   └── libe131 (lwip)
├── merge_engine
│   └── dmx_handler
└── web_server
    ├── esp_http_server
    └── cJSON
```

---

## 5. DEVELOPMENT ROADMAP

### Phase 0: Project Setup (Sprint 0)
**Goal:** Create the project structure and build system

**Tasks:**
1. Create the ESP-IDF project with `idf.py create-project`
2. Set up the partition table
3. Create the component structure from section 3.1
4. Add a `.gitignore` for ESP-IDF
5. Configure sdkconfig.defaults
6. Create Kconfig.projbuild for custom config

**Output:** The project builds successfully (empty firmware)

**References:**
- ESP-IDF Build System: https://docs.espressif.com/projects/esp-idf/en/v5.2.6/esp32s3/api-guides/build-system.html

---

### Phase 1: Storage and Configuration (Sprint 1)
**Goal:** Finish storage and configuration management

#### 1.1. Storage Manager
**Library:** `esp_littlefs`  
**API:**
```c
esp_err_t storage_init(void);
esp_err_t storage_read_file(const char *path, char *buffer, size_t *size);
esp_err_t storage_write_file(const char *path, const char *data, size_t size);
esp_err_t storage_delete_file(const char *path);
bool storage_file_exists(const char *path);
```

**Test:**
- Mount/unmount LittleFS
- Read/write test files
- Verify persistence across reboot

#### 1.2. Configuration Manager
**Libraries:** `cJSON`, `nvs_flash`, `storage_manager`  
**Config structure:** (See OVERVIEW.md section 4.1)  
**API:**
```c
esp_err_t config_init(void);
esp_err_t config_load_from_storage(void);
esp_err_t config_save_to_storage(void);
esp_err_t config_get_json_string(char **json_str);
esp_err_t config_set_from_json_string(const char *json_str);
config_t* config_get_current(void);
```

**Default config.json:**
```json
{
  "network": {
    "use_ethernet": true,
    "static_ip_enabled": false,
    "wifi_profiles": [],
    "ap_ssid": "ArtnetNode-0000",
    "ap_password": "12345678"
  },
  "ports": {
    "port1": {
      "mode": "DMX_OUT",
      "universe_primary": 0,
      "rdm_enabled": true
    },
    "port2": {
      "mode": "DMX_OUT",
      "universe_primary": 1,
      "rdm_enabled": true
    }
  },
  "merge": {
    "mode": "HTP",
    "timeout_seconds": 3
  },
  "node_info": {
    "short_name": "ArtNet-Node",
    "long_name": "Art-Net/sACN to DMX512/RDM Converter"
  }
}
```

**Test:**
- Load default config
- Modify and save
- Verify JSON serialization/deserialization

**Document:** `docs/modules/DESIGN_MODULE_Configuration.md`

---

### Phase 2: LED Manager (Sprint 2)
**Goal:** Show the system state on the WS2812

**Library:** `led_strip` (official ESP-IDF)  
**Hardware:** GPIO 48

**API:**
```c
esp_err_t led_manager_init(void);
esp_err_t led_manager_set_state(led_state_t state);
esp_err_t led_manager_blink(uint32_t rgb, uint32_t interval_ms);
esp_err_t led_manager_pulse(void); // Quick flash for packet receive
```

**States:**
- BOOT (Light blue)
- ETHERNET_OK (Green)
- WIFI_STA_OK (Green blinking)
- WIFI_AP (Purple)
- RECEIVING (White flash)
- RDM_DISCOVERY (Yellow blinking)
- ERROR (Red fast blink)

**Implementation notes:**
- Use the ESP-IDF `led_strip` component
- Run a separate task to handle blinking
- Event-driven: listen cho network events, packet events

**Test:**
- Cycle through all states
- Verify the colours with a multimeter/oscilloscope
- Check timing accuracy

**Document:** `docs/modules/DESIGN_MODULE_LED_Manager.md`

---

### Phase 3: Network Manager (Sprint 3-4)
**Goal:** Set up the Ethernet and WiFi connections

#### 3.1. Ethernet (W5500)
**Library:** `esp_eth`  
**Hardware:** SPI (CS: GPIO10, MOSI: GPIO11, MISO: GPIO13, SCK: GPIO12, INT: GPIO9)

**API:**
```c
esp_err_t network_init(void);
esp_err_t network_start_ethernet(void);
esp_err_t network_start_wifi_sta(void);
esp_err_t network_start_wifi_ap(void);
bool network_is_connected(void);
esp_err_t network_get_ip_info(esp_netif_ip_info_t *ip_info);
```

**Fallback logic:**
1. Try Ethernet (3 attempts, 10s timeout each)
2. If fail → Try WiFi STA (each profile)
3. If fail → Start WiFi AP

**Implementation:**
```c
// In network_manager.c
static void network_task(void *pvParameters) {
    while (1) {
        if (config->network.use_ethernet) {
            if (ethernet_connect(3, 10000) == ESP_OK) {
                led_manager_set_state(LED_ETHERNET_OK);
                break;
            }
        }
        
        if (wifi_sta_connect() == ESP_OK) {
            led_manager_set_state(LED_WIFI_STA_OK);
            break;
        }
        
        // Fallback to AP
        wifi_ap_start();
        led_manager_set_state(LED_WIFI_AP);
        break;
    }
}
```

**Test:**
- Ethernet connection with the W5500
- WiFi STA connection
- WiFi AP mode
- Fallback sequence
- IP assignment (DHCP and static)

**Document:** `docs/modules/DESIGN_MODULE_Network.md`

---

### Phase 4: DMX/RDM Handler (Sprint 5-6)
**Goal:** Drive the 2 DMX512/RDM ports

**Library:** `esp-dmx`  
**Hardware:**
- Port 1: TX=GPIO17, RX=GPIO16, DIR=GPIO21
- Port 2: TX=GPIO19, RX=GPIO18, DIR=GPIO20

**API:**
```c
esp_err_t dmx_handler_init(void);
esp_err_t dmx_handler_configure_port(uint8_t port, dmx_port_config_t *config);
esp_err_t dmx_handler_send_dmx(uint8_t port, const uint8_t *data, size_t size);
esp_err_t dmx_handler_read_dmx(uint8_t port, uint8_t *data, size_t *size);
esp_err_t dmx_handler_rdm_discover(uint8_t port, rdm_device_t *devices, size_t *count);
esp_err_t dmx_handler_rdm_get(uint8_t port, rdm_uid_t uid, uint16_t pid, uint8_t *data, size_t *size);
esp_err_t dmx_handler_rdm_set(uint8_t port, rdm_uid_t uid, uint16_t pid, const uint8_t *data, size_t size);
```

**Port modes:**
- DMX_OUT: Output 512 channels @ ~44Hz
- DMX_IN: Input monitoring
- RDM_MASTER: Output + RDM master
- RDM_RESPONDER: Respond to RDM requests
- DISABLED: Turn off

**Implementation notes:**
- Use `dmx_driver_install()` from esp-dmx
- Run 2 separate tasks, one per port (Core 1)
- 512-byte buffer per universe
- Blackout if there is no data within `merge_timeout`

**Example (Port 1 DMX Out):**
```c
dmx_config_t config = DMX_CONFIG_DEFAULT;
dmx_personality_t personality = {
    .footprint = 512,
    .start_address = 1
};

dmx_driver_install(DMX_NUM_1, &config, &personality, DMX_INTR_FLAGS_DEFAULT);
dmx_set_pin(DMX_NUM_1, 17, 16, 21); // TX, RX, EN

// Send loop
while (1) {
    dmx_wait_sent(DMX_NUM_1, DMX_TIMEOUT_TICK);
    dmx_write(DMX_NUM_1, dmx_buffer, 512);
    dmx_send(DMX_NUM_1, 512);
}
```

**Test:**
- DMX output with a DMX tester
- RDM discovery with real RDM devices
- Get/Set RDM parameters
- Timing verification (44Hz)

**Document:** `docs/modules/DESIGN_MODULE_DMX_RDM_Handler.md`

---

### Phase 5: Protocol Receivers (Sprint 7-8)

#### 5.1. Art-Net Receiver
**Library:** custom implementation or a ported library  
**Protocol:** Art-Net v4, UDP port 6454

**Packets to handle:**
- ArtDmx (0x5000): DMX data
- ArtPoll (0x2000): Discovery
- ArtPollReply (0x2100): Response
- ArtAddress (0x6000): Configuration

**API:**
```c
esp_err_t artnet_receiver_init(void);
esp_err_t artnet_receiver_start(void);
esp_err_t artnet_receiver_stop(void);
esp_err_t artnet_receiver_set_callback(artnet_dmx_callback_t callback, void *ctx);
```

**Callback:**
```c
typedef void (*artnet_dmx_callback_t)(uint16_t universe, const uint8_t *data, size_t size, void *ctx);
```

**Implementation:**
```c
// UDP receive task
static void artnet_receive_task(void *pvParameters) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(6454),
        .sin_addr.s_addr = INADDR_ANY
    };
    bind(sock, (struct sockaddr*)&addr, sizeof(addr));
    
    uint8_t buffer[1024];
    while (1) {
        int len = recvfrom(sock, buffer, sizeof(buffer), 0, NULL, NULL);
        if (len > 0) {
            artnet_packet_t *packet = (artnet_packet_t*)buffer;
            if (packet->opcode == ARTNET_DMX) {
                uint16_t universe = packet->universe;
                artnet_dmx_callback(universe, packet->data, packet->length, ctx);
                led_manager_pulse(); // Flash LED
            }
        }
    }
}
```

**Test:**
- Receive Art-Net DMX from QLC+, LightKey, etc.
- Verify universe routing
- ArtPoll response
- Multiple sources

**Document:** `docs/modules/DESIGN_MODULE_ArtNet_Receiver.md`

#### 5.2. sACN Receiver
**Library:** `libe131`  
**Protocol:** E1.31 (sACN), multicast

**API:**
```c
esp_err_t sacn_receiver_init(void);
esp_err_t sacn_receiver_subscribe_universe(uint16_t universe);
esp_err_t sacn_receiver_unsubscribe_universe(uint16_t universe);
esp_err_t sacn_receiver_set_callback(sacn_dmx_callback_t callback, void *ctx);
```

**Multicast groups:**
- Universe 1: 239.255.0.1
- Universe N: 239.255.(N/256).(N%256)

**Implementation:**
```c
// Using libe131
e131_socket_t sock;
e131_packet_t packet;

e131_init(&sock);
e131_join_multicast(&sock, universe);

while (1) {
    if (e131_recv(&sock, &packet) == E131_OK) {
        if (e131_pkt_validate(&packet) == E131_OK) {
            uint16_t universe = packet.universe;
            sacn_dmx_callback(universe, packet.dmp.data, 512, ctx);
            led_manager_pulse();
        }
    }
}
```

**Test:**
- Receive sACN from lighting software
- Multicast join/leave
- Priority handling
- Sequence number check

**Document:** `docs/modules/DESIGN_MODULE_sACN_Receiver.md`

---

### Phase 6: Merge Engine (Sprint 9-10)
**Goal:** Merge data from several sources

**Modes:**
- **HTP** (Highest Takes Precedence): `merged[ch] = max(src1[ch], src2[ch], ...)`
- **LTP** (Latest Takes Precedence): `merged[ch] = latest_source[ch]`
- **LAST**: Entire frame from last received source
- **BACKUP**: Primary source + backup (switch on timeout)
- **DISABLE**: No merge, single source only

**API:**
```c
esp_err_t merge_engine_init(void);
esp_err_t merge_engine_add_source(uint8_t port, uint16_t universe, const uint8_t *data, uint8_t priority);
esp_err_t merge_engine_get_merged(uint8_t port, uint8_t *data);
esp_err_t merge_engine_set_mode(uint8_t port, merge_mode_t mode);
```

**Data structure:**
```c
typedef struct {
    uint8_t data[512];
    uint8_t priority;
    uint32_t timestamp_ms;
    bool valid;
} merge_source_t;

typedef struct {
    merge_source_t sources[4]; // Max 4 sources per port
    merge_mode_t mode;
    uint32_t timeout_ms;
    uint8_t merged_output[512];
} merge_port_t;
```

**Implementation (HTP example):**
```c
void merge_engine_calculate_htp(merge_port_t *port) {
    memset(port->merged_output, 0, 512);
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    for (int i = 0; i < 4; i++) {
        if (port->sources[i].valid && 
            (now - port->sources[i].timestamp_ms) < port->timeout_ms) {
            
            for (int ch = 0; ch < 512; ch++) {
                if (port->sources[i].data[ch] > port->merged_output[ch]) {
                    port->merged_output[ch] = port->sources[i].data[ch];
                }
            }
        } else {
            port->sources[i].valid = false; // Timeout
        }
    }
}
```

**Test:**
- HTP with 2 sources
- LTP mode
- Backup failover
- Timeout handling
- Performance (merge time < 5ms)

**Document:** `docs/modules/DESIGN_MODULE_Merge_Engine.md`

---

### Phase 7: Web Server (Sprint 11-12)
**Goal:** HTTP API and WebSocket interface

**Libraries:** `esp_http_server`, `cJSON`

**Endpoints:**

| Path | Method | Description |
|------|--------|-------------|
| `/` | GET | index.html |
| `/config` | GET/POST | Get/Set full config JSON |
| `/status` | GET | Network, DMX, packet stats |
| `/wifi/scan` | GET | Scan WiFi networks |
| `/ports/config` | GET/POST | DMX port configuration |
| `/test/dmx/port1` | WebSocket | Real-time DMX test |
| `/rdm/discover/port1` | POST | Start RDM discovery |

**WebSocket messages:**
```json
// Client → Server
{
  "type": "dmx_test",
  "port": 1,
  "channel": 1,
  "value": 255
}

// Server → Client
{
  "type": "dmx_levels",
  "port": 1,
  "data": [0, 255, 128, ...]
}
```

**Implementation:**
```c
// HTTP handler
static esp_err_t config_get_handler(httpd_req_t *req) {
    char *json_str;
    config_get_json_string(&json_str);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    return ESP_OK;
}

// WebSocket handler
static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        return ESP_OK; // Handshake
    }
    
    httpd_ws_frame_t ws_pkt;
    // Receive and parse the WebSocket frame
    // Handle dmx_test commands
    return ESP_OK;
}
```

**Web files (www/):**
- `index.html`: Main UI
- `style.css`: Styling
- `script.js`: WebSocket client, API calls

**Test:**
- Load web page
- GET/POST config
- WebSocket connection
- DMX test via web
- RDM control

**Document:** `docs/modules/DESIGN_MODULE_Web_Server.md`

---

### Phase 8: Main Application Integration (Sprint 13)
**Goal:** Bring all the modules together

**main.c structure:**
```c
void app_main(void) {
    // 1. Initialize NVS
    nvs_flash_init();
    
    // 2. Initialize storage
    storage_init();
    
    // 3. Load configuration
    config_init();
    config_load_from_storage();
    
    // 4. Initialize LED
    led_manager_init();
    led_manager_set_state(LED_BOOT);
    
    // 5. Initialize network
    network_init();
    network_start(); // Ethernet → WiFi STA → AP
    
    // 6. Initialize DMX/RDM
    dmx_handler_init();
    dmx_handler_configure_port(1, config_get_port1_config());
    dmx_handler_configure_port(2, config_get_port2_config());
    
    // 7. Initialize merge engine
    merge_engine_init();
    
    // 8. Start protocol receivers
    artnet_receiver_init();
    artnet_receiver_set_callback(on_artnet_dmx, NULL);
    
    sacn_receiver_init();
    sacn_receiver_set_callback(on_sacn_dmx, NULL);
    
    // 9. Start web server
    web_server_init();
    web_server_start();
    
    // 10. Create main tasks
    xTaskCreatePinnedToCore(network_task, "network", 4096, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(dmx_output_task, "dmx_out", 4096, NULL, 10, NULL, 1);
}

// Callbacks
void on_artnet_dmx(uint16_t universe, const uint8_t *data, size_t size, void *ctx) {
    uint8_t port = config_universe_to_port(universe);
    merge_engine_add_source(port, universe, data, 100);
}

void on_sacn_dmx(uint16_t universe, const uint8_t *data, size_t size, void *ctx) {
    uint8_t port = config_universe_to_port(universe);
    merge_engine_add_source(port, universe, data, 100);
}

// Main DMX output task
void dmx_output_task(void *pvParameters) {
    uint8_t dmx_buffer[512];
    
    while (1) {
        for (uint8_t port = 1; port <= 2; port++) {
            if (config_get_port_mode(port) == DMX_OUT) {
                merge_engine_get_merged(port, dmx_buffer);
                dmx_handler_send_dmx(port, dmx_buffer, 512);
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(23)); // ~44Hz
    }
}
```

**Task allocation:**
- **Core 0:**
  - Network task (Ethernet/WiFi management)
  - Protocol receivers (Art-Net, sACN)
  - Web server
  - LED manager
  
- **Core 1:**
  - DMX output task
  - RDM handling
  - Merge engine

**Test:**
- Full integration test
- Art-Net → Merge → DMX output
- sACN → Merge → DMX output
- Web interface → Config → Apply
- RDM discovery end-to-end

---

### Phase 9: Optimization & Testing (Sprint 14-15)
**Goal:** Performance tuning and testing

**Performance targets:**
- DMX refresh rate: 40-44 Hz
- Web response time: < 200ms
- Merge processing: < 5ms
- Packet loss: < 0.1%

**Testing checklist:**
- [ ] All individual modules tested
- [ ] Integration testing with real hardware
- [ ] Long-term stability test (24h+)
- [ ] Performance profiling
- [ ] Memory leak detection
- [ ] Edge cases (network loss, rapid config changes, etc.)

**Optimization areas:**
1. **DMA for SPI**: W5500 Ethernet performance
2. **Zero-copy buffers**: Reduce memcpy overhead
3. **Task priorities**: Ensure DMX timing
4. **PSRAM usage**: Offload non-critical data

**Document:** follow `docs/TESTING_GUIDE.md`

---

## 6. REFERENCES

### 6.1. ESP-IDF Documentation
- Build System: https://docs.espressif.com/projects/esp-idf/en/v5.2.6/esp32s3/api-guides/build-system.html
- Ethernet: https://docs.espressif.com/projects/esp-idf/en/v5.2.6/esp32s3/api-reference/network/esp_eth.html
- WiFi: https://docs.espressif.com/projects/esp-idf/en/v5.2.6/esp32s3/api-reference/network/esp_wifi.html
- HTTP Server: https://docs.espressif.com/projects/esp-idf/en/v5.2.6/esp32s3/api-reference/protocols/esp_http_server.html
- LittleFS: https://github.com/joltwallet/esp_littlefs

### 6.2. External Libraries
- esp-dmx: https://github.com/someweisguy/esp-dmx
- libe131: https://github.com/hhromic/libe131
- Art-Net: https://github.com/hideakitai/ArtNet

### 6.3. Protocol Standards
- Art-Net v4: https://art-net.org.uk/
- sACN (E1.31): https://tsp.esta.org/tsp/documents/docs/ANSI_E1-31-2018.pdf
- DMX512: https://tsp.esta.org/tsp/documents/docs/ANSI-ESTA_E1-11_2008R2018.pdf
- RDM: https://tsp.esta.org/tsp/documents/docs/ANSI-ESTA_E1-20_2010.pdf

### 6.4. Project Documentation
- [OVERVIEW.md](OVERVIEW.md) - System overview
- [CODING_STANDARDS.md](CODING_STANDARDS.md) - Coding standards
- [TESTING_GUIDE.md](TESTING_GUIDE.md) - Testing procedures
- [LIBRARY_INTEGRATION_GUIDE.md](LIBRARY_INTEGRATION_GUIDE.md) - Library integration details
- [IMPLEMENTATION_ROADMAP.md](IMPLEMENTATION_ROADMAP.md) - Detailed roadmap

---

## APPENDIX A: SDKCONFIG.DEFAULTS

```ini
# ESP32-S3 Configuration
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y

# FreeRTOS
CONFIG_FREERTOS_HZ=1000
CONFIG_FREERTOS_UNICORE=n

# PSRAM
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y

# Ethernet
CONFIG_ETH_USE_SPI_ETHERNET=y
CONFIG_ETH_SPI_ETHERNET_W5500=y

# HTTP Server
CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024
CONFIG_HTTPD_MAX_URI_LEN=512
CONFIG_HTTPD_WS_SUPPORT=y

# LwIP
CONFIG_LWIP_MAX_SOCKETS=16
CONFIG_LWIP_SO_REUSE=y
CONFIG_LWIP_SO_RCVBUF=y
CONFIG_LWIP_IGMP=y

# LittleFS
CONFIG_LITTLEFS_MAX_PARTITIONS=3
CONFIG_LITTLEFS_PAGE_SIZE=256

# Logging
CONFIG_LOG_DEFAULT_LEVEL_INFO=y

# Performance
CONFIG_COMPILER_OPTIMIZATION_PERF=y
```

---

## APPENDIX B: GITIGNORE

```gitignore
# ESP-IDF
build/
sdkconfig
sdkconfig.old
dependencies.lock

# IDE
.vscode/
.idea/
*.swp
*.swo

# OS
.DS_Store
Thumbs.db

# Test artifacts
test_output/
*.log
```

---

**This document will be kept up to date during development.**
