# MODULE DESIGN: Network

**Project:** Artnet-Node-2RDM  
**Module:** Network Management  
**Version:** 1.0  
**Created:** 25/12/2025

---

## 1. Purpose

The Network module manages the device's network connections, including:
- Ethernet (W5500 SPI)
- WiFi Station mode (connects to an AP)
- WiFi Access Point mode (creates its own AP)
- Auto-fallback between the methods

---

## 2. Module architecture

### 2.1. Data structures

```c
typedef struct {
    char ssid[32];
    char password[64];
    uint8_t priority;  // 0-255, lower = higher priority
    bool use_static_ip;
    ip_config_t static_ip;
} wifi_profile_t;

typedef struct {
    bool use_dhcp;
    char ip[16];
    char netmask[16];
    char gateway[16];
    char dns1[16];
    char dns2[16];
} ip_config_t;

typedef enum {
    NETWORK_DISCONNECTED = 0,
    NETWORK_ETHERNET_CONNECTED = 1,
    NETWORK_WIFI_STA_CONNECTED = 2,
    NETWORK_WIFI_AP_ACTIVE = 3,
    NETWORK_CONNECTING = 4,
    NETWORK_ERROR = 5
} network_state_t;

typedef struct {
    network_state_t state;
    char ip_address[16];
    char netmask[16];
    char gateway[16];
    uint8_t mac[6];
    int rssi;  // WiFi only
    uint32_t packets_received;
    uint32_t packets_sent;
    uint32_t bytes_received;
    uint32_t bytes_sent;
} network_status_t;
```

---

## 3. API Public

### 3.1. Initialization

```c
/**
 * Initialize the Network module
 * @return ESP_OK on success
 */
esp_err_t network_init(void);

/**
 * Start the network connection according to the configuration
 * @return ESP_OK on success
 */
esp_err_t network_start(void);

/**
 * Stop all network interfaces
 * @return ESP_OK on success
 */
esp_err_t network_stop(void);
```

### 3.2. Ethernet Control

```c
/**
 * Initialize and connect the W5500 Ethernet
 * @param config IP configuration
 * @return ESP_OK on success
 */
esp_err_t network_ethernet_connect(const ip_config_t *config);

/**
 * Disconnect Ethernet
 * @return ESP_OK on success
 */
esp_err_t network_ethernet_disconnect(void);

/**
 * Check the Ethernet link state
 * @return true if the link is up
 */
bool network_ethernet_is_link_up(void);
```

### 3.3. WiFi Station Control

```c
/**
 * Connect to a WiFi AP using a profile
 * @param profile WiFi profile
 * @return ESP_OK on success
 */
esp_err_t network_wifi_sta_connect(const wifi_profile_t *profile);

/**
 * Disconnect WiFi STA
 * @return ESP_OK on success
 */
esp_err_t network_wifi_sta_disconnect(void);

/**
 * Scan the WiFi networks around
 * @param scan_result buffer for the results
 * @param max_aps maximum number of APs
 * @return number of APs found
 */
uint16_t network_wifi_scan(wifi_ap_record_t *scan_result, uint16_t max_aps);
```

### 3.4. WiFi Access Point Control

```c
/**
 * Create a WiFi Access Point
 * @param ssid SSID of the AP
 * @param password Password (min 8 characters)
 * @param channel Channel (1-13)
 * @param config IP configuration
 * @return ESP_OK on success
 */
esp_err_t network_wifi_ap_start(const char *ssid, 
                                const char *password,
                                uint8_t channel,
                                const ip_config_t *config);

/**
 * Stop the WiFi Access Point
 * @return ESP_OK on success
 */
esp_err_t network_wifi_ap_stop(void);

/**
 * Get the list of connected stations
 * @param stations buffer for the list
 * @param max_stations maximum number
 * @return number of connected stations
 */
uint16_t network_wifi_ap_get_stations(wifi_sta_info_t *stations, 
                                       uint16_t max_stations);
```

### 3.5. Status & Information

```c
/**
 * Get the current network state
 * @param status buffer for the status
 * @return ESP_OK on success
 */
esp_err_t network_get_status(network_status_t *status);

/**
 * Get the current IP address
 * @return IP address string, or NULL if not connected
 */
const char* network_get_ip_address(void);

/**
 * Get the MAC address
 * @param mac 6-byte buffer for the MAC
 * @return ESP_OK on success
 */
esp_err_t network_get_mac_address(uint8_t mac[6]);
```

### 3.6. Event Callbacks

```c
/**
 * Register a callback for network state changes
 * @param callback Function callback
 * @param user_data User data
 */
void network_register_state_callback(network_state_callback_t callback,
                                     void *user_data);
```

---

## 4. Operation flow

### 4.1. Auto-connect Sequence

```
1. network_start()
   └─> Read the config (use_ethernet, wifi_profiles, ap_config)
   
2. If use_ethernet == true:
   └─> network_ethernet_connect()
       └─> Try to connect (timeout 10s)
       └─> Retry 3 times
       └─> If successful → DONE
       └─> If it fails → continue to WiFi
   
3. If Ethernet fails or is disabled:
   └─> Sort wifi_profiles by priority
   └─> For each profile (high to low priority):
       └─> network_wifi_sta_connect(profile)
       └─> Timeout 15s
       └─> If successful → DONE
       └─> If it fails → try the next profile
   
4. If every WiFi STA profile fails:
   └─> network_wifi_ap_start()
       └─> Create an AP with the SSID from the config
       └─> Password from the config, or the default
       └─> Default IP: 192.168.4.1
```

### 4.2. State Machine

```
DISCONNECTED
    └─> [Start] → CONNECTING
    
CONNECTING (Ethernet)
    └─> [Link Up] → ETHERNET_CONNECTED
    └─> [Timeout/Fail] → CONNECTING (WiFi STA)
    
CONNECTING (WiFi STA)
    └─> [Got IP] → WIFI_STA_CONNECTED
    └─> [Timeout/Fail] → CONNECTING (WiFi AP)
    
CONNECTING (WiFi AP)
    └─> [AP Started] → WIFI_AP_ACTIVE
    └─> [Fail] → ERROR
    
ETHERNET_CONNECTED
    └─> [Link Down] → DISCONNECTED
    
WIFI_STA_CONNECTED
    └─> [Disconnected] → DISCONNECTED
    
WIFI_AP_ACTIVE
    └─> [Stop] → DISCONNECTED
```

---

## 5. Hardware Interface

### 5.1. W5500 Ethernet SPI

```c
#define W5500_CS_PIN    GPIO_NUM_10
#define W5500_MOSI_PIN  GPIO_NUM_11
#define W5500_MISO_PIN  GPIO_NUM_13
#define W5500_SCK_PIN   GPIO_NUM_12
#define W5500_INT_PIN   GPIO_NUM_9

// SPI Configuration
#define W5500_SPI_CLOCK_MHZ  20
#define W5500_SPI_MODE       0
```

**W5500 initialization sequence:**
1. Init SPI bus
2. Reset the W5500 (toggle the reset pin if there is one)
3. Check chip version (register 0x0039)
4. Configure MAC address
5. Configure IP/Netmask/Gateway
6. Open sockets for Art-Net (UDP 6454) and sACN

### 5.2. WiFi

- Uses the ESP-IDF WiFi driver
- Antenna: internal PCB antenna
- Power save mode: NONE (always on, for low latency)

---

## 6. Threading Model

- **Network Task** (Priority: 5, Core: 0):
  - Monitor link status
  - Handle reconnect
  - Process Ethernet interrupts
  - Update statistics

- **Event Handler**:
  - WiFi events (STA_START, STA_CONNECTED, STA_DISCONNECTED, etc.)
  - IP events (GOT_IP, LOST_IP)
  - Ethernet events (LINK_UP, LINK_DOWN)

---

## 7. Error Handling

| Error | Action |
|-----|-----------|
| Ethernet hardware not found | Log warning, fallback WiFi |
| Ethernet link down | Retry 3 times, then fall back to WiFi |
| WiFi STA connect timeout | Try next profile |
| WiFi STA password wrong | Skip profile, try next |
| WiFi AP start fail | Log error, retry with default config |
| No network available | Stay in WiFi AP mode |

---

## 8. Configuration Examples

### 8.1. Ethernet with DHCP

```json
{
  "use_ethernet": true,
  "ethernet_ip": {
    "use_dhcp": true
  }
}
```

### 8.2. Ethernet with a static IP

```json
{
  "use_ethernet": true,
  "ethernet_ip": {
    "use_dhcp": false,
    "ip": "192.168.1.100",
    "netmask": "255.255.255.0",
    "gateway": "192.168.1.1",
    "dns1": "8.8.8.8"
  }
}
```

### 8.3. WiFi Station with multiple profiles

```json
{
  "use_ethernet": false,
  "wifi_profiles": [
    {
      "ssid": "Office_Network",
      "password": "password123",
      "priority": 0,
      "use_static_ip": false
    },
    {
      "ssid": "Home_Network",
      "password": "homepass",
      "priority": 1,
      "use_static_ip": true,
      "static_ip": {
        "ip": "192.168.0.50",
        "netmask": "255.255.255.0",
        "gateway": "192.168.0.1"
      }
    }
  ]
}
```

---

## 9. Dependencies

- **ESP-IDF Components**:
  - esp_netif
  - esp_eth (Ethernet driver)
  - esp_wifi
  - lwIP (TCP/IP stack)
  - driver (SPI, GPIO)

- **External Libraries**:
  - W5500 driver (custom or esp_eth)

---

## 10. Testing Points

1. Ethernet connection - DHCP
2. Ethernet connection - Static IP
3. Ethernet link up/down detection
4. WiFi STA connection - single profile
5. WiFi STA connection - multiple profiles with priority
6. WiFi STA - wrong password handling
7. WiFi STA - network not found handling
8. WiFi AP creation
9. WiFi AP - client connection
10. Auto-fallback: Ethernet → WiFi STA → WiFi AP
11. Reconnection after network loss
12. Network statistics accuracy

---

## 11. Memory Usage

- W5500 buffers: 32 KB (chip internal)
- WiFi buffers: ~40 KB (managed by ESP-IDF)
- Module heap: ~5 KB

---

## 12. Performance

- Ethernet throughput: ~10 Mbps (practical)
- WiFi throughput: ~20-30 Mbps (depending on conditions)
- Connection time:
  - Ethernet: 2-5 seconds
  - WiFi STA: 5-15 seconds
  - WiFi AP: 1-2 seconds
- Packet latency:
  - Ethernet: < 1 ms
  - WiFi: 2-10 ms

---

## 13. Power Considerations

- Ethernet: ~150 mA active
- WiFi: ~80-200 mA (depending on TX power)
- Power save mode disabled to reduce latency
