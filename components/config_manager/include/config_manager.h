#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "board.h"

/** Number of DMX ports on this board (config_t.ports[]) */
#define CONFIG_DMX_PORTS BOARD_DMX_PORTS

// DMX port modes
typedef enum {
    DMX_MODE_DISABLED = 0,
    DMX_MODE_OUTPUT,
    DMX_MODE_INPUT,
    DMX_MODE_RDM_MASTER,
    DMX_MODE_RDM_RESPONDER
} dmx_mode_t;

// Merge modes
typedef enum {
    MERGE_MODE_HTP = 0,
    MERGE_MODE_LTP,
    MERGE_MODE_LAST,
    MERGE_MODE_BACKUP,
    MERGE_MODE_DISABLE
} merge_mode_t;

// Protocol modes
typedef enum {
    PROTOCOL_ARTNET_ONLY = 0,
    PROTOCOL_SACN_ONLY,
    PROTOCOL_ARTNET_PRIORITY,
    PROTOCOL_SACN_PRIORITY,
    PROTOCOL_MERGE_BOTH
} protocol_mode_t;

// WiFi profile configuration storage format (different from network_manager's wifi_profile_t)
typedef struct {
    char ssid[33];          /* up to 32 bytes (802.11 limit) */
    char password[65];      /* 8-63 character passphrase or 64 hex digits */
    uint8_t priority;
    bool use_static_ip;
    char static_ip[16];
    char gateway[16];
    char netmask[16];
} config_wifi_profile_t;

/* How the node joins a network (policy in network_auto_fallback.c). In every
 * mode except AP the node's own access point comes up as a rescue when it
 * has no other connection, so it can always be reached to fix settings. */
typedef enum {
    NET_MODE_AUTO = 0,   /* Ethernet when cabled, else saved WiFi, else own AP */
    NET_MODE_ETHERNET,   /* Ethernet only */
    NET_MODE_WIFI,       /* WiFi client only (Ethernet off) */
    NET_MODE_AP,         /* standalone: own access point only */
} net_mode_t;
#define NET_MODE_LAST NET_MODE_AP

// Data-loss behaviour (Art-Net 4 "failsafe"), applied once every source of
// an output port is lost (not at power-on before any data arrived)
typedef enum {
    LOSS_HOLD_LAST_LOOK = 0,   // keep outputting the last received levels
    LOSS_BLACKOUT,             // go to zero
    LOSS_FULL,                 // every channel at full
    LOSS_SCENE,                // the port's failsafe scene (zero if it has none)
} loss_behavior_t;
#define LOSS_LAST LOSS_SCENE

/* DMX output timing limits (E1.11: break >= 92 us, MAB >= 12 us; RDM
 * controllers need a break of 176-352 us) */
#define CONFIG_DMX_RATE_MIN     1
#define CONFIG_DMX_RATE_MAX     44     /* full 512-slot frames: ~44 Hz max */
#define CONFIG_DMX_RATE_DEFAULT 44
#define CONFIG_DMX_BREAK_MIN    92
#define CONFIG_DMX_BREAK_MAX    1000
#define CONFIG_DMX_BREAK_DEFAULT 176
#define CONFIG_DMX_MAB_MIN      12
#define CONFIG_DMX_MAB_MAX      1000
#define CONFIG_DMX_MAB_DEFAULT  12
#define CONFIG_DMX_SLOTS_MIN    24
#define CONFIG_DMX_SLOTS_MAX    512

/*
 * Output processing per port, applied to the merged levels just before they
 * go on the wire (node mode). Patches are applied first (in list order, each
 * reading the unpatched levels), then limits, then parks, so a park always
 * wins. JSON: "rules": [{"type":"patch","channel":101,"count":10,"from":1},
 * {"type":"limit","channel":20,"count":2,"level":128},
 * {"type":"park","channel":5,"count":1,"level":255}]
 */
#define CONFIG_PORT_RULES_MAX 8

typedef enum {
    PORT_RULE_PATCH = 1,   /* channel..channel+count-1 take the levels of from.. */
    PORT_RULE_LIMIT,       /* never above level */
    PORT_RULE_PARK,        /* always level, whatever arrives */
} port_rule_type_t;
#define PORT_RULE_FIRST PORT_RULE_PATCH
#define PORT_RULE_LAST  PORT_RULE_PARK

typedef struct {
    uint8_t type;          /* port_rule_type_t */
    uint8_t level;         /* limit / park level, 0-255 */
    uint16_t channel;      /* first channel it changes, 1-512 */
    uint16_t count;        /* number of channels, 1-(513-channel) */
    uint16_t from;         /* patch: first channel it reads, 1-(513-count) */
} port_rule_t;

/* Where an input port sends its DMX (port_config_t.send_to) */
typedef enum {
    SEND_TO_ALL = 0,    /* Art-Net broadcast, sACN multicast */
    SEND_TO_NODES,      /* ArtDmx unicast to the Art-Net nodes that output the universe
                           (found by ArtPoll; broadcast while there are none); sACN multicast */
    SEND_TO_LIST,       /* Art-Net and sACN unicast to send_list only */
} send_to_t;
#define SEND_TO_LAST SEND_TO_LIST
#define CONFIG_SEND_LIST_MAX 4

// Port configuration
typedef struct {
    dmx_mode_t mode;
    uint16_t universe_primary;    // Art-Net 15-bit Port-Address (0-32767)
    int16_t universe_secondary;   // Art-Net backup Port-Address for MERGE_MODE_BACKUP, -1 = none
    int16_t universe_offset;      // reserved
    uint16_t sacn_universe;       // sACN / E1.31 universe (1-63999)
    protocol_mode_t protocol_mode;
    merge_mode_t merge_mode;
    bool rdm_enabled;

    /* Output timing (output ports) */
    uint8_t out_rate_hz;          // frames per second (1-44)
    uint16_t break_us;            // break length
    uint16_t mab_us;              // mark-after-break length
    uint16_t out_slots;           // slots per frame (24-512); fewer = faster refresh

    /* Scenes (scene ids, 0 = none) */
    uint8_t startup_scene;        // played at power-on until a console sends data
    uint8_t failsafe_scene;       // output when sources are lost and loss = LOSS_SCENE

    /* Output processing (output ports, see port_rule_t) */
    port_rule_t rules[CONFIG_PORT_RULES_MAX];
    uint8_t rule_count;

    /* Routing (output ports): the DMX arriving on other ports' inputs, merged
     * with the network sources (local_only false) or without them (local_only
     * true: the port ignores Art-Net / sACN; with one input it is a splitter
     * that follows the input frame by frame). Bit p-1 = port p's input, never
     * the port's own bit. JSON: "local_inputs": [2, 4], "local_only": false */
    uint8_t local_inputs;
    bool local_only;

    /* Destinations (input ports), see send_to_t. Unicast is the one to use
     * on Wi-Fi: access points hold broadcast and multicast until the next
     * DTIM beacon (often 100-300 ms) and never resend a lost one.
     * JSON: "send_to": "all" | "nodes" | "list", "send_list": ["10.0.0.20"] */
    uint8_t send_to;
    uint8_t send_count;
    uint32_t send_list[CONFIG_SEND_LIST_MAX];   /* IPv4, network byte order */
} port_config_t;

/** "all" / "nodes" / "list" */
const char *config_send_to_name(uint8_t send_to);

/** Bits of every port on the board, for port_config_t.local_inputs */
#define CONFIG_PORT_BITS ((uint8_t)((1u << CONFIG_DMX_PORTS) - 1))

/* DMX tester mode (CONFIG_NODE_TESTER). The node stops being an Art-Net /
 * sACN node and each port does one tester job, driven from the web UI. */
typedef enum {
    TESTER_ROLE_OFF = 0,
    TESTER_ROLE_SEND,      /* DMX output: levels, test patterns, output timing */
    TESTER_ROLE_RECEIVE,   /* DMX input: levels, analyser, flicker watch */
    TESTER_ROLE_RDM,       /* RDM controller: discovery, identify, addressing */
} tester_role_t;
#define TESTER_ROLE_LAST TESTER_ROLE_RDM

typedef struct {
    uint8_t role;          /* tester_role_t */
    uint8_t rate_hz;       /* output timing of send / RDM ports */
    uint16_t break_us;
    uint16_t mab_us;
    uint16_t slots;
} tester_port_config_t;

#define CONFIG_ARTNET_UNIVERSE_MAX 32767
#define CONFIG_SACN_UNIVERSE_MIN   1
#define CONFIG_SACN_UNIVERSE_MAX   63999
#define CONFIG_WIFI_PROFILES_MAX   5

/* Factory AP password. It is only accepted until the first-run setup, which
 * requires a different one (or none: an empty AP password is an open
 * network). */
#define CONFIG_DEFAULT_AP_PASSWORD "12345678"
#define CONFIG_ADMIN_USER          "admin"
#define CONFIG_ADMIN_HASH_HEX      64   /* PBKDF2-HMAC-SHA256 output, hex */
#define CONFIG_ADMIN_SALT_HEX      32   /* 16 random bytes, hex */

// Main configuration
typedef struct {
    struct {
        uint8_t mode;                 /* net_mode_t */
        bool ap_always;               /* keep own AP on next to Ethernet / WiFi */
        bool eth_use_static_ip;
        char eth_static_ip[16];
        char eth_gateway[16];
        char eth_netmask[16];
        config_wifi_profile_t wifi_profiles[CONFIG_WIFI_PROFILES_MAX];
        uint8_t wifi_profile_count;
        char ap_ssid[32];
        char ap_password[64];
        uint8_t ap_channel;
        bool ap_captive;              /* devices joining the AP get the web UI (captive portal) */
    } network;
    
    port_config_t ports[CONFIG_DMX_PORTS];   // ports[0] = port 1 ("port1" in JSON)

    struct {
        uint8_t timeout_seconds;      // source timeout (1-60 s)
        loss_behavior_t loss_behavior;
    } merge;

    struct {
        char short_name[18];
        char long_name[64];
        uint8_t sacn_priority;        // priority of sACN this node transmits (DMX input)
        bool allow_ip_prog;           // consoles may change the IP address (ArtIpProg)
    } node_info;

    /* DMX tester mode. "enabled" is read at start-up only: changing it
     * takes a restart. The port jobs and timing apply live. */
    struct {
        bool enabled;
        tester_port_config_t ports[CONFIG_DMX_PORTS];
    } tester;

    /* Web UI admin credentials. Only ever read from / written to the config
     * file: config_from_json() (the web API) cannot change them. First-run
     * setup is done once there is a hash (sign-in required) or no_login is
     * set (the owner chose to run without a password); never both. */
    struct {
        char admin_hash[CONFIG_ADMIN_HASH_HEX + 1];
        char admin_salt[CONFIG_ADMIN_SALT_HEX + 1];
        uint32_t admin_iterations;
        bool no_login;
    } security;
} config_t;

/** Called after the configuration was changed and saved */
typedef void (*config_change_callback_t)(void *user_data);

/**
 * @brief Initialize configuration manager
 * @return ESP_OK on success
 */
esp_err_t config_init(void);

/**
 * @brief Load configuration from storage
 * @return ESP_OK on success
 */
esp_err_t config_load(void);

/**
 * @brief Save configuration to storage
 * @return ESP_OK on success
 */
esp_err_t config_save(void);

/**
 * @brief Get current configuration
 * @return Pointer to config structure
 */
config_t* config_get(void);

/**
 * The settings lock. config_get() hands out the live settings, which the web
 * UI, the USB setup and Art-Net (ArtAddress, ArtIpProg) all change from their
 * own tasks: hold the lock across any read-modify-write of them, so one change
 * can't undo another made at the same moment. It is recursive, and the
 * config_* functions here take it themselves. Keep it short: don't wait for
 * other locks, the network or the DMX ports while holding it.
 * config_save() may be called with it held (the file is written without it).
 */
void config_lock(void);
void config_unlock(void);

/**
 * A copy of the settings, taken under the lock (sanitised first), for code
 * that reads many of them and then does slow work. Caller frees.
 * NULL if out of memory.
 */
config_t *config_snapshot(void);

/** How config_load() found the settings at boot */
typedef enum {
    CONFIG_LOAD_NEW = 0,        /* no settings yet (first start): factory settings */
    CONFIG_LOAD_OK,             /* config.json */
    CONFIG_LOAD_FROM_BACKUP,    /* config.json damaged or missing: the copy from before the last save */
    CONFIG_LOAD_RESET,          /* config.json damaged and no good copy: factory settings */
} config_load_result_t;

config_load_result_t config_load_result(void);

/**
 * What went wrong with the stored settings at boot, as a sentence for the
 * user (storage erased, settings restored from the backup or reset), or NULL
 * if nothing did or config_boot_notice_clear() was called.
 */
const char *config_boot_notice(void);
void config_boot_notice_clear(void);

/** Settings of DMX port @p port (1..CONFIG_DMX_PORTS), NULL if out of range */
static inline port_config_t *config_port(config_t *c, int port)
{
    return (c && port >= 1 && port <= CONFIG_DMX_PORTS) ? &c->ports[port - 1] : 0;
}

/**
 * @brief Reset to default configuration and save it. The backup copy of the
 *        old settings is deleted too.
 * @return ESP_OK on success
 */
esp_err_t config_reset_to_defaults(void);

/**
 * @brief Export config as JSON string
 * @param json_str Output JSON string (caller must free)
 * @return ESP_OK on success
 */
esp_err_t config_to_json(char **json_str);

/**
 * @brief Import config from JSON string
 * @param json_str Input JSON string
 * @return ESP_OK on success
 */
esp_err_t config_from_json(const char *json_str);

/** One port's settings as a cJSON object (the "portN" object of the config) */
struct cJSON *config_port_to_json(const port_config_t *p);

/**
 * The settings as the web API and the USB setup show them: everything except
 * the admin credentials, the AP password and Wi-Fi passwords (profiles say
 * "has_password" instead). The same shape config_from_json() takes.
 * Caller frees with cJSON_Delete().
 */
struct cJSON *config_to_public_json(const config_t *c);

/** Why @p pw can't be the AP password (8-63 characters, not the factory
 *  default; empty is fine and means an open network), or NULL if it is fine */
const char *config_ap_password_problem(const char *pw);

/**
 * Check a (partial) settings document before config_from_json(): the AP
 * password and Wi-Fi passwords it sets. Returns a message for the user, or
 * NULL if it may be applied (every other value is clamped by the parser).
 */
const char *config_update_problem(const struct cJSON *doc);

/** Parse a dotted IPv4 address into network byte order. False if it isn't one. */
bool config_parse_ipv4(const char *s, uint32_t *out);

/** Why @p r is not a usable port rule, or NULL if it is fine */
const char *config_port_rule_problem(const port_rule_t *r);

/** JSON name of a rule type ("patch", "limit", "park"), NULL if unknown */
const char *config_port_rule_name(uint8_t type);

/** "off" / "send" / "receive" / "rdm" */
const char *config_tester_role_name(uint8_t role);
/** Parse a tester role name; -1 if unknown */
int config_tester_role_from_name(const char *name);

/**
 * Settings DMX port @p port runs with in tester mode: the port's normal
 * settings with the mode, RDM and output timing taken from the tester.
 */
void config_tester_port_settings(const config_t *c, int port, port_config_t *out);

/**
 * @brief Clamp every field of @p cfg to a valid value and make sure all
 *        strings are terminated. Returns true if anything was changed.
 */
bool config_sanitize(config_t *cfg);

/**
 * @brief True once an admin password has been set
 */
bool config_admin_password_set(void);

/**
 * @brief True once first-run setup is done: an admin password is set, or the
 *        owner chose to run without one (config_set_no_login())
 */
bool config_setup_done(void);

/**
 * @brief True if the web UI asks for the admin password
 *        (setup done and not running without a password)
 */
bool config_login_required(void);

/**
 * @brief Store a new admin password (salted PBKDF2) and save the config.
 *        @p password must be 8-63 characters. Turns sign-in on.
 */
esp_err_t config_set_admin_password(const char *password);

/**
 * @brief Run without a web UI password: forget the stored one, mark first-run
 *        setup as done, and save. Anyone who can reach the node can then
 *        change its settings.
 */
esp_err_t config_set_no_login(void);

/**
 * @brief Check @p password against the stored admin password
 *        (constant-time compare). False if no password is set.
 */
bool config_check_admin_password(const char *password);

/**
 * @brief Register the callback run by config_notify_changed() (one only)
 */
void config_register_change_callback(config_change_callback_t cb, void *user_data);

/**
 * @brief Tell the application the configuration changed so it can apply it
 *        live (call after config_from_json()/config_save()).
 */
void config_notify_changed(void);

#endif
