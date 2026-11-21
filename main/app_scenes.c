/*
 * Power-on scenes (shown until a console takes over a port) and the
 * ArtAddress FailRecord command (store the output as failsafe scenes).
 * Runs on the main task.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "merge_engine.h"
#include "scene_manager.h"
#include "app.h"

static const char *TAG = "main";

#define STARTUP_RELEASE_FADE_MS 1000   /* power-on scene fades out when a console takes over */

/* Each output port with a power-on scene shows it until a console sends
 * data to that port; it then fades out (STARTUP_RELEASE_FADE_MS) */
void app_start_power_on_scenes(void)
{
    config_t *c = config_get();
    for (int p = 1; p <= NPORTS; p++) {
        const port_config_t *pc = config_port(c, p);
        if (!pc->startup_scene || !app_mode_is_output(pc->mode)) continue;
        if (scene_playback_on(pc->startup_scene, p, 0) == ESP_OK) {
            g_app.startup_mask |= 1u << (p - 1);
            ESP_LOGI(TAG, "Port %d: power-on scene %u until a console takes over",
                     p, pc->startup_scene);
        } else {
            ESP_LOGW(TAG, "Port %d: power-on scene %u not found", p, pc->startup_scene);
        }
    }
}

void app_release_power_on_scenes(void)
{
    config_t *c = config_get();
    for (int p = 1; p <= NPORTS; p++) {
        uint8_t bit = 1u << (p - 1);
        if (!(g_app.startup_mask & bit)) continue;
        merge_source_info_t src[MERGE_MAX_SOURCES];
        if (merge_engine_get_sources(p, src, MERGE_MAX_SOURCES) == 0) continue;
        g_app.startup_mask &= (uint8_t)~bit;
        /* Only if the power-on scene is still what the port shows */
        if (scene_active(p) == config_port(c, p)->startup_scene) {
            ESP_LOGI(TAG, "Port %d: console took over, power-on scene released", p);
            scene_release(p, STARTUP_RELEASE_FADE_MS);
        }
    }
}

/* ArtAddress FailRecord: store what each output port shows now as its
 * failsafe scene. A scene this created before ("Failsafe Pn") is replaced;
 * a scene the user picked is never overwritten. */
void app_record_failsafe_scenes(void)
{
    config_t *c = config_get();
    dmx_scene_t *sc = malloc(sizeof(dmx_scene_t));
    bool changed = false;
    for (int p = 1; p <= NPORTS; p++) {
        port_config_t *pc = config_port(c, p);
        if (!app_mode_is_output(pc->mode)) continue;
        char name[SCENE_NAME_MAX_LEN];
        snprintf(name, sizeof(name), "Failsafe P%d", p);
        if (pc->failsafe_scene && sc && scene_get(pc->failsafe_scene, sc) == ESP_OK &&
            strcmp(sc->name, name) == 0) {
            scene_delete(pc->failsafe_scene);
        }
        uint32_t id = 0;
        if (scene_record(p, name, &id) == ESP_OK) {
            pc->failsafe_scene = (uint8_t)id;
            changed = true;
            ESP_LOGI(TAG, "Port %d: failsafe scene recorded as scene %" PRIu32, p, id);
        } else {
            artnet_receiver_set_node_report(ARTNET_RC_CONFIG_ERR, "Failsafe record failed");
        }
    }
    free(sc);
    if (changed) {
        config_save();
        app_apply_config();
    }
}
