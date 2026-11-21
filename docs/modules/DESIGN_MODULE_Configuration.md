# MODULE DESIGN: Configuration

**Project:** Artnet-Node-2RDM  
**Module:** Configuration Management  
**Version:** 1.0  
**Created:** 25/12/2025

---

## 1. Purpose

The Configuration module manages all of the system's configuration settings, including:
- Network settings (Ethernet, WiFi)
- DMX/RDM port configuration
- Protocol configuration (Art-Net, sACN)
- Merge Engine configuration
- Device information (node name, ID)

---

## 2. Module architecture

### 2.1. Main data structures

```c
typedef struct {
    // Network Configuration
    bool use_ethernet;
    wifi_profile_t wifi_profiles[5];
    char ap_ssid[32];
    char ap_password[64];
    uint8_t ap_channel;
    
    // IP Configuration
    ip_config_t ethernet_ip;
    ip_config_t wifi_sta_ip;
    ip_config_t wifi_ap_ip;
    
    // DMX Port Configuration
    dmx_mode_t port1_mode;
    dmx_mode_t port2_mode;
    uint16_t universe_primary_port1;
    uint16_t universe_primary_port2;
    int16_t universe_secondary_port1;
    int16_t universe_secondary_port2;
    int16_t universe_offset_port1;
    int16_t universe_offset_port2;
    
    // Protocol Configuration
    protocol_mode_t protocol_mode_port1;
    protocol_mode_t protocol_mode_port2;
    
    // Merge Configuration
    merge_mode_t merge_mode_port1;
    merge_mode_t merge_mode_port2;
    uint8_t merge_timeout_seconds;
    
    // RDM Configuration
    bool rdm_enabled_port1;
    bool rdm_enabled_port2;
    
    // Node Information
    char node_short_name[18];
    char node_long_name[64];
    uint8_t node_mac[6];
    
} system_config_t;
```

### 2.2. Enumerations

```c
typedef enum {
    DMX_OUT = 0,
    DMX_IN = 1,
    RDM_MASTER = 2,
    RDM_RESPONDER = 3,
    DISABLED = 4
} dmx_mode_t;

typedef enum {
    ARTNET_ONLY = 0,
    SACN_ONLY = 1,
    ARTNET_PRIORITY = 2,
    SACN_PRIORITY = 3,
    MERGE_BOTH = 4
} protocol_mode_t;

typedef enum {
    HTP = 0,      // Highest Takes Precedence
    LTP = 1,      // Lowest Takes Precedence
    LAST = 2,     // Last packet wins
    BACKUP = 3,   // Primary + Backup source
    DISABLE = 4   // No merge
} merge_mode_t;
```

---

## 3. API Public

### 3.1. Initializing and loading the configuration

```c
/**
 * Initialize the Configuration module
 * @return ESP_OK on success
 */
esp_err_t config_init(void);

/**
 * Load the configuration from storage (NVS or LittleFS)
 * @return ESP_OK on success
 */
esp_err_t config_load(void);

/**
 * Load the default configuration
 */
void config_load_defaults(void);
```

### 3.2. Save Configuration

```c
/**
 * Save the configuration to storage
 * @return ESP_OK on success
 */
esp_err_t config_save(void);

/**
 * Save the configuration to a JSON file
 * @param filename file name
 * @return ESP_OK on success
 */
esp_err_t config_save_to_json(const char *filename);
```

### 3.3. Get/Set Configuration

```c
/**
 * Get a pointer to the current configuration (read-only)
 * @return Pointer to system_config_t
 */
const system_config_t* config_get(void);

/**
 * Get a copy of the current configuration
 * @param config pointer to store the configuration in
 * @return ESP_OK on success
 */
esp_err_t config_get_copy(system_config_t *config);

/**
 * Update the whole configuration
 * @param config the new configuration
 * @return ESP_OK on success
 */
esp_err_t config_set(const system_config_t *config);

/**
 * Update the configuration of one DMX port
 * @param port_num port number (1 or 2)
 * @param mode DMX mode
 * @param primary_universe primary universe
 * @param secondary_universe secondary universe (-1 to disable)
 * @param offset Universe offset
 * @return ESP_OK on success
 */
esp_err_t config_set_port(uint8_t port_num, 
                          dmx_mode_t mode,
                          uint16_t primary_universe,
                          int16_t secondary_universe,
                          int16_t offset);
```

### 3.4. Validation

```c
/**
 * Check that a configuration is valid
 * @param config the configuration to check
 * @return true if valid
 */
bool config_validate(const system_config_t *config);

/**
 * Check that a WiFi profile is valid
 * @param profile WiFi profile
 * @return true if valid
 */
bool config_validate_wifi_profile(const wifi_profile_t *profile);
```

---

## 4. Operation flow

### 4.1. Boot sequence

```
1. config_init()
   └─> Create the mutex/semaphore
   └─> Allocate memory for the config

2. config_load()
   └─> Try reading from NVS
   └─> If that fails, try reading from LittleFS (config.json)
   └─> If that fails, load the defaults
   └─> Validate config
   └─> If invalid, load the defaults

3. Other modules call config_get() to get the configuration
```

### 4.2. Runtime update

```
1. The Web Server receives a request to change the config
2. Parse the JSON from the HTTP POST
3. Validate the new config
4. config_set() to update it
5. config_save() to write it to storage
6. Trigger the related modules to reload the config
```

---

## 5. Thread Safety

- The module uses a **mutex** to protect concurrent access
- The get/set functions are all thread-safe
- config_get() returns a const pointer, so it cannot be changed directly
- Every change must go through config_set()

---

## 6. Storage Backend

### 6.1. NVS (Non-Volatile Storage)

- Binary storage, fast
- Uses the namespace "config"
- Key: "system_config"

### 6.2. LittleFS (JSON)

- Text storage (JSON), easy to debug
- File: `/littlefs/config.json`
- Backup khi save: `config.json.bak`

### 6.3. Priority

1. Try reading NVS first
2. If NVS has nothing or fails, read LittleFS
3. If both fail, use the defaults

---

## 7. Default configuration

```c
static const system_config_t DEFAULT_CONFIG = {
    .use_ethernet = true,
    .ap_ssid = "ArtnetNode-XXXX",
    .ap_password = "12345678",
    .ap_channel = 1,
    
    .port1_mode = DMX_OUT,
    .port2_mode = DMX_OUT,
    .universe_primary_port1 = 0,
    .universe_primary_port2 = 1,
    .universe_secondary_port1 = -1,
    .universe_secondary_port2 = -1,
    .universe_offset_port1 = 0,
    .universe_offset_port2 = 0,
    
    .protocol_mode_port1 = ARTNET_PRIORITY,
    .protocol_mode_port2 = ARTNET_PRIORITY,
    
    .merge_mode_port1 = HTP,
    .merge_mode_port2 = HTP,
    .merge_timeout_seconds = 3,
    
    .rdm_enabled_port1 = true,
    .rdm_enabled_port2 = true,
    
    .node_short_name = "ArtNode-2RDM",
    .node_long_name = "ESP32 ArtNet Node 2 Port RDM"
};
```

---

## 8. Error handling

| Error | Action |
|------|-----------|
| NVS read fail | Try reading LittleFS |
| LittleFS read fail | Load defaults |
| Config invalid | Load defaults |
| Save fail | Retry 3 times, log an error |
| JSON parse error | Load defaults |

---

## 9. Dependencies

- **ESP-IDF NVS**: binary storage
- **LittleFS**: File system
- **cJSON**: Parse/generate JSON
- **FreeRTOS**: Mutex, semaphore

---

## 10. Testing Points

1. Load the defaults when there is no config
2. Load from NVS successfully
3. Load from LittleFS when NVS fails
4. Validate config - reject invalid values
5. Save/Load cycle - data integrity
6. Thread-safe concurrent access
7. JSON serialization/deserialization
8. Backup/restore config file

---

## 11. Memory Usage

- Struct size: ~400 bytes
- JSON file: ~2-4 KB
- Heap usage: ~10 KB (temporary during parse/save)

---

## 12. Performance

- Load time: < 100ms
- Save time: < 200ms
- Get config: < 1µs (pointer access)
