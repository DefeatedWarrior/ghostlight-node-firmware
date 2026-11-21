# FIRMWARE DESIGN DOCUMENT  
**PROJECT: Artnet-Node-2RDM**  
**Platform: ESP32-S3-WROOM-1-N16R8 (16MB Flash, 8MB PSRAM)**  
**Development environment: ESP-IDF v5.2.6**

**Version: 1.0**  
**Created: 25/12/2025**

---

## 1. System overview

**Artnet-Node-2RDM** is a device that converts network protocols into DMX512/RDM signals on 2 independent ports.  
The device supports:

- **Source protocols**: Art-Net v4, sACN (E1.31)
- **DMX modes**: Out, In, RDM Master, RDM Responder, Disabled (separately for each port)
- **Merge Engine**: HTP, LTP, Last, Backup
- **Network connection**: W5500 Ethernet (preferred), fallback to WiFi STA/AP
- **RDM**: discovery, get/set (bidirectional)
- **Web interface**: configuration, DMX test, RDM control, status
- **Status LED**: WS2812 (GPIO 48)
- **Storage**: LittleFS (config.json + web files)

---

## 2. Fixed hardware parameters

| Function                   | GPIO / Pin                   | Notes                                                                   |
|----------------------------|------------------------------|-------------------------------------------------------------------------|
| WS2812 LED                 | GPIO 48                      | Data pin                                                                |
| DMX Port 1 TX              | GPIO 17                      | UART TX (or GPIO matrix)                                                |
| DMX Port 1 RX              | GPIO 16                      | UART RX                                                                 |
| DMX Port 1 DIR             | GPIO 21                      | HIGH = TX, LOW = RX                                                     |
| DMX Port 2 TX              | GPIO 19                      | UART TX                                                                 |
| DMX Port 2 RX              | GPIO 18                      | UART RX                                                                 |
| DMX Port 2 DIR             | GPIO 20                      | HIGH = TX, LOW = RX                                                     |
| W5500 SPI CS               | GPIO 10                      | Chip Select                                                             |
| W5500 SPI MOSI             | GPIO 11                      | VSPI MOSI                                                               |
| W5500 SPI MISO             | GPIO 13                      | VSPI MISO                                                               |
| W5500 SPI SCK              | GPIO 12                      | VSPI SCK                                                                |
| W5500 INT (optional)       | GPIO 9                       | Interrupt                                                               |

---

## 3. System module structure

| Module                     | Main responsibility                                                               | Main dependencies                   |
|----------------------------|-----------------------------------------------------------------------------------|-------------------------------------|
| **Configuration**          | Manages all configuration settings                                                | Storage                             |
| **Network**                | Ethernet W5500 + WiFi STA/AP                                                      | —                                   |
| **LED Manager**            | Drives the WS2812 status LED                                                      | —                                   |
| **DMX/RDM Handler**        | Manages the 2 DMX512/RDM ports (In/Out)                                           | Configuration                       |
| **Merge Engine**           | Merges data from several sources                                                  | DMX/RDM Handler                     |
| **Art-Net Receiver**       | Receives & handles Art-Net UDP                                                    | Network                             |
| **sACN Receiver**          | Receives & handles sACN multicast                                                 | Network                             |
| **Web Server**             | Web interface (HTTP + WebSocket)                                                  | Configuration, DMX/RDM, Network     |
| **Storage**                | LittleFS – stores config.json & web files                                         | —                                   |

---

## 4. Detailed functional requirements for each module

### 4.1. Configuration

**Main configuration settings**:

| Group                 | Setting                                       | Data type    | Default / Range                                      |
|-----------------------|-----------------------------------------------|--------------|------------------------------------------------------|
| Network               | use_ethernet                                  | bool         | true                                                 |
|                       | wifi_profiles[5]                              | array        | [{ssid, pass, priority}]                             |
|                       | ap_ssid                                       | string       | "ArtnetNode-XXXX"                                    |
|                       | ap_password                                   | string       | "12345678"                                           |
|                       | ap_channel                                    | uint8_t      | 1–13                                                 |
| DMX Ports             | port1_mode                                    | enum         | DMX_OUT / DMX_IN / RDM_MASTER / RDM_RESPONDER / DISABLED |
|                       | port2_mode                                    | enum         | ...                                                  |
|                       | universe_primary_port1 / _port2               | uint16_t     | 0–32767                                              |
|                       | universe_secondary_port1 / _port2             | int16_t      | -1 (off) or 0–32767                                  |
|                       | universe_offset_port1 / _port2                | int16_t      | -512 to +512                                         |
| Protocol              | protocol_mode_port1 / _port2                  | enum         | ARTNET_ONLY / SACN_ONLY / ARTNET_PRIORITY / SACN_PRIORITY / MERGE_BOTH |
| Merge                 | merge_mode_port1 / _port2                     | enum         | HTP / LTP / LAST / BACKUP / DISABLE                  |
|                       | merge_timeout_seconds                         | uint8_t      | 2–10 (seconds)                                       |
| RDM                   | rdm_enabled_port1 / _port2                    | bool         | true                                                 |
| Node Info             | node_short_name                               | string       | max 18 characters                                    |
|                       | node_long_name                                | string       | max 64 characters                                    |

### 4.2. Network

- W5500 Ethernet first (3 tries, 10s timeout each)
- If Ethernet fails → try WiFi STA in the profiles' priority order
- If WiFi STA fails → switch to WiFi AP
- Static IP supported for Ethernet & WiFi STA

### 4.3. LED Manager

**LED states**:

| State                             | RGB color          | Behavior                         |
|-----------------------------------|--------------------|----------------------------------|
| Boot                              | Light blue         | Steady                           |
| Ethernet OK                       | Green              | Steady                           |
| WiFi STA OK                       | Green + blink      | Slow blink                       |
| WiFi AP active                    | Purple             | Steady                           |
| Art-Net/sACN received             | White              | One blink (max 10 Hz)            |
| RDM discovery in progress         | Yellow             | Slow blink                       |
| Serious error                     | Red                | Fast blink 200ms                 |

### 4.4. DMX/RDM Handler

- DMX Out mode: sends 512-channel DMX frames + start code 0x00
- DMX In mode: reads the DMX input and sends it to the network (Art-Net/sACN)
- RDM Master mode: discovery, get/set PIDs
- RDM Responder mode: answers RDM requests
- DMX Out rate: ~44 Hz
- Blackout if there is no new data within merge_timeout

### 4.5. Merge Engine

**Merge modes**:

| Mode     | Details                                                                        |
|----------|--------------------------------------------------------------------------------|
| HTP      | Highest Takes Precedence                                                       |
| LTP      | Lowest Takes Precedence                                                        |
| Last     | The last packet received wins                                                  |
| Backup   | Primary source + backup (switches to backup if the primary is gone > timeout)  |
| Disable  | No merging, only the preferred source is used                                  |

### 4.6. Art-Net Receiver

- Receives ArtDmx, ArtPoll, ArtPollReply, ArtSync, ArtAddress
- Supports the Net/Sub/Universe format

### 4.7. sACN Receiver

- Receives E1.31 Data packets over multicast
- Checks the Sequence Number, Source Name, Priority
- Join multicast group theo universe

### 4.8. Web Server

**Main endpoints**:

| Path                               | Method | Description                                                           |
|------------------------------------|--------|-----------------------------------------------------------------------|
| `/`                                | GET    | Main page (index.html)                                                |
| `/config`                          | GET/POST | Get/set the whole configuration                                       |
| `/status`                          | GET    | Status: IP, link, packet count, DMX levels                            |
| `/wifi/scan`                       | GET    | Scan WiFi networks                                                    |
| `/wifi/profiles`                   | GET/POST/DELETE | Manage WiFi STA profiles                                              |
| `/ap/config`                       | GET/POST | WiFi AP configuration                                                 |
| `/ports/config`                    | GET/POST | DMX/RDM mode & universe for each port                                 |
| `/protocol/config`                 | GET/POST | Art-Net / sACN mode                                                   |
| `/merge/config`                    | GET/POST | Merge Engine configuration                                            |
| `/test/dmx/port1` / `/port2`       | WS     | WebSocket real-time DMX levels & test                                 |
| `/rdm/discover/port1` / `/port2`   | POST   | Start RDM discovery                                                   |
| `/rdm/devices/port1` / `/port2`    | GET    | List of RDM devices                                                   |
| `/rdm/set/port1` / `/port2`        | POST   | Set RDM parameter                                                     |
| `/rdm/get/port1` / `/port2`        | POST   | Get RDM parameter                                                     |

**WebSocket payload**:

- `dmx_update`: levels of the 512 channels
- `dmx_test`: the client sends a channel + value
- `rdm_event`: device_found, error, response
- `status_update`: packet rate, link status

### 4.9. Storage

- LittleFS partition ~1MB
- File: `config.json`, `index.html`, `script.js`, `style.css`

---

## 5. Overall operation flow

1. **Boot** → NVS → LittleFS → load config → blue LED
2. **Network init** → Ethernet (3 tries) → WiFi STA → AP → matching LED
3. **DMX/RDM init** → configured from port_mode, universe
4. **Protocol init** → Join multicast sACN, bind UDP Art-Net
5. **Network task (Core 0)** → receives Art-Net/sACN → Merge Engine → DMX queue
6. **DMX Output task (Core 1)** → reads the queue → sends the DMX frame → handles RDM
7. **Web Server** → serves HTTP/WebSocket → updates the configuration → DMX/RDM test
8. **LED** → updates the state continuously

---

This document covers **the whole firmware design**
