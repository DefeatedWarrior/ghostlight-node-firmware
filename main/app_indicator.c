/*
 * Indicators: the status LED (network state, Art-Net locate / mute, RDM and
 * web identify), one LED per port where the board has them, and the factory
 * reset button.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "led_manager.h"
#include "merge_engine.h"
#include "dmx_tools.h"
#include "board.h"
#include "app.h"

static const char *TAG = "main";

/* Reset button (board.h, BOOT = GPIO0 on ESP-NODE-2RDM, active low). Hold
 * 10 s to factory reset (clears the admin password and the AP password
 * too). -1 = no button. */
#define RESET_BUTTON_GPIO     BOARD_RESET_BUTTON_GPIO
#define RESET_WARN_MS         3000
#define RESET_HOLD_MS         10000

// ============================================================================
// LED indicator (Art-Net locate/mute, RDM identify)
// ============================================================================

static void network_led(network_state_t state)
{
#if CONFIG_NODE_TESTER
    if (g_app.tester) {
        /* DMX tester: amber, blinking until the web UI can be reached */
        bool up = state == NETWORK_ETHERNET_CONNECTED || state == NETWORK_WIFI_STA_CONNECTED ||
                  state == NETWORK_WIFI_AP_ACTIVE;
        led_manager_set_custom((rgb_color_t){255, 110, 0},
                               up ? LED_BEHAVIOR_STATIC : LED_BEHAVIOR_SLOW_BLINK, 0, 3);
        return;
    }
#endif
    switch (state) {
        case NETWORK_ETHERNET_CONNECTED: led_manager_set_state(LED_STATE_ETHERNET_OK); break;
        case NETWORK_WIFI_STA_CONNECTED: led_manager_set_state(LED_STATE_WIFI_STA_OK); break;
        case NETWORK_WIFI_AP_ACTIVE:     led_manager_set_state(LED_STATE_WIFI_AP); break;
        case NETWORK_ERROR:              led_manager_set_state(LED_STATE_ERROR); break;
        default:                         led_manager_set_state(LED_STATE_BOOT); break;
    }
}

static bool identify_any(void)
{
    if (g_app.indicator == 1) return true;
    if (g_app.web_identify_until && esp_timer_get_time() < g_app.web_identify_until) return true;
    for (int p = 0; p < NPORTS; p++) {
        if (g_app.identify[p]) return true;
    }
    return false;
}

void app_update_indicator(void)
{
    bool locate = identify_any();
    if (locate) {
        led_manager_set_custom((rgb_color_t){255, 255, 255}, LED_BEHAVIOR_FAST_BLINK, 0, 1);
    } else if (g_app.indicator == 2) {
        led_manager_set_custom((rgb_color_t){0, 0, 0}, LED_BEHAVIOR_STATIC, 0, 1);
    } else {
        led_manager_clear();
        network_status_t st;
        memset(&st, 0, sizeof(st));
        network_get_status(&st);
        network_led(st.state);
    }
}

void app_network_state_changed(network_state_t state, void *user_data)
{
    ESP_LOGI(TAG, "Network state: %d", state);
    if (g_app.indicator == 0 && !identify_any()) {
        network_led(state);
    }
    if (state == NETWORK_ETHERNET_CONNECTED || state == NETWORK_WIFI_STA_CONNECTED ||
        state == NETWORK_WIFI_AP_ACTIVE) {
        /* Handled on the main task: re-join multicast, announce, retarget tx */
        xEventGroupSetBits(g_app.events, EV_NET);
    }
}

void app_on_rdm_identify(uint8_t port, bool identify, void *user_data)
{
    g_app.identify[port - 1] = identify;
    app_update_indicator();
}

// ============================================================================
// Identify (web) and per-port LEDs
// ============================================================================

void app_web_identify(uint32_t seconds)
{
    g_app.web_identify_until = seconds ? esp_timer_get_time() + (int64_t)seconds * 1000000 : 0;
    ESP_LOGI(TAG, "Identify %s", seconds ? "on" : "off");
    app_update_indicator();
}

uint32_t app_web_identify_left(void)
{
    int64_t left = g_app.web_identify_until - esp_timer_get_time();
    return (g_app.web_identify_until && left > 0) ? (uint32_t)((left + 999999) / 1000000) : 0;
}

static const int8_t k_port_led[NPORTS] = BOARD_PORT_LED_GPIO;

static bool board_has_port_leds(void)
{
    for (int p = 0; p < NPORTS; p++) {
        if (k_port_led[p] >= 0) return true;
    }
    return false;
}

/* One LED per port: off = disabled, slow blink = enabled but no data,
 * on = data (output: a source drives it; input: DMX received), fast blink
 * = test pattern / identify */
static void port_led_task(void *arg)
{
    (void)arg;
    for (int p = 0; p < NPORTS; p++) {
        if (k_port_led[p] < 0) continue;
        gpio_reset_pin(k_port_led[p]);
        gpio_set_direction(k_port_led[p], GPIO_MODE_OUTPUT);
        gpio_set_level(k_port_led[p], 0);
    }
    for (uint32_t tick = 0;; tick++) {
        vTaskDelay(pdMS_TO_TICKS(100));
#if CONFIG_NODE_DMX_TOOLS
        uint8_t tests = dmx_test_active_mask();
#else
        uint8_t tests = 0;
#endif
        bool ident = identify_any();
        for (int p = 0; p < NPORTS; p++) {
            if (k_port_led[p] < 0) continue;
            dmx_port_status_t ds;
            bool on = false;
            if (dmx_handler_get_port_status(p + 1, &ds) == ESP_OK && ds.is_active) {
                bool data = app_mode_is_input(ds.mode) ? ds.stats.signal_present
                                                   : (g_app.tester || merge_engine_is_output_active(p + 1));
                if (ident || g_app.identify[p] || (tests & (1u << p))) {
                    on = tick & 1;                 /* 5 Hz */
                } else {
                    on = data || ((tick / 5) & 1); /* solid, or 1 Hz */
                }
            }
            gpio_set_level(k_port_led[p], on);
        }
    }
}

// ============================================================================
// Factory reset button
// ============================================================================

#if RESET_BUTTON_GPIO >= 0
static void reset_button_task(void *arg)
{
    (void)arg;
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << RESET_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    if (gpio_config(&io) != ESP_OK) {
        vTaskDelete(NULL);
        return;
    }
    /* A pin that is already low (button held at boot, or nothing fitted and
     * the line pulled down) must go high once before a press counts, so a
     * fault can't cause a reset loop. */
    bool armed = false;
    int held_ms = 0;
    bool warned = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        bool pressed = gpio_get_level(RESET_BUTTON_GPIO) == 0;
        if (!armed) {
            armed = !pressed;
            continue;
        }
        if (!pressed) {
            if (warned) {
                ESP_LOGI(TAG, "Factory reset cancelled");
                led_manager_clear();
                app_update_indicator();
            }
            held_ms = 0;
            warned = false;
            continue;
        }
        held_ms += 50;
        if (!warned && held_ms >= RESET_WARN_MS) {
            warned = true;
            ESP_LOGW(TAG, "Keep holding BOOT for %d s to factory reset",
                     (RESET_HOLD_MS - RESET_WARN_MS) / 1000);
            led_manager_set_custom((rgb_color_t){255, 0, 0}, LED_BEHAVIOR_FAST_BLINK, 0, 0);
        }
        if (held_ms >= RESET_HOLD_MS) {
            ESP_LOGW(TAG, "Factory reset from BOOT button");
            led_manager_set_custom((rgb_color_t){255, 0, 0}, LED_BEHAVIOR_STATIC, 0, 0);
            config_reset_to_defaults();
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
    }
}
#endif

void app_indicator_start_tasks(void)
{
#if RESET_BUTTON_GPIO >= 0
    xTaskCreate(reset_button_task, "reset_btn", 3072, NULL, 2, NULL);
#endif
    if (board_has_port_leds()) {
        xTaskCreate(port_led_task, "port_led", 2560, NULL, 2, NULL);
    }
}
