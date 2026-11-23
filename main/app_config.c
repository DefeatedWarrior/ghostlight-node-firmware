/*
 * Applies the settings (config_get()) to the running system: merge engine,
 * DMX ports, the routing table read by the receive callbacks, and the sACN
 * multicast groups. Runs on the main task (EV_APPLY) under g_app.apply_lock.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "sacn_transmitter.h"
#include "merge_engine.h"
#include "scene_manager.h"
#include "rdm_bridge.h"
#include "dmx_tools.h"
#include "port_rules.h"
#include "app.h"

static const char *TAG = "main";

static const char *dmx_mode_to_string(dmx_mode_t mode)
{
    switch (mode) {
        case DMX_MODE_DISABLED:      return "Disabled";
        case DMX_MODE_OUTPUT:        return "DMX Output";
        case DMX_MODE_INPUT:         return "DMX Input";
        case DMX_MODE_RDM_MASTER:    return "RDM Master";
        case DMX_MODE_RDM_RESPONDER: return "RDM Responder";
        default:                     return "Unknown";
    }
}

void app_config_changed(void *user_data)
{
    xEventGroupSetBits(g_app.events, EV_APPLY);
}

/* Routing for output port @p p: which ports' DMX input it takes (see
 * app_on_dmx_input). An input it no longer takes is dropped from the merge
 * now instead of when it times out. */
static void set_routing(int p, route_t *r, const port_config_t *pc)
{
    uint8_t mask = (!g_app.tester && app_mode_is_output(pc->mode)) ? pc->local_inputs : 0;
    bool only = mask && pc->local_only;
    uint8_t gone = r->local_mask & (uint8_t)~mask;
    for (int k = 1; k <= NPORTS; k++) {
        if (!(gone & (1u << (k - 1)))) continue;
        static const uint8_t none[1];
        merge_input_t in = {
            .protocol = SOURCE_PROTOCOL_DMX_IN,
            .source_ip = (uint32_t)k,
            .data = none,
            .terminated = true,
        };
        merge_engine_push(p, &in);
    }
    if (mask != r->local_mask || only != r->local_only) {
        char list[4 * NPORTS + 1] = "";
        size_t o = 0;
        for (int k = 1; k <= NPORTS && o < sizeof(list); k++) {
            if (mask & (1u << (k - 1))) o += snprintf(list + o, sizeof(list) - o, "%s%d", o ? "," : "", k);
        }
        if (mask) {
            ESP_LOGI(TAG, "Port %d: routing %sDMX input of port %s", p, only ? "" : "network + ", list);
        } else if (r->local_mask) {
            ESP_LOGI(TAG, "Port %d: routing network only", p);
        }
    }
    r->local_only = only;
    r->splitter = only && (mask & (mask - 1)) == 0;
    r->local_mask = mask;

    /* Input ports: where to send */
    uint8_t n = pc->send_to == SEND_TO_LIST ? pc->send_count : 0;
    bool moved = pc->send_to != r->send_to || n != r->send_count;
    for (uint8_t i = 0; i < n; i++) moved |= pc->send_list[i] != r->send_list[i];
#if CONFIG_NODE_SACN
    if (moved && r->tx_sacn_universe) {
        /* end the stream where it went; the next frame starts the new one */
        sacn_transmitter_terminate(r->tx_sacn_universe);
        r->tx_sacn_universe = 0;
    }
#else
    (void)moved;
#endif
    for (uint8_t i = 0; i < CONFIG_SEND_LIST_MAX; i++) {
        r->send_list[i] = i < n ? pc->send_list[i] : 0;
    }
    r->send_count = n;
    if (pc->send_to != r->send_to && app_mode_is_input(pc->mode)) {
        ESP_LOGI(TAG, "Port %d: sending to %s", p,
                 pc->send_to == SEND_TO_LIST ? "the address list (unicast)" :
                 pc->send_to == SEND_TO_NODES ? "the Art-Net nodes that use the universe" :
                 "everyone (broadcast / multicast)");
    }
    r->node_count = 0;
    r->nodes_checked_us = 0;   /* look the nodes up again */
    r->send_to = pc->send_to;
}

/* Failsafe look of a port: its own loss behaviour or the node's.
 * LOSS_SCENE plays the port's failsafe scene (zero if it has none or it
 * was deleted) */
static void apply_loss_behavior(int p, const config_t *c, const port_config_t *pc)
{
    loss_behavior_t loss = pc->loss_behavior >= 0 ? (loss_behavior_t)pc->loss_behavior
                                                  : c->merge.loss_behavior;
#if CONFIG_NODE_SCENES
    if (loss != LOSS_SCENE) {
        merge_engine_set_loss_behavior(p, loss, NULL);
        return;
    }
    dmx_scene_t *sc = pc->failsafe_scene ? malloc(sizeof(dmx_scene_t)) : NULL;
    bool have = sc && scene_get(pc->failsafe_scene, sc) == ESP_OK;
    if (pc->failsafe_scene && !have) {
        ESP_LOGW(TAG, "Port %d: failsafe scene %u not found, failsafe output is zero",
                 p, pc->failsafe_scene);
    }
    merge_engine_set_loss_behavior(p, LOSS_SCENE, have ? sc->dmx_data : NULL);
    free(sc);
#else
    (void)pc;   /* config_sanitize() never leaves LOSS_SCENE without scenes */
    merge_engine_set_loss_behavior(p, loss, NULL);
#endif
}

void app_apply_config(void)
{
    xSemaphoreTake(g_app.apply_lock, portMAX_DELAY);
    /* A copy: restarting a port can take seconds, and the web UI or Art-Net
     * may change the settings meanwhile (they ask for another apply) */
    config_t *snap = config_snapshot();
    config_t *c = snap ? snap : config_get();   /* out of memory: the live ones */
    /* The self-test owns the port hardware until it finishes (it asks
     * for a full re-apply afterwards) */
    bool hw_locked = app_selftest_running();

    for (int p = 1; p <= NPORTS; p++) {
        const port_config_t *pc = config_port(c, p);
#if CONFIG_NODE_TESTER
        port_config_t tester_pc;
        if (g_app.tester) {
            /* tester mode: the port's job and timing come from c->tester */
            config_tester_port_settings(c, p, &tester_pc);
            pc = &tester_pc;
        }
#endif
        port_config_t safe_pc;
        if (g_app.safe_mode) {
            /* boot-loop guard: the DMX hardware stays off (app_boot_guard.c) */
            safe_pc = *pc;
            safe_pc.mode = DMX_MODE_DISABLED;
            pc = &safe_pc;
        }
        route_t *r = &g_app.route[p - 1];
        const port_config_t *old = &g_app.applied[p - 1];
        bool hw_changed = !g_app.applied_valid || pc->mode != old->mode ||
                          pc->rdm_enabled != old->rdm_enabled;

        merge_engine_config(p, pc->merge_mode, (uint32_t)c->merge.timeout_seconds * 1000);
        merge_engine_set_protocol_mode(p, pc->protocol_mode);
        apply_loss_behavior(p, c, pc);
        /* patch / limit / park: node mode only, the tester sends raw levels */
        port_rules_set(p, g_app.tester ? NULL : pc->rules, g_app.tester ? 0 : pc->rule_count);

        if (hw_locked) {
            continue;
        }
        if (!hw_changed && (pc->out_rate_hz != old->out_rate_hz || pc->break_us != old->break_us ||
                            pc->mab_us != old->mab_us || pc->out_slots != old->out_slots)) {
            dmx_output_timing_t t = {
                .rate_hz = pc->out_rate_hz, .break_us = pc->break_us,
                .mab_us = pc->mab_us, .slots = pc->out_slots,
            };
            dmx_handler_set_output_timing(p, &t);   /* live, from the next frame */
        }

        if (hw_changed) {
            /* Restarting a port can take a few seconds: tell the task
             * watchdog the main task is still going (before the main loop
             * starts it is not watched yet) */
            if (esp_task_wdt_status(NULL) == ESP_OK) {
                esp_task_wdt_reset();
            }
            /* stop sending first so callbacks don't route to a stopping port */
            r->mode = DMX_MODE_DISABLED;
#if CONFIG_NODE_SACN
            if (r->tx_sacn_universe) {
                sacn_transmitter_terminate(r->tx_sacn_universe);
                r->tx_sacn_universe = 0;
            }
#endif
            r->tx_started = false;

            if (g_app.tester) {
                ESP_LOGI(TAG, "Port %d: tester, %s", p, dmx_mode_to_string(pc->mode));
            } else {
                ESP_LOGI(TAG, "Port %d: %s, Art-Net %u (%u:%u:%u), sACN %u", p,
                         dmx_mode_to_string(pc->mode), pc->universe_primary,
                         (pc->universe_primary >> 8) & 0x7F, (pc->universe_primary >> 4) & 0xF,
                         pc->universe_primary & 0xF, pc->sacn_universe);
            }
            esp_err_t err = dmx_handler_configure_port(p, pc);
            if (err == ESP_OK && pc->mode != DMX_MODE_DISABLED) {
                err = dmx_handler_start_port(p);
            }
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Port %d failed to start: %s", p, esp_err_to_name(err));
                artnet_receiver_set_node_report(ARTNET_RC_DMX_ERROR, "DMX port failed to start");
            }
        }

#if CONFIG_NODE_SACN
        if (pc->protocol_mode == PROTOCOL_ARTNET_ONLY && r->tx_sacn_universe) {
            /* input port no longer sends sACN: tell receivers right away */
            sacn_transmitter_terminate(r->tx_sacn_universe);
            r->tx_sacn_universe = 0;
        }
#endif
        r->proto = pc->protocol_mode;
        r->merge = pc->merge_mode;
        r->artnet = pc->universe_primary;
        r->artnet_backup = pc->universe_secondary;
        r->sacn = pc->sacn_universe;
        dmx_handler_set_output_delta(p, pc->delta_output);
        set_routing(p, r, pc);
        r->mode = pc->mode;
#if CONFIG_NODE_RDM
        rdm_bridge_set_port_address(p, pc->universe_primary);
#endif
        g_app.applied[p - 1] = *pc;
    }
    if (!hw_locked) {
        g_app.applied_valid = true;
    }

    /* sACN: join exactly the universes the output ports listen to */
#if CONFIG_NODE_SACN
    app_update_sacn_groups();
    sacn_transmitter_set_source_name(c->node_info.short_name);
#endif

    free(snap);
    xSemaphoreGive(g_app.apply_lock);
    artnet_receiver_notify_change(false);
}

/* Self-test finished: put every port back the way the settings say */
void app_selftest_restore(void *user_data)
{
    xSemaphoreTake(g_app.apply_lock, portMAX_DELAY);
    g_app.applied_valid = false;
    xSemaphoreGive(g_app.apply_lock);
    xEventGroupSetBits(g_app.events, EV_APPLY);
}
