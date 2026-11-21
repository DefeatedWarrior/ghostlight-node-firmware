<div align="center">

# ESP-NODE-2RDM

### Professional Art-Net / sACN to DMX512/RDM Converter

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.2.6-blue.svg)](https://github.com/espressif/esp-idf)
[![Platform](https://img.shields.io/badge/Platform-ESP32--S3-green.svg)](https://www.espressif.com/en/products/socs/esp32-s3)
[![Status](https://img.shields.io/badge/Status-Hardware%20Testing-orange.svg)](PROJECT_COMPLETION_SUMMARY.md)

**2 Independent DMX/RDM Ports • ESP32-S3 • Ethernet + WiFi**

[English](#english) | [Tiếng Việt](#tiếng-việt)

</div>

---

## English

### 📖 Table of Contents
- [Overview](#overview)
- [Key Features](#key-features)
- [Hardware Specifications](#hardware-specifications)
- [Quick Start](#quick-start)
- [Documentation](#documentation)
- [Project Status](#project-status)
- [Contributing](#contributing)
- [License](#license)

### 🎯 Overview

ESP-NODE-2RDM is a professional lighting control device that converts network protocols (Art-Net/sACN) to DMX512/RDM signals. Built on ESP32-S3 platform with dual network interfaces (Ethernet + WiFi) and 2 independent DMX/RDM ports.

**Perfect for:**
- Stage lighting control systems
- Architectural lighting installations
- Entertainment venues
- Broadcasting studios
- Theatrical productions

### ✨ Key Features

#### Network Protocols
- ✅ **Art-Net v4** - Industry standard UDP protocol (port 6454)
- ✅ **sACN (E1.31)** - Streaming ACN with multicast support
- ✅ **Protocol Priority** - Configurable protocol selection and merge modes
- ✅ **Multi-Source Support** - Handle up to 4 simultaneous sources per port

#### DMX512 & RDM
- ✅ **Dual Independent Ports** - 2 fully configurable DMX/RDM ports
- ✅ **DMX Output** - ~44 Hz refresh rate, 512 channels per port
- ✅ **DMX Input** - Monitor incoming DMX data
- ✅ **RDM Master** - Device discovery, parameter get/set
- ✅ **RDM Responder** - Respond to RDM queries
- ✅ **Flexible Universe Mapping** - Per-port universe configuration with offset support

#### Advanced Merge Engine
- ✅ **HTP** (Highest Takes Precedence) - For intensity control
- ✅ **LTP** (Lowest Takes Precedence) - Alternative merge mode
- ✅ **LAST** - Last received packet wins
- ✅ **BACKUP** - Primary/backup source failover
- ✅ **Configurable Timeout** - Source timeout detection (2-10 seconds)

#### Network Connectivity
- ✅ **W5500 Ethernet** - Primary connection via SPI
- ✅ **WiFi Station** - Multiple profile support with priority
- ✅ **WiFi Access Point** - Fallback configuration mode
- ✅ **Auto-Fallback** - Automatic failover: Ethernet → WiFi STA → WiFi AP
- ✅ **Static/DHCP** - Support for both IP assignment methods

#### Web Interface
- ✅ **Configuration Portal** - Full device setup via web browser
- ✅ **Real-Time Monitoring** - Live DMX channel display via WebSocket
- ✅ **RDM Control Panel** - Device discovery and parameter management
- ✅ **Network Statistics** - Connection status and performance metrics
- ✅ **Firmware OTA Update** - Over-the-air firmware updates
- ✅ **Responsive Design** - Works on desktop and mobile devices

#### Status Indication
- ✅ **WS2812 RGB LED** - Visual status feedback
- ✅ **Network Status** - Ethernet (green), WiFi STA (cyan), WiFi AP (blue)
- ✅ **Error Indication** - Red LED for errors
- ✅ **DMX Activity** - Visual feedback for data transmission

### 🏗️ System Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     ESP-NODE-2RDM System                     │
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

### 🔧 Hardware Specifications

| Component | Specification |
|-----------|--------------|
| **MCU** | ESP32-S3-WROOM-1-N8 |
| **Flash Memory** | 8 MB |
| **PSRAM** | None |
| **Ethernet** | W5500 (SPI interface) |
| **WiFi** | 802.11 b/g/n (2.4 GHz) |
| **DMX Ports** | 2× RS485 transceivers |
| **Status LED** | WS2812 RGB (GPIO 48) |
| **Development Framework** | ESP-IDF v5.2.6 |
| **Operating Voltage** | 5V DC |
| **Power Consumption** | ~500mA typical |

#### GPIO Pinout

| Function | GPIO | Notes |
|----------|------|-------|
| WS2812 LED Data | 48 | Status indicator |
| DMX Port 1 TX | 17 | UART transmit |
| DMX Port 1 RX | 16 | UART receive |
| DMX Port 1 DIR | 21 | Direction control (HIGH=TX) |
| DMX Port 2 TX | 19 | UART transmit |
| DMX Port 2 RX | 18 | UART receive |
| DMX Port 2 DIR | 20 | Direction control (HIGH=TX) |
| W5500 CS | 10 | SPI Chip Select |
| W5500 MOSI | 11 | SPI MOSI |
| W5500 MISO | 13 | SPI MISO |
| W5500 SCK | 12 | SPI Clock |
| W5500 INT | 9 | Interrupt (optional) |

### 🚀 Quick Start

> **Boards and features:** the firmware builds for several boards
> (Ghostlight Node rev1, the ESP-NODE-2RDM N16R8 prototype, an ESP32
> DevKitC), and
> optional features (sACN, RDM, scenes, DMX tools, web log, mDNS, Ethernet)
> can be switched off at build time. Use `tools/build.sh <board>` rather
> than plain `idf.py build`. See **[docs/DEVELOPING.md](docs/DEVELOPING.md)**
> for building, the feature switches, the code layout, and how to add a
> board.

#### Prerequisites

**Software Requirements:**
- ESP-IDF v5.2.6 ([Installation Guide](https://docs.espressif.com/projects/esp-idf/en/v5.2.6/esp32s3/get-started/))
- CMake 3.16 or higher
- Python 3.8 or higher
- Git

**Hardware Requirements:**
- ESP32-S3 development board with ≥8MB Flash (no PSRAM needed)
- W5500 Ethernet module
- 2× RS485 transceivers (MAX485 or similar)
- WS2812 RGB LED (optional, for status indication)
- 5V power supply (≥1A recommended)

#### Installation

1. **Clone the Repository**
   ```bash
   git clone https://github.com/DefeatedWarrior/ghostlight-node.git
   cd ghostlight-node/firmware
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
   tools/build.sh list            # esp-node-2rdm, esp32-devkitc, ghostlight-rev1
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

#### First Boot Configuration

1. **Power On** - Status LED shows blue (boot mode)
2. **Network Connection**:
   - **If Ethernet connected**: LED turns green, device gets IP via DHCP
   - **If WiFi configured**: LED turns cyan when connected
   - **Fallback mode**: LED turns blue, device creates AP "ArtnetNode-XXXX"
3. **Access Web Interface**:
   - Ethernet/WiFi STA: `http://[device-ip]`
   - WiFi AP mode: `http://192.168.4.1`
4. **Configure Device**:
   - Set network preferences (static IP, WiFi credentials)
   - Configure DMX port modes (Output/Input/RDM)
   - Assign universes to each port
   - Set protocol priorities and merge modes
5. **Save Configuration** - Settings persist across reboots

### 📚 Documentation

Comprehensive documentation is available in Vietnamese and covers all aspects of the project:

#### Planning & Development
- **[DEVELOPMENT_SUMMARY.md](docs/DEVELOPMENT_SUMMARY.md)** - **READ FIRST** - Development overview
- **[FIRMWARE_DEVELOPMENT_PLAN.md](docs/FIRMWARE_DEVELOPMENT_PLAN.md)** - Detailed project plan and roadmap
- **[IMPLEMENTATION_ROADMAP.md](docs/IMPLEMENTATION_ROADMAP.md)** - Sprint-by-sprint implementation guide
- **[LIBRARY_INTEGRATION_GUIDE.md](docs/LIBRARY_INTEGRATION_GUIDE.md)** - Third-party library integration
- **[ALTERNATIVE_APPROACHES.md](docs/ALTERNATIVE_APPROACHES.md)** - Design alternatives comparison

#### System Overview
- **[TongQuan.md](docs/TongQuan.md)** - Complete system architecture (Vietnamese)

#### Module Design Documents
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

#### Standards & Testing
- **[CODING_STANDARDS.md](docs/CODING_STANDARDS.md)** - Mandatory coding conventions
- **[TESTING_GUIDE.md](docs/TESTING_GUIDE.md)** - Physical hardware testing procedures
- **[HARDWARE_TEST_PLAN.md](HARDWARE_TEST_PLAN.md)** - Comprehensive test plan (29 test cases)

#### Project Status
- **[PROJECT_COMPLETION_SUMMARY.md](PROJECT_COMPLETION_SUMMARY.md)** - Current project status
- **[BUGS_AND_FIXES.md](BUGS_AND_FIXES.md)** - Known issues and resolutions
- **[UPGRADE_ROADMAP.md](UPGRADE_ROADMAP.md)** - Future development roadmap
- **[PRE_RELEASE_CHECKLIST.md](PRE_RELEASE_CHECKLIST.md)** - Pre-release verification checklist
- **[BAO_CAO_HOAN_THANH.md](BAO_CAO_HOAN_THANH.md)** - Completion report (Vietnamese)

### 📊 Project Status

**Current Phase:** Phase 8 - Hardware Testing  
**Code Status:** ✅ Feature Complete  
**Documentation:** ✅ Complete  
**Next Steps:** Physical hardware testing and validation

**Version History:**
- **v1.0-rc** (Current) - Release candidate, ready for hardware testing
- **v1.0** (Planned) - First stable release after hardware validation

See [UPGRADE_ROADMAP.md](UPGRADE_ROADMAP.md) for future versions (v1.1-v2.1).

### 🐛 Troubleshooting

#### LED Status Indicators

| LED Color | Status | Meaning |
|-----------|--------|---------|
| Blue (solid) | Boot/AP Mode | Device starting or in AP fallback mode |
| Green (solid) | Ethernet Connected | Successfully connected via Ethernet |
| Cyan (solid) | WiFi STA Connected | Successfully connected via WiFi station |
| Red (blinking) | Error | System error occurred |
| Purple (blinking) | Initializing | Network initialization in progress |

#### Common Issues

**Q: Device won't connect to WiFi**
- Check WiFi credentials in configuration
- Ensure WiFi network is 2.4GHz (5GHz not supported)
- Check WiFi signal strength
- Try AP mode to reconfigure

**Q: No DMX output**
- Verify DMX port mode is set to "Output" in configuration
- Check universe assignment matches your controller
- Ensure network connection is active (check LED status)
- Verify RS485 transceiver wiring

**Q: Can't access web interface**
- Check network connection and device IP address
- In AP mode, connect to "ArtnetNode-XXXX" network
- Try pinging device IP address
- Check firewall settings

**Q: Art-Net/sACN data not received**
- Verify correct universe configuration
- Check network firewall allows UDP traffic
- Ensure controller is sending to correct IP/multicast address
- Check merge engine timeout settings

For more troubleshooting, see [TESTING_GUIDE.md](docs/TESTING_GUIDE.md) and [HARDWARE_TEST_PLAN.md](HARDWARE_TEST_PLAN.md).

### 🤝 Contributing

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

### 📞 Support

- **Issues**: [GitHub Issues](https://github.com/DefeatedWarrior/ghostlight-node/issues)
- **Discussions**: [GitHub Discussions](https://github.com/thinhh0321/ESP-NODE-2RDM/discussions)
- **Author**: thinhh0321

### 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

Copyright (c) 2025 ThinhCNTT22

### 🙏 Acknowledgments

- **Espressif Systems** - ESP-IDF framework
- **Art-Net** - Artistic Licence Ltd.
- **ESTA** - E1.31 (sACN), DMX512, and RDM standards
- Open source community for various libraries and tools

---

## Tiếng Việt

### 📖 Mục lục
- [Tổng quan](#tổng-quan)
- [Tính năng chính](#tính-năng-chính)
- [Thông số kỹ thuật](#thông-số-kỹ-thuật)
- [Hướng dẫn nhanh](#hướng-dẫn-nhanh)
- [Tài liệu dự án](#tài-liệu-dự-án)
- [Trạng thái dự án](#trạng-thái-dự-án)
- [Đóng góp](#đóng-góp)
- [Giấy phép](#giấy-phép)

### 🎯 Tổng quan

ESP-NODE-2RDM là thiết bị điều khiển ánh sáng chuyên nghiệp, chuyển đổi giao thức mạng (Art-Net/sACN) thành tín hiệu DMX512/RDM. Được xây dựng trên nền tảng ESP32-S3 với 2 giao diện mạng (Ethernet + WiFi) và 2 cổng DMX/RDM độc lập.

**Ứng dụng:**
- Hệ thống điều khiển ánh sáng sân khấu
- Chiếu sáng kiến trúc
- Địa điểm giải trí
- Studio truyền hình
- Sản xuất kịch nghệ

### ✨ Tính năng chính

#### Giao thức mạng
- ✅ **Art-Net v4** - Giao thức UDP chuẩn công nghiệp (cổng 6454)
- ✅ **sACN (E1.31)** - Streaming ACN với hỗ trợ multicast
- ✅ **Ưu tiên giao thức** - Cấu hình lựa chọn và hợp nhất giao thức
- ✅ **Đa nguồn** - Xử lý tối đa 4 nguồn đồng thời mỗi cổng

#### DMX512 & RDM
- ✅ **2 cổng độc lập** - 2 cổng DMX/RDM có thể cấu hình đầy đủ
- ✅ **DMX Output** - Tốc độ làm mới ~44 Hz, 512 kênh mỗi cổng
- ✅ **DMX Input** - Giám sát dữ liệu DMX đầu vào
- ✅ **RDM Master** - Phát hiện thiết bị, đọc/ghi tham số
- ✅ **RDM Responder** - Phản hồi truy vấn RDM
- ✅ **Ánh xạ Universe linh hoạt** - Cấu hình universe và offset cho từng cổng

#### Merge Engine nâng cao
- ✅ **HTP** (Highest Takes Precedence) - Cho điều khiển cường độ
- ✅ **LTP** (Lowest Takes Precedence) - Chế độ hợp nhất thay thế
- ✅ **LAST** - Gói tin cuối cùng nhận được
- ✅ **BACKUP** - Chuyển đổi dự phòng nguồn chính/phụ
- ✅ **Timeout có thể cấu hình** - Phát hiện mất nguồn (2-10 giây)

#### Kết nối mạng
- ✅ **W5500 Ethernet** - Kết nối chính qua SPI
- ✅ **WiFi Station** - Hỗ trợ nhiều profile với ưu tiên
- ✅ **WiFi Access Point** - Chế độ cấu hình dự phòng
- ✅ **Tự động chuyển đổi** - Ethernet → WiFi STA → WiFi AP
- ✅ **Static/DHCP** - Hỗ trợ cả hai phương thức gán IP

#### Giao diện Web
- ✅ **Cổng cấu hình** - Thiết lập đầy đủ qua trình duyệt web
- ✅ **Giám sát thời gian thực** - Hiển thị kênh DMX trực tiếp qua WebSocket
- ✅ **Bảng điều khiển RDM** - Phát hiện thiết bị và quản lý tham số
- ✅ **Thống kê mạng** - Trạng thái kết nối và chỉ số hiệu suất
- ✅ **Cập nhật OTA** - Cập nhật firmware qua không khí
- ✅ **Thiết kế responsive** - Hoạt động trên desktop và mobile

#### Chỉ báo trạng thái
- ✅ **WS2812 RGB LED** - Phản hồi trạng thái trực quan
- ✅ **Trạng thái mạng** - Ethernet (xanh lá), WiFi STA (xanh lơ), WiFi AP (xanh dương)
- ✅ **Chỉ báo lỗi** - LED đỏ cho lỗi
- ✅ **Hoạt động DMX** - Phản hồi trực quan cho truyền dữ liệu

### 🏗️ Kiến trúc hệ thống

```
┌─────────────────────────────────────────────────────────────┐
│                   Hệ thống ESP-NODE-2RDM                    │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌─────────────┐        ┌──────────────┐                    │
│  │  Bộ nhận    │◄──────►│   Merge      │                    │
│  │  giao thức  │        │   Engine     │                    │
│  │             │        │              │                    │
│  │ • Art-Net   │        │ • HTP/LTP    │                    │
│  │ • sACN      │        │ • LAST       │     ┌──────────┐   │
│  └─────────────┘        │ • BACKUP     │────►│ Cổng DMX │   │
│         ▲               └──────────────┘     │    1     │   │
│         │                       │            └──────────┘   │
│         │               ┌──────────────┐                    │
│  ┌─────────────┐        │  Quản lý     │     ┌──────────┐   │
│  │  Quản lý    │◄──────►│   cấu hình   │────►│ Cổng DMX │   │
│  │   mạng      │        └──────────────┘     │    2     │   │
│  │             │                ▲            └──────────┘   │
│  │ • Ethernet  │                │                           │
│  │ • WiFi STA  │        ┌──────────────┐                    │
│  │ • WiFi AP   │        │   Lưu trữ    │                    │
│  └─────────────┘        │  (LittleFS)  │                    │
│         ▲               └──────────────┘                    │
│         │                                                   │
│  ┌─────────────┐        ┌──────────────┐                    │
│  │ Web Server  │        │  Quản lý LED │                    │
│  │ HTTP/WS     │        │   (WS2812)   │                    │
│  └─────────────┘        └──────────────┘                    │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

**Luồng dữ liệu:**
1. Gói tin mạng (Art-Net/sACN) nhận qua Ethernet hoặc WiFi
2. Bộ nhận giao thức phân tích và xác thực gói tin
3. Merge engine kết hợp dữ liệu từ nhiều nguồn
4. Bộ xử lý DMX xuất dữ liệu đã hợp nhất ra cổng vật lý
5. Web server cung cấp giao diện cấu hình và giám sát
6. Quản lý LED hiển thị trạng thái hệ thống trực quan

### 🔧 Thông số kỹ thuật

| Linh kiện | Thông số |
|-----------|----------|
| **MCU** | ESP32-S3-WROOM-1-N8 |
| **Flash Memory** | 8 MB |
| **PSRAM** | None |
| **Ethernet** | W5500 (giao diện SPI) |
| **WiFi** | 802.11 b/g/n (2.4 GHz) |
| **Cổng DMX** | 2× RS485 transceivers |
| **LED trạng thái** | WS2812 RGB (GPIO 48) |
| **Framework phát triển** | ESP-IDF v5.2.6 |
| **Điện áp hoạt động** | 5V DC |
| **Tiêu thụ điện** | ~500mA điển hình |

#### Sơ đồ chân GPIO

| Chức năng | GPIO | Ghi chú |
|-----------|------|---------|
| WS2812 LED Data | 48 | Chỉ báo trạng thái |
| DMX Port 1 TX | 17 | UART truyền |
| DMX Port 1 RX | 16 | UART nhận |
| DMX Port 1 DIR | 21 | Điều khiển hướng (HIGH=TX) |
| DMX Port 2 TX | 19 | UART truyền |
| DMX Port 2 RX | 18 | UART nhận |
| DMX Port 2 DIR | 20 | Điều khiển hướng (HIGH=TX) |
| W5500 CS | 10 | SPI Chip Select |
| W5500 MOSI | 11 | SPI MOSI |
| W5500 MISO | 13 | SPI MISO |
| W5500 SCK | 12 | SPI Clock |
| W5500 INT | 9 | Ngắt (tùy chọn) |

### 🚀 Hướng dẫn nhanh

#### Yêu cầu

**Yêu cầu phần mềm:**
- ESP-IDF v5.2.6 ([Hướng dẫn cài đặt](https://docs.espressif.com/projects/esp-idf/en/v5.2.6/esp32s3/get-started/))
- CMake 3.16 trở lên
- Python 3.8 trở lên
- Git

**Yêu cầu phần cứng:**
- Bo mạch phát triển ESP32-S3 với ≥8MB Flash (không cần PSRAM)
- Module Ethernet W5500
- 2× RS485 transceivers (MAX485 hoặc tương tự)
- WS2812 RGB LED (tùy chọn, cho chỉ báo trạng thái)
- Nguồn điện 5V (khuyến nghị ≥1A)

#### Cài đặt

1. **Clone Repository**
   ```bash
   git clone https://github.com/DefeatedWarrior/ghostlight-node.git
   cd ghostlight-node/firmware
   ```

2. **Thiết lập môi trường ESP-IDF**
   ```bash
   # Linux/macOS
   . $IDF_PATH/export.sh
   
   # Windows (PowerShell)
   .\$IDF_PATH\export.ps1
   ```

3. **Cấu hình dự án** (Tùy chọn)
   ```bash
   idf.py menuconfig
   ```
   - Hầu hết cài đặt sử dụng giá trị mặc định từ `sdkconfig.defaults`
   - Tùy chỉnh bảng phân vùng, cài đặt WiFi, v.v. nếu cần

4. **Biên dịch Firmware**
   ```bash
   idf.py build
   ```

5. **Flash vào thiết bị**
   ```bash
   # Thay COM3 bằng cổng serial của bạn (vd: /dev/ttyUSB0 trên Linux)
   idf.py -p COM3 flash
   ```

6. **Theo dõi đầu ra Serial**
   ```bash
   idf.py -p COM3 monitor
   ```
   - Nhấn `Ctrl+]` để thoát monitor

#### Cấu hình lần đầu

1. **Bật nguồn** - LED trạng thái hiển thị màu xanh dương (chế độ khởi động)
2. **Kết nối mạng**:
   - **Nếu có Ethernet**: LED chuyển xanh lá, thiết bị nhận IP qua DHCP
   - **Nếu đã cấu hình WiFi**: LED chuyển xanh lơ khi kết nối
   - **Chế độ dự phòng**: LED chuyển xanh dương, thiết bị tạo AP "ArtnetNode-XXXX"
3. **Truy cập giao diện Web**:
   - Ethernet/WiFi STA: `http://[device-ip]`
   - Chế độ WiFi AP: `http://192.168.4.1`
4. **Cấu hình thiết bị**:
   - Đặt tùy chọn mạng (IP tĩnh, thông tin đăng nhập WiFi)
   - Cấu hình chế độ cổng DMX (Output/Input/RDM)
   - Gán universe cho mỗi cổng
   - Đặt ưu tiên giao thức và chế độ merge
5. **Lưu cấu hình** - Cài đặt được lưu giữ qua các lần khởi động lại

### 📚 Tài liệu dự án

Tài liệu toàn diện bao gồm thiết kế chi tiết, kế hoạch phát triển và quy chuẩn lập trình:

#### Kế hoạch & Phát triển

- **[📋 DEVELOPMENT_SUMMARY.md](docs/DEVELOPMENT_SUMMARY.md)** - **ĐỌC ĐẦU TIÊN** - Tóm tắt tổng quan kế hoạch
- **[📘 FIRMWARE_DEVELOPMENT_PLAN.md](docs/FIRMWARE_DEVELOPMENT_PLAN.md)** - Kế hoạch chi tiết, cấu trúc project, roadmap
- **[🔧 LIBRARY_INTEGRATION_GUIDE.md](docs/LIBRARY_INTEGRATION_GUIDE.md)** - Hướng dẫn tích hợp thư viện
- **[🗺️ IMPLEMENTATION_ROADMAP.md](docs/IMPLEMENTATION_ROADMAP.md)** - Lộ trình triển khai từng sprint
- **[🔀 ALTERNATIVE_APPROACHES.md](docs/ALTERNATIVE_APPROACHES.md)** - So sánh phương án thay thế

#### Tổng quan hệ thống

- **[TongQuan.md](docs/TongQuan.md)** - Tài liệu tổng quan kiến trúc hệ thống đầy đủ

#### Thiết kế Module

Tài liệu thiết kế chi tiết cho từng module độc lập:

1. **[Configuration Module](docs/modules/DESIGN_MODULE_Configuration.md)**

   - Quản lý cấu hình hệ thống
   - NVS và LittleFS storage
   - JSON serialization

2. **[Network Module](docs/modules/DESIGN_MODULE_Network.md)**

   - Ethernet W5500 (SPI)
   - WiFi Station/AP modes
   - Auto-fallback mechanism

3. **[LED Manager Module](docs/modules/DESIGN_MODULE_LED_Manager.md)**

   - WS2812 status LED control
   - State indication
   - Event-based triggers

4. **[DMX/RDM Handler Module](docs/modules/DESIGN_MODULE_DMX_RDM_Handler.md)**

   - DMX512 Output/Input
   - RDM Master/Responder
   - 2 independent ports

5. **[Merge Engine Module](docs/modules/DESIGN_MODULE_Merge_Engine.md)**

   - HTP/LTP/LAST/BACKUP modes
   - Multi-source merging
   - Timeout handling

6. **[Art-Net Receiver Module](docs/modules/DESIGN_MODULE_ArtNet_Receiver.md)**

   - Art-Net v4 protocol
   - ArtPoll/ArtPollReply
   - Universe routing

7. **[sACN Receiver Module](docs/modules/DESIGN_MODULE_sACN_Receiver.md)**

   - E1.31 (Streaming ACN)
   - Multicast reception
   - Priority handling

8. **[Web Server Module](docs/modules/DESIGN_MODULE_Web_Server.md)**

   - HTTP REST API
   - WebSocket real-time updates
   - Configuration interface

9. **[Storage Module](docs/modules/DESIGN_MODULE_Storage.md)**
   - LittleFS file system
   - NVS backup storage
   - Config persistence

#### Quy chuẩn & Testing

- **[CODING_STANDARDS.md](docs/CODING_STANDARDS.md)** - Quy chuẩn lập trình bắt buộc
  - Quy ước đặt tên
  - Định dạng code
  - Xử lý lỗi
  - An toàn luồng (thread safety)
  - Best practices về hiệu suất

- **[TESTING_GUIDE.md](docs/TESTING_GUIDE.md)** - Hướng dẫn testing vật lý
  - Quy trình test module
  - Integration testing
  - Performance testing
  - Hướng dẫn khắc phục sự cố

- **[HARDWARE_TEST_PLAN.md](HARDWARE_TEST_PLAN.md)** - Kế hoạch test toàn diện (29 test cases)

#### Trạng thái dự án

- **[PROJECT_COMPLETION_SUMMARY.md](PROJECT_COMPLETION_SUMMARY.md)** - Trạng thái dự án hiện tại
- **[BUGS_AND_FIXES.md](BUGS_AND_FIXES.md)** - Các vấn đề đã biết và giải pháp
- **[UPGRADE_ROADMAP.md](UPGRADE_ROADMAP.md)** - Lộ trình phát triển tương lai
- **[PRE_RELEASE_CHECKLIST.md](PRE_RELEASE_CHECKLIST.md)** - Danh sách kiểm tra trước phát hành
- **[BAO_CAO_HOAN_THANH.md](BAO_CAO_HOAN_THANH.md)** - Báo cáo hoàn thành

### 📊 Trạng thái dự án

**Giai đoạn hiện tại:** Giai đoạn 8 - Test phần cứng  
**Trạng thái code:** ✅ Hoàn thành tính năng  
**Tài liệu:** ✅ Hoàn thành  
**Bước tiếp theo:** Test và xác thực phần cứng vật lý

**Lịch sử phiên bản:**
- **v1.0-rc** (Hiện tại) - Phiên bản ứng cử viên phát hành, sẵn sàng test phần cứng
- **v1.0** (Kế hoạch) - Phiên bản ổn định đầu tiên sau xác thực phần cứng

Xem [UPGRADE_ROADMAP.md](UPGRADE_ROADMAP.md) cho các phiên bản tương lai (v1.1-v2.1).

### 🐛 Khắc phục sự cố

#### Chỉ báo LED

| Màu LED | Trạng thái | Ý nghĩa |
|---------|------------|---------|
| Xanh dương (sáng) | Khởi động/AP Mode | Thiết bị đang khởi động hoặc ở chế độ AP dự phòng |
| Xanh lá (sáng) | Kết nối Ethernet | Đã kết nối thành công qua Ethernet |
| Xanh lơ (sáng) | Kết nối WiFi STA | Đã kết nối thành công qua WiFi station |
| Đỏ (nhấp nháy) | Lỗi | Đã xảy ra lỗi hệ thống |
| Tím (nhấp nháy) | Đang khởi tạo | Đang khởi tạo mạng |

#### Vấn đề thường gặp

**H: Thiết bị không kết nối được WiFi**
- Kiểm tra thông tin đăng nhập WiFi trong cấu hình
- Đảm bảo mạng WiFi là 2.4GHz (không hỗ trợ 5GHz)
- Kiểm tra cường độ tín hiệu WiFi
- Thử chế độ AP để cấu hình lại

**H: Không có đầu ra DMX**
- Xác minh chế độ cổng DMX được đặt thành "Output" trong cấu hình
- Kiểm tra universe được gán khớp với bộ điều khiển của bạn
- Đảm bảo kết nối mạng đang hoạt động (kiểm tra trạng thái LED)
- Xác minh dây nối RS485 transceiver

**H: Không thể truy cập giao diện web**
- Kiểm tra kết nối mạng và địa chỉ IP thiết bị
- Ở chế độ AP, kết nối đến mạng "ArtnetNode-XXXX"
- Thử ping địa chỉ IP thiết bị
- Kiểm tra cài đặt tường lửa

**H: Không nhận được dữ liệu Art-Net/sACN**
- Xác minh cấu hình universe đúng
- Kiểm tra tường lửa mạng cho phép lưu lượng UDP
- Đảm bảo bộ điều khiển đang gửi đến đúng địa chỉ IP/multicast
- Kiểm tra cài đặt timeout của merge engine

Để biết thêm về khắc phục sự cố, xem [TESTING_GUIDE.md](docs/TESTING_GUIDE.md) và [HARDWARE_TEST_PLAN.md](HARDWARE_TEST_PLAN.md).

### 🤝 Đóng góp

Chúng tôi hoan nghênh các đóng góp! Khi đóng góp cho dự án này, vui lòng:

1. **Đọc tài liệu**
   - Xem xét [CODING_STANDARDS.md](docs/CODING_STANDARDS.md) để biết yêu cầu về phong cách code
   - Kiểm tra [FIRMWARE_DEVELOPMENT_PLAN.md](docs/FIRMWARE_DEVELOPMENT_PLAN.md) để biết hướng dẫn kiến trúc

2. **Yêu cầu Testing**
   - Tuân theo [TESTING_GUIDE.md](docs/TESTING_GUIDE.md) cho quy trình testing
   - Đảm bảo không có cảnh báo compiler
   - Test trên phần cứng thực tế khi có thể

3. **Chất lượng code**
   - Tuân theo quy ước đặt tên (snake_case cho C)
   - Tài liệu hóa tất cả các API công khai
   - Xử lý lỗi đúng cách
   - Xem xét an toàn luồng

4. **Pull Requests**
   - Tạo tiêu đề PR mô tả rõ ràng
   - Tài liệu hóa thay đổi kỹ lưỡng
   - Tham chiếu các vấn đề liên quan
   - Cập nhật tài liệu nếu cần

### 📞 Hỗ trợ

- **Vấn đề**: [GitHub Issues](https://github.com/DefeatedWarrior/ghostlight-node/issues)
- **Thảo luận**: [GitHub Discussions](https://github.com/thinhh0321/ESP-NODE-2RDM/discussions)
- **Tác giả**: thinhh0321

### 📄 Giấy phép

Dự án này được cấp phép theo Giấy phép MIT - xem file [LICENSE](LICENSE) để biết chi tiết.

Bản quyền (c) 2025 ThinhCNTT22

### 🙏 Cảm ơn

- **Espressif Systems** - Framework ESP-IDF
- **Art-Net** - Artistic Licence Ltd.
- **ESTA** - Tiêu chuẩn E1.31 (sACN), DMX512 và RDM
- Cộng đồng mã nguồn mở cho các thư viện và công cụ

### 🔗 Tài liệu tham khảo

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
