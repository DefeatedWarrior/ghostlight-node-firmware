/**
 * @file network_auto_fallback.c
 * @brief Network supervisor: which interfaces run, and when
 *
 * Modes (config network.mode):
 *  - Automatic: Ethernet when a cable with an address is present, otherwise
 *    the saved WiFi networks, otherwise the node's own access point.
 *  - Ethernet only / WiFi only: that interface alone.
 *  - Access point only: standalone, the node is its own network.
 * Outside "access point only" the AP also comes up as a rescue whenever
 * nothing else connects, so the node can always be reached. "Keep access
 * point on" runs it next to Ethernet / WiFi permanently.
 *
 * WiFi: each round scans, then tries the saved networks that are in range
 * (by priority, then signal), then the others (they may be hidden). Each
 * attempt has a time limit and stops early on a final answer such as a
 * wrong password. The result per network is kept for the web UI. Failed
 * rounds back off (5 s ... 60 s, 120 s while someone uses the rescue AP,
 * since every attempt briefly takes the AP off its channel).
 *
 * When the node joins a network while somebody is connected to its AP, the
 * AP stays up for LINGER_MS so that person can read the new address.
 *
 * Settings changes are applied live (network_supervisor_reconfigure()).
 */

#include "network_manager.h"
#include "config_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "net_supervisor";

#define TICK_MS                250
#define ETH_SETTLE_MS          4000     /* W5500 link negotiation after start */
#define ETH_DHCP_WAIT_MS       15000    /* cable in: wait for a lease before WiFi */
#define STA_ATTEMPT_MS         15000    /* per network: join + address */
#define STA_STALL_MS           6000     /* no event at all: kick the driver */
#define STA_RETRY_GAP_MS       2500     /* between retries of the same network */
#define RESCUE_AFTER_MS        30000    /* no connection this long: own AP */
#define LINGER_MS              120000   /* AP kept for its clients after joining */
#define TEST_HOLD_MS           6000     /* "connect now" while WiFi isn't the uplink */
#define SCAN_MAX_AGE_MS        20000
#define BACKOFF_AP_CLIENTS_MS  120000
static const uint32_t s_backoff_ms[] = {5000, 15000, 30000, 60000};
#define BACKOFF_STEPS (sizeof(s_backoff_ms) / sizeof(s_backoff_ms[0]))

#define NPROF CONFIG_WIFI_PROFILES_MAX
_Static_assert(CONFIG_WIFI_PROFILES_MAX <= NETWORK_PROFILES_MAX, "profile table too small");

typedef enum { CMD_RECONFIGURE, CMD_CONNECT, CMD_AP_DISMISS } cmd_type_t;
typedef struct {
    cmd_type_t type;
    char ssid[33];
} cmd_t;

/* Snapshot of the network settings (the config can change under us) */
typedef struct {
    uint8_t mode;
    bool ap_always;
    bool eth_static;
    char eth_ip[16], eth_mask[16], eth_gw[16];
    config_wifi_profile_t prof[NPROF];
    int nprof;
    char ap_ssid[33];
    char ap_pw[65];
    uint8_t ap_ch;
} settings_t;

static struct {
    TaskHandle_t task;
    QueueHandle_t q;
    SemaphoreHandle_t lock;          /* st */
    network_supervisor_status_t st;
    settings_t set;

    /* Ethernet */
    bool eth_absent;                 /* W5500 not found */
    int64_t eth_started_ms;
    int64_t eth_link_ms;             /* link came up at, 0 = down */

    /* WiFi station */
    bool sta_active;                 /* an attempt or a connection is current */
    bool round;                      /* a round is in progress */
    int order[NPROF];
    int norder;
    int cur;                         /* index into order[] */
    int64_t attempt_ms;
    int64_t kick_ms;                 /* last connect / retry */
    uint32_t disc_seen;              /* sta.disconnects already accounted */
    uint32_t disc_at_start;
    int final_hits;
    bool connected;
    int fail_rounds;                 /* consecutive failed rounds */
    int64_t next_round_ms;
    bool test;                       /* "connect now" */
    int64_t test_ok_ms;
    network_profile_status_t pst[NPROF];

    /* Access point */
    bool ap_up;
    int64_t ap_retry_ms;
    network_ap_reason_t ap_reason;
    int64_t no_link_ms;              /* no uplink since, 0 = have one */
    int rounds_since_link;
    int64_t linger_until_ms;
} s;

static wifi_ap_record_t s_recs[24];  /* scan buffer (task only) */

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void copy_bounded(char *dst, size_t dst_size, const char *src)
{
    size_t n = strnlen(src, dst_size - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void load_settings(settings_t *t)
{
    memset(t, 0, sizeof(*t));
    config_lock();   /* a consistent copy while the web UI may be saving */
    const config_t *c = config_get();
    t->mode = c->network.mode <= NET_MODE_LAST ? c->network.mode : NET_MODE_AUTO;
    t->ap_always = c->network.ap_always;
    t->eth_static = c->network.eth_use_static_ip;
    copy_bounded(t->eth_ip, sizeof(t->eth_ip), c->network.eth_static_ip);
    copy_bounded(t->eth_mask, sizeof(t->eth_mask), c->network.eth_netmask);
    copy_bounded(t->eth_gw, sizeof(t->eth_gw), c->network.eth_gateway);
    t->nprof = c->network.wifi_profile_count;
    if (t->nprof > NPROF) t->nprof = NPROF;
    memcpy(t->prof, c->network.wifi_profiles, sizeof(config_wifi_profile_t) * t->nprof);
    copy_bounded(t->ap_ssid, sizeof(t->ap_ssid), c->network.ap_ssid);
    copy_bounded(t->ap_pw, sizeof(t->ap_pw), c->network.ap_password);
    t->ap_ch = c->network.ap_channel;
    config_unlock();
}

static ip_config_t eth_ip_config(const settings_t *t)
{
    ip_config_t ic = {.use_dhcp = !t->eth_static};
    if (t->eth_static) {
        copy_bounded(ic.ip, sizeof(ic.ip), t->eth_ip);
        copy_bounded(ic.netmask, sizeof(ic.netmask), t->eth_mask);
        copy_bounded(ic.gateway, sizeof(ic.gateway), t->eth_gw);
    }
    return ic;
}

/* Ethernet is only ever wanted when it is built in (menuconfig: ESP-NODE >
 * Features > Wired Ethernet); without it "automatic" is Wi-Fi, else own AP */
#if CONFIG_NODE_ETHERNET
#define ETH_BUILT_IN true
#else
#define ETH_BUILT_IN false
#endif

static const char *mode_name(uint8_t m)
{
    switch (m) {
    case NET_MODE_ETHERNET: return "Ethernet only";
    case NET_MODE_WIFI:     return "WiFi only";
    case NET_MODE_AP:       return "access point only";
    default:                return "automatic";
    }
}

static int find_profile(const char *ssid)
{
    for (int i = 0; i < s.set.nprof; i++) {
        if (strcmp(s.set.prof[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

static uint8_t ap_clients(void)
{
    if (!s.ap_up) return 0;
    wifi_sta_info_t sta[4];
    return (uint8_t)network_wifi_ap_get_stations(sta, 4);
}

// ============================================================================
// WiFi station
// ============================================================================

static void sta_off(void)
{
    if (s.sta_active) {
        network_wifi_sta_stop();
    }
    s.sta_active = false;
    s.connected = false;
    s.round = false;
}

static void start_attempt(int64_t now, int idx)
{
    const config_wifi_profile_t *cp = &s.set.prof[idx];
    wifi_profile_t p = {0};
    copy_bounded(p.ssid, sizeof(p.ssid), cp->ssid);
    copy_bounded(p.password, sizeof(p.password), cp->password);
    p.priority = cp->priority;
    p.use_static_ip = cp->use_static_ip;
    if (cp->use_static_ip) {
        p.static_ip.use_dhcp = false;
        copy_bounded(p.static_ip.ip, sizeof(p.static_ip.ip), cp->static_ip);
        copy_bounded(p.static_ip.netmask, sizeof(p.static_ip.netmask), cp->netmask);
        copy_bounded(p.static_ip.gateway, sizeof(p.static_ip.gateway), cp->gateway);
    }

    network_sta_info_t sta;
    network_get_sta_info(&sta);
    s.disc_seen = s.disc_at_start = sta.disconnects;
    s.final_hits = 0;
    s.connected = false;
    s.attempt_ms = now;
    s.kick_ms = now;
    s.pst[idx].tried = true;
    s.pst[idx].reason = 0;
    s.sta_active = true;

    ESP_LOGI(TAG, "Trying WiFi \"%s\"%s", p.ssid, s.pst[idx].visible ? "" : " (not seen in scan)");
    if (network_wifi_sta_connect(&p) != ESP_OK) {
        s.attempt_ms = now - STA_ATTEMPT_MS;   /* give up on the next tick */
    }
}

static void end_round(int64_t now)
{
    sta_off();
    s.fail_rounds++;
    s.rounds_since_link++;
    uint32_t wait = s_backoff_ms[(s.fail_rounds - 1) < (int)BACKOFF_STEPS ?
                                 (s.fail_rounds - 1) : (int)BACKOFF_STEPS - 1];
    if (ap_clients() > 0 && wait < BACKOFF_AP_CLIENTS_MS) wait = BACKOFF_AP_CLIENTS_MS;
    s.next_round_ms = now + wait;
    ESP_LOGW(TAG, "No saved WiFi network joined, next try in %lu s", (unsigned long)(wait / 1000));
}

static void next_candidate(int64_t now)
{
    s.cur++;
    if (s.cur >= s.norder) {
        bool was_test = s.test;
        s.test = false;
        if (was_test) {
            /* a failed "connect now" doesn't count against the back-off */
            sta_off();
            s.next_round_ms = now + s_backoff_ms[0];
        } else {
            end_round(now);
        }
        return;
    }
    start_attempt(now, s.order[s.cur]);
}

/* Order of this round: in range by priority then signal, then the rest */
static void start_round(int64_t now)
{
    uint16_t n = network_wifi_scan_cached(s_recs, sizeof(s_recs) / sizeof(s_recs[0]),
                                          SCAN_MAX_AGE_MS);
    for (int i = 0; i < s.set.nprof; i++) {
        s.pst[i].visible = false;
        s.pst[i].rssi = -127;
        for (int r = 0; r < n; r++) {
            if (strncmp((const char *)s_recs[r].ssid, s.set.prof[i].ssid, 33) == 0) {
                s.pst[i].visible = true;
                if (s_recs[r].rssi > s.pst[i].rssi) s.pst[i].rssi = s_recs[r].rssi;
            }
        }
    }

    s.norder = 0;
    for (int i = 0; i < s.set.nprof; i++) s.order[s.norder++] = i;
    for (int i = 1; i < s.norder; i++) {   /* insertion sort, n <= 5 */
        int v = s.order[i], j = i - 1;
        while (j >= 0) {
            const network_profile_status_t *a = &s.pst[s.order[j]], *b = &s.pst[v];
            int pa = s.set.prof[s.order[j]].priority, pb = s.set.prof[v].priority;
            bool after = (a->visible != b->visible) ? !a->visible :
                         (pa != pb) ? pa > pb : a->rssi < b->rssi;
            if (!after) break;
            s.order[j + 1] = s.order[j];
            j--;
        }
        s.order[j + 1] = v;
    }

    s.round = true;
    s.cur = -1;
    next_candidate(now);
}

static void sta_service(int64_t now, bool wanted, bool run)
{
    if (!run || s.set.nprof == 0) {
        if (s.sta_active || s.round) sta_off();
        s.test = false;
        return;
    }

    network_sta_info_t sta;
    network_get_sta_info(&sta);

    if (!s.sta_active) {
        if (now >= s.next_round_ms) start_round(now);
        return;
    }

    int idx = (s.cur >= 0 && s.cur < s.norder) ? s.order[s.cur] : -1;
    if (idx < 0) {
        sta_off();
        return;
    }
    const char *ssid = s.set.prof[idx].ssid;

    if (sta.state == NET_STA_CONNECTED) {
        if (!s.connected) {
            s.connected = true;
            s.fail_rounds = 0;
            s.pst[idx].reason = 0;
            ESP_LOGI(TAG, "WiFi \"%s\" connected, address %s (signal %d dBm)",
                     ssid, sta.ip, sta.rssi);
            if (s.test) s.test_ok_ms = now;
        }
        if (s.test) {
            if (wanted) {
                s.test = false;              /* it is the connection now */
            } else if (now - s.test_ok_ms > TEST_HOLD_MS) {
                ESP_LOGI(TAG, "WiFi \"%s\" works; not needed in this mode, leaving it", ssid);
                s.test = false;
                sta_off();
                s.next_round_ms = now;
            }
        }
        return;
    }

    if (s.connected) {
        /* Dropped: the driver doesn't reconnect itself; retry this network */
        s.connected = false;
        s.attempt_ms = now;
        s.kick_ms = now;
        s.disc_seen = s.disc_at_start = sta.disconnects;
        s.final_hits = 0;
        ESP_LOGW(TAG, "WiFi \"%s\" lost (%s), reconnecting", ssid,
                 network_wifi_reason_text(sta.reason));
        network_wifi_sta_retry();
        return;
    }

    /* Attempt in progress */
    if (sta.disconnects != s.disc_seen) {
        s.disc_seen = sta.disconnects;
        s.pst[idx].reason = sta.reason;
        if (network_wifi_reason_is_final(sta.reason)) s.final_hits++;
    }
    /* Wrong password / not found get one retry (handshake timeouts can be
     * a weak signal); not found for a network missing from the scan, or a
     * security mismatch, give up at once. */
    int final_limit = 2;
    if (sta.reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
        sta.reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD ||
        sta.reason == WIFI_REASON_802_1X_AUTH_FAILED ||
        (sta.reason == WIFI_REASON_NO_AP_FOUND && !s.pst[idx].visible)) {
        final_limit = 1;
    }
    int64_t age = now - s.attempt_ms;
    if (s.final_hits >= final_limit || age > STA_ATTEMPT_MS) {
        if (age > STA_ATTEMPT_MS && s.final_hits < final_limit) {
            s.pst[idx].reason = (sta.state == NET_STA_GETTING_IP) ? NETWORK_REASON_NO_DHCP :
                                s.pst[idx].reason ? s.pst[idx].reason : NETWORK_REASON_TIMEOUT;
        }
        ESP_LOGW(TAG, "WiFi \"%s\": %s", ssid, network_wifi_reason_text(s.pst[idx].reason));
        next_candidate(now);
        return;
    }
    if (sta.state == NET_STA_FAILED && now - s.kick_ms > STA_RETRY_GAP_MS) {
        s.kick_ms = now;
        network_wifi_sta_retry();
    } else if (sta.state == NET_STA_CONNECTING && sta.disconnects == s.disc_at_start &&
               now - s.kick_ms > STA_STALL_MS) {
        s.kick_ms = now;                     /* connect request got lost */
        network_wifi_sta_retry();
    }
}

// ============================================================================
// Commands
// ============================================================================

static void reconfigure(int64_t now)
{
    settings_t old = s.set;
    load_settings(&s.set);
    const settings_t *t = &s.set;

    if (t->mode != old.mode || t->ap_always != old.ap_always) {
        ESP_LOGI(TAG, "Network mode: %s%s", mode_name(t->mode),
                 t->ap_always ? ", access point always on" : "");
        s.no_link_ms = 0;
        s.rounds_since_link = 0;
        s.next_round_ms = 0;
        s.fail_rounds = 0;
    }

    if (t->eth_static != old.eth_static || strcmp(t->eth_ip, old.eth_ip) ||
        strcmp(t->eth_mask, old.eth_mask) || strcmp(t->eth_gw, old.eth_gw)) {
        ip_config_t ic = eth_ip_config(t);
        network_ethernet_set_ip(&ic);   /* no-op until the driver is installed */
    }

    if (s.ap_up && (strcmp(t->ap_ssid, old.ap_ssid) || strcmp(t->ap_pw, old.ap_pw) ||
                    t->ap_ch != old.ap_ch)) {
        ESP_LOGI(TAG, "Access point settings changed, restarting it");
        network_wifi_ap_start(t->ap_ssid, t->ap_pw, t->ap_ch, NULL);
    }

    if (t->nprof != old.nprof ||
        memcmp(t->prof, old.prof, sizeof(config_wifi_profile_t) * t->nprof) != 0) {
        /* Keep a working connection whose settings didn't change */
        bool keep = false;
        if (s.connected && s.cur >= 0 && s.cur < s.norder) {
            const config_wifi_profile_t *cur = &old.prof[s.order[s.cur]];
            int ni = find_profile(cur->ssid);
            /* The address fields only matter with a static address: on
             * DHCP, edits to the unused fields keep the connection */
            if (ni >= 0 && strcmp(t->prof[ni].password, cur->password) == 0 &&
                t->prof[ni].use_static_ip == cur->use_static_ip &&
                (!cur->use_static_ip ||
                 (strcmp(t->prof[ni].static_ip, cur->static_ip) == 0 &&
                  strcmp(t->prof[ni].netmask, cur->netmask) == 0 &&
                  strcmp(t->prof[ni].gateway, cur->gateway) == 0))) {
                keep = true;
                s.order[0] = ni;
                s.norder = 1;
                s.cur = 0;
            }
        }
        network_profile_status_t oldst[NPROF];
        memcpy(oldst, s.pst, sizeof(oldst));
        memset(s.pst, 0, sizeof(s.pst));
        for (int i = 0; i < t->nprof; i++) {
            for (int j = 0; j < old.nprof; j++) {
                if (strcmp(t->prof[i].ssid, old.prof[j].ssid) == 0 &&
                    strcmp(t->prof[i].password, old.prof[j].password) == 0) {
                    s.pst[i] = oldst[j];     /* unchanged network: keep its result */
                }
            }
        }
        if (!keep) {
            sta_off();
            s.test = false;
        }
        s.fail_rounds = 0;
        s.next_round_ms = 0;
    }
    (void)now;
}

static void handle_cmd(const cmd_t *cmd, int64_t now)
{
    switch (cmd->type) {
    case CMD_RECONFIGURE:
        reconfigure(now);
        break;
    case CMD_CONNECT: {
        reconfigure(now);   /* the network may have just been saved */
        int idx = find_profile(cmd->ssid);
        if (idx < 0) break;
        if (s.connected && s.cur >= 0 && s.cur < s.norder && s.order[s.cur] == idx) {
            break;          /* already on it */
        }
        sta_off();
        s.test = true;
        s.round = true;
        s.order[0] = idx;
        s.norder = 1;
        s.cur = -1;
        next_candidate(now);
        break;
    }
    case CMD_AP_DISMISS:
        if (s.ap_reason == NET_AP_LINGER) s.linger_until_ms = now;
        break;
    }
}

// ============================================================================
// Main loop
// ============================================================================

static void tick(int64_t now)
{
    const settings_t *t = &s.set;
    network_eth_info_t eth;
    network_get_eth_info(&eth);

    /* ---- Ethernet ---- */
    bool eth_wanted = ETH_BUILT_IN && (t->mode == NET_MODE_AUTO || t->mode == NET_MODE_ETHERNET) &&
                      !s.eth_absent;
    if (eth_wanted && !eth.running) {
        ip_config_t ic = eth_ip_config(t);
        esp_err_t err = network_ethernet_connect(&ic);
        if (err == ESP_OK) {
            s.eth_started_ms = now;
        } else {
            s.eth_absent = true;
            ESP_LOGW(TAG, "%s", err == ESP_ERR_NOT_SUPPORTED ? "Ethernet is not built into this firmware"
                                                             : "No Ethernet (W5500 not found)");
        }
        network_get_eth_info(&eth);
    } else if (!eth_wanted && eth.running) {
        network_ethernet_disconnect();
        network_get_eth_info(&eth);
    }
    if (eth.link) {
        if (!s.eth_link_ms) s.eth_link_ms = now;
    } else {
        s.eth_link_ms = 0;
    }
    bool eth_ready = eth_wanted && eth.ready;
    bool eth_pending = eth_wanted && eth.running && !eth.ready &&
                       (now - s.eth_started_ms < ETH_SETTLE_MS ||
                        (eth.link && now - s.eth_link_ms < ETH_DHCP_WAIT_MS));

    /* ---- WiFi station ---- */
    bool sta_wanted = t->nprof > 0 &&
                      (t->mode == NET_MODE_WIFI ||
                       (t->mode == NET_MODE_AUTO && !eth_ready && !eth_pending));
    sta_service(now, sta_wanted, sta_wanted || s.test);
    bool sta_ready = sta_wanted && s.connected;

    /* ---- Access point ---- */
    bool uplink = eth_ready || sta_ready;
    if (uplink) {
        s.no_link_ms = 0;
        s.rounds_since_link = 0;
    } else if (!s.no_link_ms) {
        s.no_link_ms = now;
    }
    bool trying = eth_pending || (sta_wanted && s.rounds_since_link == 0);
    bool rescue = !uplink && t->mode != NET_MODE_AP &&
                  (!trying || now - s.no_link_ms > RESCUE_AFTER_MS);

    uint8_t clients = ap_clients();
    network_ap_reason_t want = NET_AP_OFF;
    if (t->mode == NET_MODE_AP) {
        want = NET_AP_STANDALONE;
    } else if (t->ap_always) {
        want = NET_AP_ALWAYS;
    } else if (rescue) {
        want = NET_AP_RESCUE;
    } else if (s.ap_up && clients > 0) {
        /* Joined a network while someone is on the AP: keep it a while */
        if (s.ap_reason != NET_AP_LINGER) {
            s.linger_until_ms = now + LINGER_MS;
            want = NET_AP_LINGER;
            ESP_LOGI(TAG, "Connected; access point stays up %d s for its clients",
                     LINGER_MS / 1000);
        } else if (now < s.linger_until_ms) {
            want = NET_AP_LINGER;
        }
    }

    if (want != NET_AP_OFF && !s.ap_up && now >= s.ap_retry_ms) {
        static const char *why[] = {"", "standalone", "always on", "no other connection", ""};
        ESP_LOGI(TAG, "Access point \"%s\" at http://" NETWORK_AP_IP "/ (%s)",
                 t->ap_ssid, why[want]);
        if (network_wifi_ap_start(t->ap_ssid, t->ap_pw, t->ap_ch, NULL) == ESP_OK) {
            s.ap_up = true;
        } else {
            s.ap_retry_ms = now + 5000;
        }
    } else if (want == NET_AP_OFF && s.ap_up) {
        network_wifi_ap_stop();
        s.ap_up = false;
    }
    s.ap_reason = s.ap_up ? want : NET_AP_OFF;

    /* ---- Status for the web UI ---- */
    network_supervisor_status_t st = {0};
    st.mode = t->mode;
    st.ap_always = t->ap_always;
    st.eth_wanted = ETH_BUILT_IN && (t->mode == NET_MODE_AUTO || t->mode == NET_MODE_ETHERNET);
    st.eth = eth;
    st.sta_wanted = sta_wanted;
    network_get_sta_info(&st.sta);
    st.profile_count = (uint8_t)t->nprof;
    for (int i = 0; i < t->nprof; i++) {
        st.profiles[i] = s.pst[i];
        copy_bounded(st.profiles[i].ssid, sizeof(st.profiles[i].ssid), t->prof[i].ssid);
    }
    st.sta_retry_in_s = (sta_wanted && !s.sta_active && s.next_round_ms > now) ?
                        (uint32_t)((s.next_round_ms - now + 999) / 1000) : 0;
    st.sta_test = s.test;
    network_get_ap_info(&st.ap);
    st.ap_reason = s.ap_reason;
    st.ap_off_in_s = (s.ap_reason == NET_AP_LINGER && s.linger_until_ms > now) ?
                     (uint32_t)((s.linger_until_ms - now + 999) / 1000) : 0;
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.st = st;
    xSemaphoreGive(s.lock);
}

static void supervisor_task(void *arg)
{
    load_settings(&s.set);
    ESP_LOGI(TAG, "Network supervisor started: %s%s, %d saved WiFi network(s)",
             mode_name(s.set.mode), s.set.ap_always ? ", access point always on" : "",
             s.set.nprof);
    /* Task watchdog: a supervisor stuck in a driver call restarts the node
     * (CONFIG_ESP_TASK_WDT_PANIC) instead of leaving it unreachable */
    esp_task_wdt_add(NULL);
    for (;;) {
        esp_task_wdt_reset();
        cmd_t cmd;
        if (xQueueReceive(s.q, &cmd, pdMS_TO_TICKS(TICK_MS)) == pdTRUE) {
            handle_cmd(&cmd, now_ms());
        }
        tick(now_ms());
    }
}

esp_err_t network_start_with_fallback(void)
{
    if (s.task) return ESP_OK;
    s.q = xQueueCreate(4, sizeof(cmd_t));
    s.lock = xSemaphoreCreateMutex();
    if (!s.q || !s.lock) return ESP_ERR_NO_MEM;
    if (xTaskCreate(supervisor_task, "net_supervisor", 6144, NULL, 5, &s.task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create network supervisor task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t post(cmd_type_t type, const char *ssid)
{
    if (!s.q) return ESP_ERR_INVALID_STATE;
    cmd_t cmd = {.type = type};
    if (ssid) copy_bounded(cmd.ssid, sizeof(cmd.ssid), ssid);
    return xQueueSend(s.q, &cmd, pdMS_TO_TICKS(100)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void network_supervisor_reconfigure(void)
{
    post(CMD_RECONFIGURE, NULL);
}

esp_err_t network_supervisor_connect_now(const char *ssid)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    return post(CMD_CONNECT, ssid);
}

void network_supervisor_ap_dismiss(void)
{
    post(CMD_AP_DISMISS, NULL);
}

void network_supervisor_get_status(network_supervisor_status_t *status)
{
    if (!status) return;
    if (!s.lock) {
        memset(status, 0, sizeof(*status));
        return;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    *status = s.st;
    xSemaphoreGive(s.lock);
}
