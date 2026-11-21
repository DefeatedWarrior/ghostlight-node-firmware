# MODULE DESIGN: LED Manager

**Project:** Artnet-Node-2RDM  
**Module:** LED Status Manager  
**Version:** 1.0  
**Created:** 25/12/2025

---

## 1. Purpose

The LED Manager module drives the WS2812 LED to show the device's operating state:
- Boot state
- Network state (Ethernet, WiFi STA, WiFi AP)
- DMX receive/send activity
- RDM discovery
- System errors

---

## 2. Hardware Interface

```c
#define WS2812_GPIO     GPIO_NUM_48
#define WS2812_COUNT    1            // number of LEDs
#define WS2812_RMT_CHANNEL  0        // RMT channel
```

**Driver:** ESP-IDF RMT (Remote Control) peripheral

---

## 3. Module architecture

### 3.1. Data structures

```c
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_color_t;

typedef enum {
    LED_STATE_BOOT = 0,
    LED_STATE_ETHERNET_CONNECTED,
    LED_STATE_WIFI_STA_CONNECTED,
    LED_STATE_WIFI_AP_ACTIVE,
    LED_STATE_DATA_RECEIVING,
    LED_STATE_RDM_DISCOVERY,
    LED_STATE_ERROR
} led_state_t;

typedef enum {
    LED_BEHAVIOR_STATIC = 0,        // steady
    LED_BEHAVIOR_SLOW_BLINK,        // slow blink 1Hz
    LED_BEHAVIOR_FAST_BLINK,        // fast blink 5Hz
    LED_BEHAVIOR_PULSE,             // one blink per event
    LED_BEHAVIOR_BREATH             // Breathing effect
} led_behavior_t;

typedef struct {
    led_state_t state;
    rgb_color_t color;
    led_behavior_t behavior;
    uint32_t duration_ms;           // display time (0 = forever)
    uint8_t priority;               // 0-255, lower = higher priority
} led_event_t;
```

---

## 4. API Public

### 4.1. Initialization

```c
/**
 * Initialize the LED Manager
 * @return ESP_OK on success
 */
esp_err_t led_manager_init(void);

/**
 * Stop the LED Manager
 * @return ESP_OK on success
 */
esp_err_t led_manager_deinit(void);
```

### 4.2. Set LED State

```c
/**
 * Set the LED state (uses a preset)
 * @param state LED state
 * @return ESP_OK on success
 */
esp_err_t led_manager_set_state(led_state_t state);

/**
 * Set the LED with a custom color and behavior
 * @param color RGB color
 * @param behavior behavior (static, blink, pulse, etc.)
 * @param duration_ms display time (0 = forever)
 * @param priority Priority (0 = highest)
 * @return ESP_OK on success
 */
esp_err_t led_manager_set_custom(rgb_color_t color,
                                 led_behavior_t behavior,
                                 uint32_t duration_ms,
                                 uint8_t priority);
```

### 4.3. Event-based Control

```c
/**
 * Trigger an LED pulse (one short blink)
 * Used for data packets, RDM responses, etc.
 * @param color pulse color
 * @return ESP_OK on success
 */
esp_err_t led_manager_pulse(rgb_color_t color);

/**
 * Trigger an error blink (fast red blink)
 * @param duration_ms blink time
 * @return ESP_OK on success
 */
esp_err_t led_manager_error_blink(uint32_t duration_ms);
```

### 4.4. Direct Control

```c
/**
 * Set the LED color directly (bypasses the state machine)
 * @param color RGB color
 * @return ESP_OK on success
 */
esp_err_t led_manager_set_color_direct(rgb_color_t color);

/**
 * Turn the LED off
 * @return ESP_OK on success
 */
esp_err_t led_manager_off(void);
```

---

## 5. Preset LED States

| State | RGB color | Behavior | Description |
|-------|---------|----------|-------|
| BOOT | (0, 50, 100) | Static | Light blue |
| ETHERNET_CONNECTED | (0, 255, 0) | Static | Steady green |
| WIFI_STA_CONNECTED | (0, 200, 0) | Slow Blink | Slow green blink |
| WIFI_AP_ACTIVE | (128, 0, 128) | Static | Steady purple |
| DATA_RECEIVING | (255, 255, 255) | Pulse | One white blink |
| RDM_DISCOVERY | (255, 200, 0) | Slow Blink | Slow yellow blink |
| ERROR | (255, 0, 0) | Fast Blink | Fast red blink |

---

## 6. Operation flow

### 6.1. State Machine

```
1. Boot:
   └─> LED = BOOT (blue)
   
2. Network connecting:
   └─> LED stays BOOT
   
3. Ethernet connected:
   └─> LED = ETHERNET_CONNECTED (steady green)
   
4. WiFi STA connected:
   └─> LED = WIFI_STA_CONNECTED (blinking green)
   
5. WiFi AP active:
   └─> LED = WIFI_AP_ACTIVE (purple)
   
6. Art-Net/sACN packet received:
   └─> 50ms white LED pulse (max 10Hz so it doesn't blink too fast)
   └─> Return to network state
   
7. RDM discovery:
   └─> LED = RDM_DISCOVERY (blinking yellow) for the whole discovery
   └─> Return to network state khi xong
   
8. Error:
   └─> LED = ERROR (fast red blink)
   └─> Auto return to network state sau 5s
```

### 6.2. Priority System

- LED states have a priority
- An event with a higher priority (lower number) overrides the current event
- An event with duration > 0 expires by itself and returns to the previous state

**Priority levels:**
- 0: Error (highest)
- 1: RDM Discovery
- 2: Data Receiving (pulse)
- 3: Network states
- 4: Boot

---

## 7. Threading Model

- **LED Task** (Priority: 3, Core: 1):
  - Runs continuously
  - Process LED state queue
  - Update WS2812 via RMT
  - Handle blinking/breathing timing

- **Queue**:
  - Size: 10 events
  - Timeout: 100ms

---

## 8. Timing Parameters

```c
#define LED_PULSE_DURATION_MS       50      // Pulse duration
#define LED_SLOW_BLINK_PERIOD_MS    1000    // 1Hz
#define LED_FAST_BLINK_PERIOD_MS    200     // 5Hz
#define LED_BREATH_PERIOD_MS        2000    // 0.5Hz
#define LED_DATA_PULSE_MAX_RATE_HZ  10      // Max pulse rate
```

---

## 9. Implementation Details

### 9.1. WS2812 Driver

```c
/**
 * Initialize RMT for WS2812
 */
static esp_err_t ws2812_init(void);

/**
 * Set WS2812 color
 * @param r Red (0-255)
 * @param g Green (0-255)
 * @param b Blue (0-255)
 */
static void ws2812_set_color(uint8_t r, uint8_t g, uint8_t b);

/**
 * WS2812 timing (T0H, T0L, T1H, T1L)
 * Based on WS2812 datasheet
 */
#define WS2812_T0H_NS   350
#define WS2812_T0L_NS   900
#define WS2812_T1H_NS   900
#define WS2812_T1L_NS   350
#define WS2812_RESET_US 50
```

### 9.2. Blink Implementation

```c
static void led_task(void *pvParameters) {
    led_event_t current_event;
    uint32_t blink_state = 0;
    uint32_t last_update = 0;
    
    while (1) {
        // Check for new events
        if (xQueueReceive(led_queue, &current_event, pdMS_TO_TICKS(10))) {
            // Process new event
        }
        
        // Update LED based on behavior
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        
        switch (current_event.behavior) {
            case LED_BEHAVIOR_STATIC:
                // Just set color once
                break;
                
            case LED_BEHAVIOR_SLOW_BLINK:
            case LED_BEHAVIOR_FAST_BLINK:
                // Toggle on/off based on period
                if (now - last_update > period / 2) {
                    blink_state = !blink_state;
                    ws2812_set_color(blink_state ? r : 0, 
                                     blink_state ? g : 0, 
                                     blink_state ? b : 0);
                    last_update = now;
                }
                break;
                
            case LED_BEHAVIOR_PULSE:
                // One-shot pulse
                ws2812_set_color(r, g, b);
                vTaskDelay(pdMS_TO_TICKS(duration));
                // Return to previous state
                break;
                
            case LED_BEHAVIOR_BREATH:
                // Sinusoidal brightness
                float brightness = (sin(2 * PI * now / period) + 1) / 2;
                ws2812_set_color(r * brightness, g * brightness, b * brightness);
                break;
        }
        
        // Check for expiration
        if (current_event.duration_ms > 0) {
            if (now - event_start_time > current_event.duration_ms) {
                // Restore previous state
            }
        }
    }
}
```

---

## 10. Rate Limiting

- Data pulse events are rate-limited so the LED doesn't blink too fast
- Max rate: 10 Hz (100ms between pulses)
- A timestamp tracks the last pulse

```c
static uint32_t last_data_pulse_time = 0;

esp_err_t led_manager_pulse(rgb_color_t color) {
    uint32_t now = esp_timer_get_time() / 1000; // ms
    
    if (now - last_data_pulse_time < 100) {
        // Skip pulse - too soon
        return ESP_OK;
    }
    
    last_data_pulse_time = now;
    // Trigger pulse
    return ESP_OK;
}
```

---

## 11. Error Handling

| Error | Action |
|-----|-----------|
| RMT init fail | Log error, LED disabled |
| WS2812 not responding | Log warning, continue |
| Queue full | Drop oldest event |

---

## 12. Dependencies

- **ESP-IDF Components**:
  - driver/rmt
  - freertos/task
  - freertos/queue

---

## 13. Testing Points

1. Boot LED color
2. Ethernet connected state
3. WiFi STA connected state  
4. WiFi AP state
5. Data pulse on packet receive
6. Data pulse rate limiting
7. RDM discovery blink
8. Error blink
9. Priority override
10. Timed event expiration
11. Blink frequency accuracy
12. Color accuracy

---

## 14. Memory Usage

- Heap: ~2 KB
- Stack (task): 2 KB
- Queue: 10 events × 24 bytes = 240 bytes

---

## 15. Performance

- LED update rate: up to 100 Hz
- Latency: < 10ms from event to LED update
- CPU usage: < 1%

---

## 16. Configuration

```c
// LED brightness (0-255)
#define LED_BRIGHTNESS_BOOT       80
#define LED_BRIGHTNESS_NORMAL     255
#define LED_BRIGHTNESS_ERROR      255
```

Brightness can be adjusted to dim the LED if needed.
