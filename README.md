<div align="center">

# Ghostlight Node Firmware

### Professional Art-Net / sACN to DMX512/RDM Converter

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.2.6-blue.svg)](https://github.com/espressif/esp-idf)
[![Platform](https://img.shields.io/badge/Platform-ESP32--S3%20%7C%20ESP32-green.svg)](#-supported-hardware)
[![Status](https://img.shields.io/badge/Status-Hardware%20Testing-orange.svg)](docs/history/PROJECT_COMPLETION_SUMMARY.md)

**2 Independent DMX/RDM Ports • ESP32-S3 or ESP32 • Ethernet + WiFi**

</div>

---

## 📖 Table of Contents
- [Overview](#-overview)
- [Key Features](#-key-features)
- [Supported Hardware](#-supported-hardware)
- [Install from your browser](#-install-from-your-browser)
- [Quick Start](#-quick-start)
- [Documentation](#-documentation)
- [Project Status](#-project-status)
- [Contributing](#-contributing)
- [License](#-license)

## 🎯 Overview

Ghostlight Node is a professional lighting control device that converts network protocols (Art-Net/sACN) to DMX512/RDM signals. It runs on ESP32-S3 and ESP32 boards, with Ethernet (W5500) and WiFi and 2 independent DMX/RDM ports.

It is the firmware for the **Ghostlight Node**, whose hardware (KiCad
project, BOM, assembly and docs) is in [ghostlight-node](https://github.com/DefeatedWarrior/ghostlight-node).
It is not tied to that board: it also runs on ESP32-S3 and ESP32 dev boards
with a W5500 and two RS485 transceivers, and other boards can be added. See
[Supported Hardware](#-supported-hardware).

**Perfect for:**
- Stage lighting control systems
- Architectural lighting installations
- Entertainment venues
- Broadcasting studios
- Theatrical productions

## ✨ Key Features

### Network Protocols
- ✅ **Art-Net v4** - Industry standard UDP protocol (port 6454)
- ✅ **sACN (E1.31)** - Streaming ACN with multicast support
- ✅ **Protocol Priority** - Configurable protocol selection and merge modes
- ✅ **Multi-Source Support** - Handle up to 4 simultaneous sources per port

### DMX512 & RDM
- ✅ **Dual Independent Ports** - 2 fully configurable DMX/RDM ports
- ✅ **DMX Output** - ~44 Hz refresh rate, 512 channels per port
- ✅ **DMX Input** - Monitor incoming DMX data and send it to the network: to everyone (broadcast / multicast), to the Art-Net nodes that output that universe, or to up to 4 addresses you list. The last two send unicast, which Wi-Fi delivers far more reliably
- ✅ **RDM Master** - Device discovery, parameter get/set
- ✅ **RDM Responder** - Respond to RDM queries
- ✅ **Flexible Universe Mapping** - Per-port universe configuration with offset support
- ✅ **Channel Rules** - Per output port, up to 8: **patch** a block of channels onto others, **limit** channels to a maximum level, **park** channels at a fixed level, whatever the console sends
- ✅ **DMX Splitter / Merger** - An output port can take another port's DMX input: on its own (a splitter that follows the desk frame by frame and ignores the network) or merged with the network sources like another console

### Advanced Merge Engine
- ✅ **HTP** (Highest Takes Precedence) - For intensity control
- ✅ **LTP** (Latest Takes Precedence) - The most recent change to a channel wins
- ✅ **LAST** - Last received packet wins
- ✅ **BACKUP** - Primary/backup source failover
- ✅ **Configurable Timeout** - Source timeout detection (2-10 seconds)

### Network Connectivity
- ✅ **W5500 Ethernet** - Primary connection via SPI
- ✅ **WiFi Station** - Multiple profile support with priority
- ✅ **WiFi Access Point** - Fallback configuration mode; phones and laptops that join it open the setup page by themselves (captive portal, can be turned off)
- ✅ **Auto-Fallback** - Automatic failover: Ethernet → WiFi STA → WiFi AP
- ✅ **Static/DHCP** - Support for both IP assignment methods

### Web Interface
- ✅ **Configuration Portal** - Full device setup via web browser
- ✅ **Real-Time Monitoring** - Live DMX channel display via WebSocket
- ✅ **RDM Control Panel** - Device discovery and parameter management
- ✅ **Network Statistics** - Connection status and performance metrics
- ✅ **Signal Check** - Every Art-Net / sACN sender the node hears (also on universes no port uses), with packet rate, lost packets and gaps, and plain-language hints: universe off by one, protocol filtered out, preview data, sACN announced but blocked, console on another subnet, weak Wi-Fi
- ✅ **Other Art-Net Nodes** - Every node, console and gateway that answers an ArtPoll, with its ports and universes; warns about IP address clashes (also with this node) and two inputs sending the same universe
- ✅ **Firmware OTA Update** - Over-the-air firmware updates. The node refuses firmware built for another board; firmware from before this check goes on over USB with the browser flasher
- ✅ **Safe Settings** - Each save keeps the previous copy; a damaged settings file falls back to it, and the web UI says so instead of quietly going back to factory settings
- ✅ **Diagnostics** - Log, crash report, and memory / task-stack headroom (also logged every 30 s, as a warning when low)

### DMX Tester Mode
- ✅ **One switch in the web UI** - turns the node into a DMX tester; it stays one after a restart until you switch back
- ✅ **Receive** - live levels, refresh rate, slot count, break / MAB, line errors, start codes, frame-interval jitter with a histogram, signal-loss alarm
- ✅ **Flicker watch** - channels that change while they should hold still, with min / max and change counts
- ✅ **Send** - faders, set ranges, test patterns, adjustable refresh / break / MAB / slots
- ✅ **Cable test** - port 1 out to port 2 in, checks every value arrives
- ✅ **RDM** - discover fixtures, set their DMX address, make them identify
- While it is a tester the node ignores Art-Net and sACN; the network is only up for the web page
- ✅ **Responsive Design** - Works on desktop and mobile devices

### Status Indication
- ✅ **WS2812 RGB LED** - Visual status feedback
- ✅ **Network Status** - Ethernet (green), WiFi STA (cyan), WiFi AP (blue)
- ✅ **Error Indication** - Red LED for errors
- ✅ **DMX Activity** - Visual feedback for data transmission

## 🏗️ System Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                   Ghostlight Node System                    │
├─────────────────────────────────────────────────────────────┤
│                                                               │
│  ┌─────────────┐        ┌──────────────┐                    │
│  │   Network   │◄──────►│    Merge     │                    │
│  │  Receivers  │        │   Engine     │                    │
│  │             │        │              │                    │
│  │ • Art-Net   │        │ • HTP/LTP    │                    │
│  │ • sACN      │        │ • LAST       │     ┌──────────┐  │
│  └─────────────┘        │ • BACKUP     │────►│ DMX Port │  │
│         ▲               └──────────────┘     │    1     │  │
│         │                       │            └──────────┘  │
│         │               ┌──────────────┐                    │
│  ┌─────────────┐        │ Configuration│     ┌──────────┐  │
│  │   Network   │◄──────►│   Manager    │────►│ DMX Port │  │
│  │  Manager    │        └──────────────┘     │    2     │  │
│  │             │                ▲             └──────────┘  │
│  │ • Ethernet  │                │                           │
│  │ • WiFi STA  │        ┌──────────────┐                    │
│  │ • WiFi AP   │        │   Storage    │                    │
│  └─────────────┘        │  (LittleFS)  │                    │
│         ▲               └──────────────┘                    │
│         │                                                    │
│  ┌─────────────┐        ┌──────────────┐                    │
│  │ Web Server  │        │ LED Manager  │                    │
│  │ HTTP/WS     │        │  (WS2812)    │                    │
│  └─────────────┘        └──────────────┘                    │
│                                                               │
└─────────────────────────────────────────────────────────────┘
```

**Data Flow:**
1. Network packets (Art-Net/sACN) received via Ethernet or WiFi
2. Protocol receivers parse and validate packets
3. Merge engine combines data from multiple sources
4. DMX handler outputs merged data to physical ports
5. Web server provides configuration and monitoring interface
6. LED manager shows system status visually

## 🔧 Supported Hardware

The firmware is not tied to one board. It runs on **ESP32-S3** and original
**ESP32** chips. Each board is a small profile (a pin header and a settings
file) picked at build time with `tools/build.sh <board>`:

| Board | Hardware | Flash / PSRAM | Status |
|---|---|---|---|
| `esp-node-2rdm` | ESP32-S3 dev board (N16R8) + W5500 + 2 RS485 transceivers | 16 MB / 8 MB | the prototype; runs |
| `ghostlight-rev1` | [Ghostlight Node](https://github.com/DefeatedWarrior/ghostlight-node) rev1 PCB (ESP32-S3-WROOM-1-N8) | 8 MB / none | builds; not run on a board yet |
| `esp32-devkitc` | ESP32 DevKitC (WROOM-32) + W5500 + 2 RS485 transceivers | 4 MB / none | runs (1.2.0 on a DevKitC); DMX ports not checked yet |
| `esp32s3-devkitc` | ESP32-S3 DevKitC-1 + 2 RS485 transceivers (Wi-Fi only, no W5500) | 8 MB / none | builds; never run on hardware |

**Your own board** needs:
- an ESP32-S3 or ESP32 with at least 4 MB of flash (PSRAM is optional);
- one RS485 transceiver per DMX port (MAX485, SN65HVD72 or similar), wired to
  a UART TX pin, an RX pin and a direction pin.

Everything else is optional: a W5500 on SPI for Ethernet (without one the node
uses WiFi), a WS2812 status LED, an LED per port, switchable bus bias. A new
profile is one pin header and one `sdkconfig.defaults` file; see
[Adding a board](docs/DEVELOPING.md#adding-a-board).

| Feature | Specification |
|-----------|--------------|
| **Ethernet** | W5500 (SPI), optional |
| **WiFi** | 802.11 b/g/n (2.4 GHz), station and access point |
| **DMX Ports** | 2, each output, input or RDM controller |
| **Development Framework** | ESP-IDF v5.2.6 |

### GPIO Pinout

| Function | `ghostlight-rev1` | `esp-node-2rdm` | `esp32-devkitc` |
|---|---|---|---|
| DMX 1 TX / RX / DIR | 17 / 16 / 39 | 17 / 16 / 39 | 17 / 16 / 21 |
| DMX 2 TX / RX / DIR | 15 / 18 / 40 | 15 / 18 / 40 | 25 / 26 / 27 |
| DMX bias switches | 1 / 2 | – | – |
| Port LEDs | 5 / 6 | – | 32 / 33 |
| W5500 SCK / MOSI / MISO | 12 / 11 / 13 | 12 / 11 / 13 | 18 / 23 / 19 |
| W5500 CS / INT / RST | 10 / 9 / 4 | 10 / 9 / – | 5 / 4 / 22 |
| WS2812 status LED | 48 | 48 | 13 |
| Factory reset (hold 10 s) | 0 (BOOT) | 0 (BOOT) | 0 (BOOT) |

DIR is high to transmit. The pin headers are in
`components/board/include/boards/`.

## 💻 Install from your browser

You don't need ESP-IDF to put the firmware on a node. Open the
**[web flasher](https://defeatedwarrior.github.io/flasher.html)**
in Chrome or Edge on a desktop or laptop, pick your board, plug the node in
over USB and click **Install**. Installing erases the node, including its
settings and scenes.

Then, on the same page, **Set up over USB** gives the node its name, admin
password, Wi-Fi network and DMX universes while it is still plugged in, and
shows the address to open its web UI at. No need to join the node's own
Wi-Fi first.

To update a node that already runs this firmware and keep its settings,
download `ghostlight-node-<board>-<version>-ota.bin` from the
[releases page](https://github.com/DefeatedWarrior/ghostlight-node-firmware/releases)
and upload it in the node's web UI under Firmware Update. Each release also
has `ghostlight-node-<board>-<version>-full.bin`, the complete image, to
flash at 0x0 with any ESP flashing tool.

## 🚀 Quick Start

> **Boards and features:** the firmware builds for several boards
> (Ghostlight Node rev1, the ESP-NODE-2RDM N16R8 prototype, an ESP32
> DevKitC), and
> optional features (sACN, RDM, scenes, DMX tools, web log, mDNS, Ethernet)
> can be switched off at build time. Use `tools/build.sh <board>` rather
> than plain `idf.py build`. See **[docs/DEVELOPING.md](docs/DEVELOPING.md)**
> for building, the feature switches, the code layout, and how to add a
> board.

### Prerequisites

**Software Requirements:**
- ESP-IDF v5.2.6 ([Installation Guide](https://docs.espressif.com/projects/esp-idf/en/v5.2.6/esp32s3/get-started/))
- CMake 3.16 or higher
- Python 3.8 or higher
- Git

**Hardware Requirements:**
- One of the [supported boards](#-supported-hardware), or your own ESP32-S3 / ESP32 board with ≥4MB Flash (no PSRAM needed)
- W5500 Ethernet module (optional: without it the node uses WiFi)
- 2× RS485 transceivers (MAX485 or similar)
- WS2812 RGB LED (optional, for status indication)
- 5V power supply (≥1A recommended)

### Installation

1. **Clone the Repository**
   ```bash
   git clone https://github.com/DefeatedWarrior/ghostlight-node-firmware.git
   cd ghostlight-node-firmware
   ```

2. **Set up ESP-IDF Environment**
   ```bash
   # Linux/macOS
   . $IDF_PATH/export.sh
   
   # Windows (PowerShell)
   .\$IDF_PATH\export.ps1
   ```

3. **Configure the Project** (Optional)
   ```bash
   tools/build.sh esp-node-2rdm menuconfig    # ESP-NODE > Features
   ```
   - Most settings use sensible defaults from `sdkconfig.defaults`
   - Customize partition table, WiFi settings, etc. if needed

4. **Build the Firmware**
   ```bash
   tools/build.sh list            # esp-node-2rdm, esp32-devkitc, esp32s3-devkitc, ghostlight-rev1
   tools/build.sh esp-node-2rdm
   ```

5. **Flash to Device**
   ```bash
   # Replace COM3 with your serial port (e.g., /dev/ttyUSB0 on Linux)
   tools/build.sh esp-node-2rdm -p COM3 flash
   ```

6. **Monitor Serial Output**
   ```bash
   tools/build.sh esp-node-2rdm -p COM3 monitor
   ```
   - Press `Ctrl+]` to exit monitor

### First Boot Configuration

1. **Power On** - Status LED shows blue (boot mode)
2. **Network Connection**:
   - **If Ethernet connected**: LED turns green, device gets IP via DHCP
   - **If WiFi configured**: LED turns cyan when connected
   - **Fallback mode**: LED turns blue, device creates AP "ArtnetNode-XXXX"
3. **Access Web Interface**:
   - Ethernet/WiFi STA: `http://[device-ip]`
   - WiFi AP mode: `http://192.168.4.1`
   - **First-run setup** asks for an admin password for the web UI
     (username `admin`) and a new password for the node's own Wi-Fi. You can
     skip either one: tick **No password for this page** (anyone on the
     network can then change settings) or **Open access point**. Both can be
     changed later, under System > Admin password and Network > Access point.
4. **Configure Device**:
   - Set network preferences (static IP, WiFi credentials)
   - Configure DMX port modes (Output/Input/RDM)
   - Assign universes to each port
   - Set protocol priorities and merge modes
5. **Save Configuration** - Settings persist across reboots

## 📚 Documentation

Start with **[docs/DEVELOPING.md](docs/DEVELOPING.md)**. It describes the code as it is now: building, boards, feature switches and the code layout.

The other documents are the original design plans, written before most of the code. Where they disagree with the code, the code is right.

### Planning & Development
- **[DEVELOPMENT_SUMMARY.md](docs/DEVELOPMENT_SUMMARY.md)** - Summary of the original plan
- **[FIRMWARE_DEVELOPMENT_PLAN.md](docs/FIRMWARE_DEVELOPMENT_PLAN.md)** - Detailed project plan and roadmap
- **[IMPLEMENTATION_ROADMAP.md](docs/IMPLEMENTATION_ROADMAP.md)** - Sprint-by-sprint implementation guide
- **[LIBRARY_INTEGRATION_GUIDE.md](docs/LIBRARY_INTEGRATION_GUIDE.md)** - Third-party library integration
- **[ALTERNATIVE_APPROACHES.md](docs/ALTERNATIVE_APPROACHES.md)** - Design alternatives comparison

### System Overview
- **[OVERVIEW.md](docs/OVERVIEW.md)** - Complete system architecture

### Module Design Documents
Detailed design documentation for each component:

1. **[Configuration Module](docs/modules/DESIGN_MODULE_Configuration.md)** - System configuration management
2. **[Network Module](docs/modules/DESIGN_MODULE_Network.md)** - Ethernet and WiFi connectivity
3. **[LED Manager Module](docs/modules/DESIGN_MODULE_LED_Manager.md)** - Status LED control
4. **[DMX/RDM Handler Module](docs/modules/DESIGN_MODULE_DMX_RDM_Handler.md)** - DMX512 and RDM implementation
5. **[Merge Engine Module](docs/modules/DESIGN_MODULE_Merge_Engine.md)** - Multi-source data merging
6. **[Art-Net Receiver Module](docs/modules/DESIGN_MODULE_ArtNet_Receiver.md)** - Art-Net protocol handler
7. **[sACN Receiver Module](docs/modules/DESIGN_MODULE_sACN_Receiver.md)** - sACN/E1.31 protocol handler
8. **[Web Server Module](docs/modules/DESIGN_MODULE_Web_Server.md)** - HTTP/WebSocket interface
9. **[Storage Module](docs/modules/DESIGN_MODULE_Storage.md)** - Configuration persistence

### Standards & Testing
- **[CODING_STANDARDS.md](docs/CODING_STANDARDS.md)** - Mandatory coding conventions
- **[TESTING_GUIDE.md](docs/TESTING_GUIDE.md)** - Physical hardware testing procedures
- **[HARDWARE_TEST_PLAN.md](docs/history/HARDWARE_TEST_PLAN.md)** - Comprehensive test plan (29 test cases)

### Project History
Status reports from the v1.0-rc stage (December 2025), kept for reference. They describe the firmware before the rework, so parts are out of date: RDM and scenes, for example, are now implemented.

- **[PROJECT_COMPLETION_SUMMARY.md](docs/history/PROJECT_COMPLETION_SUMMARY.md)** - Project status at v1.0-rc
- **[BUGS_AND_FIXES.md](docs/history/BUGS_AND_FIXES.md)** - Known issues and resolutions
- **[UPGRADE_ROADMAP.md](docs/history/UPGRADE_ROADMAP.md)** - Future development roadmap
- **[PRE_RELEASE_CHECKLIST.md](docs/history/PRE_RELEASE_CHECKLIST.md)** - Pre-release verification checklist
- **[COMPLETION_REPORT.md](docs/history/COMPLETION_REPORT.md)** - Completion report

## 📊 Project Status

**Current Phase:** Phase 8 - Hardware Testing  
**Code Status:** ✅ Feature Complete  
**Documentation:** ✅ Complete  
**Next Steps:** Physical hardware testing and validation

**Version History:**
- **v1.0-rc** (Current) - Release candidate, ready for hardware testing
- **v1.0** (Planned) - First stable release after hardware validation

See [UPGRADE_ROADMAP.md](docs/history/UPGRADE_ROADMAP.md) for future versions (v1.1-v2.1).

## 🐛 Troubleshooting

### LED Status Indicators

| LED Color | Status | Meaning |
|-----------|--------|---------|
| Blue (solid) | Boot/AP Mode | Device starting or in AP fallback mode |
| Green (solid) | Ethernet Connected | Successfully connected via Ethernet |
| Cyan (solid) | WiFi STA Connected | Successfully connected via WiFi station |
| Red (blinking) | Error | System error occurred |
| Purple (blinking) | Initializing | Network initialization in progress |

### Common Issues

**Q: Device won't connect to WiFi**
- Check WiFi credentials in configuration
- Ensure WiFi network is 2.4GHz (5GHz not supported)
- Check WiFi signal strength
- Try AP mode to reconfigure

**Q: No DMX output**
- Look at the **Signal check** card on the Overview page first
- A **park** or **limit** channel rule (Ports page) holds channels whatever arrives
- A port set to take another port's DMX **input only (splitter)** ignores Art-Net and sACN
- Verify DMX port mode is set to "Output" in configuration
- Check universe assignment matches your controller
- Ensure network connection is active (check LED status)
- Verify RS485 transceiver wiring

**Q: The web UI says there was a settings problem at start-up**
- The settings file was damaged (usually power lost mid-save, or worn flash). The
  node started with the copy saved before the last change, or with factory
  settings if that copy was damaged too, and says which. Check the settings,
  then click **Got it**

**Q: A firmware upload is refused**
- "This firmware is for the ... board": pick the file for this board (System page
  shows which board the node is)
- "This file does not say which board it is for": firmware from before the board
  check. Install it over USB with the browser flasher

**Q: Can't access web interface**
- Another device may have the same IP address: the **Other Art-Net nodes** card
  on the Overview page flags address clashes
- Check network connection and device IP address
- In AP mode, connect to "ArtnetNode-XXXX" network
- Try pinging device IP address
- Check firewall settings

**Q: Art-Net/sACN data not received**
- Open the **Signal check** card on the Overview page: it lists everything the node
  hears and says what looks wrong (wrong universe, protocol filtered, preview data,
  multicast blocked, another subnet)
- Verify correct universe configuration
- Check network firewall allows UDP traffic
- Ensure controller is sending to correct IP/multicast address
- Check merge engine timeout settings

For more troubleshooting, see [TESTING_GUIDE.md](docs/TESTING_GUIDE.md) and [HARDWARE_TEST_PLAN.md](docs/history/HARDWARE_TEST_PLAN.md).

## 🤝 Contributing

Contributions are welcome! When contributing to this project, please:

1. **Read Documentation**
   - Review [CODING_STANDARDS.md](docs/CODING_STANDARDS.md) for code style requirements
   - Check [FIRMWARE_DEVELOPMENT_PLAN.md](docs/FIRMWARE_DEVELOPMENT_PLAN.md) for architecture guidelines

2. **Testing Requirements**
   - Follow [TESTING_GUIDE.md](docs/TESTING_GUIDE.md) for testing procedures
   - Ensure no compiler warnings
   - Test on actual hardware when possible

3. **Code Quality**
   - Follow naming conventions (snake_case for C)
   - Document all public APIs
   - Handle errors properly
   - Consider thread safety

4. **Pull Requests**
   - Create descriptive PR titles
   - Document changes thoroughly
   - Reference related issues
   - Update documentation if needed

## 📞 Support

- **Issues**: [GitHub Issues](https://github.com/DefeatedWarrior/ghostlight-node-firmware/issues)
- **Author**: thinhh0321

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

Copyright (c) 2025 ThinhCNTT22

## 🙏 Acknowledgments

- **Espressif Systems** - ESP-IDF framework
- **Art-Net** - Artistic Licence Ltd.
- **ESTA** - E1.31 (sACN), DMX512, and RDM standards
- Open source community for various libraries and tools

## 🔗 References

- [Art-Net Protocol](https://art-net.org.uk/)
- [sACN (E1.31) Standard](https://tsp.esta.org/tsp/documents/docs/ANSI_E1-31-2018.pdf)
- [DMX512 Standard](https://tsp.esta.org/tsp/documents/docs/ANSI-ESTA_E1-11_2008R2018.pdf)
- [RDM Standard](https://tsp.esta.org/tsp/documents/docs/ANSI-ESTA_E1-20_2010.pdf)
- [ESP-IDF Documentation](https://docs.espressif.com/projects/esp-idf/en/v5.2.6/)

---

<div align="center">

**Made with ❤️ by thinhh0321**

*Professional DMX512/RDM solution for the lighting industry*

</div>
