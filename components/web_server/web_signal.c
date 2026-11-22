/*
 * Signal check and captive portal (web_server):
 *   GET /api/network/sources   every Art-Net / sACN sender heard, which ports
 *                              use it, and the node's own addresses, for the
 *                              "why is nothing lighting up?" card
 *   GET /api/network/nodes     the other Art-Net nodes (their ArtPollReply);
 *                              also asks them to announce themselves (ArtPoll)
 *   404 handler                redirects a device on the node's own Wi-Fi to
 *                              the setup page (captive portal)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "config_manager.h"
#include "network_manager.h"
#include "merge_engine.h"
#include "port_rules.h"
#include "source_monitor.h"
#include "dmx_tester.h"
#include "dmx_handler.h"
#include "artnet_receiver.h"
#include "web_internal.h"

static const char *TAG = "web_signal";

static void ip_str(uint32_t ip, char out[16])
{
    const uint8_t *b = (const uint8_t *)&ip;   /* network byte order */
    snprintf(out, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static bool is_output(dmx_mode_t m)
{
    return m == DMX_MODE_OUTPUT || m == DMX_MODE_RDM_MASTER;
}

/* Does output port @p pc take this sender's universe? @p filtered: it would,
 * but its protocol setting drops this protocol. Mirrors main/app_routing.c
 * (a port routed to DMX inputs only takes nothing from the network). */
static bool port_takes(const port_config_t *pc, uint8_t proto, uint16_t universe, bool *filtered)
{
    *filtered = false;
    if (!is_output(pc->mode) || (pc->local_inputs && pc->local_only)) return false;
    bool match, blocked;
    if (proto == SRCMON_ARTNET) {
        match = universe == pc->universe_primary ||
                (pc->merge_mode == MERGE_MODE_BACKUP && pc->universe_secondary >= 0 &&
                 universe == (uint16_t)pc->universe_secondary);
        blocked = pc->protocol_mode == PROTOCOL_SACN_ONLY;
    } else {
        match = universe == pc->sacn_universe;
        blocked = pc->protocol_mode == PROTOCOL_ARTNET_ONLY;
    }
    if (!match) return false;
    *filtered = blocked;
    return !blocked;
}

static void add_interface(cJSON *arr, const char *name, const char *ip, const char *mask)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return;
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddStringToObject(o, "ip", ip);
    cJSON_AddStringToObject(o, "netmask", mask);
    cJSON_AddItemToArray(arr, o);
}

static esp_err_t sources_get(httpd_req_t *req)
{
    if (!web_require_auth(req)) return ESP_OK;
    const config_t *cfg = config_get();

    cJSON *json = cJSON_CreateObject();
    if (!json) {
        web_send_error(req, 500, "Out of memory");
        return ESP_OK;
    }
    /* Tester mode has no Art-Net / sACN sockets */
    cJSON_AddBoolToObject(json, "listening", !dmx_tester_enabled());

    network_supervisor_status_t *sup = calloc(1, sizeof(*sup));
    cJSON *ifs = cJSON_AddArrayToObject(json, "interfaces");
    if (sup && ifs) {
        network_supervisor_get_status(sup);
        if (sup->eth.ready) add_interface(ifs, "ethernet", sup->eth.ip, sup->eth.netmask);
        if (sup->sta.state == NET_STA_CONNECTED) {
            add_interface(ifs, "wifi", sup->sta.ip, sup->sta.netmask);
            cJSON *w = cJSON_GetArrayItem(ifs, cJSON_GetArraySize(ifs) - 1);
            if (w) {
                cJSON_AddNumberToObject(w, "rssi", sup->sta.rssi);
                cJSON_AddStringToObject(w, "ssid", sup->sta.ssid);
                cJSON_AddNumberToObject(w, "channel", sup->sta.channel);
            }
        }
        uint32_t ap_ip, ap_mask;
        if (network_ap_address(&ap_ip, &ap_mask)) {
            char ip[16], mask[16];
            ip_str(ap_ip, ip);
            ip_str(ap_mask, mask);
            add_interface(ifs, "ap", ip, mask);
            cJSON *a = cJSON_GetArrayItem(ifs, cJSON_GetArraySize(ifs) - 1);
            if (a) cJSON_AddNumberToObject(a, "clients", sup->ap.clients);
        }
    }
    free(sup);

    cJSON *ports = cJSON_AddArrayToObject(json, "ports");
    merge_source_info_t *ms = calloc(MERGE_MAX_SOURCES, sizeof(*ms));
    for (int p = 1; ports && p <= CONFIG_DMX_PORTS; p++) {
        const port_config_t *pc = &cfg->ports[p - 1];
        cJSON *o = cJSON_CreateObject();
        if (!o) break;
        cJSON_AddNumberToObject(o, "port", p);
        cJSON_AddNumberToObject(o, "mode", pc->mode);
        cJSON_AddBoolToObject(o, "output", is_output(pc->mode));
        cJSON_AddNumberToObject(o, "protocol_mode", pc->protocol_mode);
        cJSON_AddNumberToObject(o, "merge_mode", pc->merge_mode);
        cJSON_AddNumberToObject(o, "artnet", pc->universe_primary);
        cJSON_AddNumberToObject(o, "artnet_backup", pc->universe_secondary);
        cJSON_AddNumberToObject(o, "sacn", pc->sacn_universe);
        int live = 0, contributing = 0;
        uint8_t n = ms ? merge_engine_get_sources((uint8_t)p, ms, MERGE_MAX_SOURCES) : 0;
        for (uint8_t i = 0; i < n; i++) {
            if (ms[i].age_ms < 2500) live++;
            if (ms[i].contributing) contributing++;
        }
        cJSON_AddNumberToObject(o, "live_sources", live);
        cJSON_AddNumberToObject(o, "contributing", contributing);
        cJSON_AddBoolToObject(o, "rules", port_rules_active((uint8_t)p));
        cJSON *inputs = cJSON_AddArrayToObject(o, "local_inputs");
        for (int k = 1; inputs && is_output(pc->mode) && k <= CONFIG_DMX_PORTS; k++) {
            if (pc->local_inputs & (1u << (k - 1))) cJSON_AddItemToArray(inputs, cJSON_CreateNumber(k));
        }
        cJSON_AddBoolToObject(o, "local_only", is_output(pc->mode) && pc->local_inputs && pc->local_only);
        dmx_port_status_t st;
        bool input = pc->mode == DMX_MODE_INPUT || pc->mode == DMX_MODE_RDM_RESPONDER;
        cJSON_AddBoolToObject(o, "input", input);
        if (input && dmx_handler_get_port_status((uint8_t)p, &st) == ESP_OK) {
            cJSON_AddBoolToObject(o, "signal", st.stats.signal_present);
        }
        cJSON_AddItemToArray(ports, o);
    }
    free(ms);

    srcmon_source_t *src = calloc(SRCMON_MAX_SOURCES, sizeof(*src));
    int ns = src ? srcmon_get_sources(src, SRCMON_MAX_SOURCES) : 0;
    cJSON *arr = cJSON_AddArrayToObject(json, "sources");
    for (int i = 0; arr && i < ns; i++) {
        const srcmon_source_t *s = &src[i];
        cJSON *o = cJSON_CreateObject();
        if (!o) break;
        char ip[16];
        ip_str(s->ip, ip);
        cJSON_AddStringToObject(o, "protocol", s->proto == SRCMON_SACN ? "sacn" : "artnet");
        cJSON_AddStringToObject(o, "ip", ip);
        cJSON_AddStringToObject(o, "name", s->name);
        cJSON_AddNumberToObject(o, "universe", s->universe);
        if (s->proto == SRCMON_SACN) {
            cJSON_AddNumberToObject(o, "priority", s->priority);
            cJSON_AddBoolToObject(o, "pap", s->pap);
            cJSON_AddBoolToObject(o, "preview", s->preview);
        }
        cJSON_AddNumberToObject(o, "slots", s->slots);
        cJSON_AddBoolToObject(o, "sequenced", s->sequenced);
        cJSON_AddNumberToObject(o, "packets", s->packets);
        cJSON_AddNumberToObject(o, "lost", s->lost);
        cJSON_AddNumberToObject(o, "lost_recent", s->lost_recent);
        cJSON_AddNumberToObject(o, "late", s->late);
        cJSON_AddNumberToObject(o, "rate", s->rate_x10 / 10.0);
        cJSON_AddNumberToObject(o, "max_gap_ms", s->max_gap_ms);
        cJSON_AddNumberToObject(o, "age_ms", s->age_ms);
        cJSON *used = cJSON_AddArrayToObject(o, "ports");
        cJSON *filtered = cJSON_AddArrayToObject(o, "filtered");
        for (int p = 1; used && filtered && p <= CONFIG_DMX_PORTS; p++) {
            bool f;
            if (port_takes(&cfg->ports[p - 1], s->proto, s->universe, &f)) {
                cJSON_AddItemToArray(used, cJSON_CreateNumber(p));
            } else if (f) {
                cJSON_AddItemToArray(filtered, cJSON_CreateNumber(p));
            }
        }
        cJSON_AddItemToArray(arr, o);
    }
    free(src);

    srcmon_advert_t *adv = calloc(SRCMON_MAX_ADVERTS, sizeof(*adv));
    int na = adv ? srcmon_get_adverts(adv, SRCMON_MAX_ADVERTS) : 0;
    arr = cJSON_AddArrayToObject(json, "advertised");
    for (int i = 0; arr && i < na; i++) {
        cJSON *o = cJSON_CreateObject();
        if (!o) break;
        char ip[16];
        ip_str(adv[i].ip, ip);
        cJSON_AddStringToObject(o, "ip", ip);
        cJSON_AddStringToObject(o, "name", adv[i].name);
        cJSON *u = cJSON_AddArrayToObject(o, "universes");
        for (int k = 0; u && k < adv[i].stored; k++) {
            cJSON_AddItemToArray(u, cJSON_CreateNumber(adv[i].universes[k]));
        }
        cJSON_AddNumberToObject(o, "count", adv[i].count);
        cJSON_AddNumberToObject(o, "age_ms", adv[i].age_ms);
        cJSON_AddItemToArray(arr, o);
    }
    free(adv);

    web_send_json(req, json, 200);
    cJSON_Delete(json);
    return ESP_OK;
}

static esp_err_t nodes_get(httpd_req_t *req)
{
    if (!web_require_auth(req)) return ESP_OK;
    cJSON *json = cJSON_CreateObject();
    srcmon_node_t *nodes = calloc(SRCMON_MAX_NODES, sizeof(*nodes));
    if (!json || !nodes) {
        cJSON_Delete(json);
        free(nodes);
        web_send_error(req, 500, "Out of memory");
        return ESP_OK;
    }
    bool listening = !dmx_tester_enabled() && artnet_receiver_is_running();
    cJSON_AddBoolToObject(json, "listening", listening);
    /* replies come in over the next second or so: the page asks again */
    cJSON_AddBoolToObject(json, "polled", listening && artnet_receiver_poll_network());

    int n = srcmon_get_nodes(nodes, SRCMON_MAX_NODES);
    cJSON *arr = cJSON_AddArrayToObject(json, "nodes");
    for (int i = 0; arr && i < n; i++) {
        const artnet_poll_reply_t *r = &nodes[i].reply;
        cJSON *o = cJSON_CreateObject();
        if (!o) break;
        char ip[16], mac[18];
        ip_str(r->ip, ip);
        cJSON_AddStringToObject(o, "ip", ip);
        if (r->from_ip != r->ip) {
            ip_str(r->from_ip, ip);
            cJSON_AddStringToObject(o, "from", ip);
        }
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 r->mac[0], r->mac[1], r->mac[2], r->mac[3], r->mac[4], r->mac[5]);
        cJSON_AddStringToObject(o, "mac", mac);
        cJSON_AddStringToObject(o, "short_name", r->short_name);
        cJSON_AddStringToObject(o, "long_name", r->long_name);
        cJSON_AddNumberToObject(o, "firmware", r->firmware);
        cJSON_AddNumberToObject(o, "oem", r->oem);
        cJSON_AddNumberToObject(o, "esta", r->esta);
        cJSON_AddNumberToObject(o, "style", r->style);
        cJSON_AddNumberToObject(o, "bind_index", r->bind_index);
        cJSON_AddBoolToObject(o, "dhcp", (r->status2 & 0x02) != 0);
        cJSON_AddBoolToObject(o, "conflict", nodes[i].ip_conflict);
        cJSON_AddNumberToObject(o, "replies", nodes[i].replies);
        cJSON_AddNumberToObject(o, "age_ms", nodes[i].age_ms);
        cJSON *ports = cJSON_AddArrayToObject(o, "ports");
        for (int k = 0; ports && k < r->num_ports; k++) {
            cJSON *po = cJSON_CreateObject();
            if (!po) break;
            uint8_t t = r->ports[k].type;
            cJSON_AddBoolToObject(po, "output", (t & 0x80) != 0);
            cJSON_AddBoolToObject(po, "input", (t & 0x40) != 0);
            cJSON_AddNumberToObject(po, "protocol", t & 0x3F);   /* 0 = DMX512 */
            cJSON_AddNumberToObject(po, "in", r->ports[k].in_address);
            cJSON_AddNumberToObject(po, "out", r->ports[k].out_address);
            cJSON_AddBoolToObject(po, "in_data", (r->ports[k].good_input & 0x80) != 0);
            cJSON_AddBoolToObject(po, "out_data", (r->ports[k].good_output & 0x80) != 0);
            cJSON_AddItemToArray(ports, po);
        }
        cJSON_AddItemToArray(arr, o);
    }
    free(nodes);
    web_send_json(req, json, 200);
    cJSON_Delete(json);
    return ESP_OK;
}

/*
 * Unknown page. A device on the node's own Wi-Fi checking for internet
 * (captive.apple.com/hotspot-detect.html, connectivitycheck.gstatic.com/
 * generate_204, ...) reaches here because the node answers its DNS: send it
 * to the setup page, and it opens that by itself.
 */
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err)
{
    const config_t *cfg = config_get();
    uint32_t ap_ip, ap_mask;
    if (cfg && cfg->network.ap_captive && network_ap_address(&ap_ip, &ap_mask)) {
        uint32_t client = web_client_ip(req);
        char ap[16], host[64] = "";
        ip_str(ap_ip, ap);
        httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
        char *colon = strchr(host, ':');
        if (colon) *colon = '\0';
        if (client && ((client ^ ap_ip) & ap_mask) == 0 && strcmp(host, ap) != 0) {
            char location[32];
            snprintf(location, sizeof(location), "http://%s/", ap);
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", location);
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            httpd_resp_set_type(req, "text/html");
            char body[96];
            snprintf(body, sizeof(body), "<a href=\"%s\">Ghostlight Node setup</a>", location);
            httpd_resp_sendstr(req, body);
            return ESP_OK;
        }
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_FAIL;   /* as without a handler: close the connection */
}

void web_signal_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        { .uri = "/api/network/sources", .method = HTTP_GET, .handler = sources_get },
        { .uri = "/api/network/nodes", .method = HTTP_GET, .handler = nodes_get },
    };
    _Static_assert(sizeof(routes) / sizeof(routes[0]) == WEB_SIGNAL_ROUTES, "update WEB_SIGNAL_ROUTES");
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        if (httpd_register_uri_handler(server, &routes[i]) != ESP_OK) {
            ESP_LOGE(TAG, "Could not register %s", routes[i].uri);
        }
    }
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found);
}
