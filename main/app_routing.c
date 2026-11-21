/*
 * Network <-> DMX routing.
 *
 * Output ports:  Art-Net rx / sACN rx -> merge engine (per port) -> DMX output
 *                task, which pulls a fresh merged frame for every frame it sends.
 * Input ports:   DMX input task -> on change (plus a short repeat and a 1 s
 *                keepalive) -> ArtDmx and/or sACN.
 * Sync:          ArtSync / E1.31 sync packets latch the merged levels and
 *                trigger the DMX frame straight away.
 *
 * Everything here runs on the network / DMX tasks: it reads g_app.route and
 * hands anything slow to the main task through g_app.events.
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "led_manager.h"
#include "artnet_transmitter.h"
#include "sacn_transmitter.h"
#include "merge_engine.h"
#include "dmx_tools.h"
#include "app.h"

static const char *TAG = "main";

#define INPUT_KEEPALIVE_US    (1000 * 1000)   /* resend unchanged input every 1 s */
#define INPUT_REPEAT_FRAMES   3               /* send each change 3 times */

void app_on_network_up(void)
{
    network_ip_info_t ni;
    if (network_get_ip_info(&ni) != ESP_OK || ni.ip == 0) {
        return;
    }
    struct in_addr ip = {.s_addr = ni.ip};
    ESP_LOGI(TAG, "Network up on %s (%s)%s", inet_ntoa(ip),
             ni.state == NETWORK_ETHERNET_CONNECTED ? "Ethernet" :
             ni.state == NETWORK_WIFI_STA_CONNECTED ? "WiFi" : "AP",
             g_app.tester ? ", DMX tester mode: web UI only" : "");
    if (g_app.tester) {
        return;   /* no Art-Net / sACN to point at the new interface */
    }

    /* lwIP only joins multicast groups on interfaces that exist at join
     * time, so every new interface needs the sACN groups joined again */
#if CONFIG_NODE_SACN
    sacn_receiver_refresh_subscriptions();
    sacn_transmitter_set_interface(ni.ip);
#endif
    artnet_transmitter_set_destination(ni.netmask ? ((ni.ip & ni.netmask) | ~ni.netmask) : 0);
    /* Let controllers see the node (and its new IP) straight away */
    artnet_receiver_notify_change(true);
}

// ============================================================================
// Network -> DMX
// ============================================================================

void app_on_artnet_dmx(uint16_t universe, const uint8_t *data,
                       uint16_t length, uint8_t sequence, uint32_t source_ip,
                       void *user_data)
{
    bool routed = false;

    for (int p = 0; p < NPORTS; p++) {
        const route_t *r = &g_app.route[p];
        if (!app_mode_is_output(r->mode) || r->proto == PROTOCOL_SACN_ONLY) continue;

        bool primary = (universe == r->artnet);
        bool backup = !primary && r->merge == MERGE_MODE_BACKUP &&
                      r->artnet_backup >= 0 && universe == (uint16_t)r->artnet_backup;
        if (!primary && !backup) continue;

        merge_input_t in = {
            .protocol = SOURCE_PROTOCOL_ARTNET,
            .source_ip = source_ip,
            .priority = MERGE_ARTNET_PRIORITY,
            .sequence = sequence,
            .check_sequence = sequence != 0,   /* 0 = sender doesn't sequence */
            .start_code = 0x00,
            .data = data,
            .length = length,
            .backup = backup,
        };
        merge_engine_push(p + 1, &in);
        routed = true;
        if (g_app.startup_mask & (1u << p)) xEventGroupSetBits(g_app.events, EV_TAKEOVER);
    }

    if (routed) {
        led_manager_pulse();
        return;
    }

    /* Universe nobody listens to: the most common cause of "no output". */
    static uint32_t miss_count = 0;
    static uint16_t last_miss = 0xFFFF;
    miss_count++;
    if (universe != last_miss || (miss_count % 500) == 1) {
        last_miss = universe;
        char on[8 * NPORTS + 1];
        size_t o = 0;
        on[0] = '\0';
        for (int p = 0; p < NPORTS && o < sizeof(on); p++) {
            o += snprintf(on + o, sizeof(on) - o, "%s%u", p ? ", " : "", g_app.route[p].artnet);
        }
        ESP_LOGW(TAG, "Art-Net %u:%u:%u (port-address %u) not routed; ports are on %s",
                 (universe >> 8) & 0x7F, (universe >> 4) & 0xF, universe & 0xF, universe, on);
    }
}

void app_on_sacn_dmx(const sacn_packet_info_t *pkt, void *user_data)
{
    if (pkt->preview && !pkt->terminated) {
        return;   /* preview data is for visualisers, not live output */
    }

    bool routed = false;
    for (int p = 0; p < NPORTS; p++) {
        const route_t *r = &g_app.route[p];
        if (!app_mode_is_output(r->mode) || r->proto == PROTOCOL_ARTNET_ONLY) continue;
        if (pkt->universe != r->sacn) continue;

        merge_input_t in = {
            .protocol = SOURCE_PROTOCOL_SACN,
            .source_ip = pkt->source_ip,
            .cid = pkt->cid,
            .name = pkt->source_name,
            .priority = pkt->priority,
            .sequence = pkt->sequence,
            .check_sequence = true,
            .start_code = pkt->start_code,
            .data = pkt->data,
            .length = pkt->length,
            .terminated = pkt->terminated,
            .sync_address = pkt->sync_address,
            .force_sync = (pkt->options & SACN_OPT_FORCE_SYNC) != 0,
        };
        merge_engine_push(p + 1, &in);
        routed = true;
        if ((g_app.startup_mask & (1u << p)) && !pkt->terminated) {
            xEventGroupSetBits(g_app.events, EV_TAKEOVER);
        }
    }
    if (routed && pkt->start_code == 0x00) {
        led_manager_pulse();
    }
}

// ============================================================================
// Synchronous output
// ============================================================================

static void trigger_ports(uint8_t mask)
{
    for (int p = 0; p < NPORTS; p++) {
        if (mask & (1u << p)) {
            dmx_handler_trigger_output(p + 1);
        }
    }
}

void app_on_artnet_sync(uint32_t source_ip, void *user_data)
{
    trigger_ports(merge_engine_artnet_sync(source_ip));
}

void app_on_sacn_sync(uint16_t sync_universe, const uint8_t *cid, uint32_t source_ip,
                      void *user_data)
{
    trigger_ports(merge_engine_sacn_sync(sync_universe));
}

/*
 * sACN multicast groups: the output ports' data universes plus every
 * synchronization universe their sources announce (joined while a source
 * uses it, left once it times out). Called from the main task only.
 */
void app_update_sacn_groups(void)
{
    uint16_t unis[SACN_MAX_UNIVERSES];
    uint8_t n = 0;
    for (int p = 0; p < NPORTS && n < SACN_MAX_UNIVERSES; p++) {
        if (!app_mode_is_output(g_app.route[p].mode) || g_app.route[p].proto == PROTOCOL_ARTNET_ONLY) continue;
        uint16_t cand[1 + MERGE_MAX_SOURCES];
        uint8_t c = 0;
        cand[c++] = g_app.route[p].sacn;
        c += merge_engine_sync_universes(p + 1, &cand[c], MERGE_MAX_SOURCES);
        for (uint8_t k = 0; k < c && n < SACN_MAX_UNIVERSES; k++) {
            bool dup = false;
            for (uint8_t j = 0; j < n; j++) dup |= unis[j] == cand[k];
            if (!dup) unis[n++] = cand[k];
        }
    }
    static uint16_t last[SACN_MAX_UNIVERSES];
    static uint8_t last_n = 0xFF;
    if (n == last_n && memcmp(unis, last, n * sizeof(uint16_t)) == 0) {
        return;
    }
    sacn_receiver_set_universes(unis, n);
    memcpy(last, unis, n * sizeof(uint16_t));
    last_n = n;
}

/* DMX output task asks for every frame it sends. A running test pattern
 * (web DMX tools / self-test) replaces the output completely. */
void app_dmx_frame_source(uint8_t port, uint8_t *data, void *user_data)
{
#if CONFIG_NODE_DMX_TOOLS
    if (dmx_test_fill(port, data)) return;
#endif
#if CONFIG_NODE_TESTER
    if (g_app.tester) {
        dmx_tester_get_levels(port, data);
        return;
    }
#endif
    merge_engine_get_output(port, data);
}

// ============================================================================
// DMX -> Network
// ============================================================================

void app_on_dmx_input(uint8_t port, const uint8_t *data, size_t size, void *user_data)
{
    route_t *r = &g_app.route[port - 1];
    (void)size;   /* always a full 512-slot buffer (short frames are zero-filled) */
#if CONFIG_NODE_TESTER
    if (g_app.tester) {
        if (!app_selftest_running()) dmx_tester_on_frame(port, data);
        return;   /* tester: nothing goes to the network */
    }
#endif
    if (!app_mode_is_input(r->mode) || !network_is_connected() || app_selftest_running()) {
        return;   /* (self-test data stays off the network) */
    }

    int64_t now = esp_timer_get_time();
    if (memcmp(r->last_tx, data, 512) != 0) {
        memcpy(r->last_tx, data, 512);
        r->repeat = INPUT_REPEAT_FRAMES;
    } else if (r->repeat == 0 && r->tx_started && (now - r->last_tx_us) < INPUT_KEEPALIVE_US) {
        return;   /* unchanged: only a keepalive every second */
    }
    if (r->repeat > 0) r->repeat--;
    r->last_tx_us = now;
    r->tx_started = true;

    if (r->proto != PROTOCOL_SACN_ONLY) {
        artnet_transmitter_send_port(r->artnet, port - 1, r->last_tx, 512);
    }
#if CONFIG_NODE_SACN
    if (r->proto != PROTOCOL_ARTNET_ONLY) {
        uint16_t uni = r->sacn;
        if (r->tx_sacn_universe && r->tx_sacn_universe != uni) {
            sacn_transmitter_terminate(r->tx_sacn_universe);
        }
        r->tx_sacn_universe = uni;
        sacn_transmitter_send(uni, r->last_tx, 512, config_get()->node_info.sacn_priority);
    }
#endif
    led_manager_pulse();
}

void app_on_dmx_loss(uint8_t port, void *user_data)
{
    route_t *r = &g_app.route[port - 1];
#if CONFIG_NODE_TESTER
    if (g_app.tester) {
        ESP_LOGW(TAG, "Port %u: DMX input lost", port);
        dmx_tester_on_loss(port);
        return;
    }
#endif
    ESP_LOGW(TAG, "Port %u: DMX input lost, stopping network stream", port);
#if CONFIG_NODE_SACN
    if (r->tx_sacn_universe) {
        sacn_transmitter_terminate(r->tx_sacn_universe);
        r->tx_sacn_universe = 0;
    }
#endif
    r->tx_started = false;
    artnet_receiver_notify_change(false);
}
