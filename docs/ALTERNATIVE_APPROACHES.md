# ALTERNATIVE APPROACHES AND COMPARISON
**Project: ESP-NODE-2RDM**

**Version:** 1.0  
**Date:** 25/12/2025

---

## CONTENTS
1. [Overview](#1-overview)
2. [DMX/RDM Implementation](#2-dmxrdm-implementation)
3. [sACN Implementation](#3-sacn-implementation)
4. [Art-Net Implementation](#4-art-net-implementation)
5. [Storage Solutions](#5-storage-solutions)
6. [Web Server Options](#6-web-server-options)
7. [Network Stack](#7-network-stack)
8. [Merge Engine Algorithms](#8-merge-engine-algorithms)
9. [Task Architecture](#9-task-architecture)
10. [Final recommendation](#10-final-recommendation)

---

## 1. OVERVIEW

This document compares the different ways each module in the project could be implemented, covering:
- Pros / Cons
- Complexity
- Performance
- Maintainability
- Recommendation

---

## 2. DMX/RDM IMPLEMENTATION

### Option 1: esp-dmx Library (RECOMMENDED)

**Repository:** https://github.com/someweisguy/esp-dmx

**Pros:**
- ✅ Dedicated library, well maintained
- ✅ Full DMX512 + RDM support
- ✅ Accurate timing (hardware UART)
- ✅ Simple, easy-to-use API
- ✅ Already tested with many RDM devices
- ✅ Complete documentation
- ✅ Supports multiple ports

**Cons:**
- ❌ Depends on an external library
- ❌ You need to understand the RDM protocol to debug it

**Complexity:** Low  
**Performance:** Excellent (hardware UART)  
**Maintainability:** Cao (well-maintained library)

**Use case:** Every serious DMX/RDM project

---

### Option 2: Custom DMX Implementation

**Approach:** Write our own DMX driver on the UART HAL

**Pros:**
- ✅ Full control over implementation
- ✅ No external dependencies
- ✅ Can be optimised for a specific use case

**Cons:**
- ❌ Very complex (DMX timing is critical)
- ❌ RDM is very complex (discovery, collision detection)
- ❌ Must be tested against many devices ourselves
- ❌ Takes a long time to develop
- ❌ Timing bugs are easy to introduce

**Complexity:** Very high  
**Performance:** Depends on the implementation  
**Maintainability:** Low (custom code)

**Use case:** Only for special requirements that esp-dmx cannot meet

**Conclusion:** ❌ NOT recommended - esp-dmx is good enough

---

### Option 3: Arduino DMX Library Port

**Examples:** DMXSerial, DmxSimple

**Pros:**
- ✅ Many libraries available
- ✅ Simple for basic DMX output

**Cons:**
- ❌ Arduino libraries are hard to port to ESP-IDF
- ❌ Usually no RDM support
- ❌ Performance is not optimal
- ❌ Depends on the Arduino framework

**Conclusion:** ❌ NOT suitable for an ESP-IDF project

---

## 3. sACN IMPLEMENTATION

### Option 1: libe131 (RECOMMENDED)

**Repository:** https://github.com/hhromic/libe131

**Pros:**
- ✅ Lightweight (~500 lines)
- ✅ Pure C, easy to port
- ✅ Full multicast support
- ✅ Packet validation built-in
- ✅ Simple API

**Cons:**
- ❌ Needs a CMakeLists.txt wrapper
- ❌ Documentation is a bit sparse

**Complexity:** Low  
**Performance:** Excellent  
**Maintainability:** Cao

**Use case:** Default choice cho sACN receiver

---

### Option 2: Custom sACN Implementation

**Approach:** Implement the E1.31 protocol ourselves

**sACN packet structure:**
```c
typedef struct {
    // Root Layer
    uint16_t preamble_size;
    uint16_t postamble_size;
    uint8_t acn_pid[12];
    uint16_t flags_length;
    uint32_t vector;
    uint8_t cid[16];
    
    // Framing Layer
    uint16_t framing_flags_length;
    uint32_t framing_vector;
    uint8_t source_name[64];
    uint8_t priority;
    uint16_t reserved;
    uint8_t sequence;
    uint8_t options;
    uint16_t universe;
    
    // DMP Layer
    uint16_t dmp_flags_length;
    uint8_t dmp_vector;
    uint8_t address_type;
    uint16_t first_address;
    uint16_t address_increment;
    uint16_t property_value_count;
    uint8_t start_code;
    uint8_t dmx_data[512];
} __attribute__((packed)) sacn_packet_t;
```

**Pros:**
- ✅ No dependencies
- ✅ Full control
- ✅ Can be optimised for a specific use case

**Cons:**
- ❌ The protocol is fairly complex (3 layers)
- ❌ Multicast join/leave has to be handled
- ❌ The validation logic needs careful implementation
- ❌ Takes development time

**Complexity:** Medium  
**Performance:** Depends on the implementation

**Conclusion:** ⚠️ Only use this if libe131 has problems

---

### Option 3: ESP-IDF Examples

**Source:** ESP-IDF has no sACN example

**Conclusion:** ❌ Not available

---

## 4. ART-NET IMPLEMENTATION

### Option 1: Custom Minimal Implementation (RECOMMENDED)

**Approach:** Write a minimal Art-Net receiver ourselves

**Why it is recommended:**
- ✅ Art-Net is much simpler than sACN
- ✅ Only the ArtDmx packet needs handling (18-byte header + 512 data)
- ✅ No dependencies
- ✅ Full control
- ✅ Code < 200 lines

**Implementation sketch:**
```c
// Art-Net header is just 18 bytes
typedef struct {
    uint8_t id[8];       // "Art-Net\0"
    uint16_t opcode;     // 0x5000 for ArtDmx
    uint16_t version;    // 14
    uint8_t sequence;
    uint8_t physical;
    uint16_t universe;   // 15 bits
    uint16_t length;     // Big-endian
    uint8_t data[512];
} artnet_dmx_t;

// UDP receive loop
while (1) {
    recvfrom(sock, buffer, 1024, 0, NULL, NULL);
    if (memcmp(buffer, "Art-Net\0", 8) == 0) {
        artnet_dmx_t *pkt = (artnet_dmx_t*)buffer;
        if (pkt->opcode == 0x5000) {
            // Process DMX data
        }
    }
}
```

**Complexity:** Low  
**Development time:** ~4 hours

---

### Option 2: Port hideakitai/ArtNet

**Repository:** https://github.com/hideakitai/ArtNet

**Pros:**
- ✅ Full Art-Net v4 support
- ✅ Handles ArtPoll, ArtPollReply, etc.

**Cons:**
- ❌ Arduino library, needs porting
- ❌ Complicated dependencies
- ❌ Overkill cho simple receiver
- ❌ Porting takes time

**Conclusion:** ❌ Not recommended - a custom implementation is much simpler

---

### Option 3: libartnet

**Repository:** https://github.com/OpenLightingProject/libartnet

**Pros:**
- ✅ Production-quality
- ✅ Full protocol support

**Cons:**
- ❌ Heavy library
- ❌ Designed for Linux
- ❌ Hard to port to the ESP32

**Conclusion:** ❌ Not suitable for embedded use

---

## 5. STORAGE SOLUTIONS

### Option 1: LittleFS (RECOMMENDED)

**Component:** esp_littlefs

**Pros:**
- ✅ Modern file system
- ✅ Wear leveling built-in
- ✅ Power-fail safe
- ✅ Better than SPIFFS
- ✅ Standard file I/O API (fopen, fread, fwrite)
- ✅ Support large files

**Cons:**
- ❌ A little slower than NVS for small data
- ❌ Needs its own partition

**Use case:** Config files, web files, logs

---

### Option 2: NVS (Non-Volatile Storage)

**Pros:**
- ✅ Built-in ESP-IDF
- ✅ Very fast for key-value data
- ✅ Does not need a large partition

**Cons:**
- ❌ Key-value only, not a file system
- ❌ Hard to store large JSON
- ❌ More complicated API

**Use case:** Backup storage cho critical config (WiFi credentials, etc.)

---

### Option 3: SPIFFS

**Pros:**
- ✅ Legacy support

**Cons:**
- ❌ Deprecated trong ESP-IDF
- ❌ More bugs than LittleFS
- ❌ Not power-fail safe

**Conclusion:** ❌ NOT recommended - use LittleFS instead

---

### Option 4: FAT File System

**Pros:**
- ✅ Compatible with PC
- ✅ Easy to debug (mount it on a PC)

**Cons:**
- ❌ Not optimised for flash
- ❌ No wear leveling
- ❌ Wastes space

**Conclusion:** ❌ Not suitable

---

**Overall recommendation:**
- **Primary storage:** LittleFS (config.json, web files)
- **Backup storage:** NVS (critical WiFi credentials)

---

## 6. WEB SERVER OPTIONS

### Option 1: esp_http_server (RECOMMENDED)

**Component:** ESP-IDF official

**Pros:**
- ✅ Official, stable
- ✅ WebSocket support built-in
- ✅ Chunked transfer encoding
- ✅ Multi-threading support
- ✅ Complete documentation

**Cons:**
- ❌ The API is a bit verbose
- ❌ Buffers must be handled manually

**Use case:** Default choice

---

### Option 2: ESP Async WebServer

**Repository:** External library

**Pros:**
- ✅ Asynchronous
- ✅ Better performance cho concurrent requests
- ✅ Elegant API

**Cons:**
- ❌ Not official
- ❌ Complex dependency chain
- ❌ Harder to debug
- ❌ Overkill cho this project

**Conclusion:** ⚠️ Only use this if high-performance concurrent web serving is needed

---

### Option 3: Custom HTTP Server

**Approach:** Raw sockets + HTTP parsing

**Conclusion:** ❌ Absolutely NO - don't reinvent the wheel

---

## 7. NETWORK STACK

### Option 1: lwIP (ESP-IDF Default) (RECOMMENDED)

**Pros:**
- ✅ Built-in ESP-IDF
- ✅ Mature, stable
- ✅ Full TCP/IP stack
- ✅ IGMP multicast support
- ✅ Good performance

**Cons:**
- ❌ Configuration can be complicated

**Use case:** Always use this

---

### Option 2: Custom TCP/IP

**Conclusion:** ❌ Never - this is insane

---

## 8. MERGE ENGINE ALGORITHMS

### HTP (Highest Takes Precedence)

**Implementation:**
```c
void merge_htp(uint8_t *output, merge_source_t sources[], int num_sources) {
    for (int ch = 0; ch < 512; ch++) {
        uint8_t max = 0;
        for (int src = 0; src < num_sources; src++) {
            if (sources[src].valid && sources[src].data[ch] > max) {
                max = sources[src].data[ch];
            }
        }
        output[ch] = max;
    }
}
```

**Complexity:** O(n * 512) where n = sources  
**Use case:** Lighting (multiple operators, highest wins)

---

### LTP (Latest Takes Precedence)

**Implementation:**
```c
void merge_ltp(uint8_t *output, merge_source_t sources[], int num_sources) {
    for (int ch = 0; ch < 512; ch++) {
        uint32_t latest_time = 0;
        uint8_t latest_value = 0;
        
        for (int src = 0; src < num_sources; src++) {
            if (sources[src].valid && 
                sources[src].timestamp_ms > latest_time &&
                sources[src].data[ch] != output[ch]) {
                
                latest_time = sources[src].timestamp_ms;
                latest_value = sources[src].data[ch];
            }
        }
        
        if (latest_time > 0) {
            output[ch] = latest_value;
        }
    }
}
```

**Complexity:** O(n * 512)  
**Use case:** Moving lights (last command wins)

---

### LAST (Entire Frame)

**Implementation:**
```c
void merge_last(uint8_t *output, merge_source_t sources[], int num_sources) {
    uint32_t latest_time = 0;
    int latest_src = -1;
    
    for (int src = 0; src < num_sources; src++) {
        if (sources[src].valid && sources[src].timestamp_ms > latest_time) {
            latest_time = sources[src].timestamp_ms;
            latest_src = src;
        }
    }
    
    if (latest_src >= 0) {
        memcpy(output, sources[latest_src].data, 512);
    }
}
```

**Complexity:** O(n)  
**Use case:** Simplest, entire universe from one source

---

### BACKUP (Primary + Failover)

**Implementation:**
```c
void merge_backup(uint8_t *output, merge_source_t sources[], int num_sources, uint32_t timeout) {
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Check primary (index 0)
    if (sources[0].valid && (now - sources[0].timestamp_ms) < timeout) {
        memcpy(output, sources[0].data, 512);
        return;
    }
    
    // Fallback to backup (index 1)
    if (num_sources > 1 && sources[1].valid && 
        (now - sources[1].timestamp_ms) < timeout) {
        memcpy(output, sources[1].data, 512);
        return;
    }
    
    // No valid source → blackout
    memset(output, 0, 512);
}
```

**Complexity:** O(1)  
**Use case:** Critical installations with backup console

---

**Performance comparison:**
| Mode | Complexity | Processing Time (estimate) |
|------|-----------|---------------------------|
| HTP | O(n*512) | ~2-4ms for 4 sources |
| LTP | O(n*512) | ~3-5ms for 4 sources |
| LAST | O(n) | <0.5ms |
| BACKUP | O(1) | <0.1ms |

**Recommendation:** Implement all modes, let the user choose

---

## 9. TASK ARCHITECTURE

### Option 1: Core Pinning (RECOMMENDED)

**Approach:** Pin time-critical tasks to Core 1

**Architecture:**
```
Core 0 (Protocol)              Core 1 (DMX Output)
├── Network task               ├── DMX Port 1 task
├── Art-Net receiver           ├── DMX Port 2 task
├── sACN receiver              └── RDM handler
├── Web server
├── LED manager
└── Merge engine
```

**Pros:**
- ✅ DMX timing is not interrupted by the network
- ✅ Predictable performance
- ✅ Easy to debug

**Cons:**
- ❌ Inter-core communication needs care

---

### Option 2: Single-core (Not for ESP32-S3)

**Conclusion:** ❌ Waste of dual-core capability

---

### Option 3: Dynamic Load Balancing

**Approach:** FreeRTOS automatic scheduling

**Cons:**
- ❌ Unpredictable DMX timing
- ❌ Hard to debug performance issues

**Conclusion:** ❌ Not recommended for real-time DMX

---

## 10. FINAL RECOMMENDATION

### Must-Use Libraries

| Component | Library | Reason |
|-----------|---------|--------|
| DMX/RDM | esp-dmx | Best DMX library for ESP32 |
| sACN | libe131 | Lightweight, proven |
| Art-Net | Custom minimal | Simple protocol, <200 lines |
| Storage | LittleFS | Modern, reliable |
| Web Server | esp_http_server | Official, stable |
| LED | led_strip | Official RMT driver |
| JSON | cJSON | Lightweight |
| Network | lwIP | Built-in |

### Architecture Summary

```
Application Layer
├── Web Server (esp_http_server)
├── Configuration (cJSON + LittleFS)
└── LED Manager (led_strip)

Protocol Layer
├── Art-Net Receiver (custom)
├── sACN Receiver (libe131)
└── Merge Engine (custom)

Output Layer
└── DMX/RDM Handler (esp-dmx)

Infrastructure
├── Network Manager (esp_eth + esp_wifi)
└── Storage Manager (esp_littlefs)
```

### Why This Stack?

1. **Maximum Library Reuse:** 80% existing libraries
2. **Minimal Custom Code:** only the Art-Net receiver and the merge engine are written ourselves (both simple)
3. **Production Quality:** every library is proven
4. **Easy Maintenance:** Well-documented components
5. **Good Performance:** Hardware-accelerated where possible

### Development Effort Estimate

| Component | Effort | Reason |
|-----------|--------|--------|
| Project Setup | Low | ESP-IDF templates |
| Storage + Config | Low | Standard file I/O |
| LED Manager | Low | Simple led_strip API |
| Network | Medium | Fallback logic |
| DMX/RDM | Low | esp-dmx handles complexity |
| Art-Net | Low | Simple protocol |
| sACN | Low | libe131 integration |
| Merge Engine | Medium | Algorithm implementation |
| Web Server | Medium | UI development |
| Integration | Medium | Task coordination |
| Testing | High | Physical hardware testing |

**Total:** ~15 sprints (no specific time estimate)

---

## CONCLUSION

The **"Library-First"** strategy is the optimal choice:
- ✅ Shorter development time
- ✅ Higher reliability (proven code)
- ✅ Easy to maintain
- ✅ Focus on the business logic instead of reinventing

**Only write our own code when:**
- Simple protocol (Art-Net)
- Custom logic required (Merge Engine)
- No suitable library exists

**Avoid:**
- ❌ Reinvent TCP/IP stack
- ❌ A custom DMX driver (esp-dmx is already excellent)
- ❌ Porting Arduino libraries (hard and not optimal)

---

**This document is the result of analysing the feasible options. The final recommendation has been folded into FIRMWARE_DEVELOPMENT_PLAN.md.**
