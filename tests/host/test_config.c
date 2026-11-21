/* Unit tests: components/config_manager (JSON round trip and sanitising). */
#include "config_manager.h"
#include "fakes.h"
#include "cJSON.h"
#include <stdlib.h>

static void reset(void);
#define TEST_SETUP() reset()
#include "test.h"

static config_t *cfg;

static void reset(void)
{
    host_fakes_reset();
    config_init();
    cfg = config_get();
}

static config_t defaults(void)
{
    config_t saved = *cfg, d;
    config_init();
    d = *cfg;
    *cfg = saved;
    return d;
}

/* Every field set to a valid value that differs from the default */
static void set_everything(config_t *c)
{
    c->network.mode = NET_MODE_ETHERNET;
    c->network.ap_always = true;
    c->network.eth_use_static_ip = true;
    snprintf(c->network.eth_static_ip, 16, "10.1.2.3");
    snprintf(c->network.eth_gateway, 16, "10.1.2.254");
    snprintf(c->network.eth_netmask, 16, "255.255.0.0");
    snprintf(c->network.ap_ssid, 32, "Rig \"AP\" \\ é");
    snprintf(c->network.ap_password, 64, "%s", "pass word with spaces & <tags>");
    c->network.ap_channel = 11;
    c->network.wifi_profile_count = 2;
    snprintf(c->network.wifi_profiles[0].ssid, 33, "%s", "VenueNet-0123456789abcdef-012345");   /* 32 */
    snprintf(c->network.wifi_profiles[0].password, 65, "%s",
             "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");          /* 64 */
    c->network.wifi_profiles[0].priority = 200;
    c->network.wifi_profiles[1].use_static_ip = true;
    snprintf(c->network.wifi_profiles[1].ssid, 33, "Backup");
    snprintf(c->network.wifi_profiles[1].password, 65, "backup-pass");
    snprintf(c->network.wifi_profiles[1].static_ip, 16, "192.168.50.7");
    snprintf(c->network.wifi_profiles[1].gateway, 16, "192.168.50.1");
    snprintf(c->network.wifi_profiles[1].netmask, 16, "255.255.255.0");
    for (int i = 0; i < CONFIG_DMX_PORTS; i++) {
        port_config_t *p = &c->ports[i];
        p->mode = i ? DMX_MODE_RDM_RESPONDER : DMX_MODE_INPUT;
        p->universe_primary = (uint16_t)(32767 - i);
        p->universe_secondary = (int16_t)(100 + i);
        p->universe_offset = (int16_t)(-5 - i);
        p->sacn_universe = (uint16_t)(63999 - i);
        p->protocol_mode = PROTOCOL_SACN_PRIORITY;
        p->merge_mode = MERGE_MODE_BACKUP;
        p->rdm_enabled = false;
        p->out_rate_hz = 30;
        p->break_us = 300;
        p->mab_us = 40;
        p->out_slots = 128;
        p->startup_scene = 7;
        p->failsafe_scene = 255;
    }
    c->merge.timeout_seconds = 60;
    c->merge.loss_behavior = LOSS_SCENE;
    snprintf(c->node_info.short_name, 18, "Stage-Left-Node!!");    /* 17 */
    snprintf(c->node_info.long_name, 64, "%s",
             "A long name that fills all sixty-three bytes of the field.....");
    c->node_info.sacn_priority = 200;
    c->node_info.allow_ip_prog = false;
}

/* ---- round trip --------------------------------------------------------- */

static void test_defaults_are_clean(void)
{
    config_t c = *cfg;
    CHECK(!config_sanitize(&c));
    CHECK_MEM(&c, cfg, sizeof(c));
    CHECK_EQ(cfg->ports[0].universe_primary, 0);
    CHECK_EQ(cfg->ports[0].sacn_universe, 1);
    CHECK_EQ(cfg->ports[1].universe_primary, 1);
    CHECK_EQ(cfg->ports[1].sacn_universe, 2);
    CHECK_STR(cfg->network.ap_ssid, "ESP-NODE-CDEF");
}

static void test_round_trip_every_field(void)
{
    set_everything(cfg);
    config_t want = *cfg;
    CHECK(!config_sanitize(&want));          /* the values above are all valid */
    CHECK_MEM(&want, cfg, sizeof(want));

    char *json = NULL;
    CHECK_EQ(config_to_json(&json), ESP_OK);
    CHECK(json != NULL);
    config_init();                           /* back to defaults */
    CHECK_EQ(config_from_json(json), ESP_OK);
    CHECK_MEM(cfg, &want, sizeof(want));

    /* and once more: the output is stable */
    char *json2 = NULL;
    CHECK_EQ(config_to_json(&json2), ESP_OK);
    int same = strcmp(json, json2) == 0;
    free(json);
    free(json2);
    CHECK(same);
}

static void test_round_trip_through_file(void)
{
    set_everything(cfg);
    snprintf(cfg->security.admin_hash, sizeof(cfg->security.admin_hash), "%064d", 1);
    snprintf(cfg->security.admin_salt, sizeof(cfg->security.admin_salt), "%032d", 2);
    cfg->security.admin_iterations = 4096;
    config_t want = *cfg;
    CHECK_EQ(config_save(), ESP_OK);
    CHECK(host_storage_data != NULL);
    config_init();
    CHECK_EQ(config_load(), ESP_OK);
    CHECK_MEM(cfg, &want, sizeof(want));
    CHECK_EQ(host_storage_writes, 1);        /* a clean file is not rewritten */
}

static void test_partial_update(void)
{
    config_t before = *cfg;
    CHECK_EQ(config_from_json("{\"port2\":{\"merge_mode\":1},\"node_info\":{\"short_name\":\"X\"}}"), ESP_OK);
    CHECK_EQ(cfg->ports[1].merge_mode, MERGE_MODE_LTP);
    CHECK_STR(cfg->node_info.short_name, "X");
    cfg->ports[1].merge_mode = before.ports[1].merge_mode;
    memcpy(cfg->node_info.short_name, before.node_info.short_name, sizeof(before.node_info.short_name));
    CHECK_MEM(cfg, &before, sizeof(before));
}

static void test_bad_json_leaves_config_alone(void)
{
    config_t before = *cfg;
    static const char *bad[] = {
        "", "{", "[]", "null", "42", "\"str\"", "{\"network\":", "{\"a\":1,}", "\xff\xfe",
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK_EQ(config_from_json(bad[i]), ESP_FAIL);
    }
    CHECK_EQ(config_from_json(NULL), ESP_ERR_INVALID_ARG);
    CHECK_MEM(cfg, &before, sizeof(before));
}

static void test_wrong_types_ignored(void)
{
    config_t before = *cfg;
    CHECK_EQ(config_from_json(
        "{\"network\":[],\"port1\":\"x\",\"port2\":{\"mode\":\"2\",\"rdm_enabled\":\"yes\","
        "\"universe_primary\":null},\"merge\":5,\"node_info\":{\"short_name\":7,"
        "\"long_name\":{}}}"), ESP_OK);
    CHECK_MEM(cfg, &before, sizeof(before));
}

static void test_from_json_clamps_numbers(void)
{
    CHECK_EQ(config_from_json(
        "{\"network\":{\"mode\":99,\"ap_channel\":0},"
        "\"port1\":{\"mode\":-3,\"universe_primary\":1e9,\"universe_secondary\":-50,"
        "\"sacn_universe\":0,\"protocol_mode\":7,\"merge_mode\":-1,\"out_rate_hz\":1000,"
        "\"break_us\":5,\"mab_us\":1e99,\"out_slots\":1,\"startup_scene\":300,"
        "\"failsafe_scene\":-1e99,\"universe_offset\":99999},"
        "\"merge\":{\"timeout_seconds\":0,\"loss_behavior\":4.5},"
        "\"node_info\":{\"sacn_priority\":255}}"), ESP_OK);
    CHECK_EQ(cfg->network.mode, NET_MODE_LAST);
    CHECK_EQ(cfg->network.ap_channel, 1);
    port_config_t *p = &cfg->ports[0];
    CHECK_EQ(p->mode, DMX_MODE_DISABLED);
    CHECK_EQ(p->universe_primary, CONFIG_ARTNET_UNIVERSE_MAX);
    CHECK_EQ(p->universe_secondary, -1);
    CHECK_EQ(p->sacn_universe, CONFIG_SACN_UNIVERSE_MIN);
    CHECK_EQ(p->protocol_mode, PROTOCOL_MERGE_BOTH);
    CHECK_EQ(p->merge_mode, MERGE_MODE_HTP);
    CHECK_EQ(p->out_rate_hz, CONFIG_DMX_RATE_MAX);
    CHECK_EQ(p->break_us, CONFIG_DMX_BREAK_MIN);
    CHECK_EQ(p->mab_us, CONFIG_DMX_MAB_MAX);
    CHECK_EQ(p->out_slots, CONFIG_DMX_SLOTS_MIN);
    CHECK_EQ(p->startup_scene, 255);
    CHECK_EQ(p->failsafe_scene, 0);
    CHECK_EQ(p->universe_offset, 32767);
    CHECK_EQ(cfg->merge.timeout_seconds, 1);
    CHECK_EQ(cfg->merge.loss_behavior, LOSS_SCENE);
    CHECK_EQ(cfg->node_info.sacn_priority, 200);
    config_t c = *cfg;
    CHECK(!config_sanitize(&c));
}

static void test_bool_fields_accept_numbers(void)
{
    CHECK_EQ(config_from_json("{\"network\":{\"ap_always\":1},\"node_info\":{\"allow_ip_prog\":0},"
                              "\"port1\":{\"rdm_enabled\":false}}"), ESP_OK);
    CHECK(cfg->network.ap_always);
    CHECK(!cfg->node_info.allow_ip_prog);
    CHECK(!cfg->ports[0].rdm_enabled);
}

static void test_long_strings_truncated(void)
{
    char json[2048];
    char longs[300];
    memset(longs, 'z', sizeof(longs) - 1);
    longs[sizeof(longs) - 1] = '\0';
    snprintf(json, sizeof(json),
             "{\"network\":{\"ap_ssid\":\"%s\",\"eth_static_ip\":\"%s\"},"
             "\"node_info\":{\"short_name\":\"%s\",\"long_name\":\"%s\"}}",
             longs, longs, longs, longs);
    CHECK_EQ(config_from_json(json), ESP_OK);
    CHECK_EQ(strlen(cfg->network.ap_ssid), sizeof(cfg->network.ap_ssid) - 1);
    CHECK_EQ(strlen(cfg->network.eth_static_ip), sizeof(cfg->network.eth_static_ip) - 1);
    CHECK_EQ(strlen(cfg->node_info.short_name), sizeof(cfg->node_info.short_name) - 1);
    CHECK_EQ(strlen(cfg->node_info.long_name), sizeof(cfg->node_info.long_name) - 1);
}

static void test_web_api_cannot_set_credentials(void)
{
    CHECK_EQ(config_from_json("{\"security\":{\"admin_hash\":\"0000000000000000000000000000000000000000000000000000000000000000\","
                              "\"admin_salt\":\"00000000000000000000000000000000\",\"admin_iterations\":4096}}"),
             ESP_OK);
    CHECK(!config_admin_password_set());
    CHECK_EQ(config_from_json("{\"security\":{\"no_login\":true}}"), ESP_OK);
    CHECK(!config_setup_done());
}

static void test_admin_password(void)
{
    CHECK(!config_check_admin_password("whatever1"));
    CHECK_EQ(config_set_admin_password("short"), ESP_ERR_INVALID_ARG);
    CHECK_EQ(config_set_admin_password("correct horse"), ESP_OK);
    CHECK(config_admin_password_set());
    CHECK(config_check_admin_password("correct horse"));
    CHECK(!config_check_admin_password("correct horsf"));
    CHECK(!config_check_admin_password(NULL));
    config_t c = *cfg;
    CHECK(!config_sanitize(&c));             /* a stored credential is valid */
}

static void test_no_login(void)
{
    CHECK(!config_setup_done());             /* factory state: first-run setup */
    CHECK(!config_login_required());
    CHECK_EQ(config_set_no_login(), ESP_OK);
    CHECK(config_setup_done());
    CHECK(!config_login_required());
    CHECK(!config_admin_password_set());

    /* survives a restart */
    config_init();
    CHECK(!config_setup_done());
    CHECK_EQ(config_load(), ESP_OK);
    CHECK(config_setup_done());
    CHECK(!config_login_required());

    /* setting a password turns sign-in back on */
    CHECK_EQ(config_set_admin_password("correct horse"), ESP_OK);
    CHECK(config_login_required());
    CHECK(!cfg->security.no_login);
    config_init();
    CHECK_EQ(config_load(), ESP_OK);
    CHECK(config_login_required());

    /* and turning it off again forgets the password */
    CHECK_EQ(config_set_no_login(), ESP_OK);
    CHECK(!config_login_required());
    CHECK(!config_check_admin_password("correct horse"));
}

/* ---- Shared checks of the web API and the USB setup -------------------- */

static void test_update_problem(void)
{
    struct { const char *json; bool ok; } cases[] = {
        { "{}", true },
        { "{\"node_info\":{\"short_name\":\"x\"}}", true },
        { "{\"network\":{\"ap_password\":\"a good one\"}}", true },
        { "{\"network\":{\"ap_password\":\"12345678\"}}", false },   /* factory default */
        { "{\"network\":{\"ap_password\":\"short\"}}", false },
        { "{\"network\":{\"ap_password\":\"\"}}", true },            /* open network */
        { "{\"network\":{\"ap_password\":5}}", false },
        { "{\"network\":{\"wifi_profiles\":[{\"ssid\":\"A\",\"password\":\"\"}]}}", true },
        { "{\"network\":{\"wifi_profiles\":[{\"ssid\":\"A\",\"password\":\"1234567\"}]}}", false },
        { "{\"network\":{\"wifi_profiles\":[{\"ssid\":\"A\",\"password\":\"12345678\"}]}}", true },
        { "[1,2]", false },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        cJSON *doc = cJSON_Parse(cases[i].json);
        CHECK(doc != NULL);
        const char *problem = config_update_problem(doc);
        if ((problem == NULL) != cases[i].ok) {
            printf("    %s -> %s\n", cases[i].json, problem ? problem : "accepted");
        }
        CHECK_EQ(problem == NULL, cases[i].ok);
        cJSON_Delete(doc);
    }
    CHECK(config_ap_password_problem(NULL) != NULL);
}

static void test_public_json_has_no_secrets(void)
{
    CHECK_EQ(config_set_admin_password("correct horse"), ESP_OK);
    CHECK_EQ(config_from_json("{\"network\":{\"ap_password\":\"ap secret 1\","
                              "\"wifi_profiles\":[{\"ssid\":\"Venue\",\"password\":\"wifi secret\"}]}}"),
             ESP_OK);
    cJSON *pub = config_to_public_json(cfg);
    CHECK(pub != NULL);
    char *s = cJSON_PrintUnformatted(pub);
    CHECK(s != NULL);
    CHECK(strstr(s, "ap secret 1") == NULL);
    CHECK(strstr(s, "wifi secret") == NULL);
    CHECK(strstr(s, "security") == NULL);
    CHECK(strstr(s, "admin_hash") == NULL);
    CHECK(strstr(s, "\"has_password\":true") != NULL);
    CHECK(strstr(s, "\"ap_open\":false") != NULL);
    /* It is the shape config_from_json() takes: feeding it back changes nothing */
    config_t before = *cfg;
    CHECK_EQ(config_from_json(s), ESP_OK);
    CHECK_MEM(&before, cfg, sizeof(before));
    cJSON_free(s);
    cJSON_Delete(pub);
}

/* ---- WiFi profiles ------------------------------------------------------- */

static void test_wifi_profiles(void)
{
    CHECK_EQ(config_from_json(
        "{\"network\":{\"wifi_profiles\":["
        "{\"ssid\":\"A\",\"password\":\"pa\"},"
        "{\"ssid\":\"\"},"                       /* empty: skipped */
        "{\"password\":\"x\"},"                  /* no SSID: skipped */
        "7,"                                     /* not an object */
        "{\"ssid\":\"A\",\"password\":\"dup\"},"  /* duplicate: skipped */
        "{\"ssid\":\"B\",\"password\":\"pb\",\"priority\":999,\"use_static_ip\":true},"
        "{\"ssid\":\"C\"},{\"ssid\":\"D\"},{\"ssid\":\"E\"},{\"ssid\":\"F\"}"   /* F: over the limit */
        "]}}"), ESP_OK);
    CHECK_EQ(cfg->network.wifi_profile_count, CONFIG_WIFI_PROFILES_MAX);
    CHECK_STR(cfg->network.wifi_profiles[0].ssid, "A");
    CHECK_STR(cfg->network.wifi_profiles[0].password, "pa");
    CHECK_STR(cfg->network.wifi_profiles[1].ssid, "B");
    CHECK_EQ(cfg->network.wifi_profiles[1].priority, 255);
    CHECK(cfg->network.wifi_profiles[1].use_static_ip);
    CHECK_STR(cfg->network.wifi_profiles[4].ssid, "E");

    /* The web UI sends profiles without passwords: the stored one is kept */
    CHECK_EQ(config_from_json("{\"network\":{\"wifi_profiles\":[{\"ssid\":\"B\"},{\"ssid\":\"New\"}]}}"), ESP_OK);
    CHECK_EQ(cfg->network.wifi_profile_count, 2);
    CHECK_STR(cfg->network.wifi_profiles[0].password, "pb");
    CHECK_STR(cfg->network.wifi_profiles[1].password, "");
    CHECK_STR(cfg->network.wifi_profiles[2].ssid, "");

    CHECK_EQ(config_from_json("{\"network\":{\"wifi_profiles\":[]}}"), ESP_OK);
    CHECK_EQ(cfg->network.wifi_profile_count, 0);
}

/* ---- loading from storage ------------------------------------------------- */

static void test_load_missing_file_saves_defaults(void)
{
    CHECK_EQ(config_load(), ESP_OK);
    CHECK_EQ(host_storage_writes, 1);
    CHECK(host_storage_data != NULL);
}

static void test_load_corrupt_file_uses_defaults(void)
{
    config_t want = defaults();
    cfg->ports[0].merge_mode = MERGE_MODE_LTP;
    host_storage_put("config.json", "{\"port1\":", 9);
    CHECK_EQ(config_load(), ESP_OK);
    CHECK_MEM(cfg, &want, sizeof(want));
    CHECK_EQ(host_storage_writes, 1);       /* replaced by a good file */
}

static void test_load_oversized_file(void)
{
    /* More than the 8 KiB the loader reads: the JSON is cut and rejected */
    size_t n = 9000;
    char *big = malloc(n);
    memset(big, ' ', n);
    const char *head = "{\"merge\":{\"timeout_seconds\":9}";
    memcpy(big, head, strlen(head));
    big[n - 1] = '}';
    host_storage_put("config.json", big, n);
    free(big);
    CHECK_EQ(config_load(), ESP_OK);
    CHECK_EQ(cfg->merge.timeout_seconds, 3);
}

static void test_load_legacy_file(void)
{
    /* No "version": sACN universes follow the Art-Net universe */
    const char *legacy = "{\"port1\":{\"universe_primary\":5},\"port2\":{\"universe_primary\":0},"
                         "\"network\":{\"use_ethernet\":false}}";
    host_storage_put("config.json", legacy, strlen(legacy));
    CHECK_EQ(config_load(), ESP_OK);
    CHECK_EQ(cfg->ports[0].sacn_universe, 5);
    CHECK_EQ(cfg->ports[1].sacn_universe, 2);   /* universe 0 is not valid sACN */
    CHECK_EQ(cfg->network.mode, NET_MODE_WIFI);
    CHECK_EQ(host_storage_writes, 1);           /* upgraded file written back */
}

static void test_load_bad_credentials_dropped(void)
{
    const char *f = "{\"version\":2,\"security\":{\"admin_hash\":\"nothex\",\"admin_salt\":\"00\","
                    "\"admin_iterations\":4096}}";
    host_storage_put("config.json", f, strlen(f));
    CHECK_EQ(config_load(), ESP_OK);
    CHECK(!config_admin_password_set());
}

/* ---- config_sanitize ------------------------------------------------------ */

static void test_sanitize_clamps_every_field(void)
{
    config_t c = *cfg, d = defaults();
    c.network.mode = 200;
    c.network.ap_channel = 14;
    for (int i = 0; i < CONFIG_DMX_PORTS; i++) {
        port_config_t *p = &c.ports[i];
        p->mode = (dmx_mode_t)9;
        p->universe_primary = 40000;
        p->universe_secondary = -2;
        p->sacn_universe = 64000;
        p->protocol_mode = (protocol_mode_t)5;
        p->merge_mode = (merge_mode_t)-1;
        p->out_rate_hz = 45;
        p->break_us = 91;
        p->mab_us = 1001;
        p->out_slots = 23;
    }
    c.merge.timeout_seconds = 61;
    c.merge.loss_behavior = (loss_behavior_t)4;
    c.node_info.sacn_priority = 201;
    CHECK(config_sanitize(&c));
    CHECK_MEM(&c, &d, sizeof(c));            /* every field back to its default */

    /* the other end of each range */
    c.network.ap_channel = 0;
    for (int i = 0; i < CONFIG_DMX_PORTS; i++) {
        port_config_t *p = &c.ports[i];
        p->sacn_universe = 0;
        p->out_rate_hz = 0;
        p->break_us = 1001;
        p->mab_us = 11;
        p->out_slots = 513;
        p->universe_secondary = -32768;
    }
    c.merge.timeout_seconds = 0;
    CHECK(config_sanitize(&c));
    CHECK_MEM(&c, &d, sizeof(c));
}

static void test_sanitize_keeps_range_edges(void)
{
    config_t c = *cfg;
    c.network.mode = NET_MODE_LAST;
    c.network.ap_channel = 13;
    port_config_t *p = &c.ports[0];
    p->mode = DMX_MODE_DISABLED;
    p->universe_primary = CONFIG_ARTNET_UNIVERSE_MAX;
    p->universe_secondary = CONFIG_ARTNET_UNIVERSE_MAX;
    p->sacn_universe = CONFIG_SACN_UNIVERSE_MAX;
    p->protocol_mode = PROTOCOL_ARTNET_ONLY;
    p->merge_mode = MERGE_MODE_DISABLE;
    p->out_rate_hz = CONFIG_DMX_RATE_MIN;
    p->break_us = CONFIG_DMX_BREAK_MAX;
    p->mab_us = CONFIG_DMX_MAB_MIN;
    p->out_slots = CONFIG_DMX_SLOTS_MIN;
    c.merge.timeout_seconds = 1;
    c.merge.loss_behavior = LOSS_LAST;
    c.node_info.sacn_priority = 0;
    config_t before = c;
    CHECK(!config_sanitize(&c));
    CHECK_MEM(&c, &before, sizeof(c));
}

static void test_sanitize_terminates_strings(void)
{
    config_t c = *cfg;
    memset(c.network.eth_static_ip, 'a', sizeof(c.network.eth_static_ip));
    memset(c.network.eth_gateway, 'b', sizeof(c.network.eth_gateway));
    memset(c.network.eth_netmask, 'c', sizeof(c.network.eth_netmask));
    memset(c.network.ap_ssid, 'd', sizeof(c.network.ap_ssid));
    memset(c.network.ap_password, 'e', sizeof(c.network.ap_password));
    memset(c.node_info.short_name, 'f', sizeof(c.node_info.short_name));
    memset(c.node_info.long_name, 'g', sizeof(c.node_info.long_name));
    for (int i = 0; i < CONFIG_WIFI_PROFILES_MAX; i++) {
        config_wifi_profile_t *w = &c.network.wifi_profiles[i];
        memset(w->ssid, 'h', sizeof(w->ssid));
        memset(w->password, 'h', sizeof(w->password));
        memset(w->static_ip, 'h', sizeof(w->static_ip));
        memset(w->gateway, 'h', sizeof(w->gateway));
        memset(w->netmask, 'h', sizeof(w->netmask));
    }
    c.network.wifi_profile_count = 1;
    memset(c.security.admin_hash, 'i', sizeof(c.security.admin_hash));
    memset(c.security.admin_salt, 'j', sizeof(c.security.admin_salt));
    config_sanitize(&c);
    CHECK_EQ(strlen(c.network.eth_static_ip), sizeof(c.network.eth_static_ip) - 1);
    CHECK_EQ(strlen(c.network.eth_gateway), sizeof(c.network.eth_gateway) - 1);
    CHECK_EQ(strlen(c.network.eth_netmask), sizeof(c.network.eth_netmask) - 1);
    CHECK_EQ(strlen(c.network.ap_ssid), sizeof(c.network.ap_ssid) - 1);
    CHECK_EQ(strlen(c.network.ap_password), sizeof(c.network.ap_password) - 1);
    CHECK_EQ(strlen(c.node_info.short_name), sizeof(c.node_info.short_name) - 1);
    CHECK_EQ(strlen(c.node_info.long_name), sizeof(c.node_info.long_name) - 1);
    for (int i = 0; i < CONFIG_WIFI_PROFILES_MAX; i++) {
        config_wifi_profile_t *w = &c.network.wifi_profiles[i];
        CHECK_EQ(strnlen(w->ssid, sizeof(w->ssid)), sizeof(w->ssid) - 1);
        CHECK_EQ(strnlen(w->password, sizeof(w->password)), sizeof(w->password) - 1);
        CHECK_EQ(strnlen(w->static_ip, sizeof(w->static_ip)), sizeof(w->static_ip) - 1);
        CHECK_EQ(strnlen(w->gateway, sizeof(w->gateway)), sizeof(w->gateway) - 1);
        CHECK_EQ(strnlen(w->netmask, sizeof(w->netmask)), sizeof(w->netmask) - 1);
    }
    CHECK_EQ(c.security.admin_hash[0], '\0');   /* not hex: credential dropped */
    CHECK(!config_sanitize(&c));
}

static void test_sanitize_wifi_profiles(void)
{
    config_t c = *cfg;
    c.network.wifi_profile_count = 200;
    for (int i = 0; i < CONFIG_WIFI_PROFILES_MAX; i++) {
        snprintf(c.network.wifi_profiles[i].ssid, 33, "%s", (i % 2) ? "" : "net");
        c.network.wifi_profiles[i].priority = (uint8_t)i;
    }
    CHECK(config_sanitize(&c));
    CHECK_EQ(c.network.wifi_profile_count, 3);   /* the empty SSIDs are dropped */
    CHECK_EQ(c.network.wifi_profiles[0].priority, 0);
    CHECK_EQ(c.network.wifi_profiles[1].priority, 2);
    CHECK_EQ(c.network.wifi_profiles[2].priority, 4);
}

static void test_sanitize_names_and_passwords(void)
{
    config_t c = *cfg;
    c.network.ap_ssid[0] = '\0';
    c.node_info.short_name[0] = '\0';
    snprintf(c.network.ap_password, 64, "1234567");   /* shorter than WPA2 allows */
    CHECK(config_sanitize(&c));
    CHECK_STR(c.network.ap_ssid, "ESP-NODE-CDEF");
    CHECK_STR(c.node_info.short_name, "ESP-NODE-CDEF");
    CHECK_STR(c.network.ap_password, CONFIG_DEFAULT_AP_PASSWORD);
    c.network.ap_password[0] = '\0';                  /* open AP is allowed */
    CHECK(!config_sanitize(&c));
}

static void test_sanitize_credentials(void)
{
    config_t c = *cfg;
    snprintf(c.security.admin_hash, sizeof(c.security.admin_hash), "%064d", 0);
    snprintf(c.security.admin_salt, sizeof(c.security.admin_salt), "%032d", 0);
    c.security.admin_iterations = 1000;
    CHECK(!config_sanitize(&c));
    c.security.admin_iterations = 999;
    CHECK(config_sanitize(&c));
    CHECK_EQ(c.security.admin_hash[0], '\0');
    snprintf(c.security.admin_hash, sizeof(c.security.admin_hash), "%064d", 0);
    snprintf(c.security.admin_salt, sizeof(c.security.admin_salt), "%031d", 0);   /* short salt */
    c.security.admin_iterations = 4096;
    CHECK(config_sanitize(&c));
    CHECK_EQ(c.security.admin_hash[0], '\0');
    /* a stored password and "no password" at once: the password wins */
    snprintf(c.security.admin_salt, sizeof(c.security.admin_salt), "%032d", 0);
    snprintf(c.security.admin_hash, sizeof(c.security.admin_hash), "%064d", 0);
    c.security.admin_iterations = 4096;
    c.security.no_login = true;
    CHECK(config_sanitize(&c));
    CHECK(!c.security.no_login);
    CHECK(c.security.admin_hash[0] != '\0');
}

int main(void)
{
    RUN(test_defaults_are_clean);
    RUN(test_round_trip_every_field);
    RUN(test_round_trip_through_file);
    RUN(test_partial_update);
    RUN(test_bad_json_leaves_config_alone);
    RUN(test_wrong_types_ignored);
    RUN(test_from_json_clamps_numbers);
    RUN(test_bool_fields_accept_numbers);
    RUN(test_long_strings_truncated);
    RUN(test_web_api_cannot_set_credentials);
    RUN(test_admin_password);
    RUN(test_no_login);
    RUN(test_wifi_profiles);
    RUN(test_load_missing_file_saves_defaults);
    RUN(test_load_corrupt_file_uses_defaults);
    RUN(test_load_oversized_file);
    RUN(test_load_legacy_file);
    RUN(test_load_bad_credentials_dropped);
    RUN(test_sanitize_clamps_every_field);
    RUN(test_sanitize_keeps_range_edges);
    RUN(test_sanitize_terminates_strings);
    RUN(test_sanitize_wifi_profiles);
    RUN(test_sanitize_names_and_passwords);
    RUN(test_sanitize_credentials);
    RUN(test_update_problem);
    RUN(test_public_json_has_no_secrets);
    return test_summary();
}
