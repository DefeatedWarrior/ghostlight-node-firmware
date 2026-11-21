/**
 * ESP-NODE — Art-Net 4 / sACN (E1.31) to DMX512/RDM node: start-up and the
 * main task. See app.h for how the application is split across main/.
 *
 * Configuration changes from the web UI or Art-Net ArtAddress are applied
 * live by the main task (only network settings need a restart).
 */
#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
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
#include "diag.h"
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
}

// ============================================================================
// app_main
// ============================================================================

void app_main(void)
{
    diag_init();   /* first: keeps the log for the web UI, notes the reset reason */
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

    ESP_ERROR_CHECK(artnet_transmitter_init());
#if CONFIG_NODE_SACN
    ESP_ERROR_CHECK(sacn_transmitter_init(config->node_info.short_name));
#endif

    network_set_hostname(hostname);
    network_register_state_callback(app_network_state_changed, NULL);
    ESP_ERROR_CHECK(network_start_with_fallback());
#if CONFIG_NODE_MDNS
    start_mdns(config, hostname);
#endif

    ESP_ERROR_CHECK(artnet_receiver_init());
    ESP_ERROR_CHECK(artnet_receiver_set_callback(app_on_artnet_dmx, NULL));
    ESP_ERROR_CHECK(artnet_receiver_set_status_callback(app_artnet_status, NULL));
    ESP_ERROR_CHECK(artnet_receiver_set_address_callback(app_artnet_address, NULL));
    ESP_ERROR_CHECK(artnet_receiver_set_sync_callback(app_on_artnet_sync, NULL));
    ESP_ERROR_CHECK(artnet_receiver_set_ipprog_callback(app_on_artnet_ipprog, NULL));
    ESP_ERROR_CHECK(artnet_receiver_start());
    /* ArtDmx we send uses UDP source port 6454, as the spec requires */
    artnet_transmitter_use_socket(artnet_receiver_get_socket());

#if CONFIG_NODE_SACN
    ESP_ERROR_CHECK(sacn_receiver_init());
    sacn_receiver_set_local_cid(sacn_transmitter_get_cid());
    ESP_ERROR_CHECK(sacn_receiver_set_callback(app_on_sacn_dmx, NULL));
    ESP_ERROR_CHECK(sacn_receiver_set_sync_callback(app_on_sacn_sync, NULL));
    ESP_ERROR_CHECK(sacn_receiver_start());
#endif

    /* DMX ports, merge settings and sACN subscriptions */
    app_apply_config();
#if CONFIG_NODE_SCENES
    app_start_power_on_scenes();
#endif

#if CONFIG_NODE_RDM
    /* RDM over Art-Net: discovers RDM ports once they are running */
    if (rdm_bridge_start() != ESP_OK) {
        ESP_LOGE(TAG, "RDM bridge failed to start");
    }
#endif

    config_register_change_callback(app_config_changed, NULL);
    static const web_server_hooks_t hooks = {
        .identify = app_web_identify,
        .identify_left = app_web_identify_left,
    };
    web_server_set_hooks(&hooks);
    ESP_ERROR_CHECK(web_server_init(NULL));
    ESP_ERROR_CHECK(web_server_start());

    app_indicator_start_tasks();   /* reset button, port LEDs */

    artnet_receiver_set_node_report(ARTNET_RC_POWER_OK, "Power On Tests successful");
    ESP_LOGI(TAG, "System initialized");

    bool ota_marked = false;
    bool identify_was_on = false;
    int64_t last_stats = esp_timer_get_time();

    for (;;) {
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

        int64_t now = esp_timer_get_time();
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
