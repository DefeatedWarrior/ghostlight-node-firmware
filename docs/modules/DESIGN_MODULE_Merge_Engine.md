# MODULE DESIGN: Merge Engine

**Project:** Artnet-Node-2RDM  
**Module:** Data Merge Engine  
**Version:** 1.0  
**Created:** 25/12/2025

---

## 1. Purpose

The Merge Engine module combines DMX data from several different sources:
- Several Art-Net sources
- Several sACN sources
- Several universes (primary + secondary)
- Different protocols (Art-Net + sACN)

The Merge Engine applies different merge algorithms and handles timeouts.

---

## 2. Module architecture

### 2.1. Data structures

```c
typedef enum {
    MERGE_MODE_HTP = 0,      // Highest Takes Precedence
    MERGE_MODE_LTP = 1,      // Lowest Takes Precedence  
    MERGE_MODE_LAST = 2,     // Last packet wins
    MERGE_MODE_BACKUP = 3,   // Primary + Backup source
    MERGE_MODE_DISABLE = 4   // No merge, single source only
} merge_mode_t;

typedef enum {
    SOURCE_ARTNET = 0,
    SOURCE_SACN = 1
} source_protocol_t;

typedef struct {
    uint8_t data[512];          // DMX data
    uint64_t timestamp;         // Microseconds
    uint32_t sequence;          // Sequence number (if available)
    uint8_t priority;           // Source priority (sACN only)
    char source_name[64];       // Source identifier
    uint32_t source_ip;         // Source IP address
    source_protocol_t protocol; // Art-Net or sACN
    bool is_valid;              // Data valid flag
} dmx_source_data_t;

typedef struct {
    uint8_t port_num;           // 1 or 2
    uint16_t universe;          // Target universe
    merge_mode_t mode;
    uint32_t timeout_us;        // Timeout in microseconds
    
    // Source tracking (max 4 sources per universe)
    dmx_source_data_t sources[4];
    uint8_t source_count;
    
    // Output buffer
    uint8_t merged_data[512];
    uint64_t last_merge_time;
    bool output_active;
} merge_context_t;
```

---

## 3. API Public

### 3.1. Initialization

```c
/**
 * Initialize the Merge Engine
 * @return ESP_OK on success
 */
esp_err_t merge_engine_init(void);

/**
 * Configure merging for one port
 * @param port Port number (1 or 2)
 * @param mode Merge mode
 * @param timeout_ms Timeout in milliseconds
 * @return ESP_OK on success
 */
esp_err_t merge_engine_config(uint8_t port, merge_mode_t mode, 
                              uint32_t timeout_ms);
```

### 3.2. Input Data

```c
/**
 * Feed Art-Net data into the merge engine
 * @param port Port number
 * @param universe Universe number
 * @param data DMX data (512 bytes)
 * @param sequence Sequence number
 * @param source_ip Source IP address
 * @return ESP_OK on success
 */
esp_err_t merge_engine_push_artnet(uint8_t port, uint16_t universe,
                                   const uint8_t *data, uint8_t sequence,
                                   uint32_t source_ip);

/**
 * Feed sACN data into the merge engine
 * @param port Port number
 * @param universe Universe number
 * @param data DMX data (512 bytes)
 * @param sequence Sequence number
 * @param priority Priority (0-200)
 * @param source_name Source name
 * @param source_ip Source IP address
 * @return ESP_OK on success
 */
esp_err_t merge_engine_push_sacn(uint8_t port, uint16_t universe,
                                 const uint8_t *data, uint8_t sequence,
                                 uint8_t priority, const char *source_name,
                                 uint32_t source_ip);

/**
 * Feed DMX Input data into the merge engine
 * @param port Port number
 * @param data DMX data (512 bytes)
 * @return ESP_OK on success
 */
esp_err_t merge_engine_push_dmx_in(uint8_t port, const uint8_t *data);
```

### 3.3. Output Data

```c
/**
 * Get the merged data for one port
 * @param port Port number
 * @param data Buffer to store merged data (512 bytes)
 * @return ESP_OK if there is data, ESP_ERR_TIMEOUT on timeout
 */
esp_err_t merge_engine_get_output(uint8_t port, uint8_t *data);

/**
 * Check whether the output is active
 * @param port Port number
 * @return true if there was data within the timeout period
 */
bool merge_engine_is_output_active(uint8_t port);

/**
 * Force blackout (clear all sources)
 * @param port Port number
 * @return ESP_OK on success
 */
esp_err_t merge_engine_blackout(uint8_t port);
```

### 3.4. Statistics

```c
/**
 * Get information about the active sources
 * @param port Port number
 * @param sources Buffer to store source info
 * @param max_sources Maximum number of sources
 * @return Number of active sources
 */
uint8_t merge_engine_get_active_sources(uint8_t port, 
                                       dmx_source_data_t *sources,
                                       uint8_t max_sources);

/**
 * Reset merge statistics
 * @param port Port number
 * @return ESP_OK on success
 */
esp_err_t merge_engine_reset_stats(uint8_t port);
```

---

## 4. Merge Algorithms

### 4.1. HTP (Highest Takes Precedence)

```c
static void merge_htp(merge_context_t *ctx) {
    memset(ctx->merged_data, 0, 512);
    
    for (int i = 0; i < ctx->source_count; i++) {
        if (!is_source_timeout(&ctx->sources[i], ctx->timeout_us)) {
            for (int ch = 0; ch < 512; ch++) {
                // Take the highest value
                if (ctx->sources[i].data[ch] > ctx->merged_data[ch]) {
                    ctx->merged_data[ch] = ctx->sources[i].data[ch];
                }
            }
        }
    }
    
    ctx->output_active = has_any_active_source(ctx);
}
```

**Use:** lighting control with several consoles controlling at once.

### 4.2. LTP (Lowest Takes Precedence)

```c
static void merge_ltp(merge_context_t *ctx) {
    memset(ctx->merged_data, 255, 512);
    
    for (int i = 0; i < ctx->source_count; i++) {
        if (!is_source_timeout(&ctx->sources[i], ctx->timeout_us)) {
            for (int ch = 0; ch < 512; ch++) {
                // Take the lowest value
                if (ctx->sources[i].data[ch] < ctx->merged_data[ch]) {
                    ctx->merged_data[ch] = ctx->sources[i].data[ch];
                }
            }
        }
    }
    
    ctx->output_active = has_any_active_source(ctx);
}
```

**Use:** rarely used, but possible for special effects.

### 4.3. LAST (Last Packet Wins)

```c
static void merge_last(merge_context_t *ctx) {
    uint64_t latest_time = 0;
    dmx_source_data_t *latest_source = NULL;
    
    // Find the most recent source
    for (int i = 0; i < ctx->source_count; i++) {
        if (!is_source_timeout(&ctx->sources[i], ctx->timeout_us)) {
            if (ctx->sources[i].timestamp > latest_time) {
                latest_time = ctx->sources[i].timestamp;
                latest_source = &ctx->sources[i];
            }
        }
    }
    
    if (latest_source) {
        memcpy(ctx->merged_data, latest_source->data, 512);
        ctx->output_active = true;
    } else {
        memset(ctx->merged_data, 0, 512);
        ctx->output_active = false;
    }
}
```

**Use:** several backup systems, fast switching between sources.

### 4.4. BACKUP (Primary + Backup)

```c
static void merge_backup(merge_context_t *ctx) {
    dmx_source_data_t *primary = NULL;
    dmx_source_data_t *backup = NULL;
    
    // Identify primary (highest priority)
    for (int i = 0; i < ctx->source_count; i++) {
        if (!is_source_timeout(&ctx->sources[i], ctx->timeout_us)) {
            if (primary == NULL || 
                ctx->sources[i].priority > primary->priority) {
                backup = primary;
                primary = &ctx->sources[i];
            } else if (backup == NULL || 
                       ctx->sources[i].priority > backup->priority) {
                backup = &ctx->sources[i];
            }
        }
    }
    
    // Use primary if available, else backup
    if (primary) {
        memcpy(ctx->merged_data, primary->data, 512);
        ctx->output_active = true;
    } else if (backup) {
        memcpy(ctx->merged_data, backup->data, 512);
        ctx->output_active = true;
    } else {
        memset(ctx->merged_data, 0, 512);
        ctx->output_active = false;
    }
}
```

**Use:** critical systems with redundant controllers.

### 4.5. DISABLE (No Merge)

```c
static void merge_disable(merge_context_t *ctx) {
    // Only use the highest priority source
    dmx_source_data_t *primary = NULL;
    
    for (int i = 0; i < ctx->source_count; i++) {
        if (!is_source_timeout(&ctx->sources[i], ctx->timeout_us)) {
            if (primary == NULL || 
                ctx->sources[i].priority > primary->priority) {
                primary = &ctx->sources[i];
            }
        }
    }
    
    if (primary) {
        memcpy(ctx->merged_data, primary->data, 512);
        ctx->output_active = true;
    } else {
        memset(ctx->merged_data, 0, 512);
        ctx->output_active = false;
    }
}
```

**Use:** single source only, no merging.

---

## 5. Source Management

### 5.1. Source Tracking

```c
static void update_or_add_source(merge_context_t *ctx, 
                                const dmx_source_data_t *new_source) {
    // Find existing source by IP
    int existing_idx = -1;
    for (int i = 0; i < ctx->source_count; i++) {
        if (ctx->sources[i].source_ip == new_source->source_ip &&
            ctx->sources[i].protocol == new_source->protocol) {
            existing_idx = i;
            break;
        }
    }
    
    if (existing_idx >= 0) {
        // Update existing source
        memcpy(&ctx->sources[existing_idx], new_source, 
               sizeof(dmx_source_data_t));
    } else if (ctx->source_count < 4) {
        // Add new source
        memcpy(&ctx->sources[ctx->source_count], new_source,
               sizeof(dmx_source_data_t));
        ctx->source_count++;
    } else {
        // Replace oldest timeout source
        int oldest_idx = find_oldest_timeout_source(ctx);
        if (oldest_idx >= 0) {
            memcpy(&ctx->sources[oldest_idx], new_source,
                   sizeof(dmx_source_data_t));
        }
    }
}
```

### 5.2. Timeout Detection

```c
static bool is_source_timeout(dmx_source_data_t *source, 
                             uint32_t timeout_us) {
    if (!source->is_valid) {
        return true;
    }
    
    uint64_t now = esp_timer_get_time();
    uint64_t elapsed = now - source->timestamp;
    
    return (elapsed > timeout_us);
}

static void cleanup_timeout_sources(merge_context_t *ctx) {
    for (int i = 0; i < ctx->source_count; i++) {
        if (is_source_timeout(&ctx->sources[i], ctx->timeout_us)) {
            ctx->sources[i].is_valid = false;
        }
    }
}
```

---

## 6. Priority Handling

### 6.1. Art-Net Priority

Art-Net has no priority field in its packets. The priority is set by:
1. The protocol mode setting (ARTNET_PRIORITY)
2. The IP address (if the priorities are equal)

### 6.2. sACN Priority

sACN has a priority field (0-200) in its packets:
- 0: Lowest
- 100: Default
- 200: Highest

### 6.3. Protocol Priority

When merging Art-Net and sACN:
- ARTNET_PRIORITY: Art-Net wins
- SACN_PRIORITY: sACN wins
- MERGE_BOTH: merge by the sACN priority, Art-Net = 100

```c
static uint8_t get_source_priority(dmx_source_data_t *source, 
                                  protocol_mode_t proto_mode) {
    if (proto_mode == PROTOCOL_MODE_ARTNET_PRIORITY) {
        return (source->protocol == SOURCE_ARTNET) ? 200 : 100;
    } else if (proto_mode == PROTOCOL_MODE_SACN_PRIORITY) {
        return (source->protocol == SOURCE_SACN) ? 200 : 100;
    } else {
        // MERGE_BOTH - use actual priority
        if (source->protocol == SOURCE_SACN) {
            return source->priority;
        } else {
            return 100;  // Art-Net default
        }
    }
}
```

---

## 7. Threading Model

- **Merge Task** (Priority: 9, Core: 1, Stack: 4KB) × 2 ports
  - Runs continuously at 50 Hz
  - Merges the data from the sources
  - Pushes the output to the DMX Handler
  - Cleanup timeout sources

```c
static void merge_task(void *pvParameters) {
    uint8_t port = (uint8_t)pvParameters;
    merge_context_t *ctx = &merge_contexts[port - 1];
    
    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(20);  // 50 Hz
    
    while (1) {
        // Cleanup timeout sources
        cleanup_timeout_sources(ctx);
        
        // Perform merge based on mode
        switch (ctx->mode) {
            case MERGE_MODE_HTP:
                merge_htp(ctx);
                break;
            case MERGE_MODE_LTP:
                merge_ltp(ctx);
                break;
            case MERGE_MODE_LAST:
                merge_last(ctx);
                break;
            case MERGE_MODE_BACKUP:
                merge_backup(ctx);
                break;
            case MERGE_MODE_DISABLE:
                merge_disable(ctx);
                break;
        }
        
        // Send output to DMX handler
        if (ctx->output_active) {
            dmx_send_frame(port, ctx->merged_data);
        } else {
            dmx_blackout(port);
        }
        
        ctx->last_merge_time = esp_timer_get_time();
        
        // Wait for next period
        vTaskDelayUntil(&last_wake, period);
    }
}
```

---

## 8. Performance Optimization

### 8.1. Lock-free Input

- Double buffering for input data
- Input functions do not block the merge task

### 8.2. SIMD Operations

- Could use ESP32-S3 SIMD instructions for HTP/LTP
- Optimize memcpy with DMA

---

## 9. Error Handling

| Error | Action |
|-----|-----------|
| Invalid data pointer | Return error, log warning |
| Source buffer full | Replace oldest timeout source |
| Merge task crash | Watchdog reset |

---

## 10. Dependencies

- **ESP-IDF Components**:
  - freertos/task
  - esp_timer
- **Internal Modules**:
  - DMX Handler (output)
  - Configuration (merge mode)

---

## 11. Testing Points

1. HTP merge - 2 sources
2. HTP merge - 4 sources
3. LTP merge
4. LAST merge - timing
5. BACKUP merge - failover
6. Source timeout detection
7. Source replacement when buffer full
8. Priority handling (sACN)
9. Protocol priority (Art-Net vs sACN)
10. Blackout on all timeout
11. Performance - merge latency
12. Sequence number handling

---

## 12. Memory Usage

- Merge context: 3 KB × 2 ports = 6 KB
- Task stacks: 4 KB × 2 = 8 KB
- **Total: ~14 KB**

---

## 13. Performance

- Merge rate: 50 Hz
- Merge latency: < 1ms
- Max sources: 4 per universe
- CPU usage: ~2-5% (depending on mode)

---

## 14. Configuration Example

```json
{
  "merge_mode_port1": "HTP",
  "merge_mode_port2": "BACKUP",
  "merge_timeout_seconds": 3,
  "protocol_mode_port1": "MERGE_BOTH",
  "protocol_mode_port2": "ARTNET_PRIORITY"
}
```
