/*
 * Network <-> DMX routing.
 *
 * Output ports:  Art-Net rx / sACN rx -> merge engine (per port) -> DMX output
 *                task, which pulls a fresh merged frame for every frame it sends.
 * Input ports:   DMX input task -> on change (plus a short repeat and a 1 s
 *                keepalive) -> ArtDmx and/or sACN.
 * Local DMX:     DMX input task -> merge engine of each output port that takes
 *                that input (merged with the network, or alone as a splitter,
 *                which then sends a DMX frame for every frame received).
 * Sync:          ArtSync / E1.31 sync packets latch the merged levels and
 *                trigger the DMX frame straight away.
 *
 * Everything here runs on the network / DMX tasks: it reads g_app.route and
 * hands anything slow to the main task through g_app.events.
 */
#include <inttypes.h>
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
#include "port_rules.h"
#include "source_monitor.h"
#include "app.h"

static const char *TAG = "main";

#define INPUT_KEEPALIVE_US    (1000 * 1000)   /* resend unchanged input every 1 s */
#define INPUT_REPEAT_FRAMES   3               /* send each change 3 times */
#define APP_NOT_ROUTED_NOTE_US (5 * 1000 * 1000) /* "not routed" log at most every 5 s */
#define INPUT_NODES_CHECK_US  (1000 * 1000)   /* send_to "nodes": look them up every 1 s */

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

    srcmon_packet_t seen = {
        .proto = SRCMON_ARTNET,
        .ip = source_ip,
        .universe = universe,
        .sequence = sequence,
        .slots = length,
    };
    srcmon_note(&seen);   /* routed or not: the signal check lists everything */

    for (int p = 0; p < NPORTS; p++) {
        const route_t *r = &g_app.route[p];
        if (!app_mode_is_output(r->mode) || r->proto == PROTOCOL_SACN_ONLY || r->local_only) continue;

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

    /* Universe nobody listens to: the most common cause of "no output", so say
     * so, but at most every few seconds. A console line takes milliseconds and
     * blocks this task; logging every stray packet capped Art-Net at ~130
     * packets/s on a network carrying other universes. */
    static int64_t next_note_us;
    static uint32_t missed;
    missed++;
    int64_t now = esp_timer_get_time();
    if (now >= next_note_us) {
        next_note_us = now + APP_NOT_ROUTED_NOTE_US;
        char on[8 * NPORTS + 1];
        size_t o = 0;
        on[0] = '\0';
        for (int p = 0; p < NPORTS && o < sizeof(on); p++) {
            o += snprintf(on + o, sizeof(on) - o, "%s%u", p ? ", " : "", g_app.route[p].artnet);
        }
        ESP_LOGW(TAG, "Art-Net %u:%u:%u (port-address %u) not routed (%" PRIu32
                 " such packets since the last note); ports are on %s",
                 (universe >> 8) & 0x7F, (universe >> 4) & 0xF, universe & 0xF, universe,
                 missed, on);
        missed = 0;
    }
}

void app_on_sacn_dmx(const sacn_packet_info_t *pkt, void *user_data)
{
    if (!pkt->terminated) {
        srcmon_packet_t seen = {
            .proto = SRCMON_SACN,
            .ip = pkt->source_ip,
            .cid = pkt->cid,
            .universe = pkt->universe,
            .name = pkt->source_name,
            .priority = pkt->priority,
            .start_code = pkt->start_code,
            .preview = pkt->preview,
            .sequence = pkt->sequence,
            .slots = pkt->length,
        };
        srcmon_note(&seen);
    }
    if (pkt->preview && !pkt->terminated) {
        return;   /* preview data is for visualisers, not live output */
    }

    bool routed = false;
    for (int p = 0; p < NPORTS; p++) {
        const route_t *r = &g_app.route[p];
        if (!app_mode_is_output(r->mode) || r->proto == PROTOCOL_ARTNET_ONLY || r->local_only) continue;
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

/* Other nodes' ArtPollReply: the node list and address clash warnings */
void app_on_artnet_poll_reply(const artnet_poll_reply_t *reply, bool ip_conflict, void *user_data)
{
    srcmon_note_node(reply, ip_conflict);
    static int64_t last_warn_us;
    int64_t now = esp_timer_get_time();
    if (ip_conflict && (last_warn_us == 0 || now - last_warn_us > 60 * 1000000LL)) {
        last_warn_us = now;
        const uint8_t *m = reply->mac;
        ESP_LOGW(TAG, "IP address clash: \"%s\" (MAC %02X:%02X:%02X:%02X:%02X:%02X) uses this node's address",
                 reply->short_name, m[0], m[1], m[2], m[3], m[4], m[5]);
    }
}

/* Universe discovery: only for the signal check (which universes a console
 * sends that this node is not listening to) */
void app_on_sacn_discovery(const uint8_t *cid, const char *source_name, uint32_t source_ip,
                           uint8_t page, uint8_t last_page, const uint8_t *universes_be,
                           uint16_t count, void *user_data)
{
    srcmon_note_discovery(cid, source_ip, source_name, page, last_page, universes_be, count);
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
        const route_t *r = &g_app.route[p];
        if (!app_mode_is_output(r->mode) || r->proto == PROTOCOL_ARTNET_ONLY || r->local_only) continue;
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
 * (web DMX tools / self-test) replaces the output completely; otherwise the
 * port's patch / limit / park rules shape the merged levels. */
bool app_dmx_frame_source(uint8_t port, uint8_t *data, void *user_data)
{
#if CONFIG_NODE_DMX_TOOLS
    if (dmx_test_fill(port, data)) return true;
#endif
#if CONFIG_NODE_TESTER
    if (g_app.tester) {
        dmx_tester_get_levels(port, data);
        return true;
    }
#endif
    merge_engine_get_output(port, data);
    port_rules_apply(port, data);
    /* loss behaviour "stop output": nothing goes out until a source is back */
    return !merge_engine_output_stopped(port);
}

// ============================================================================
// DMX input -> other ports (splitter / merger)
// ============================================================================

/* Output ports that take input port @p in_port's DMX. The input port number
 * stands in for the sender's address in the merge engine. */
static void feed_local_outputs(uint8_t in_port, const uint8_t *data)
{
    char name[16];
    snprintf(name, sizeof(name), "Port %u DMX in", in_port);
    for (int p = 0; p < NPORTS; p++) {
        const route_t *o = &g_app.route[p];
        if (!(o->local_mask & (1u << (in_port - 1))) || !app_mode_is_output(o->mode)) continue;
        merge_input_t in = {
            .protocol = SOURCE_PROTOCOL_DMX_IN,
            .source_ip = in_port,
            .name = name,
            .priority = MERGE_ARTNET_PRIORITY,
            .start_code = 0x00,
            .data = data,
            .length = 512,
        };
        merge_engine_push(p + 1, &in);
        if (o->splitter) {
            /* one input and no network: follow it frame by frame (with
             * several inputs the port keeps its own refresh rate) */
            dmx_handler_trigger_output(p + 1);
        }
        if (g_app.startup_mask & (1u << p)) xEventGroupSetBits(g_app.events, EV_TAKEOVER);
    }
}

// ============================================================================
// DMX -> Network
// ============================================================================

void app_on_dmx_input(uint8_t port, const uint8_t *data, size_t size, void *user_data)
{
    route_t *r = &g_app.route[port - 1];
    (void)size;   /* always a full 512-slot buffer (dmx_handler fills in what a short frame leaves out) */
#if CONFIG_NODE_TESTER
    if (g_app.tester) {
        if (!app_selftest_running()) dmx_tester_on_frame(port, data);
        return;   /* tester: nothing goes to the network */
    }
#endif
    if (!app_mode_is_input(r->mode) || app_selftest_running()) {
        return;   /* (self-test data stays where it is) */
    }
    feed_local_outputs(port, data);
    if (!network_is_connected()) {
        return;
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

    /* Destinations: unicast to the list, or to the Art-Net nodes that output
     * this universe (looked up once a second; broadcast while there are
     * none), else broadcast / multicast */
    uint32_t list[CONFIG_SEND_LIST_MAX];
    int nlist = 0;
    uint8_t send_to = r->send_to;
    if (send_to == SEND_TO_LIST) {
        nlist = r->send_count;
        if (nlist > CONFIG_SEND_LIST_MAX) nlist = CONFIG_SEND_LIST_MAX;
        for (int i = 0; i < nlist; i++) list[i] = r->send_list[i];
    } else if (send_to == SEND_TO_NODES && now - r->nodes_checked_us >= INPUT_NODES_CHECK_US) {
        r->nodes_checked_us = now;
        r->node_count = (uint8_t)srcmon_output_nodes(r->artnet, r->node_ips, APP_SEND_NODES_MAX);
    }

    if (r->proto != PROTOCOL_SACN_ONLY) {
        if (nlist) {
            artnet_transmitter_send_port_to(r->artnet, port - 1, r->last_tx, 512, list, nlist);
        } else if (send_to == SEND_TO_NODES && r->node_count) {
            artnet_transmitter_send_port_to(r->artnet, port - 1, r->last_tx, 512,
                                            r->node_ips, r->node_count);
        } else {
            artnet_transmitter_send_port(r->artnet, port - 1, r->last_tx, 512);
        }
    }
#if CONFIG_NODE_SACN
    if (r->proto != PROTOCOL_ARTNET_ONLY) {
        uint16_t uni = r->sacn;
        if (r->tx_sacn_universe && r->tx_sacn_universe != uni) {
            sacn_transmitter_terminate(r->tx_sacn_universe);
        }
        r->tx_sacn_universe = uni;
        sacn_transmitter_send_to(uni, r->last_tx, 512, config_get()->node_info.sacn_priority,
                                 list, nlist);
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
