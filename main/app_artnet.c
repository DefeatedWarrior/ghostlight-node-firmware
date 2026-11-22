/*
 * Art-Net node behaviour: what ArtPollReply reports (artnet_status), remote
 * programming with ArtAddress and ArtIpProg. Changes are written to the
 * settings here and saved / applied by the main task (EV_SAVE, EV_APPLY,
 * EV_NETCFG).
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "lwip/inet.h"
#include "merge_engine.h"
#include "scene_manager.h"
#include "rdm_bridge.h"
#include "app.h"

static const char *TAG = "main";

/* Art-Net VersInfo (BCD-ish major.minor), keep in step with PROJECT_VER */
#define FW_VERSION_ARTNET     0x0100

// ============================================================================
// Art-Net node status / ArtAddress
// ============================================================================

void app_artnet_status(artnet_node_status_t *st, void *user_data)
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
    st->indicator = g_app.indicator;

    for (int p = 0; p < ARTNET_NODE_PORTS && p < NPORTS; p++) {
        const port_config_t *pc = config_port(c, p + 1);
        artnet_port_status_t *ap = &st->ports[p];
        dmx_port_status_t ds;
        memset(&ds, 0, sizeof(ds));
        dmx_handler_get_port_status(p + 1, &ds);

        ap->enabled = pc->mode != DMX_MODE_DISABLED;
        ap->is_input = app_mode_is_input(pc->mode);
        ap->port_address = pc->universe_primary & 0x7FFF;
        ap->ltp = pc->merge_mode == MERGE_MODE_LTP;
        ap->sacn = pc->protocol_mode == PROTOCOL_SACN_ONLY;
        /* Output ports are RDM gateways (ArtTodRequest / ArtRdm) */
#if CONFIG_NODE_RDM
        ap->rdm_capable = app_mode_is_output(pc->mode);
        ap->rdm_enabled = ds.rdm_available;
        ap->rdm_discovering = ds.rdm_discovering;
        ap->rdm_background = rdm_bridge_background_enabled(p + 1);
#else
        ap->rdm_capable = false;
        ap->rdm_enabled = false;
        ap->rdm_discovering = false;
        ap->rdm_background = false;
#endif
        ap->input_errors = ds.stats.error_count > g_app.route[p].err_baseline;
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

void app_artnet_address(const artnet_address_cmd_t *cmd, void *user_data)
{
    int p = (cmd->port >= 1 && cmd->port <= NPORTS) ? cmd->port : 1;
    bool changed = false, short_named = false, long_named = false;
    struct in_addr from = {.s_addr = cmd->source_ip};

    ESP_LOGI(TAG, "ArtAddress from %s for port %d (command 0x%02x)", inet_ntoa(from), p, cmd->command);

    uint8_t op = cmd->command;
    uint8_t hi = op & 0xF0, idx = op & 0x0F;

    /* The settings part, under the settings lock (the web UI may be saving
     * at the same moment); what it sets off happens after */
    config_lock();
    config_t *c = config_get();
    port_config_t *pc = config_port(c, p);

    if (cmd->short_name && strcmp(cmd->short_name, c->node_info.short_name) != 0) {
        snprintf(c->node_info.short_name, sizeof(c->node_info.short_name), "%s", cmd->short_name);
        short_named = changed = true;
    }
    if (cmd->long_name && strcmp(cmd->long_name, c->node_info.long_name) != 0) {
        snprintf(c->node_info.long_name, sizeof(c->node_info.long_name), "%s", cmd->long_name);
        long_named = changed = true;
    }

    uint16_t addr = pc->universe_primary;
    uint16_t old_addr = addr;
    if (cmd->reset_address) addr = (uint16_t)(p - 1);   /* default: port N on universe N-1 */
    if (cmd->net >= 0)      addr = (addr & 0x00FF) | ((uint16_t)(cmd->net & 0x7F) << 8);
    if (cmd->sub >= 0)      addr = (addr & 0x7F0F) | ((uint16_t)(cmd->sub & 0x0F) << 4);
    if (cmd->universe >= 0) addr = (addr & 0x7FF0) | (uint16_t)(cmd->universe & 0x0F);
    if (addr != pc->universe_primary) {
        pc->universe_primary = addr;
        changed = true;
    }
    if (cmd->acn_priority <= 200 && cmd->acn_priority != c->node_info.sacn_priority) {
        c->node_info.sacn_priority = cmd->acn_priority;
        changed = true;
    }

    uint8_t loss = c->merge.loss_behavior;
    switch (op) {
        case ARTNET_AC_FAIL_HOLD:  loss = LOSS_HOLD_LAST_LOOK; break;
        case ARTNET_AC_FAIL_ZERO:  loss = LOSS_BLACKOUT; break;
        case ARTNET_AC_FAIL_FULL:  loss = LOSS_FULL; break;
        case ARTNET_AC_FAIL_SCENE: loss = LOSS_SCENE; break;
        default: break;
    }
    if (loss != c->merge.loss_behavior) {
        c->merge.loss_behavior = loss;
        changed = true;
    }

    /* Per-port commands: one port per BindIndex, only port 0 of the page exists */
    if (idx == 0) {
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
        } else if (hi == ARTNET_AC_RDM_ENABLE0 && !pc->rdm_enabled) {
            pc->rdm_enabled = true; changed = true;
        } else if (hi == ARTNET_AC_RDM_DISABLE0 && pc->rdm_enabled) {
            pc->rdm_enabled = false; changed = true;
        }
    }
    if (changed) {
        config_sanitize(c);
    }
    config_unlock();

    if (short_named) artnet_receiver_set_node_report(ARTNET_RC_SH_NAME_OK, "Short name programmed");
    if (long_named) artnet_receiver_set_node_report(ARTNET_RC_LO_NAME_OK, "Long name programmed");
    if (addr != old_addr) ESP_LOGI(TAG, "Port %d Port-Address %u -> %u", p, old_addr, addr);

    switch (op) {
        case ARTNET_AC_LED_NORMAL: g_app.indicator = 0; app_update_indicator(); break;
        case ARTNET_AC_LED_MUTE:   g_app.indicator = 2; app_update_indicator(); break;
        case ARTNET_AC_LED_LOCATE: g_app.indicator = 1; app_update_indicator(); break;
        case ARTNET_AC_RESET_RX_FLAGS:
            for (int i = 0; i < NPORTS; i++) {
                dmx_port_status_t ds;
                if (dmx_handler_get_port_status(i + 1, &ds) == ESP_OK) {
                    g_app.route[i].err_baseline = ds.stats.error_count;
                }
            }
            break;
        case ARTNET_AC_FAIL_RECORD:
#if CONFIG_NODE_SCENES
            /* File writes: done on the main task */
            xEventGroupSetBits(g_app.events, EV_FAILREC);
#else
            artnet_receiver_set_node_report(ARTNET_RC_CONFIG_ERR, "Failsafe record needs scenes");
#endif
            break;
        default:
            if (idx == 0 && hi == ARTNET_AC_CLEAR_OP0) {
#if CONFIG_NODE_SCENES
                scene_release(p, 0);
#endif
                merge_engine_blackout(p);
            }
            break;
    }

    if (changed) {
        xEventGroupSetBits(g_app.events, EV_SAVE | EV_APPLY);
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

static void program_ip(const artnet_ipprog_cmd_t *cmd, artnet_ipprog_reply_t *reply,
                       const network_ip_info_t *nip, const network_sta_info_t *sta_in,
                       struct in_addr from);

void app_on_artnet_ipprog(const artnet_ipprog_cmd_t *cmd, artnet_ipprog_reply_t *reply,
                          void *user_data)
{
    network_ip_info_t ni;
    memset(&ni, 0, sizeof(ni));
    network_get_ip_info(&ni);
    memcpy(reply->ip, &ni.ip, 4);
    memcpy(reply->mask, &ni.netmask, 4);
    memcpy(reply->gateway, &ni.gateway, 4);
    reply->dhcp = ni.dhcp;

    if (!cmd->program) return;   /* query */
    struct in_addr from = {.s_addr = cmd->source_ip};
    network_sta_info_t sta;
    memset(&sta, 0, sizeof(sta));
    if (ni.state == NETWORK_WIFI_STA_CONNECTED) {
        network_get_sta_info(&sta);
    }

    /* Under the settings lock from here (the web UI may be saving) */
    config_lock();
    program_ip(cmd, reply, &ni, &sta, from);
    config_unlock();
}

static void program_ip(const artnet_ipprog_cmd_t *cmd, artnet_ipprog_reply_t *reply,
                       const network_ip_info_t *nip, const network_sta_info_t *sta_in,
                       struct in_addr from)
{
    config_t *c = config_get();
    network_ip_info_t ni = *nip;
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
        config_wifi_profile_t *wp = NULL;
        for (int i = 0; i < c->network.wifi_profile_count; i++) {
            if (strcmp(c->network.wifi_profiles[i].ssid, sta_in->ssid) == 0) {
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
    xEventGroupSetBits(g_app.events, EV_SAVE | EV_NETCFG);
}
