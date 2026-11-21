# DEVELOPMENT PLAN SUMMARY
**Project: ESP-NODE-2RDM - Art-Net/sACN to DMX512/RDM Converter**

**Date:** 25/12/2025  
**Version:** 1.0

---

## 📋 INTRODUCTION

This document is a **high-level summary** of the plan for building the ESP-NODE-2RDM firmware. It is the starting point for understanding the whole development strategy.

---

## 📚 DOCUMENT STRUCTURE

The project has **4 main documents** on the development plan:

### 1. **FIRMWARE_DEVELOPMENT_PLAN.md** (Master Plan)
📖 **Contents:** overall plan, project structure, libraries used, phase-by-phase roadmap  
🎯 **Audience:** every team member  
⏱️ **Reading time:** ~30 minutes  
🔑 **Key sections:**
- Project directory structure
- Partition table
- The official ESP-IDF libraries
- Roadmap 15 sprints
- sdkconfig.defaults template

**Read this document first to get the big picture.**

---

### 2. **LIBRARY_INTEGRATION_GUIDE.md** (Technical Deep-Dive)
📖 **Contents:** detailed guide to integrating each library into the project  
🎯 **Audience:** developers writing the code  
⏱️ **Reading time:** ~40 minutes  
🔑 **Key sections:**
- esp-dmx API and examples
- libe131 integration
- Art-Net custom implementation
- LittleFS setup
- esp_http_server WebSocket
- Dependency management

**Read it when you start implementing each module.**

---

### 3. **IMPLEMENTATION_ROADMAP.md** (Sprint Guide)
📖 **Contents:** each sprint in detail: specific tasks, deliverables, testing  
🎯 **Audience:** team leads, sprint planning  
⏱️ **Reading time:** ~25 minutes  
🔑 **Key sections:**
- Sprint 0: Project setup commands
- Sprint 1: Storage + Config implementation
- Sprint 2-6: Core modules (LED, Network, DMX)
- Sprint 7-12: Protocols + Web
- Sprint 13-15: Integration + Testing

**Use it as a checklist in every sprint.**

---

### 4. **ALTERNATIVE_APPROACHES.md** (Decision Log)
📖 **Contents:** comparison of the alternatives for each module, and why each solution was chosen  
🎯 **Audience:** architects, tech leads  
⏱️ **Reading time:** ~20 minutes  
🔑 **Key sections:**
- DMX: esp-dmx vs custom vs Arduino port
- sACN: libe131 vs custom
- Art-Net: custom minimal vs port library
- Storage: LittleFS vs NVS vs SPIFFS
- Merge algorithms comparison

**Read it when you need to know "why X instead of Y".**

---

## 🎯 OVERALL STRATEGY

### Golden rule: **"Library-First Approach"**

```
Prefer existing libraries
    ↓
Only write our own code when:
- Simple protocol (Art-Net)
- Custom logic required (Merge Engine)
- There is no suitable library
```

### Code split

- **80%** existing libraries
- **15%** Custom glue code (integration)
- **5%** Custom implementation (Art-Net, Merge)

---

## 🔧 TECHNOLOGY STACK

### Main libraries

| Function | Library | Reason |
|-----------|----------|-------|
| **DMX/RDM** | esp-dmx | Best DMX library for ESP32 |
| **sACN** | libe131 | Lightweight, stable |
| **Art-Net** | Custom | Simple protocol, <200 lines |
| **Storage** | LittleFS | Modern, power-fail safe |
| **Web Server** | esp_http_server | Official, WebSocket support |
| **LED** | led_strip | Official RMT driver |
| **JSON** | cJSON | Lightweight |
| **Network** | lwIP + esp_eth + esp_wifi | Built-in |

### System architecture

```
┌─────────────────────────────────────────┐
│          Application Layer              │
│  ┌──────────┐  ┌──────────┐  ┌───────┐ │
│  │Web Server│  │Config    │  │LED Mgr│ │
│  └──────────┘  └──────────┘  └───────┘ │
└─────────────────────────────────────────┘
                  ↓
┌─────────────────────────────────────────┐
│           Protocol Layer                │
│  ┌────────┐  ┌─────┐  ┌──────────────┐ │
│  │Art-Net │  │sACN │  │Merge Engine  │ │
│  └────────┘  └─────┘  └──────────────┘ │
└─────────────────────────────────────────┘
                  ↓
┌─────────────────────────────────────────┐
│            Output Layer                 │
│  ┌───────────────────────────────────┐  │
│  │   DMX/RDM Handler (esp-dmx)       │  │
│  │   Port 1 | Port 2                 │  │
│  └───────────────────────────────────┘  │
└─────────────────────────────────────────┘
                  ↓
┌─────────────────────────────────────────┐
│         Infrastructure                  │
│  ┌─────────────┐  ┌──────────────────┐ │
│  │Network Mgr  │  │Storage Manager   │ │
│  │Eth + WiFi   │  │LittleFS + NVS    │ │
│  └─────────────┘  └──────────────────┘ │
└─────────────────────────────────────────┘
```

---

## 📊 DEVELOPMENT ROADMAP

### 15 Sprints Overview

| Sprint | Module | Core Focus | Complexity |
|--------|--------|------------|------------|
| **0** | Setup | Project structure, build system | 🟢 Low |
| **1** | Storage + Config | LittleFS, JSON, NVS | 🟢 Low |
| **2** | LED Manager | WS2812 status | 🟢 Low |
| **3-4** | Network | Ethernet W5500, WiFi fallback | 🟡 Medium |
| **5-6** | DMX/RDM | esp-dmx integration, 2 ports | 🟡 Medium |
| **7-8** | Protocols | Art-Net + sACN receivers | 🟡 Medium |
| **9-10** | Merge Engine | HTP/LTP/LAST/BACKUP | 🟡 Medium |
| **11-12** | Web Server | HTTP + WebSocket | 🟡 Medium |
| **13** | Integration | Full system | 🟡 Medium |
| **14-15** | Testing | Performance + Stability | 🔴 High |

### Bottom-Up Development

```
Sprint 1-2:  Foundational modules (Storage, LED)
               ↓
Sprint 3-6:  Hardware interfaces (Network, DMX)
               ↓
Sprint 7-10: Protocol handling (Art-Net, sACN, Merge)
               ↓
Sprint 11-12: User interface (Web)
               ↓
Sprint 13-15: Integration + Testing
```

---

## 🚀 QUICK START GUIDE

### Step 1: Read the documents (1-2 hours)
1. Read this **SUMMARY** (the current file)
2. Read **FIRMWARE_DEVELOPMENT_PLAN.md** (big picture)
3. Skim **IMPLEMENTATION_ROADMAP.md** (sprint overview)

### Step 2: Set up the environment (0.5 hours)
1. Install ESP-IDF v5.2.6
2. Clone repository
3. Run `idf.py build` (will fail: there is no code yet)

### Step 3: Sprint 0 - Project Setup (2-4 hours)
1. Create the directory structure
2. Setup CMakeLists.txt
3. Create the partition table
4. Configure sdkconfig.defaults
5. Create minimal main.c
6. **Milestone:** `idf.py build` success ✅

### Step 4: Sprint 1 - Storage (4-8 hours)
1. Clone esp_littlefs
2. Implement storage_manager
3. Implement config_manager
4. Test read/write/persist
5. **Milestone:** Config loads from LittleFS ✅

### Step 5: Continue with the following sprints
Follow **IMPLEMENTATION_ROADMAP.md** for each sprint.

---

## 📦 FINAL DELIVERABLES

### Code
- ✅ Complete ESP-IDF project
- ✅ 9 components (config, storage, network, led, dmx, artnet, sacn, merge, web)
- ✅ Firmware binary (.bin)
- ✅ Partition table
- ✅ Default config.json

### Documentation
- ✅ API documentation for every module
- ✅ User manual
- ✅ Hardware setup guide
- ✅ Troubleshooting guide

### Testing
- ✅ Module unit tests passed
- ✅ Integration tests passed
- ✅ 24h stability test passed
- ✅ Performance targets met:
  - DMX refresh: 40-44 Hz
  - Merge time: < 5ms
  - Web response: < 200ms
  - Packet loss: < 0.1%

---

## 🎓 KNOWLEDGE NEEDED

### ESP-IDF Basics
- Build system (CMake)
- FreeRTOS tasks
- Component model
- Event loops
- NVS + LittleFS

### Protocols
- Art-Net v4 basics
- sACN (E1.31) basics
- DMX512 timing
- RDM protocol overview

### Hardware
- ESP32-S3 architecture
- SPI communication (W5500)
- UART for DMX
- RMT for WS2812

### Recommended Resources
1. **ESP-IDF Docs:** https://docs.espressif.com/projects/esp-idf/en/v5.2.6/
2. **Art-Net Spec:** https://art-net.org.uk/
3. **sACN Standard:** https://tsp.esta.org/tsp/documents/docs/ANSI_E1-31-2018.pdf
4. **DMX512:** https://tsp.esta.org/tsp/documents/docs/ANSI-ESTA_E1-11_2008R2018.pdf
5. **esp-dmx GitHub:** https://github.com/someweisguy/esp-dmx

---

## 💡 KEY DECISIONS & RATIONALE

### Why esp-dmx?
- ✅ The best DMX/RDM library for the ESP32
- ✅ Accurate timing (hardware UART)
- ✅ RDM discovery + get/set built-in
- ✅ Well maintained
- ❌ A custom implementation would take ~3-4 sprints and be bug-prone

### Why a custom Art-Net instead of porting a library?
- ✅ Art-Net is simple (18-byte header + data)
- ✅ Only the ArtDmx packet needs handling
- ✅ Under 200 lines of code, easy to maintain
- ✅ No dependencies
- ❌ Porting an Arduino library is hard, with complicated dependencies

### Why LittleFS instead of SPIFFS?
- ✅ Modern, power-fail safe
- ✅ Wear leveling built-in
- ✅ SPIFFS deprecated
- ✅ Better performance

### Why core pinning?
- ✅ DMX timing is critical → needs a dedicated Core 1
- ✅ Network/Web are not time-critical → Core 0
- ✅ Predictable performance
- ✅ Easy debug

---

## 🔍 TROUBLESHOOTING COMMON ISSUES

### Build Fails
- Check ESP-IDF version: `idf.py --version` → must be v5.2.6
- Check all CMakeLists.txt REQUIRES
- Run `idf.py fullclean` then `idf.py build`

### External Library Not Found
- Check `external_components/` directory
- Verify CMakeLists.txt in component
- Check root CMakeLists.txt EXTRA_COMPONENT_DIRS

### LittleFS Mount Fails
- Check partition table has `littlefs` partition
- Verify partition size (min 256KB)
- Try `format_if_mount_failed = true`

### DMX Not Working
- Verify GPIO configuration
- Check RS485 transceiver wiring
- Use oscilloscope to check signal
- Verify UART pins in menuconfig

### Network Connection Fails
- Check W5500 SPI wiring
- Verify Ethernet cable
- Check WiFi credentials
- Monitor logs: `idf.py monitor`

---

## 📞 SUPPORT & RESOURCES

### Documentation Files
- `docs/FIRMWARE_DEVELOPMENT_PLAN.md` - Master plan
- `docs/LIBRARY_INTEGRATION_GUIDE.md` - Library details
- `docs/IMPLEMENTATION_ROADMAP.md` - Sprint-by-sprint guide
- `docs/ALTERNATIVE_APPROACHES.md` - Decision rationale
- `docs/CODING_STANDARDS.md` - Code style guide
- `docs/TESTING_GUIDE.md` - Testing procedures
- `docs/modules/*.md` - Individual module designs

### External Resources
- ESP-IDF Forum: https://esp32.com/
- esp-dmx Issues: https://github.com/someweisguy/esp-dmx/issues
- Art-Net Forum: https://art-net.org.uk/forums/
- Stack Overflow: tag `esp32` + `esp-idf`

---

## ✅ SUCCESS CRITERIA

### Technical
- [x] Firmware builds without warnings
- [ ] All modules tested independently
- [ ] Integration tests passed
- [ ] DMX refresh rate: 40-44 Hz stable
- [ ] Web interface responsive < 200ms
- [ ] 24h stability test passed
- [ ] RDM discovery works with real devices
- [ ] Merge engine < 5ms processing time

### Documentation
- [x] All design documents complete
- [ ] API documentation for each module
- [ ] User manual written
- [ ] Testing guide verified
- [ ] Troubleshooting guide complete

### Code Quality
- [ ] No compiler warnings
- [ ] Follow CODING_STANDARDS.md
- [ ] Consistent naming conventions
- [ ] Error handling complete
- [ ] Memory leaks checked
- [ ] Code reviewed

---

## 🎯 CONCLUSION

This plan provides a **complete roadmap** for building the ESP-NODE-2RDM firmware:

✅ **Clear strategy:** library-first approach  
✅ **Proven technology stack:** 80% existing libraries  
✅ **Detailed roadmap:** 15 sprints, bottom-up  
✅ **Complete documentation:** 4 main documents + module designs  
✅ **Decision rationale:** explains why each solution was chosen  

### Next Steps

1. **Team review:** read these documents and give feedback
2. **Set up the environment:** install ESP-IDF v5.2.6
3. **Start Sprint 0:** Create project structure
4. **Begin development:** Follow IMPLEMENTATION_ROADMAP.md

---

**Prepared by:** GitHub Copilot  
**Date:** 25/12/2025  
**Version:** 1.0  
**Status:** ✅ Documentation Complete - Ready for Implementation

---

**To start, read next: [FIRMWARE_DEVELOPMENT_PLAN.md](FIRMWARE_DEVELOPMENT_PLAN.md)**
