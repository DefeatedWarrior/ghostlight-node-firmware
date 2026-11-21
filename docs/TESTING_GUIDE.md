# PHYSICAL TESTING GUIDE

**Project:** Artnet-Node-2RDM  
**Document:** Physical Testing Guide  
**Version:** 1.0  
**Created:** 25/12/2025

---

## PURPOSE

This document explains in detail how to test the firmware physically after flashing it to the device, covering:

- Testing each module on its own
- Testing the modules together
- Verifying end-to-end function
- Performance testing
- Stress testing
- Troubleshooting

---

## PREPARATION

### Equipment needed

1. **ESP32-S3 Board** with the firmware flashed
2. **DMX Controller/Console** (or software such as QLC+, Freestyler)
3. **DMX Fixtures** (LED fixtures, moving heads, etc.) - at least 2 fixtures
4. **DMX Cables** and XLR connectors
5. **Network Equipment**:
   - Router/Switch Ethernet
   - Ethernet cable Cat5e/Cat6
   - Computer with Art-Net/sACN software
6. **RDM Devices** (optional, cho RDM testing)
7. **Multimeter** (optional, to measure voltage/signal)
8. **Logic Analyzer** or Oscilloscope (optional, to debug DMX timing)

### Software Tools

1. **Art-Net Software**:

   - QLC+ (free, cross-platform)
   - Freestyler DMX (Windows)
   - DMXControl 3 (Windows)
   - LightJams (Windows/Mac)

2. **sACN Software**:

   - QLC+ (supports E1.31)
   - OLA (Open Lighting Architecture)
   - ETC Eos offline

3. **Network Tools**:

   - Wireshark (packet capture)
   - Advanced IP Scanner
   - Angry IP Scanner

4. **Web Browser**:
   - Chrome/Firefox/Edge (to access the web interface)

---

## 1. INITIAL CHECKS

### 1.1. Power On Test

**Goal:** Verify the device boots correctly

**Steps:**

1. Connect 5V DC power to the board
2. Watch the LED status:
   - ✅ LED lights **light blue** (Boot state)
   - ⏱️ After 2-5 seconds, the LED colour changes with the network state

**Expected result:**

- LED is not blinking red (error)
- Board is not resetting repeatedly
- WiFi AP "ArtnetNode-XXXX" is visible if there is no Ethernet

**Troubleshooting:**

- LED blinks red fast → Check the serial log; possibly a hardware fault or corrupt firmware
- No LED → Check the power supply, check the GPIO 48 connection

### 1.2. Serial Monitor Check

**Goal:** Read the boot logs and debug info

**Steps:**

1. Connect USB serial (115200 baud)
2. Open a serial monitor (PuTTY, screen, Arduino IDE)
3. Reset board (press EN button)
4. Watch the boot sequence

**Expected log:**

```
ESP-ROM:esp32s3-20210327
Build:Mar 27 2021
...
I (XXX) STORAGE: LittleFS mounted, total=1048576, used=XXXX
I (XXX) CONFIG: Configuration loaded
I (XXX) NETWORK: Initializing network...
I (XXX) NETWORK: Ethernet link up
I (XXX) NETWORK: IP address: 192.168.1.100
I (XXX) DMX: Port 1 initialized (mode: OUT, universe: 0)
I (XXX) DMX: Port 2 initialized (mode: OUT, universe: 1)
I (XXX) ARTNET: Listening on UDP port 6454
I (XXX) SACN: Listening on UDP port 5568
I (XXX) WEBSERVER: HTTP server started on port 80
I (XXX) SYSTEM: Boot completed in XXX ms
```

---

## 2. MODULE TESTING

### 2.1. Network Module

#### Test 2.1.1: Ethernet Connection (Priority)

**Steps:**

1. Connect an Ethernet cable from the board to a router/switch
2. Watch the LED status:
   - ✅ **Steady green** = Ethernet connected
3. Check serial log:
   ```
   I (XXX) NETWORK: Ethernet link up
   I (XXX) NETWORK: IP address: 192.168.1.XXX
   ```
4. From a computer, ping the IP address:
   ```bash
   ping 192.168.1.100
   ```
   - ✅ Replies are received

**Test cases:**

- [ ] Ethernet DHCP - gets an IP automatically
- [ ] Ethernet Static IP - static IP configuration
- [ ] Ethernet cable unplug/replug - reconnect trong 5s
- [ ] Ethernet link down - fallback to WiFi

#### Test 2.1.2: WiFi Station Mode

**Steps:**

1. Do not connect Ethernet
2. Configure a WiFi profile over serial or the web
3. Reboot board
4. Watch the LED:
   - ✅ **Slow-blinking green** = WiFi STA connected
5. Check the IP address in the serial log

**Test cases:**

- [ ] WiFi STA connect - correct password
- [ ] WiFi STA wrong password - skips to the next profile
- [ ] WiFi STA out of range - skip profile
- [ ] Multiple profiles - connect theo priority

#### Test 2.1.3: WiFi Access Point Mode

**Steps:**

1. No Ethernet connected and no valid WiFi profile
2. The board creates a WiFi AP by itself
3. Watch the LED:
   - ✅ **Steady purple** = WiFi AP active
4. From a phone/laptop, scan for WiFi:
   - ✅ SSID visible: "ArtnetNode-XXXX"
5. Connect to the AP (password: "12345678" or as configured)
6. Open a browser at http://192.168.4.1

**Test cases:**

- [ ] AP mode activates when no other network is available
- [ ] AP SSID is shown correctly
- [ ] AP password authentication
- [ ] Client device gets an IP from DHCP
- [ ] Web interface accessible

---

### 2.2. LED Manager Module

#### Test 2.2.1: LED Status Indication

**Steps:**

1. Test each network state:

   - Boot: light blue
   - Ethernet: steady green
   - WiFi STA: slow-blinking green
   - WiFi AP: steady purple

2. Test data receiving:

   - Send an Art-Net/sACN packet
   - ✅ LED blinks white once, briefly

3. Test error state:
   - Trigger error (disconnect critical component)
   - ✅ LED blinks red fast

**Test cases:**

- [ ] Boot LED color
- [ ] Network status LEDs
- [ ] Data pulse on packet (max 10 Hz)
- [ ] RDM discovery LED (blinking yellow)
- [ ] Error LED (fast-blinking red)

---

### 2.3. DMX/RDM Handler Module

#### Test 2.3.1: DMX Output Port 1

**Setup:**

- Connect a DMX cable from Port 1 to a DMX fixture
- Configure Port 1: Mode = DMX_OUT, Universe = 0

**Steps:**

1. From an Art-Net controller, send DMX data to Universe 0
2. Watch the fixture:
   - ✅ The fixture receives the DMX signal (LED indicator blinks)
   - ✅ Fixture respond theo DMX values
3. Test the channels:
   - Channel 1 = 255 → Fixture full brightness/value
   - Channel 1 = 0 → Fixture off/zero
   - Channel 1 = 128 → Fixture 50%

**Test cases:**

- [ ] DMX output active
- [ ] All 512 channels work correctly
- [ ] DMX timing (44 Hz frame rate)
- [ ] No flickering
- [ ] Blackout on timeout (3s no data)

#### Test 2.3.2: DMX Output Port 2

**Steps:** Same as Port 1, but:

- Connect Port 2
- Universe = 1 (or as configured)
- Test independently of Port 1

**Test cases:**

- [ ] Port 2 works independently of Port 1
- [ ] Send data to both ports at the same time
- [ ] Verify there is no crosstalk between the 2 ports

#### Test 2.3.3: DMX Input Mode

**Setup:**

- Configure Port 1: Mode = DMX_IN
- Connect a DMX controller/console to the Port 1 input

**Steps:**

1. From the DMX console, send DMX data
2. Access web interface, check DMX levels page
3. Verify the DMX values are shown correctly

**Test cases:**

- [ ] DMX input detection
- [ ] All 512 channels read correctly
- [ ] Web interface updates in real-time
- [ ] DMX input → network output (if configured)

#### Test 2.3.4: RDM Discovery (Master Mode)

**Setup:**

- Configure Port 1: Mode = RDM_MASTER
- Connect an RDM-capable fixture to Port 1

**Steps:**

1. Access web interface → RDM page → Port 1
2. Click "Start Discovery"
3. Watch the LED: blinking yellow
4. After 5-10 seconds, check the device list
5. ✅ The fixture UID appears in the list

**Test cases:**

- [ ] RDM discovery finds devices
- [ ] Device information is complete (UID, model, manufacturer)
- [ ] Discovery with multiple devices (2-5 devices)
- [ ] Re-discovery after adding/removing a device

#### Test 2.3.5: RDM Get/Set Parameters

**Setup:** After a successful discovery

**Steps:**

1. Select a device from the list
2. **RDM GET**: Read the DMX start address

   - Click "Get DMX Address"
   - ✅ Shows the current address (e.g., 001)

3. **RDM SET**: Change the DMX start address

   - Enter the new address: 010
   - Click "Set DMX Address"
   - ✅ The fixture updates its address
   - Verify by doing a GET again

4. **Identify Device**:
   - Click "Identify"
   - ✅ The fixture blinks/flashes to identify itself

**Test cases:**

- [ ] GET DMX Start Address
- [ ] SET DMX Start Address
- [ ] GET Device Info
- [ ] GET Manufacturer Label
- [ ] GET Device Model Description
- [ ] Identify Device ON/OFF

---

### 2.4. Merge Engine Module

#### Test 2.4.1: HTP Merge Mode

**Setup:**

- Configure Port 1: Merge Mode = HTP, Universe = 0
- 2 Art-Net sources sending to the same Universe 0

**Steps:**

1. Source 1 (IP .10): Ch1=100, Ch2=50
2. Source 2 (IP .20): Ch1=50, Ch2=200
3. Check output:
   - ✅ Ch1 = 100 (highest from Source 1)
   - ✅ Ch2 = 200 (highest from Source 2)

**Test cases:**

- [ ] HTP merge with 2 sources
- [ ] HTP merge with 4 sources
- [ ] One source timeout → use remaining source
- [ ] All sources timeout → blackout

#### Test 2.4.2: LTP Merge Mode

**Steps:** Same as HTP, but:

- Configure Merge Mode = LTP
- Verify output = LOWEST values from the sources

#### Test 2.4.3: LAST Merge Mode

**Steps:**

1. Source 1 sends: Ch1=100
2. Wait 1 second
3. Source 2 sends: Ch1=50
4. ✅ Output = 50 (from last received source)

**Test cases:**

- [ ] Last packet wins
- [ ] Switching between sources
- [ ] Timeout detection

#### Test 2.4.4: BACKUP Merge Mode

**Steps:**

1. Source 1 (priority 100): Ch1=100
2. Source 2 (priority 50): Ch1=200
3. ✅ Output = 100 (priority source)
4. Disconnect Source 1
5. Wait 3 seconds (timeout)
6. ✅ Output = 200 (fallback to backup)

**Test cases:**

- [ ] Primary source active
- [ ] Backup source active when primary timeout
- [ ] Recovery when the primary comes back

---

### 2.5. Art-Net Receiver Module

#### Test 2.5.1: Art-Net DMX Reception

**Setup:**

- QLC+ or other Art-Net software
- Configure output Universe 0 → the ESP32's IP

**Steps:**

1. In QLC+, create a fixture on Universe 0
2. Set channel values
3. Start output
4. ✅ DMX fixture connected to Port 1 responds

**Test cases:**

- [ ] Receive Art-Net packets
- [ ] Correct universe routing
- [ ] Multiple universes (0, 1) → (Port 1, Port 2)
- [ ] Sequence number tracking
- [ ] Packet statistics accurate

#### Test 2.5.2: Art-Poll Reply

**Steps:**

1. From an Art-Net controller, send ArtPoll
2. Check controller's node list
3. ✅ "ArtNode-2RDM" appears with the right IP
4. Verify node information:
   - Short name
   - Long name
   - Number of ports (2)
   - Universe assignments

**Test cases:**

- [ ] ArtPoll response
- [ ] Node information correct
- [ ] Port status correct (active/inactive)

---

### 2.6. sACN Receiver Module

#### Test 2.6.1: sACN Multicast Reception

**Setup:**

- QLC+ with E1.31 (sACN) output enabled
- Universe 1 → Multicast 239.255.0.1

**Steps:**

1. Enable E1.31 output trong QLC+
2. Set fixture values
3. ✅ DMX fixture responds

**Test cases:**

- [ ] Receive sACN multicast packets
- [ ] Correct universe routing
- [ ] Priority handling (0-200)
- [ ] Source name tracking
- [ ] Sequence number checking

#### Test 2.6.2: sACN Priority

**Steps:**

1. Source 1 (priority 100): Ch1=100
2. Source 2 (priority 150): Ch1=200
3. ✅ Output = 200 (higher priority wins)

**Test cases:**

- [ ] Priority comparison
- [ ] Priority override
- [ ] Priority with merge modes

---

### 2.7. Web Server Module

#### Test 2.7.1: Web Interface Access

**Steps:**

1. Open browser, navigate to http://[ESP32_IP]
2. ✅ Web page loads successfully
3. Verify UI elements:
   - Navigation menu
   - Status dashboard
   - Configuration pages

**Test cases:**

- [ ] HTTP server responds
- [ ] Static files load (HTML, CSS, JS)
- [ ] No 404 errors
- [ ] Responsive design (mobile/desktop)

#### Test 2.7.2: Configuration APIs

**Test GET /api/config:**

```bash
curl http://192.168.1.100/api/config
```

✅ Returns JSON with full configuration

**Test POST /api/config:**

```bash
curl -X POST http://192.168.1.100/api/config \
  -H "Content-Type: application/json" \
  -d '{"universe_primary_port1": 5}'
```

✅ Configuration updated

**Test cases:**

- [ ] GET config returns valid JSON
- [ ] POST config updates successfully
- [ ] Invalid JSON returns 400 error
- [ ] Config persists after reboot

#### Test 2.7.3: WiFi Management

**Steps:**

1. Access /api/wifi/scan
2. ✅ Returns list of WiFi networks
3. Add WiFi profile:
   - SSID, password, priority
4. ✅ Profile saved
5. Reboot → ✅ Connect to new profile

**Test cases:**

- [ ] WiFi scan works
- [ ] Add/delete profiles
- [ ] Profile priority sorting
- [ ] Connect to configured profile

#### Test 2.7.4: WebSocket Real-time Updates

**Steps:**

1. Open web interface → DMX Monitor page
2. Open WebSocket connection to /ws/dmx/1
3. Send DMX data from an Art-Net controller
4. ✅ DMX levels update in real time on the web page

**Test cases:**

- [ ] WebSocket connection established
- [ ] Real-time DMX level updates
- [ ] Multiple WebSocket clients
- [ ] WebSocket disconnect/reconnect

---

### 2.8. Storage Module

#### Test 2.8.1: Configuration Persistence

**Steps:**

1. Change configuration qua web interface
2. Reboot device (power cycle)
3. ✅ Configuration retained sau reboot
4. Verify via /api/config

**Test cases:**

- [ ] Config save to NVS
- [ ] Config save to LittleFS JSON
- [ ] Config load priority (NVS → File → Defaults)
- [ ] Config backup file creation

#### Test 2.8.2: Factory Reset

**Steps:**

1. Call /api/system/factory-reset
2. Device reboots
3. ✅ All settings return to their defaults
4. WiFi AP mode active (no saved profiles)

**Test cases:**

- [ ] Factory reset clears NVS
- [ ] Factory reset clears config file
- [ ] Default config loaded
- [ ] Web files intact (not deleted)

---

## 3. INTEGRATION TESTING

### 3.1. Full System Test

**Scenario:** Complete Art-Net to DMX workflow

**Setup:**

- Ethernet connected
- 2 DMX fixtures on Port 1 and Port 2
- QLC+ with 2 universes

**Steps:**

1. **Network**: Verify Ethernet connected
2. **Art-Net**: QLC+ sends to Universe 0, 1
3. **Merge**: Single source, no merge
4. **DMX Output**: Both fixtures respond correctly
5. **LED**: White pulse on packet receive
6. **Web**: Monitor DMX levels real-time

**Pass criteria:**

- [ ] End-to-end latency < 10ms
- [ ] No packet loss
- [ ] Stable operation > 30 minutes
- [ ] No memory leaks
- [ ] CPU usage < 50%

### 3.2. Multi-Source Merge Test

**Scenario:** 2 controllers merge HTP to single port

**Setup:**

- 2 Art-Net sources (QLC+ on 2 computers)
- Both send to Universe 0
- Merge Mode = HTP

**Steps:**

1. Source 1: Ch1-10 = 100
2. Source 2: Ch11-20 = 200
3. ✅ Output: Ch1-10=100, Ch11-20=200
4. Source 1: Ch1=255
5. ✅ Output: Ch1=255 (higher wins)

**Pass criteria:**

- [ ] Correct HTP merge
- [ ] No flickering
- [ ] Smooth transitions
- [ ] Timeout handling correct

---

## 4. PERFORMANCE TESTING

### 4.1. Maximum Packet Rate

**Goal:** Verify the device can handle a high packet rate

**Test:**

1. QLC+ send Art-Net at max rate (44 Hz per universe)
2. Monitor with Wireshark
3. Check the DMX output does not drop frames

**Metrics:**

- Packet rate: > 40 Hz
- CPU usage: < 80%
- DMX frame rate: stable 44 Hz
- Latency: < 5ms

### 4.2. Multiple Universe Stress Test

**Test:**

1. Send data to Universe 0-9 (10 universes)
2. The device subscribes only to Universe 0, 1
3. ✅ The device is not overloaded
4. Ports 1 and 2 stay stable

**Metrics:**

- CPU usage: < 70%
- Memory usage: stable
- No crashes
- Other universes ignored correctly

### 4.3. Long-term Stability Test

**Test:**

1. Run continuously for 24 hours
2. Monitor memory usage
3. Check uptime

**Pass criteria:**

- [ ] No crashes
- [ ] No memory leaks
- [ ] Stable performance
- [ ] No unexpected reboots

---

## 5. ERROR HANDLING & RECOVERY

### 5.1. Network Disconnect Recovery

**Test:**

1. Ethernet connected, DMX output active
2. Unplug Ethernet cable
3. ✅ Fallback to WiFi trong 10s
4. DMX output continues
5. Replug Ethernet
6. ✅ Switch back to Ethernet

### 5.2. Power Failure Recovery

**Test:**

1. Device running
2. Cut the power suddenly
3. Power it back up
4. ✅ Device boots, loads config
5. Reconnects to network
6. Resumes DMX output

### 5.3. Invalid Data Handling

**Test:**

1. Send malformed Art-Net packet
2. ✅ Device drops packet, logs error
3. Device continues operating normally

---

## 6. TROUBLESHOOTING

### Common Issues

| Symptom                             | Possible Cause                 | Solution                                      |
| ----------------------------------- | ------------------------------ | --------------------------------------------- |
| LED blinks red fast                 | System error, boot fail        | Check serial log, reflash firmware            |
| No LED                              | Power issue, GPIO 48 fail      | Check power supply, check WS2812 connection   |
| Does not connect to WiFi            | Wrong password, out of range   | Check WiFi credentials, move closer to AP     |
| No Art-Net received                 | Wrong universe, firewall       | Check universe config, disable firewall       |
| DMX fixture does not respond        | Cable issue, wrong universe    | Check DMX cable, verify universe mapping      |
| DMX flickering                      | Packet loss, merge timeout     | Check network quality, increase merge timeout |
| Web interface does not load         | Network issue, server crash    | Check IP address, reboot device               |
| RDM discovery finds no device       | RDM not supported, cable issue | Verify device supports RDM, check cable       |

### Debug Tools

**Serial Monitor:**

```bash
# Linux/Mac
screen COM3 115200

# Windows
PuTTY → Serial → COM3 → 115200
```

**Network Packet Capture:**

```bash
# Wireshark filter for Art-Net
udp.port == 6454

# Wireshark filter for sACN
udp.port == 5568
```

**Check Device IP:**

```bash
# Scan network
nmap -sn 192.168.1.0/24

# Or use Advanced IP Scanner (Windows)
```

---

## 7. TEST REPORT TEMPLATE

```markdown
# Test Report - ESP-NODE-2RDM

**Date:** [Date]
**Firmware Version:** [Version]
**Tester:** [Name]

## Test Summary

- Total Tests: XX
- Passed: XX
- Failed: XX
- Skipped: XX

## Module Test Results

### Network Module

- [x] Ethernet Connection - PASS
- [x] WiFi STA Mode - PASS
- [x] WiFi AP Mode - PASS

### DMX Module

- [x] Port 1 Output - PASS
- [x] Port 2 Output - PASS
- [ ] DMX Input - FAIL (see notes)

### RDM Module

- [x] Discovery - PASS
- [x] GET Parameters - PASS
- [x] SET Parameters - PASS

## Issues Found

1. [Issue description]
   - Severity: High/Medium/Low
   - Reproducible: Yes/No
   - Workaround: [If any]

## Notes

[Additional observations]

## Conclusion

[Overall assessment - PASS/FAIL]
```

---

## APPENDIX

### A. Quick Test Checklist

```
BASIC TESTS (Required)
□ Power on - LED boot color
□ Network connect - Ethernet or WiFi
□ Web interface accessible
□ DMX Port 1 output works
□ DMX Port 2 output works
□ Art-Net reception
□ Config save/load
□ Factory reset

ADVANCED TESTS (Optional)
□ RDM discovery
□ RDM get/set
□ sACN reception
□ Merge modes (HTP, LTP, LAST, BACKUP)
□ Multi-source merge
□ WebSocket real-time updates
□ Long-term stability (24h)

PERFORMANCE (Optional)
□ Max packet rate test
□ CPU usage < 80%
□ Memory stable
□ Latency < 10ms
```

### B. Test Equipment Recommendations

**Budget Setup ($100-200):**

- ESP32-S3 board
- 1-2 cheap DMX LED fixtures
- Home router
- Computer with QLC+ (free)

**Professional Setup ($500-1000):**

- ESP32-S3 board
- 2-4 RDM-capable fixtures
- Managed Ethernet switch
- DMX controller/console
- Logic analyzer
- Multiple test computers

---

**END OF TESTING GUIDE**

Following this testing procedure makes sure the firmware is stable and meets the spec before release!
