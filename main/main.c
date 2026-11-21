/**
 * ESP-NODE-2RDM — Art-Net 4 / sACN (E1.31) to 2x DMX512/RDM node
 *
 * Data path (output ports):
 *   Art-Net rx / sACN rx  ->  merge engine (per port)  ->  DMX output task
 *   The DMX output task pulls a fresh merged frame for every DMX frame it
 *   sends (frame source callback), so there is no extra copy task or delay.
 *
 * Data path (input ports):
 *   DMX input task -> on change (plus a short repeat and a 1 s keepalive)
 *   -> ArtDmx and/or sACN; sACN streams are terminated when the input is lost.
 *
 * Synchronous output: ArtSync / E1.31 synchronization packets latch the
 * merged levels (merge engine) and trigger the DMX frame straight away, so
 * every universe of a synchronised controller changes on the same frame.
 *
 * RDM: the RDM bridge answers ArtTodRequest/ArtTodControl and passes ArtRdm
 * GET/SET to the fixtures (discovery runs on the node).
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
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "mdns.h"
#include "lwip/inet.h"
#include "storage_manager.h"
#include "config_manager.h"
#include "led_manager.h"
#include "network_manager.h"
#include "dmx_handler.h"
#include "artnet_receiver.h"
#include "sacn_receiver.h"
#include "artnet_transmitter.h"
#include "sacn_transmitter.h"
#include "merge_engine.h"
#include "web_server.h"
#include "scene_manager.h"
#include "rdm_bridge.h"
#include "dmx_tools.h"
#include "diag.h"
#include "board.h"

static const char *TAG = "main";

/* Art-Net VersInfo (BCD-ish major.minor), keep in step with PROJECT_VER */
#define FW_VERSION_ARTNET     0x0100

#define INPUT_KEEPALIVE_US    (1000 * 1000)   /* resend unchanged input every 1 s */
#define INPUT_REPEAT_FRAMES   3               /* send each change 3 times */
#define OTA_VALID_AFTER_US    (30LL * 1000 * 1000)
#define STATS_INTERVAL_US     (30LL * 1000 * 1000)

/* Reset button (board.h, BOOT = GPIO0 on ESP-NODE-2RDM, active low). Hold
 * 10 s to factory reset (clears the admin password and the AP password
 * too). -1 = no button. */
#define RESET_BUTTON_GPIO     BOARD_RESET_BUTTON_GPIO
#define RESET_WARN_MS         3000
#define RESET_HOLD_MS         10000

#define NPORTS                CONFIG_DMX_PORTS
#define STARTUP_RELEASE_FADE_MS 1000   /* power-on scene fades out when a console takes over */
#define IPPROG_APPLY_DELAY_MS 300      /* let ArtIpProgReply leave before the IP changes */

#define EV_APPLY    BIT0   /* apply config to the running system */
#define EV_SAVE     BIT1   /* save config (changed by ArtAddress) */
#define EV_NET      BIT2   /* an interface got an address */
#define EV_FAILREC  BIT3   /* ArtAddress FailRecord: store the output as failsafe scenes */
#define EV_TAKEOVER BIT4   /* network data arrived on a port playing its power-on scene */
#define EV_NETCFG   BIT5   /* network settings changed (ArtIpProg) */

/* Live routing for one port, read by the receive callbacks */
typedef struct {
    volatile dmx_mode_t mode;
    volatile protocol_mode_t proto;
    volatile merge_mode_t merge;
    volatile uint16_t artnet;          /* 15-bit Port-Address */
    volatile int16_t artnet_backup;    /* -1 = none */
    volatile uint16_t sacn;            /* 1..63999 */

    /* DMX input -> network */
    uint8_t last_tx[512];
    int64_t last_tx_us;
    uint8_t repeat;
    bool tx_started;
    uint16_t tx_sacn_universe;         /* universe the sACN stream runs on */
    uint32_t err_baseline;             /* ArtAddress ResetRxFlags */
} route_t;

static route_t s_route[NPORTS];
static port_config_t s_applied[NPORTS];
static bool s_applied_valid;
static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_apply_lock;
static volatile uint8_t s_indicator;   /* 0 normal, 1 locate, 2 mute */
static volatile bool s_identify[NPORTS];
static volatile int64_t s_web_identify_until;   /* web "Identify" button, 0 = off */
static volatile uint8_t s_startup_mask;         /* ports showing their power-on scene */

static inline bool mode_is_output(dmx_mode_t m)
{
    return m == DMX_MODE_OUTPUT || m == DMX_MODE_RDM_MASTER;
}

static inline bool mode_is_input(dmx_mode_t m)
{
    return m == DMX_MODE_INPUT || m == DMX_MODE_RDM_RESPONDER;
}

static port_config_t *port_cfg(config_t *c, int port)
{
    return config_port(c, port);
}

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

// ============================================================================
// LED indicator (Art-Net locate/mute, RDM identify)
// ============================================================================

static void network_led(network_state_t state)
{
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
    if (s_indicator == 1) return true;
    if (s_web_identify_until && esp_timer_get_time() < s_web_identify_until) return true;
    for (int p = 0; p < NPORTS; p++) {
        if (s_identify[p]) return true;
    }
    return false;
}

static void update_indicator(void)
{
    bool locate = identify_any();
    if (locate) {
        led_manager_set_custom((rgb_color_t){255, 255, 255}, LED_BEHAVIOR_FAST_BLINK, 0, 1);
    } else if (s_indicator == 2) {
        led_manager_set_custom((rgb_color_t){0, 0, 0}, LED_BEHAVIOR_STATIC, 0, 1);
    } else {
        led_manager_clear();
        network_status_t st;
        memset(&st, 0, sizeof(st));
        network_get_status(&st);
        network_led(st.state);
    }
}

// ============================================================================
// Network callbacks
// ============================================================================

static void network_state_changed(network_state_t state, void *user_data)
{
    ESP_LOGI(TAG, "Network state: %d", state);
    if (s_indicator == 0 && !identify_any()) {
        network_led(state);
    }
    if (state == NETWORK_ETHERNET_CONNECTED || state == NETWORK_WIFI_STA_CONNECTED ||
        state == NETWORK_WIFI_AP_ACTIVE) {
        /* Handled on the main task: re-join multicast, announce, retarget tx */
        xEventGroupSetBits(s_events, EV_NET);
    }
}

static void on_network_up(void)
{
    network_ip_info_t ni;
    if (network_get_ip_info(&ni) != ESP_OK || ni.ip == 0) {
        return;
    }
    struct in_addr ip = {.s_addr = ni.ip};
    ESP_LOGI(TAG, "Network up on %s (%s)", inet_ntoa(ip),
             ni.state == NETWORK_ETHERNET_CONNECTED ? "Ethernet" :
             ni.state == NETWORK_WIFI_STA_CONNECTED ? "WiFi" : "AP");

    /* lwIP only joins multicast groups on interfaces that exist at join
     * time, so every new interface needs the sACN groups joined again */
    sacn_receiver_refresh_subscriptions();
    sacn_transmitter_set_interface(ni.ip);
    artnet_transmitter_set_destination(ni.netmask ? ((ni.ip & ni.netmask) | ~ni.netmask) : 0);
    /* Let controllers see the node (and its new IP) straight away */
    artnet_receiver_notify_change(true);
}

// ============================================================================
// Network -> DMX
// ============================================================================

static void on_artnet_dmx(uint16_t universe, const uint8_t *data,
                          uint16_t length, uint8_t sequence, uint32_t source_ip,
                          void *user_data)
{
    bool routed = false;

    for (int p = 0; p < NPORTS; p++) {
        const route_t *r = &s_route[p];
        if (!mode_is_output(r->mode) || r->proto == PROTOCOL_SACN_ONLY) continue;

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
        if (s_startup_mask & (1u << p)) xEventGroupSetBits(s_events, EV_TAKEOVER);
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
            o += snprintf(on + o, sizeof(on) - o, "%s%u", p ? ", " : "", s_route[p].artnet);
        }
        ESP_LOGW(TAG, "Art-Net %u:%u:%u (port-address %u) not routed; ports are on %s",
                 (universe >> 8) & 0x7F, (universe >> 4) & 0xF, universe & 0xF, universe, on);
    }
}

static void on_sacn_dmx(const sacn_packet_info_t *pkt, void *user_data)
{
    if (pkt->preview && !pkt->terminated) {
        return;   /* preview data is for visualisers, not live output */
    }

    bool routed = false;
    for (int p = 0; p < NPORTS; p++) {
        const route_t *r = &s_route[p];
        if (!mode_is_output(r->mode) || r->proto == PROTOCOL_ARTNET_ONLY) continue;
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
        if ((s_startup_mask & (1u << p)) && !pkt->terminated) {
            xEventGroupSetBits(s_events, EV_TAKEOVER);
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

static void on_artnet_sync(uint32_t source_ip, void *user_data)
{
    trigger_ports(merge_engine_artnet_sync(source_ip));
}

static void on_sacn_sync(uint16_t sync_universe, const uint8_t *cid, uint32_t source_ip,
                         void *user_data)
{
    trigger_ports(merge_engine_sacn_sync(sync_universe));
}

/*
 * sACN multicast groups: the output ports' data universes plus every
 * synchronization universe their sources announce (joined while a source
 * uses it, left once it times out). Called from the main task only.
 */
static void update_sacn_groups(void)
{
    uint16_t unis[SACN_MAX_UNIVERSES];
    uint8_t n = 0;
    for (int p = 0; p < NPORTS && n < SACN_MAX_UNIVERSES; p++) {
        if (!mode_is_output(s_route[p].mode) || s_route[p].proto == PROTOCOL_ARTNET_ONLY) continue;
        uint16_t cand[1 + MERGE_MAX_SOURCES];
        uint8_t c = 0;
        cand[c++] = s_route[p].sacn;
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
static void dmx_frame_source(uint8_t port, uint8_t *data, void *user_data)
{
    if (dmx_test_fill(port, data)) return;
    merge_engine_get_output(port, data);
}

// ============================================================================
// DMX -> Network
// ============================================================================

static void on_dmx_input(uint8_t port, const uint8_t *data, size_t size, void *user_data)
{
    route_t *r = &s_route[port - 1];
    (void)size;   /* always a full 512-slot buffer (short frames are zero-filled) */
    if (!mode_is_input(r->mode) || !network_is_connected() || dmx_selftest_running()) {
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
    if (r->proto != PROTOCOL_ARTNET_ONLY) {
        uint16_t uni = r->sacn;
        if (r->tx_sacn_universe && r->tx_sacn_universe != uni) {
            sacn_transmitter_terminate(r->tx_sacn_universe);
        }
        r->tx_sacn_universe = uni;
        sacn_transmitter_send(uni, r->last_tx, 512, config_get()->node_info.sacn_priority);
    }
    led_manager_pulse();
}

static void on_dmx_loss(uint8_t port, void *user_data)
{
    route_t *r = &s_route[port - 1];
    ESP_LOGW(TAG, "Port %u: DMX input lost, stopping network stream", port);
    if (r->tx_sacn_universe) {
        sacn_transmitter_terminate(r->tx_sacn_universe);
        r->tx_sacn_universe = 0;
    }
    r->tx_started = false;
    artnet_receiver_notify_change(false);
}

static void on_rdm_identify(uint8_t port, bool identify, void *user_data)
{
    s_identify[port - 1] = identify;
    update_indicator();
}

// ============================================================================
// Art-Net node status / ArtAddress
// ============================================================================

static void artnet_status(artnet_node_status_t *st, void *user_data)
{
    config_t *c = config_get();

    snprintf(st->short_name, sizeof(st->short_name), "%s", c->node_info.short_name);
    snprintf(st->long_name, sizeof(st->long_name), "%s", c->node_info.long_name);

    network_ip_info_t ni;
    memset(&ni, 0, sizeof(ni));
    network_get_ip_info(&ni);
    memcpy(st->ip, &ni.ip, 4);          /* network order = wire order */
    memcpy(st->netmask, &ni.netmask, 4);
    memcpy(st->mac, ni.mac, 6);
    st->dhcp = ni.dhcp;
    st->firmware_version = FW_VERSION_ARTNET;
    switch (c->merge.loss_behavior) {
        case LOSS_BLACKOUT: st->failsafe = ARTNET_FAILSAFE_ZERO; break;
        case LOSS_FULL:     st->failsafe = ARTNET_FAILSAFE_FULL; break;
        case LOSS_SCENE:    st->failsafe = ARTNET_FAILSAFE_SCENE; break;
        default:            st->failsafe = ARTNET_FAILSAFE_HOLD; break;
    }
    st->indicator = s_indicator;

    for (int p = 0; p < ARTNET_NODE_PORTS && p < NPORTS; p++) {
        const port_config_t *pc = port_cfg(c, p + 1);
        artnet_port_status_t *ap = &st->ports[p];
        dmx_port_status_t ds;
        memset(&ds, 0, sizeof(ds));
        dmx_handler_get_port_status(p + 1, &ds);

        ap->enabled = pc->mode != DMX_MODE_DISABLED;
        ap->is_input = mode_is_input(pc->mode);
        ap->port_address = pc->universe_primary & 0x7FFF;
        ap->ltp = pc->merge_mode == MERGE_MODE_LTP;
        ap->sacn = pc->protocol_mode == PROTOCOL_SACN_ONLY;
        /* Output ports are RDM gateways (ArtTodRequest / ArtRdm) */
        ap->rdm_capable = mode_is_output(pc->mode);
        ap->rdm_enabled = ds.rdm_available;
        ap->rdm_discovering = ds.rdm_discovering;
        ap->rdm_background = rdm_bridge_background_enabled(p + 1);
        ap->input_errors = ds.stats.error_count > s_route[p].err_baseline;
        memset(ap->responder_uid, 0, 6);
        if (pc->mode == DMX_MODE_RDM_RESPONDER) {
            dmx_handler_get_uid(p + 1, ap->responder_uid);
        }

        if (ap->is_input) {
            ap->data_active = ds.stats.signal_present;
            ap->merging = false;
        } else {
            ap->data_active = merge_engine_is_output_active(p + 1);
            merge_source_info_t src[MERGE_MAX_SOURCES];
            uint8_t n = merge_engine_get_sources(p + 1, src, MERGE_MAX_SOURCES);
            int contributing = 0;
            for (uint8_t i = 0; i < n; i++) {
                if (src[i].contributing) contributing++;
            }
            ap->merging = contributing > 1;
        }
    }
}

static void artnet_address(const artnet_address_cmd_t *cmd, void *user_data)
{
    config_t *c = config_get();
    int p = (cmd->port >= 1 && cmd->port <= NPORTS) ? cmd->port : 1;
    port_config_t *pc = port_cfg(c, p);
    bool changed = false;
    struct in_addr from = {.s_addr = cmd->source_ip};

    ESP_LOGI(TAG, "ArtAddress from %s for port %d (command 0x%02x)", inet_ntoa(from), p, cmd->command);

    if (cmd->short_name && strcmp(cmd->short_name, c->node_info.short_name) != 0) {
        snprintf(c->node_info.short_name, sizeof(c->node_info.short_name), "%s", cmd->short_name);
        artnet_receiver_set_node_report(ARTNET_RC_SH_NAME_OK, "Short name programmed");
        changed = true;
    }
    if (cmd->long_name && strcmp(cmd->long_name, c->node_info.long_name) != 0) {
        snprintf(c->node_info.long_name, sizeof(c->node_info.long_name), "%s", cmd->long_name);
        artnet_receiver_set_node_report(ARTNET_RC_LO_NAME_OK, "Long name programmed");
        changed = true;
    }

    uint16_t addr = pc->universe_primary;
    if (cmd->reset_address) addr = (uint16_t)(p - 1);   /* default: port N on universe N-1 */
    if (cmd->net >= 0)      addr = (addr & 0x00FF) | ((uint16_t)(cmd->net & 0x7F) << 8);
    if (cmd->sub >= 0)      addr = (addr & 0x7F0F) | ((uint16_t)(cmd->sub & 0x0F) << 4);
    if (cmd->universe >= 0) addr = (addr & 0x7FF0) | (uint16_t)(cmd->universe & 0x0F);
    if (addr != pc->universe_primary) {
        ESP_LOGI(TAG, "Port %d Port-Address %u -> %u", p, pc->universe_primary, addr);
        pc->universe_primary = addr;
        changed = true;
    }
    if (cmd->acn_priority <= 200 && cmd->acn_priority != c->node_info.sacn_priority) {
        c->node_info.sacn_priority = cmd->acn_priority;
        changed = true;
    }

    uint8_t op = cmd->command;
    uint8_t hi = op & 0xF0, idx = op & 0x0F;
    switch (op) {
        case ARTNET_AC_NONE:
            break;
        case ARTNET_AC_CANCEL_MERGE:
            break;   /* sources are merged per protocol rules, nothing latched */
        case ARTNET_AC_LED_NORMAL: s_indicator = 0; update_indicator(); break;
        case ARTNET_AC_LED_MUTE:   s_indicator = 2; update_indicator(); break;
        case ARTNET_AC_LED_LOCATE: s_indicator = 1; update_indicator(); break;
        case ARTNET_AC_RESET_RX_FLAGS:
            for (int i = 0; i < NPORTS; i++) {
                dmx_port_status_t ds;
                if (dmx_handler_get_port_status(i + 1, &ds) == ESP_OK) {
                    s_route[i].err_baseline = ds.stats.error_count;
                }
            }
            break;
        case ARTNET_AC_FAIL_HOLD:
            if (c->merge.loss_behavior != LOSS_HOLD_LAST_LOOK) {
                c->merge.loss_behavior = LOSS_HOLD_LAST_LOOK;
                changed = true;
            }
            break;
        case ARTNET_AC_FAIL_ZERO:
            if (c->merge.loss_behavior != LOSS_BLACKOUT) {
                c->merge.loss_behavior = LOSS_BLACKOUT;
                changed = true;
            }
            break;
        case ARTNET_AC_FAIL_FULL:
            if (c->merge.loss_behavior != LOSS_FULL) {
                c->merge.loss_behavior = LOSS_FULL;
                changed = true;
            }
            break;
        case ARTNET_AC_FAIL_SCENE:
            if (c->merge.loss_behavior != LOSS_SCENE) {
                c->merge.loss_behavior = LOSS_SCENE;
                changed = true;
            }
            break;
        case ARTNET_AC_FAIL_RECORD:
            /* File writes: done on the main task */
            xEventGroupSetBits(s_events, EV_FAILREC);
            break;
        default:
            if (idx != 0) {
                break;   /* one port per BindIndex: only port 0 of the page exists */
            }
            if (hi == ARTNET_AC_MERGE_LTP0 && pc->merge_mode != MERGE_MODE_LTP) {
                pc->merge_mode = MERGE_MODE_LTP; changed = true;
            } else if (hi == ARTNET_AC_MERGE_HTP0 && pc->merge_mode != MERGE_MODE_HTP) {
                pc->merge_mode = MERGE_MODE_HTP; changed = true;
            } else if (hi == 0x20 && pc->mode != DMX_MODE_OUTPUT && pc->mode != DMX_MODE_RDM_MASTER) {
                pc->mode = DMX_MODE_OUTPUT; changed = true;        /* DirectionTx */
            } else if (hi == 0x30 && pc->mode != DMX_MODE_INPUT && pc->mode != DMX_MODE_RDM_RESPONDER) {
                pc->mode = DMX_MODE_INPUT; changed = true;         /* DirectionRx */
            } else if (hi == ARTNET_AC_ARTNET_SEL0 && pc->protocol_mode != PROTOCOL_ARTNET_ONLY) {
                pc->protocol_mode = PROTOCOL_ARTNET_ONLY; changed = true;
            } else if (hi == ARTNET_AC_ACN_SEL0 && pc->protocol_mode != PROTOCOL_SACN_ONLY) {
                pc->protocol_mode = PROTOCOL_SACN_ONLY; changed = true;
            } else if (hi == ARTNET_AC_CLEAR_OP0) {
                scene_release(p, 0);
                merge_engine_blackout(p);
            } else if (hi == ARTNET_AC_RDM_ENABLE0 && !pc->rdm_enabled) {
                pc->rdm_enabled = true; changed = true;
            } else if (hi == ARTNET_AC_RDM_DISABLE0 && pc->rdm_enabled) {
                pc->rdm_enabled = false; changed = true;
            }
            break;
    }

    if (changed) {
        config_sanitize(c);
        xEventGroupSetBits(s_events, EV_SAVE | EV_APPLY);
    }
}

// ============================================================================
// Apply configuration
// ============================================================================

static void config_changed(void *user_data)
{
    xEventGroupSetBits(s_events, EV_APPLY);
}

/* Failsafe look of a port: LOSS_SCENE plays the port's failsafe scene
 * (zero if it has none or it was deleted) */
static void apply_loss_behavior(int p, const config_t *c, const port_config_t *pc)
{
    loss_behavior_t loss = c->merge.loss_behavior;
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
}

static void apply_config(void)
{
    xSemaphoreTake(s_apply_lock, portMAX_DELAY);
    config_t *c = config_get();
    config_sanitize(c);
    /* The self-test owns the port hardware until it finishes (it asks
     * for a full re-apply afterwards) */
    bool hw_locked = dmx_selftest_running();

    for (int p = 1; p <= NPORTS; p++) {
        const port_config_t *pc = port_cfg(c, p);
        route_t *r = &s_route[p - 1];
        const port_config_t *old = &s_applied[p - 1];
        bool hw_changed = !s_applied_valid || pc->mode != old->mode ||
                          pc->rdm_enabled != old->rdm_enabled;

        merge_engine_config(p, pc->merge_mode, (uint32_t)c->merge.timeout_seconds * 1000);
        merge_engine_set_protocol_mode(p, pc->protocol_mode);
        apply_loss_behavior(p, c, pc);

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
            /* stop sending first so callbacks don't route to a stopping port */
            r->mode = DMX_MODE_DISABLED;
            if (r->tx_sacn_universe) {
                sacn_transmitter_terminate(r->tx_sacn_universe);
                r->tx_sacn_universe = 0;
            }
            r->tx_started = false;

            ESP_LOGI(TAG, "Port %d: %s, Art-Net %u (%u:%u:%u), sACN %u", p,
                     dmx_mode_to_string(pc->mode), pc->universe_primary,
                     (pc->universe_primary >> 8) & 0x7F, (pc->universe_primary >> 4) & 0xF,
                     pc->universe_primary & 0xF, pc->sacn_universe);
            esp_err_t err = dmx_handler_configure_port(p, pc);
            if (err == ESP_OK && pc->mode != DMX_MODE_DISABLED) {
                err = dmx_handler_start_port(p);
            }
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Port %d failed to start: %s", p, esp_err_to_name(err));
                artnet_receiver_set_node_report(ARTNET_RC_DMX_ERROR, "DMX port failed to start");
            }
        }

        if (pc->protocol_mode == PROTOCOL_ARTNET_ONLY && r->tx_sacn_universe) {
            /* input port no longer sends sACN: tell receivers right away */
            sacn_transmitter_terminate(r->tx_sacn_universe);
            r->tx_sacn_universe = 0;
        }
        r->proto = pc->protocol_mode;
        r->merge = pc->merge_mode;
        r->artnet = pc->universe_primary;
        r->artnet_backup = pc->universe_secondary;
        r->sacn = pc->sacn_universe;
        r->mode = pc->mode;
        rdm_bridge_set_port_address(p, pc->universe_primary);
        s_applied[p - 1] = *pc;
    }
    if (!hw_locked) {
        s_applied_valid = true;
    }

    /* sACN: join exactly the universes the output ports listen to */
    update_sacn_groups();
    sacn_transmitter_set_source_name(c->node_info.short_name);

    xSemaphoreGive(s_apply_lock);
    artnet_receiver_notify_change(false);
}

/* Self-test finished: put every port back the way the settings say */
static void selftest_restore(void *user_data)
{
    xSemaphoreTake(s_apply_lock, portMAX_DELAY);
    s_applied_valid = false;
    xSemaphoreGive(s_apply_lock);
    xEventGroupSetBits(s_events, EV_APPLY);
}

// ============================================================================
// Power-on scene / failsafe record
// ============================================================================

/* Each output port with a power-on scene shows it until a console sends
 * data to that port; it then fades out (STARTUP_RELEASE_FADE_MS) */
static void start_power_on_scenes(void)
{
    config_t *c = config_get();
    for (int p = 1; p <= NPORTS; p++) {
        const port_config_t *pc = port_cfg(c, p);
        if (!pc->startup_scene || !mode_is_output(pc->mode)) continue;
        if (scene_playback_on(pc->startup_scene, p, 0) == ESP_OK) {
            s_startup_mask |= 1u << (p - 1);
            ESP_LOGI(TAG, "Port %d: power-on scene %u until a console takes over",
                     p, pc->startup_scene);
        } else {
            ESP_LOGW(TAG, "Port %d: power-on scene %u not found", p, pc->startup_scene);
        }
    }
}

static void release_power_on_scenes(void)
{
    config_t *c = config_get();
    for (int p = 1; p <= NPORTS; p++) {
        uint8_t bit = 1u << (p - 1);
        if (!(s_startup_mask & bit)) continue;
        merge_source_info_t src[MERGE_MAX_SOURCES];
        if (merge_engine_get_sources(p, src, MERGE_MAX_SOURCES) == 0) continue;
        s_startup_mask &= (uint8_t)~bit;
        /* Only if the power-on scene is still what the port shows */
        if (scene_active(p) == port_cfg(c, p)->startup_scene) {
            ESP_LOGI(TAG, "Port %d: console took over, power-on scene released", p);
            scene_release(p, STARTUP_RELEASE_FADE_MS);
        }
    }
}

/* ArtAddress FailRecord: store what each output port shows now as its
 * failsafe scene. A scene this created before ("Failsafe Pn") is replaced;
 * a scene the user picked is never overwritten. */
static void record_failsafe_scenes(void)
{
    config_t *c = config_get();
    dmx_scene_t *sc = malloc(sizeof(dmx_scene_t));
    bool changed = false;
    for (int p = 1; p <= NPORTS; p++) {
        port_config_t *pc = port_cfg(c, p);
        if (!mode_is_output(pc->mode)) continue;
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
        apply_config();
    }
}

// ============================================================================
// ArtIpProg (remote IP programming)
// ============================================================================

static void ip_to_bytes(const char *s, uint8_t out[4])
{
    struct in_addr a = {0};
    memset(out, 0, 4);
    if (s && s[0] && inet_aton(s, &a)) memcpy(out, &a.s_addr, 4);
}

static void bytes_to_ip(const uint8_t b[4], char *out, size_t size)
{
    snprintf(out, size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static bool ip_nonzero(const uint8_t b[4])
{
    return b[0] | b[1] | b[2] | b[3];
}

static void on_artnet_ipprog(const artnet_ipprog_cmd_t *cmd, artnet_ipprog_reply_t *reply,
                             void *user_data)
{
    config_t *c = config_get();
    network_ip_info_t ni;
    memset(&ni, 0, sizeof(ni));
    network_get_ip_info(&ni);
    memcpy(reply->ip, &ni.ip, 4);
    memcpy(reply->mask, &ni.netmask, 4);
    memcpy(reply->gateway, &ni.gateway, 4);
    reply->dhcp = ni.dhcp;

    if (!cmd->program) return;   /* query */
    struct in_addr from = {.s_addr = cmd->source_ip};
    if (!c->node_info.allow_ip_prog) {
        ESP_LOGW(TAG, "ArtIpProg from %s ignored (disabled in the settings)", inet_ntoa(from));
        return;
    }

    /* Which settings: the interface the console reached us on */
    bool *use_static;
    char *ip, *gw, *mask;
    const char *what;
    if (ni.state == NETWORK_ETHERNET_CONNECTED) {
        use_static = &c->network.eth_use_static_ip;
        ip = c->network.eth_static_ip;
        gw = c->network.eth_gateway;
        mask = c->network.eth_netmask;
        what = "Ethernet";
    } else if (ni.state == NETWORK_WIFI_STA_CONNECTED) {
        network_sta_info_t sta;
        network_get_sta_info(&sta);
        config_wifi_profile_t *wp = NULL;
        for (int i = 0; i < c->network.wifi_profile_count; i++) {
            if (strcmp(c->network.wifi_profiles[i].ssid, sta.ssid) == 0) {
                wp = &c->network.wifi_profiles[i];
                break;
            }
        }
        if (!wp) return;
        use_static = &wp->use_static_ip;
        ip = wp->static_ip;
        gw = wp->gateway;
        mask = wp->netmask;
        what = "WiFi";
    } else {
        ESP_LOGW(TAG, "ArtIpProg ignored: the access point address is fixed");
        return;
    }

    bool was_static = *use_static;
    char old_ip[16], old_gw[16], old_mask[16];
    snprintf(old_ip, sizeof(old_ip), "%s", ip);
    snprintf(old_gw, sizeof(old_gw), "%s", gw);
    snprintf(old_mask, sizeof(old_mask), "%s", mask);

    if (cmd->dhcp || cmd->reset_default) {
        *use_static = false;
    } else if (cmd->set_ip || cmd->set_mask || cmd->set_gateway) {
        /* Start from what is in use now, so a partial command keeps the rest */
        uint8_t nip[4], nmask[4], ngw[4];
        memcpy(nip, reply->ip, 4);
        memcpy(nmask, reply->mask, 4);
        memcpy(ngw, reply->gateway, 4);
        if (*use_static) {
            ip_to_bytes(ip, nip);
            ip_to_bytes(mask, nmask);
            ip_to_bytes(gw, ngw);
        }
        if (cmd->set_ip) memcpy(nip, cmd->ip, 4);
        if (cmd->set_mask) memcpy(nmask, cmd->mask, 4);
        if (cmd->set_gateway) memcpy(ngw, cmd->gateway, 4);
        if (!ip_nonzero(nip) || !ip_nonzero(nmask)) {
            ESP_LOGW(TAG, "ArtIpProg ignored: no address / mask");
            return;
        }
        *use_static = true;
        bytes_to_ip(nip, ip, 16);
        bytes_to_ip(nmask, mask, 16);
        if (ip_nonzero(ngw)) {
            bytes_to_ip(ngw, gw, 16);
        } else {
            gw[0] = '\0';
        }
    }

    bool changed = was_static != *use_static ||
                   (*use_static && (strcmp(old_ip, ip) || strcmp(old_gw, gw) || strcmp(old_mask, mask)));
    if (!changed) return;

    if (*use_static) {
        ip_to_bytes(ip, reply->ip);
        ip_to_bytes(mask, reply->mask);
        ip_to_bytes(gw, reply->gateway);
        reply->dhcp = false;
        ESP_LOGW(TAG, "ArtIpProg from %s: %s static %s / %s gw %s", inet_ntoa(from), what,
                 ip, mask, gw[0] ? gw : "-");
    } else {
        reply->dhcp = true;
        ESP_LOGW(TAG, "ArtIpProg from %s: %s back to DHCP", inet_ntoa(from), what);
    }
    config_sanitize(c);
    xEventGroupSetBits(s_events, EV_SAVE | EV_NETCFG);
}

// ============================================================================
// Identify (web) and per-port LEDs
// ============================================================================

static void web_identify(uint32_t seconds)
{
    s_web_identify_until = seconds ? esp_timer_get_time() + (int64_t)seconds * 1000000 : 0;
    ESP_LOGI(TAG, "Identify %s", seconds ? "on" : "off");
    update_indicator();
}

static uint32_t web_identify_left(void)
{
    int64_t left = s_web_identify_until - esp_timer_get_time();
    return (s_web_identify_until && left > 0) ? (uint32_t)((left + 999999) / 1000000) : 0;
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
        uint8_t tests = dmx_test_active_mask();
        bool ident = identify_any();
        for (int p = 0; p < NPORTS; p++) {
            if (k_port_led[p] < 0) continue;
            dmx_port_status_t ds;
            bool on = false;
            if (dmx_handler_get_port_status(p + 1, &ds) == ESP_OK && ds.is_active) {
                bool data = mode_is_input(ds.mode) ? ds.stats.signal_present
                                                   : merge_engine_is_output_active(p + 1);
                if (ident || s_identify[p] || (tests & (1u << p))) {
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

static void log_stats(void)
{
    for (int p = 1; p <= NPORTS; p++) {
        dmx_port_status_t st;
        merge_stats_t ms;
        if (dmx_handler_get_port_status(p, &st) != ESP_OK || !st.is_active) continue;
        if (mode_is_output(st.mode) && merge_engine_get_stats(p, &ms) == ESP_OK) {
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
    sacn_stats_t ss;
    if (artnet_receiver_get_stats(&as) == ESP_OK && sacn_receiver_get_stats(&ss) == ESP_OK) {
        ESP_LOGI(TAG, "Art-Net rx %" PRIu32 " (dmx %" PRIu32 ", poll %" PRIu32 "), "
                 "sACN rx %" PRIu32 " (data %" PRIu32 ", %u universe(s)), heap %" PRIu32 " (min %" PRIu32 ")",
                 as.packets_received, as.dmx_packets, as.poll_packets,
                 ss.packets_received, ss.data_packets, sacn_receiver_get_subscription_count(),
                 esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    }
}

// ============================================================================
// app_main
// ============================================================================

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
                update_indicator();
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

void app_main(void)
{
    diag_init();   /* first: keeps the log for the web UI, notes the reset reason */
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "ESP-NODE-2RDM firmware %s (%s %s), ESP-IDF %s",
             app->version, app->date, app->time, esp_get_idf_version());

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_events = xEventGroupCreate();
    s_apply_lock = xSemaphoreCreateMutex();
    if (!s_events || !s_apply_lock) {
        ESP_LOGE(TAG, "Out of memory");
        esp_restart();
    }
    for (int i = 0; i < NPORTS; i++) {
        s_route[i].mode = DMX_MODE_DISABLED;
        s_route[i].artnet_backup = -1;
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
    ESP_ERROR_CHECK(scene_manager_init());
    ESP_ERROR_CHECK(dmx_handler_init());
    dmx_selftest_set_restore_callback(selftest_restore, NULL);
    for (int p = 1; p <= NPORTS; p++) {
        dmx_handler_set_frame_source(p, dmx_frame_source, NULL);
        dmx_handler_register_rx_callback(p, on_dmx_input, NULL);
        dmx_handler_register_loss_callback(p, on_dmx_loss, NULL);
        dmx_handler_register_identify_callback(p, on_rdm_identify, NULL);
    }

    /* Network (asynchronous: Ethernet -> WiFi -> AP, supervised forever) */
    char hostname[32];
    make_hostname(config->node_info.short_name, hostname, sizeof(hostname));
    ESP_ERROR_CHECK(network_init());   /* starts lwIP: no socket() before this */

    ESP_ERROR_CHECK(artnet_transmitter_init());
    ESP_ERROR_CHECK(sacn_transmitter_init(config->node_info.short_name));

    network_set_hostname(hostname);
    network_register_state_callback(network_state_changed, NULL);
    ESP_ERROR_CHECK(network_start_with_fallback());
    start_mdns(config, hostname);

    ESP_ERROR_CHECK(artnet_receiver_init());
    ESP_ERROR_CHECK(artnet_receiver_set_callback(on_artnet_dmx, NULL));
    ESP_ERROR_CHECK(artnet_receiver_set_status_callback(artnet_status, NULL));
    ESP_ERROR_CHECK(artnet_receiver_set_address_callback(artnet_address, NULL));
    ESP_ERROR_CHECK(artnet_receiver_set_sync_callback(on_artnet_sync, NULL));
    ESP_ERROR_CHECK(artnet_receiver_set_ipprog_callback(on_artnet_ipprog, NULL));
    ESP_ERROR_CHECK(artnet_receiver_start());
    /* ArtDmx we send uses UDP source port 6454, as the spec requires */
    artnet_transmitter_use_socket(artnet_receiver_get_socket());

    ESP_ERROR_CHECK(sacn_receiver_init());
    sacn_receiver_set_local_cid(sacn_transmitter_get_cid());
    ESP_ERROR_CHECK(sacn_receiver_set_callback(on_sacn_dmx, NULL));
    ESP_ERROR_CHECK(sacn_receiver_set_sync_callback(on_sacn_sync, NULL));
    ESP_ERROR_CHECK(sacn_receiver_start());

    /* DMX ports, merge settings and sACN subscriptions */
    apply_config();
    start_power_on_scenes();

    /* RDM over Art-Net: discovers RDM ports once they are running */
    if (rdm_bridge_start() != ESP_OK) {
        ESP_LOGE(TAG, "RDM bridge failed to start");
    }

    config_register_change_callback(config_changed, NULL);
    static const web_server_hooks_t hooks = {
        .identify = web_identify,
        .identify_left = web_identify_left,
    };
    web_server_set_hooks(&hooks);
    ESP_ERROR_CHECK(web_server_init(NULL));
    ESP_ERROR_CHECK(web_server_start());

#if RESET_BUTTON_GPIO >= 0
    xTaskCreate(reset_button_task, "reset_btn", 3072, NULL, 2, NULL);
#endif
    if (board_has_port_leds()) {
        xTaskCreate(port_led_task, "port_led", 2560, NULL, 2, NULL);
    }

    artnet_receiver_set_node_report(ARTNET_RC_POWER_OK, "Power On Tests successful");
    ESP_LOGI(TAG, "System initialized");

    bool ota_marked = false;
    bool identify_was_on = false;
    int64_t last_stats = esp_timer_get_time();

    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_events,
                                               EV_APPLY | EV_SAVE | EV_NET | EV_FAILREC |
                                               EV_TAKEOVER | EV_NETCFG,
                                               pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
        if (bits & EV_SAVE) {
            config_save();
        }
        if (bits & EV_APPLY) {
            apply_config();
        }
        if (bits & EV_NET) {
            on_network_up();
        }
        if (bits & EV_FAILREC) {
            record_failsafe_scenes();
        }
        if ((bits & EV_TAKEOVER) || s_startup_mask) {
            release_power_on_scenes();
        }
        if (bits & EV_NETCFG) {
            vTaskDelay(pdMS_TO_TICKS(IPPROG_APPLY_DELAY_MS));
            network_supervisor_reconfigure();
        }
        /* Web identify ran out: back to the normal LED */
        bool ident_on = s_web_identify_until != 0 && web_identify_left() > 0;
        if (identify_was_on && !ident_on) {
            s_web_identify_until = 0;
            update_indicator();
        }
        identify_was_on = ident_on;

        sacn_transmitter_tick();
        update_sacn_groups();   /* follow sync universes as sources come and go */

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
