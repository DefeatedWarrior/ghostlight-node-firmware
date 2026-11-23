/**
 * ESP-NODE — Art-Net 4 / sACN (E1.31) to DMX512/RDM node: start-up and the
 * main task. See app.h for how the application is split across main/.
 *
 * Configuration changes from the web UI or Art-Net ArtAddress are applied
 * live by the main task (only network settings need a restart).
 */
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "storage_manager.h"
#include "led_manager.h"
#include "artnet_transmitter.h"
#include "sacn_transmitter.h"
#include "merge_engine.h"
#include "web_server.h"
#include "scene_manager.h"
#include "rdm_bridge.h"
#include "dmx_tools.h"
#include "dmx_tester.h"
#include "serial_setup.h"
#include "diag.h"
#include "source_monitor.h"
#include "board.h"
#include "app.h"

static const char *TAG = "main";

#define OTA_VALID_AFTER_US    (30LL * 1000 * 1000)
#define STATS_INTERVAL_US     (30LL * 1000 * 1000)
#define IPPROG_APPLY_DELAY_MS 300      /* let ArtIpProgReply leave before the IP changes */

app_state_t g_app;

// ============================================================================
// Helpers
// ============================================================================

static void make_hostname(const char *name, char *out, size_t size)
{
    size_t j = 0;
    for (size_t i = 0; name[i] && j < size - 1; i++) {
        char ch = name[i];
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-') {
            out[j++] = ch;
        } else if (ch >= 'A' && ch <= 'Z') {
            out[j++] = ch - 'A' + 'a';
        } else if ((ch == ' ' || ch == '_') && j > 0 && out[j - 1] != '-') {
            out[j++] = '-';
        }
    }
    while (j > 0 && out[j - 1] == '-') j--;
    out[j] = '\0';
    if (j == 0) {
        snprintf(out, size, "esp-node");
    }
}

#if CONFIG_NODE_MDNS
static void start_mdns(const config_t *c, const char *hostname)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(hostname);
    mdns_instance_name_set(c->node_info.long_name);
    mdns_service_add(c->node_info.short_name, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: http://%s.local/", hostname);
}
#endif

static void log_stats(void)
{
    for (int p = 1; p <= NPORTS; p++) {
        dmx_port_status_t st;
        merge_stats_t ms;
        if (dmx_handler_get_port_status(p, &st) != ESP_OK || !st.is_active) continue;
        if (app_mode_is_output(st.mode) && merge_engine_get_stats(p, &ms) == ESP_OK) {
            ESP_LOGI(TAG, "Port %d out: %" PRIu32 " frames, %" PRIu32 " source(s), %s, "
                     "seq drops %" PRIu32 ", timeouts %" PRIu32,
                     p, st.stats.frames_sent, ms.active_sources,
                     merge_engine_is_output_active(p) ? "live" : "idle",
                     ms.sequence_drops, ms.source_timeouts);
        } else {
            ESP_LOGI(TAG, "Port %d in: %" PRIu32 " frames (%u slots), signal %s, errors %" PRIu32,
                     p, st.stats.frames_received, st.stats.last_slot_count,
                     st.stats.signal_present ? "yes" : "no", st.stats.error_count);
        }
    }
    artnet_stats_t as;
#if CONFIG_NODE_SACN
    sacn_stats_t ss;
    if (artnet_receiver_get_stats(&as) == ESP_OK && sacn_receiver_get_stats(&ss) == ESP_OK) {
        ESP_LOGI(TAG, "Art-Net rx %" PRIu32 " (dmx %" PRIu32 ", poll %" PRIu32 "), "
                 "sACN rx %" PRIu32 " (data %" PRIu32 ", %u universe(s)), heap %" PRIu32 " (min %" PRIu32 ")",
                 as.packets_received, as.dmx_packets, as.poll_packets,
                 ss.packets_received, ss.data_packets, sacn_receiver_get_subscription_count(),
                 esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    }
#else
    if (artnet_receiver_get_stats(&as) == ESP_OK) {
        ESP_LOGI(TAG, "Art-Net rx %" PRIu32 " (dmx %" PRIu32 ", poll %" PRIu32 "), "
                 "heap %" PRIu32 " (min %" PRIu32 ")",
                 as.packets_received, as.dmx_packets, as.poll_packets,
                 esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    }
#endif

    /* Headroom: internal RAM (drivers, Wi-Fi, lwIP need it; PSRAM can't
     * stand in) and the tightest task stack, with a warning when low */
    diag_health_t *h = malloc(sizeof(*h));
    if (h) {
        diag_health(h);
        const char *tight = h->stacks ? h->stack[0].name : "?";
        uint32_t tight_free = h->stacks ? h->stack[0].stack_free : 0;
        bool low = h->internal_min_free < DIAG_INTERNAL_LOW_BYTES ||
                   (h->stacks && tight_free < DIAG_STACK_LOW_BYTES);
        /* The lowest figures never go back up: warn while internal RAM is
         * low right now, and when a lowest figure first falls below its
         * limit or falls further, not every 30 s for something long past */
        static uint32_t warned_ram = UINT32_MAX, warned_stack = UINT32_MAX;
        bool ram_new_low = h->internal_min_free < DIAG_INTERNAL_LOW_BYTES &&
                           h->internal_min_free + 1024 <= warned_ram;
        bool stack_new_low = h->stacks && tight_free < DIAG_STACK_LOW_BYTES &&
                             tight_free < warned_stack;
        if (ram_new_low) warned_ram = h->internal_min_free;
        if (stack_new_low) warned_stack = tight_free;
        bool warn = h->internal_free < DIAG_INTERNAL_LOW_BYTES || ram_new_low || stack_new_low;
        esp_log_level_t level = warn ? ESP_LOG_WARN : ESP_LOG_INFO;
        ESP_LOG_LEVEL(level, TAG,
                      "Internal RAM %" PRIu32 " free (lowest %" PRIu32 ", largest block %" PRIu32 "), "
                      "tightest stack %s %" PRIu32 " bytes free%s",
                      h->internal_free, h->internal_min_free, h->internal_largest,
                      tight, tight_free, low ? " - LOW" : "");
        free(h);
    }
}

// ============================================================================
// app_main
// ============================================================================

void app_main(void)
{
    diag_init();   /* first: keeps the log for the web UI, notes the reset reason */
    app_boot_guard_begin();   /* after diag_init: needs the reset reason */
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "ESP-NODE-2RDM firmware %s (%s %s), ESP-IDF %s, board %s",
             app->version, app->date, app->time, esp_get_idf_version(), BOARD_NAME);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    g_app.events = xEventGroupCreate();
    g_app.apply_lock = xSemaphoreCreateMutex();
    if (!g_app.events || !g_app.apply_lock) {
        ESP_LOGE(TAG, "Out of memory");
        esp_restart();
    }
    for (int i = 0; i < NPORTS; i++) {
        g_app.route[i].mode = DMX_MODE_DISABLED;
        g_app.route[i].artnet_backup = -1;
    }

    ESP_ERROR_CHECK(storage_init());
    ESP_ERROR_CHECK(config_init());
    config_load();   /* falls back to defaults by itself */
    config_t *config = config_get();
    ESP_LOGI(TAG, "Node \"%s\"", config->node_info.short_name);
#if CONFIG_NODE_TESTER
    /* DMX tester mode is chosen once, here: the web UI saves it and restarts */
    g_app.tester = config->tester.enabled;
    if (dmx_tester_init(g_app.tester) != ESP_OK) {
        ESP_LOGE(TAG, "Not enough memory for DMX tester mode, starting as a node");
        g_app.tester = false;
        dmx_tester_init(false);
    }
    if (g_app.tester) {
        ESP_LOGW(TAG, "DMX TESTER MODE: Art-Net, sACN, RDM bridge and scenes are off");
    }
#endif

    ESP_ERROR_CHECK(led_manager_init());
    led_manager_set_state(LED_STATE_BOOT);

    /* Merge engine before the DMX ports: output tasks pull from it.
     * Scenes before the first apply (failsafe / power-on scenes). */
    ESP_ERROR_CHECK(merge_engine_init());
#if CONFIG_NODE_SCENES
    ESP_ERROR_CHECK(scene_manager_init());
#endif
    ESP_ERROR_CHECK(dmx_handler_init());
#if CONFIG_NODE_DMX_TOOLS
    dmx_selftest_set_restore_callback(app_selftest_restore, NULL);
#endif
    for (int p = 1; p <= NPORTS; p++) {
        dmx_handler_set_frame_source(p, app_dmx_frame_source, NULL);
        dmx_handler_register_rx_callback(p, app_on_dmx_input, NULL);
        dmx_handler_register_loss_callback(p, app_on_dmx_loss, NULL);
#if CONFIG_NODE_RDM
        dmx_handler_register_identify_callback(p, app_on_rdm_identify, NULL);
#endif
    }

    /* Network (asynchronous: Ethernet -> WiFi -> AP, supervised forever) */
    char hostname[32];
    make_hostname(config->node_info.short_name, hostname, sizeof(hostname));
    ESP_ERROR_CHECK(network_init());   /* starts lwIP: no socket() before this */

    if (!g_app.tester) {
        ESP_ERROR_CHECK(artnet_transmitter_init());
#if CONFIG_NODE_SACN
        ESP_ERROR_CHECK(sacn_transmitter_init(config->node_info.short_name));
#endif
    }

    network_set_hostname(hostname);
    network_register_state_callback(app_network_state_changed, NULL);
    ESP_ERROR_CHECK(network_start_with_fallback());
#if CONFIG_NODE_MDNS
    start_mdns(config, hostname);
#endif

    /* Tester mode is not a node: no Art-Net / sACN socket at all, so
     * nothing is received and controllers don't see it (no ArtPollReply) */
    if (!g_app.tester) {
        srcmon_reset();   /* before the receivers: they report every sender */
        ESP_ERROR_CHECK(artnet_receiver_init());
        ESP_ERROR_CHECK(artnet_receiver_set_callback(app_on_artnet_dmx, NULL));
        ESP_ERROR_CHECK(artnet_receiver_set_status_callback(app_artnet_status, NULL));
        ESP_ERROR_CHECK(artnet_receiver_set_address_callback(app_artnet_address, NULL));
        ESP_ERROR_CHECK(artnet_receiver_set_sync_callback(app_on_artnet_sync, NULL));
        ESP_ERROR_CHECK(artnet_receiver_set_poll_reply_callback(app_on_artnet_poll_reply, NULL));
        ESP_ERROR_CHECK(artnet_receiver_set_ipprog_callback(app_on_artnet_ipprog, NULL));
        ESP_ERROR_CHECK(artnet_receiver_start());
        /* ArtDmx we send uses UDP source port 6454, as the spec requires */
        artnet_transmitter_use_socket(artnet_receiver_get_socket());

#if CONFIG_NODE_SACN
        ESP_ERROR_CHECK(sacn_receiver_init());
        sacn_receiver_set_local_cid(sacn_transmitter_get_cid());
        ESP_ERROR_CHECK(sacn_receiver_set_callback(app_on_sacn_dmx, NULL));
        ESP_ERROR_CHECK(sacn_receiver_set_sync_callback(app_on_sacn_sync, NULL));
        ESP_ERROR_CHECK(sacn_receiver_set_discovery_callback(app_on_sacn_discovery, NULL));
        ESP_ERROR_CHECK(sacn_receiver_start());
#endif
    }

    /* DMX ports, merge settings and sACN subscriptions */
    app_apply_config();
    if (!g_app.tester) {
#if CONFIG_NODE_SCENES
        app_start_power_on_scenes();
#endif
#if CONFIG_NODE_RDM
        /* RDM over Art-Net: discovers RDM ports once they are running */
        if (rdm_bridge_start() != ESP_OK) {
            ESP_LOGE(TAG, "RDM bridge failed to start");
        }
#endif
    }

    config_register_change_callback(app_config_changed, NULL);
    static const web_server_hooks_t hooks = {
        .identify = app_web_identify,
        .identify_left = app_web_identify_left,
        .safe_mode = app_safe_mode_notice,
        .leave_safe_mode = app_leave_safe_mode,
    };
    web_server_set_hooks(&hooks);
    ESP_ERROR_CHECK(web_server_init(NULL));
    ESP_ERROR_CHECK(web_server_start());
#if CONFIG_NODE_SERIAL_SETUP
    serial_setup_start();   /* settings over USB (browser flasher) */
#endif

    app_indicator_start_tasks();   /* reset button, port LEDs */

    if (g_app.safe_mode) {
        artnet_receiver_set_node_report(ARTNET_RC_DMX_ERROR,
                                        "Safe mode after repeated crashes: DMX ports off");
    } else {
        artnet_receiver_set_node_report(ARTNET_RC_POWER_OK, "Power On Tests successful");
    }
    ESP_LOGI(TAG, "System initialized");

    bool ota_marked = false;
    bool identify_was_on = false;
    int64_t last_stats = esp_timer_get_time();

    /* Task watchdog: the loop wakes every second; saving, applying settings
     * and restarting ports take well under the 10 s limit */
    esp_task_wdt_add(NULL);
    for (;;) {
        esp_task_wdt_reset();
        EventBits_t bits = xEventGroupWaitBits(g_app.events,
                                               EV_APPLY | EV_SAVE | EV_NET | EV_FAILREC |
                                               EV_TAKEOVER | EV_NETCFG,
                                               pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
        if (bits & EV_SAVE) {
            config_save();
        }
        if (bits & EV_APPLY) {
            app_apply_config();
        }
        if (bits & EV_NET) {
            app_on_network_up();
        }
#if CONFIG_NODE_SCENES
        if (bits & EV_FAILREC) {
            app_record_failsafe_scenes();
        }
        if ((bits & EV_TAKEOVER) || g_app.startup_mask) {
            app_release_power_on_scenes();
        }
#endif
        if (bits & EV_NETCFG) {
            vTaskDelay(pdMS_TO_TICKS(IPPROG_APPLY_DELAY_MS));
            network_supervisor_reconfigure();
        }
        /* Web identify ran out: back to the normal LED */
        bool ident_on = g_app.web_identify_until != 0 && app_web_identify_left() > 0;
        if (identify_was_on && !ident_on) {
            g_app.web_identify_until = 0;
            app_update_indicator();
        }
        identify_was_on = ident_on;

#if CONFIG_NODE_SACN
        sacn_transmitter_tick();
        app_update_sacn_groups();   /* follow sync universes as sources come and go */
#endif
        /* Input ports sending to "the nodes that use the universe": keep the
         * node list fresh (ArtPoll, at most every ARTNET_SELF_POLL_MS) */
        for (int p = 0; p < NPORTS; p++) {
            if (!g_app.tester && app_mode_is_input(g_app.route[p].mode) &&
                g_app.route[p].send_to == SEND_TO_NODES) {
                artnet_receiver_poll_network();
                break;
            }
        }

        int64_t now = esp_timer_get_time();
        app_boot_guard_tick();
        if (!ota_marked && now > OTA_VALID_AFTER_US) {
            /* Healthy for 30 s: keep this firmware (no-op unless it was
             * just installed by OTA with rollback enabled) */
            esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Firmware marked valid");
            }
            ota_marked = true;
        }
        if (now - last_stats > STATS_INTERVAL_US) {
            last_stats = now;
            log_stats();
        }
    }
}
