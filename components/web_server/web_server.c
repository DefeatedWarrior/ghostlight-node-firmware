/**
 * @file web_server.c
 * @brief Web Server Implementation
 * 
 * Provides HTTP REST API and WebSocket server for device configuration
 * and real-time monitoring.
 * 
 * Key Features:
 * - RESTful JSON API for configuration and control
 * - WebSocket for real-time DMX monitoring
 * - Static file serving for web UI
 * - Thread-safe operation
 * 
 * Memory Usage: ~20KB (server context + handlers)
 * Stack: 8KB per connection (configurable)
 * 
 * FIXES APPLIED:
 *  1. WebSocket URI changed from "/ws/?" to "/ws" - the wildcard never matched
 *     exact path "/ws" that the frontend connects to.
 *  2. uri_match_fn set to httpd_uri_match_wildcard so "/api/ports/?/config"
 *     and "/api/ports/?/blackout" wildcard routes actually work.
 *  3. max_open_sockets raised to WEB_SERVER_MAX_OPEN_SOCKETS (13) so the
 *     server does not drop connections under polling load, which was causing
 *     spurious 404 log spam even for registered routes.
 *  4. send_json_response now correctly maps all HTTP status codes instead of
 *     always falling through to "500 Internal Server Error".
 *  5. ws_handler no longer reuses the receive ws_pkt struct for sends -
 *     each send uses its own local httpd_ws_frame_t to avoid payload/len
 *     corruption on ping/close frames.
 *  6. WebSocket client cleanup: disconnected fds are removed from ws_clients
 *     when httpd_ws_send_frame_async fails (prevents stale fd accumulation).
 */

#include "web_server.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "lwip/sockets.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "esp_random.h"
#include "board.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>

// Component includes
#include "config_manager.h"
#include "network_manager.h"
#include "dmx_handler.h"
#include "artnet_receiver.h"
#include "sacn_receiver.h"
#include "merge_engine.h"
#include "scene_manager.h"
#include "dmx_tools.h"
#include "dmx_tester.h"
#include "web_internal.h"

// Embedded web files
#include "web_files.h"

static const char *TAG = "web_server";

// WebSocket frame opcodes
#define WS_OPCODE_TEXT   0x01
#define WS_OPCODE_BINARY 0x02
#define WS_OPCODE_CLOSE  0x08
#define WS_OPCODE_PING   0x09
#define WS_OPCODE_PONG   0x0A

// Maximum WebSocket clients
#define MAX_WS_CLIENTS 4

// DMX constants for validation
#if CONFIG_IDF_TARGET_ESP32S3
#define WEB_CHIP_NAME "ESP32-S3"
#elif CONFIG_IDF_TARGET_ESP32
#define WEB_CHIP_NAME "ESP32"
#else
#define WEB_CHIP_NAME CONFIG_IDF_TARGET
#endif

#define DMX_MAX_PORTS CONFIG_DMX_PORTS
#define DMX_MAX_CHANNELS 512
#define DMX_MAX_VALUE 255


// Open socket budget. httpd needs max_open_sockets + 3 lwIP sockets, and the
// node also needs ~6 UDP sockets (Art-Net rx/tx, sACN rx/tx, mDNS), so keep
// headroom instead of taking every socket lwIP has.
#define WEB_SERVER_SOCKETS_RESERVED 9
#if (CONFIG_LWIP_MAX_SOCKETS - WEB_SERVER_SOCKETS_RESERVED) > 10
#define WEB_SERVER_MAX_OPEN_SOCKETS 10
#elif (CONFIG_LWIP_MAX_SOCKETS - WEB_SERVER_SOCKETS_RESERVED) < 4
#define WEB_SERVER_MAX_OPEN_SOCKETS 4
#else
#define WEB_SERVER_MAX_OPEN_SOCKETS (CONFIG_LWIP_MAX_SOCKETS - WEB_SERVER_SOCKETS_RESERVED)
#endif

// Largest JSON request body accepted
#define WEB_BODY_MAX 8192

/**
 * @brief Server state
 */
static struct {
    httpd_handle_t server;
    bool initialized;
    bool running;
    web_server_config_t config;

    // Statistics
    uint32_t total_requests;
    uint8_t active_connections;

    // WebSocket clients
    ws_client_t ws_clients[MAX_WS_CLIENTS];
    uint8_t ws_client_count;
    SemaphoreHandle_t ws_mutex;

    // WebSocket DMX streaming task
    TaskHandle_t ws_stream_task;
    volatile bool ws_stream_run;
    SemaphoreHandle_t ws_stream_done;
} server_state = {
    .server = NULL,
    .initialized = false,
    .running = false,
    .ws_stream_task = NULL,
};

// Forward declarations
static esp_err_t api_config_get_handler(httpd_req_t *req);
static esp_err_t api_config_post_handler(httpd_req_t *req);
static esp_err_t api_network_status_handler(httpd_req_t *req);
static esp_err_t api_network_wifi_scan_handler(httpd_req_t *req);
static esp_err_t api_network_wifi_connect_handler(httpd_req_t *req);
static esp_err_t api_network_ap_off_handler(httpd_req_t *req);
static bool port_levels(int port, uint8_t data[512], bool *input);
static esp_err_t api_ports_status_handler(httpd_req_t *req);
static esp_err_t api_port_config_get_handler(httpd_req_t *req);
static esp_err_t api_port_blackout_handler(httpd_req_t *req);
static esp_err_t api_port_dmx_handler(httpd_req_t *req);
static esp_err_t api_system_info_handler(httpd_req_t *req);
static esp_err_t api_system_stats_handler(httpd_req_t *req);
static esp_err_t api_system_restart_handler(httpd_req_t *req);
static esp_err_t api_system_factory_reset_handler(httpd_req_t *req);
static esp_err_t api_system_ota_handler(httpd_req_t *req);
static esp_err_t api_system_firmware_rollback_handler(httpd_req_t *req);
static esp_err_t api_rdm_discover_handler(httpd_req_t *req);
static esp_err_t api_rdm_identify_handler(httpd_req_t *req);
static esp_err_t api_scenes_list_handler(httpd_req_t *req);
static esp_err_t api_scenes_record_handler(httpd_req_t *req);
static esp_err_t api_scenes_playback_handler(httpd_req_t *req);
static esp_err_t api_scenes_delete_handler(httpd_req_t *req);
static esp_err_t api_scenes_release_handler(httpd_req_t *req);
static esp_err_t api_rdm_devices_handler(httpd_req_t *req);
static esp_err_t api_rdm_address_handler(httpd_req_t *req);
static esp_err_t ws_handler(httpd_req_t *req);
static esp_err_t root_handler(httpd_req_t *req);
static esp_err_t js_main_handler(httpd_req_t *req);

// Helper functions
static void send_json_response(httpd_req_t *req, cJSON *json, int status);
static void send_error_response(httpd_req_t *req, int status, const char *message);
static int get_port_from_uri(const char *uri);
static bool require_auth(httpd_req_t *req);
static bool require_auth_page(httpd_req_t *req);
static bool ws_token_ok(httpd_req_t *req);
static void auth_invalidate(void);
static esp_err_t api_auth_status_handler(httpd_req_t *req);
static esp_err_t api_setup_handler(httpd_req_t *req);
static esp_err_t api_auth_password_handler(httpd_req_t *req);
static void ws_dmx_stream_task(void *arg);
static esp_err_t read_body(httpd_req_t *req, char **out);
static cJSON *read_json_body(httpd_req_t *req);
static void ws_close_fn(httpd_handle_t hd, int sockfd);
static void ws_remove_fd(int fd);
static void restart_later(uint32_t ms);
static const char *fw_version(void);

// FIX 4 helper: map integer status code to the HTTP status line string
static const char *http_status_str(int status)
{
    switch (status) {
        case 200: return "200 OK";
        case 201: return "201 Created";
        case 202: return "202 Accepted";
        case 204: return "204 No Content";
        case 400: return "400 Bad Request";
        case 401: return "401 Unauthorized";
        case 403: return "403 Forbidden";
        case 404: return "404 Not Found";
        case 409: return "409 Conflict";
        case 413: return "413 Payload Too Large";
        case 429: return "429 Too Many Requests";
        case 500: return "500 Internal Server Error";
        case 503: return "503 Service Unavailable";
        default:  return "500 Internal Server Error";
    }
}

// ============================================================================
// Public API Implementation
// ============================================================================

esp_err_t web_server_init(const web_server_config_t *config)
{
    if (server_state.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Initializing web server...");
    
    // Set configuration
    if (config) {
        server_state.config = *config;
    } else {
        // Default configuration
        server_state.config.port = WEB_SERVER_DEFAULT_PORT;
        server_state.config.max_connections = WEB_SERVER_MAX_CONNECTIONS;
        server_state.config.stack_size_kb = 8;
        server_state.config.task_priority = 5;
        server_state.config.enable_websocket = true;
    }
    
    // Create WebSocket mutex
    server_state.ws_mutex = xSemaphoreCreateMutex();
    server_state.ws_stream_done = xSemaphoreCreateBinary();
    if (!server_state.ws_mutex || !server_state.ws_stream_done) {
        ESP_LOGE(TAG, "Failed to create WebSocket mutex");
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize WebSocket clients array
    memset(server_state.ws_clients, 0, sizeof(server_state.ws_clients));
    server_state.ws_client_count = 0;

    // Web UI login (see "Authentication" below)
    auth_invalidate();
    if (!config_setup_done()) {
        ESP_LOGW(TAG, "No admin password set - web UI is in first-run setup mode");
    } else if (!config_login_required()) {
        ESP_LOGW(TAG, "Web UI sign-in is off: anyone on the network can change settings");
    }

    server_state.initialized = true;
    ESP_LOGI(TAG, "Web server initialized on port %d", server_state.config.port);
    
    return ESP_OK;
}

esp_err_t web_server_start(void)
{
    if (!server_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (server_state.running) {
        ESP_LOGW(TAG, "Already running");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Starting web server...");
    
    // Configure HTTP server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = server_state.config.port;
    config.stack_size = server_state.config.stack_size_kb * 1024;
    config.task_priority = server_state.config.task_priority;
    config.lru_purge_enable = true;

    // Raise open socket limit. Default (7) is too low when the web UI polls
    // multiple endpoints every 2 s while also holding a WebSocket connection.
    config.max_open_sockets = WEB_SERVER_MAX_OPEN_SOCKETS;

    // Allocate enough URI handler slots for all registered routes.
    // Count: / + /main.js + /api/config x2 + /api/network/status +
    // /api/network/wifi-scan + /api/ports/* x6 + /api/system/* x5 +
    // /api/rdm/discover + /api/rdm/identify + /api/scenes x3 (list/record/playback) +
    // /api/scenes/1..16 DELETE x16 + /ws = 43 routes → use 48 for headroom.
    config.max_uri_handlers = 72 + 3 * CONFIG_DMX_PORTS + WEB_TESTER_ROUTES;

    // Remove WebSocket clients whenever their socket closes (browser tab
    // closed, WiFi dropped, LRU purge) - not only on a CLOSE frame.
    config.close_fn = ws_close_fn;

    // Do NOT set uri_match_fn = httpd_uri_match_wildcard globally.
    // In ESP-IDF 5.2.x the wildcard matcher breaks exact-path routes that
    // contain '/' separators (e.g. "/api/system/info" returns 404 even when
    // correctly registered). The two routes that needed wildcards are replaced
    // below with explicit per-port registrations instead.
    
    // Start server
    if (httpd_start(&server_state.server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }
    
    // Register URI handlers
    
    // Root page
    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_state.server, &root_uri);
    
    // JavaScript file
    httpd_uri_t js_main_uri = {
        .uri = "/main.js",
        .method = HTTP_GET,
        .handler = js_main_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_state.server, &js_main_uri);
    
    // Configuration APIs
    httpd_uri_t config_get_uri = {
        .uri = "/api/config",
        .method = HTTP_GET,
        .handler = api_config_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_state.server, &config_get_uri);
    
    httpd_uri_t config_post_uri = {
        .uri = "/api/config",
        .method = HTTP_POST,
        .handler = api_config_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_state.server, &config_post_uri);
    
    // Network APIs
    httpd_uri_t network_status_uri = {
        .uri = "/api/network/status",
        .method = HTTP_GET,
        .handler = api_network_status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_state.server, &network_status_uri);
    
    // Port APIs
    httpd_uri_t ports_status_uri = {
        .uri = "/api/ports/status",
        .method = HTTP_GET,
        .handler = api_ports_status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_state.server, &ports_status_uri);
    
    // Explicit per-port routes replace wildcards.
    // Wildcards require uri_match_fn = httpd_uri_match_wildcard which breaks
    // all exact-path routes in ESP-IDF 5.2.x.
    for (int p = 1; p <= CONFIG_DMX_PORTS; p++) {
        static char uris[CONFIG_DMX_PORTS][3][32];
        snprintf(uris[p - 1][0], sizeof(uris[0][0]), "/api/ports/%d/config", p);
        snprintf(uris[p - 1][1], sizeof(uris[0][0]), "/api/ports/%d/blackout", p);
        snprintf(uris[p - 1][2], sizeof(uris[0][0]), "/api/ports/%d/dmx", p);
        const httpd_uri_t r[] = {
            { .uri = uris[p - 1][0], .method = HTTP_GET,  .handler = api_port_config_get_handler },
            { .uri = uris[p - 1][1], .method = HTTP_POST, .handler = api_port_blackout_handler },
            { .uri = uris[p - 1][2], .method = HTTP_GET,  .handler = api_port_dmx_handler },
        };
        for (size_t i = 0; i < sizeof(r) / sizeof(r[0]); i++) {
            httpd_register_uri_handler(server_state.server, &r[i]);
        }
    }

    /* Diagnostics and DMX tools (web_tools.c), DMX tester (web_tester.c) */
    web_tools_register(server_state.server);
    web_tester_register(server_state.server);

    // System APIs
    httpd_uri_t system_info_uri = {
        .uri = "/api/system/info",
        .method = HTTP_GET,
        .handler = api_system_info_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_state.server, &system_info_uri);
    
    httpd_uri_t system_stats_uri = {
        .uri = "/api/system/stats",
        .method = HTTP_GET,
        .handler = api_system_stats_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_state.server, &system_stats_uri);
    
    httpd_uri_t system_restart_uri = {
        .uri = "/api/system/restart",
        .method = HTTP_POST,
        .handler = api_system_restart_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_state.server, &system_restart_uri);

    httpd_uri_t system_factory_reset_uri = {
        .uri = "/api/system/factory-reset",
        .method = HTTP_POST,
        .handler = api_system_factory_reset_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_state.server, &system_factory_reset_uri);

    httpd_uri_t system_ota_uri = {
        .uri = "/api/system/firmware-upload",
        .method = HTTP_POST,
        .handler = api_system_ota_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_state.server, &system_ota_uri);

    httpd_uri_t rdm_discover_uri = {
        .uri = "/api/rdm/discover",
        .method = HTTP_POST,
        .handler = api_rdm_discover_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_state.server, &rdm_discover_uri);

    httpd_uri_t rdm_identify_uri = {
        .uri = "/api/rdm/identify",
        .method = HTTP_POST,
        .handler = api_rdm_identify_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_state.server, &rdm_identify_uri);

    // Network WiFi scan
    httpd_uri_t network_wifi_scan_uri = {
        .uri = "/api/network/wifi-scan",
        .method = HTTP_GET,
        .handler = api_network_wifi_scan_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_state.server, &network_wifi_scan_uri);

    const httpd_uri_t network_uris[] = {
        { .uri = "/api/network/wifi/connect", .method = HTTP_POST, .handler = api_network_wifi_connect_handler },
        { .uri = "/api/network/ap/off",       .method = HTTP_POST, .handler = api_network_ap_off_handler },
    };
    for (size_t i = 0; i < sizeof(network_uris) / sizeof(network_uris[0]); i++) {
        httpd_register_uri_handler(server_state.server, &network_uris[i]);
    }

    // Firmware rollback
    httpd_uri_t firmware_rollback_uri = {
        .uri = "/api/system/firmware-rollback",
        .method = HTTP_POST,
        .handler = api_system_firmware_rollback_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_state.server, &firmware_rollback_uri);

    // Scene manager
    httpd_uri_t scenes_list_uri = {
        .uri = "/api/scenes",
        .method = HTTP_GET,
        .handler = api_scenes_list_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_state.server, &scenes_list_uri);

    httpd_uri_t scenes_record_uri = {
        .uri = "/api/scenes/record",
        .method = HTTP_POST,
        .handler = api_scenes_record_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_state.server, &scenes_record_uri);

    httpd_uri_t scenes_playback_uri = {
        .uri = "/api/scenes/playback",
        .method = HTTP_POST,
        .handler = api_scenes_playback_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_state.server, &scenes_playback_uri);

    // DELETE /api/scenes/1 .. /api/scenes/16 — register individual scene delete URIs
    // We use a single handler that parses the trailing integer from the URI.
    // Register one wildcard-style approach: explicit per-id is impractical (16 slots).
    // Instead register once with a path that the handler will dispatch.
    // Note: we reuse the scenes_delete handler via a single catch-all delete route.
    // Because wildcard matching is disabled globally, we register
    // /api/scenes/delete as an alternative POST endpoint in addition to
    // storing the scene ID in the body, and ALSO register explicit DELETE
    // routes for IDs 1-16.
    for (int sid = 1; sid <= SCENE_MAX_COUNT; sid++) {
        static char scene_delete_uris[SCENE_MAX_COUNT][32];
        snprintf(scene_delete_uris[sid - 1], sizeof(scene_delete_uris[0]),
                 "/api/scenes/%d", sid);
        httpd_uri_t del_uri = {
            .uri     = scene_delete_uris[sid - 1],
            .method  = HTTP_DELETE,
            .handler = api_scenes_delete_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_state.server, &del_uri);
    }

    httpd_uri_t scenes_release_uri = {
        .uri = "/api/scenes/release",
        .method = HTTP_POST,
        .handler = api_scenes_release_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_state.server, &scenes_release_uri);

    httpd_uri_t rdm_devices_uri = {
        .uri = "/api/rdm/devices",
        .method = HTTP_GET,
        .handler = api_rdm_devices_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_state.server, &rdm_devices_uri);

    httpd_uri_t rdm_address_uri = {
        .uri = "/api/rdm/address",
        .method = HTTP_POST,
        .handler = api_rdm_address_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_state.server, &rdm_address_uri);

    // Login / first-run setup
    const httpd_uri_t auth_uris[] = {
        { .uri = "/api/auth/status",   .method = HTTP_GET,  .handler = api_auth_status_handler },
        { .uri = "/api/setup",         .method = HTTP_POST, .handler = api_setup_handler },
        { .uri = "/api/auth/password", .method = HTTP_POST, .handler = api_auth_password_handler },
    };
    for (size_t i = 0; i < sizeof(auth_uris) / sizeof(auth_uris[0]); i++) {
        httpd_register_uri_handler(server_state.server, &auth_uris[i]);
    }

    // WebSocket handler
    if (server_state.config.enable_websocket) {
    // FIX 1: Use exact "/ws" instead of "/ws/*".
        // The frontend connects to ws://device/ws (no trailing path), which
        // never matched the wildcard pattern "/ws/*". The wildcard also has
        // a subtle problem: with uri_match_fn = httpd_uri_match_wildcard,
        // a pattern like "/ws/*" requires at least one character after the slash,
        // so "/ws/" would work but "/ws" would not. Using the exact path "/ws"
        // is correct and unambiguous.
        httpd_uri_t ws_uri = {
            .uri = "/ws",
            .method = HTTP_GET,
            .handler = ws_handler,
            .user_ctx = NULL,
            .is_websocket = true
        };
        httpd_register_uri_handler(server_state.server, &ws_uri);
    }
    
    server_state.running = true;
    ESP_LOGI(TAG, "Web server started successfully on port %d", server_state.config.port);

    // Start WebSocket DMX streaming task
    if (server_state.config.enable_websocket) {
        server_state.ws_stream_run = true;
        xSemaphoreTake(server_state.ws_stream_done, 0);
        if (xTaskCreatePinnedToCore(ws_dmx_stream_task, "ws_dmx_stream", 6144, NULL, 3,
                                    &server_state.ws_stream_task, 0) != pdPASS) {
            server_state.ws_stream_run = false;
            server_state.ws_stream_task = NULL;
            ESP_LOGW(TAG, "WebSocket stream task not started");
        }
    }

    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (!server_state.running) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Stopping web server...");

    // Stop WebSocket streaming task (it exits by itself; never delete a task
    // that may be holding ws_mutex)
    if (server_state.ws_stream_task) {
        server_state.ws_stream_run = false;
        xSemaphoreTake(server_state.ws_stream_done, pdMS_TO_TICKS(1000));
        server_state.ws_stream_task = NULL;
    }

    if (server_state.server) {
        httpd_stop(server_state.server);
        server_state.server = NULL;
    }

    server_state.running = false;
    ESP_LOGI(TAG, "Web server stopped");
    
    return ESP_OK;
}

esp_err_t web_server_deinit(void)
{
    if (server_state.running) {
        web_server_stop();
    }
    
    if (server_state.ws_mutex) {
        vSemaphoreDelete(server_state.ws_mutex);
        server_state.ws_mutex = NULL;
    }
    if (server_state.ws_stream_done) {
        vSemaphoreDelete(server_state.ws_stream_done);
        server_state.ws_stream_done = NULL;
    }
    
    server_state.initialized = false;
    ESP_LOGI(TAG, "Web server deinitialized");
    
    return ESP_OK;
}

bool web_server_is_running(void)
{
    return server_state.running;
}

// ============================================================================
// Shared with web_tools.c (web_internal.h)
// ============================================================================

static const web_server_hooks_t *s_hooks;

void web_server_set_hooks(const web_server_hooks_t *hooks)
{
    s_hooks = hooks;
}

const web_server_hooks_t *web_hooks(void)
{
    return s_hooks;
}

bool web_require_auth(httpd_req_t *req)
{
    server_state.total_requests++;
    return require_auth(req);
}

void web_send_json(httpd_req_t *req, cJSON *json, int status)
{
    send_json_response(req, json, status);
}

void web_send_error(httpd_req_t *req, int status, const char *message)
{
    send_error_response(req, status, message);
}

cJSON *web_read_json(httpd_req_t *req)
{
    return read_json_body(req);
}

const char *web_status_line(int status)
{
    return http_status_str(status);
}

esp_err_t web_server_ws_send(const char *path, const uint8_t *data,
                             size_t len, bool is_binary)
{
    if (!server_state.running || !path || !data) {
        return ESP_ERR_INVALID_ARG;
    }
    
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t *)data;
    ws_pkt.len = len;
    ws_pkt.type = is_binary ? HTTPD_WS_TYPE_BINARY : HTTPD_WS_TYPE_TEXT;
    
    xSemaphoreTake(server_state.ws_mutex, portMAX_DELAY);
    
    // Remove stale client entries when sends fail.
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (server_state.ws_clients[i].active &&
            strcmp(server_state.ws_clients[i].path, path) == 0) {
            esp_err_t ret = ESP_FAIL;
            if (httpd_ws_get_fd_info(server_state.server, server_state.ws_clients[i].fd) ==
                HTTPD_WS_CLIENT_WEBSOCKET) {
                ret = httpd_ws_send_frame_async(server_state.server,
                                                server_state.ws_clients[i].fd, &ws_pkt);
            }
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "WS send failed fd=%d, removing client",
                         server_state.ws_clients[i].fd);
                server_state.ws_clients[i].active = false;
                if (server_state.ws_client_count > 0) {
                    server_state.ws_client_count--;
                }
            }
        }
    }
    
    xSemaphoreGive(server_state.ws_mutex);
    
    return ESP_OK;
}

uint8_t web_server_ws_get_client_count(const char *path)
{
    if (!server_state.running) {
        return 0;
    }
    
    uint8_t count = 0;
    
    xSemaphoreTake(server_state.ws_mutex, portMAX_DELAY);
    
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (server_state.ws_clients[i].active) {
            if (path == NULL || strcmp(server_state.ws_clients[i].path, path) == 0) {
                count++;
            }
        }
    }
    
    xSemaphoreGive(server_state.ws_mutex);
    
    return count;
}

esp_err_t web_server_get_stats(uint32_t *total_requests,
                               uint8_t *active_connections,
                               uint8_t *ws_clients)
{
    if (total_requests) {
        *total_requests = server_state.total_requests;
    }
    if (active_connections) {
        *active_connections = server_state.active_connections;
    }
    if (ws_clients) {
        *ws_clients = server_state.ws_client_count;
    }
    
    return ESP_OK;
}

// ============================================================================
// HTTP Request Handlers
// ============================================================================

/**
 * @brief Root page handler
 */
static esp_err_t root_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    if (!require_auth_page(req)) return ESP_OK;
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
#if WEB_FILES_GZIPPED
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
#endif
    httpd_resp_send(req, (const char *)index_html, index_html_len);
    return ESP_OK;
}

/**
 * @brief GET /main.js - Serve JavaScript file
 */
static esp_err_t js_main_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    if (!require_auth_page(req)) return ESP_OK;
    
    httpd_resp_set_type(req, "application/javascript");
    /* no-cache: after an OTA update the browser must not keep running the
     * old script against the new API */
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
#if WEB_FILES_GZIPPED
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
#endif
    httpd_resp_send(req, (const char *)main_js, main_js_len);
    return ESP_OK;
}

/**
 * @brief GET /api/config - Get full configuration
 */
static esp_err_t api_config_get_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    if (!require_auth(req)) return ESP_OK;

    config_t *config = config_get();
    if (!config) {
        send_error_response(req, 500, "Failed to get configuration");
        return ESP_OK;
    }
    
    cJSON *json = config_to_public_json(config);
    if (!json) {
        send_error_response(req, 500, "Out of memory");
        return ESP_OK;
    }

    send_json_response(req, json, 200);
    cJSON_Delete(json);
    
    return ESP_OK;
}

/**
 * @brief POST /api/config - Set configuration
 */
static esp_err_t api_config_post_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    if (!require_auth(req)) return ESP_OK;

    char *buf = NULL;
    esp_err_t err = read_body(req, &buf);
    if (err != ESP_OK) {
        send_error_response(req, err == ESP_ERR_INVALID_SIZE ? 400 : 500,
                            err == ESP_ERR_INVALID_SIZE ? "Request body missing or too large"
                                                        : "Failed to read request body");
        return ESP_OK;
    }

    // Passwords it sets must be valid (the rest is clamped when parsed)
    cJSON *doc = cJSON_Parse(buf);
    const bool has_network = cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(doc, "network"));
    const char *problem = doc ? config_update_problem(doc) : NULL;
    cJSON_Delete(doc);
    if (problem) {
        free(buf);
        send_error_response(req, 400, problem);
        return ESP_OK;
    }

    // Merges the (possibly partial) document onto the current configuration;
    // every value is range-checked by the config manager.
    err = config_from_json(buf);
    free(buf);
    if (err != ESP_OK) {
        send_error_response(req, 400, "Invalid JSON");
        return ESP_OK;
    }

    // Save configuration to storage
    err = config_save();
    if (err != ESP_OK) {
        send_error_response(req, 500, "Failed to save configuration");
        return ESP_OK;
    }

    // Everything applies immediately, network settings included
    config_notify_changed();
    if (has_network) {
        network_supervisor_reconfigure();
    }

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "ok");
    cJSON_AddStringToObject(response, "message", "Configuration saved and applied");
    send_json_response(req, response, 200);
    cJSON_Delete(response);
    
    ESP_LOGI(TAG, "Configuration updated via API");
    
    return ESP_OK;
}

/**
 * @brief GET /api/network/status - Get network status
 */
static esp_err_t api_network_status_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    if (!require_auth(req)) return ESP_OK;

    network_status_t status;
    if (network_get_status(&status) != ESP_OK) {
        send_error_response(req, 500, "Failed to get network status");
        return ESP_OK;
    }
    network_supervisor_status_t sup;
    network_supervisor_get_status(&sup);

    const char *mode_str, *primary;
    switch (status.state) {
        case NETWORK_ETHERNET_CONNECTED: mode_str = "Ethernet";     primary = "ethernet"; break;
        case NETWORK_WIFI_STA_CONNECTED: mode_str = "WiFi";         primary = "wifi";     break;
        case NETWORK_WIFI_AP_ACTIVE:     mode_str = "Access point"; primary = "ap";       break;
        case NETWORK_CONNECTING:         mode_str = "Connecting";   primary = "none";     break;
        default:                         mode_str = "Disconnected"; primary = "none";     break;
    }
    static const char *net_modes[] = {"auto", "ethernet", "wifi", "ap"};
    static const char *sta_states[] = {"off", "connecting", "getting_ip", "connected", "failed"};
    static const char *ap_reasons[] = {"off", "standalone", "always", "rescue", "linger"};
    char mac[18];

    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "state", status.state);
    cJSON_AddStringToObject(json, "mode", mode_str);
    cJSON_AddStringToObject(json, "primary", primary);
    cJSON_AddBoolToObject(json, "connected", network_is_connected());
    const char *ip = network_get_ip_address();
    cJSON_AddStringToObject(json, "ip_address", ip ? ip : "");
    if (ip) {
        cJSON_AddStringToObject(json, "netmask", status.netmask);
        cJSON_AddStringToObject(json, "gateway", status.gateway);
    }
    snprintf(mac, sizeof(mac), MACSTR, MAC2STR(status.mac));
    cJSON_AddStringToObject(json, "mac", mac);
    if (status.state == NETWORK_WIFI_STA_CONNECTED) {
        cJSON_AddNumberToObject(json, "rssi", status.rssi);
    }
    cJSON_AddStringToObject(json, "net_mode", sup.mode <= NET_MODE_LAST ? net_modes[sup.mode] : "auto");
    cJSON_AddBoolToObject(json, "ap_always", sup.ap_always);
    cJSON_AddStringToObject(json, "hostname", network_get_hostname());

#if CONFIG_NODE_ETHERNET
    cJSON *eth = cJSON_AddObjectToObject(json, "ethernet");
#else
    cJSON *eth = NULL;   /* not built in: no "ethernet" object at all */
#endif
    if (eth) {
        cJSON_AddBoolToObject(eth, "enabled", sup.eth_wanted);
        cJSON_AddBoolToObject(eth, "present", sup.eth.present);
        cJSON_AddBoolToObject(eth, "running", sup.eth.running);
        cJSON_AddBoolToObject(eth, "link", sup.eth.link);
        cJSON_AddBoolToObject(eth, "ready", sup.eth.ready);
        cJSON_AddBoolToObject(eth, "dhcp", sup.eth.dhcp);
        cJSON_AddStringToObject(eth, "ip", sup.eth.ready ? sup.eth.ip : "");
        cJSON_AddStringToObject(eth, "netmask", sup.eth.ready ? sup.eth.netmask : "");
        cJSON_AddStringToObject(eth, "gateway", sup.eth.ready ? sup.eth.gateway : "");
        snprintf(mac, sizeof(mac), MACSTR, MAC2STR(sup.eth.mac));
        cJSON_AddStringToObject(eth, "mac", mac);
    }

    cJSON *wifi = cJSON_AddObjectToObject(json, "wifi");
    if (wifi) {
        bool sta_up = sup.sta.state == NET_STA_CONNECTED;
        cJSON_AddBoolToObject(wifi, "enabled", sup.sta_wanted);
        cJSON_AddStringToObject(wifi, "state",
                                sup.sta.state <= NET_STA_FAILED ? sta_states[sup.sta.state] : "off");
        cJSON_AddStringToObject(wifi, "ssid", sup.sta.ssid);
        cJSON_AddBoolToObject(wifi, "dhcp", sup.sta.dhcp);
        cJSON_AddStringToObject(wifi, "ip", sta_up ? sup.sta.ip : "");
        cJSON_AddStringToObject(wifi, "netmask", sta_up ? sup.sta.netmask : "");
        cJSON_AddStringToObject(wifi, "gateway", sta_up ? sup.sta.gateway : "");
        if (sta_up) {
            cJSON_AddNumberToObject(wifi, "rssi", sup.sta.rssi);
            cJSON_AddNumberToObject(wifi, "channel", sup.sta.channel);
        }
        cJSON_AddNumberToObject(wifi, "reason", sup.sta.reason);
        cJSON_AddStringToObject(wifi, "reason_text", network_wifi_reason_text(sup.sta.reason));
        cJSON_AddNumberToObject(wifi, "retry_in", sup.sta_retry_in_s);
        cJSON_AddBoolToObject(wifi, "testing", sup.sta_test);
        snprintf(mac, sizeof(mac), MACSTR, MAC2STR(sup.sta.mac));
        cJSON_AddStringToObject(wifi, "mac", mac);
        cJSON *profs = cJSON_AddArrayToObject(wifi, "networks");
        for (int i = 0; profs && i < sup.profile_count && i < NETWORK_PROFILES_MAX; i++) {
            const network_profile_status_t *ps = &sup.profiles[i];
            cJSON *o = cJSON_CreateObject();
            if (!o) break;
            cJSON_AddStringToObject(o, "ssid", ps->ssid);
            cJSON_AddBoolToObject(o, "connected", sta_up && strcmp(sup.sta.ssid, ps->ssid) == 0);
            cJSON_AddBoolToObject(o, "visible", ps->visible);
            if (ps->visible) cJSON_AddNumberToObject(o, "rssi", ps->rssi);
            cJSON_AddBoolToObject(o, "tried", ps->tried);
            cJSON_AddNumberToObject(o, "reason", ps->reason);
            cJSON_AddStringToObject(o, "reason_text", network_wifi_reason_text(ps->reason));
            cJSON_AddItemToArray(profs, o);
        }
    }

    cJSON *ap = cJSON_AddObjectToObject(json, "ap");
    if (ap) {
        cJSON_AddBoolToObject(ap, "active", sup.ap.active);
        cJSON_AddStringToObject(ap, "ssid", sup.ap.ssid[0] ? sup.ap.ssid : config_get()->network.ap_ssid);
        cJSON_AddStringToObject(ap, "ip", sup.ap.active ? sup.ap.ip : "");
        cJSON_AddNumberToObject(ap, "channel", sup.ap.channel);
        cJSON_AddNumberToObject(ap, "clients", sup.ap.clients);
        cJSON_AddStringToObject(ap, "reason",
                                sup.ap_reason <= NET_AP_LINGER ? ap_reasons[sup.ap_reason] : "off");
        cJSON_AddNumberToObject(ap, "off_in", sup.ap_off_in_s);
        snprintf(mac, sizeof(mac), MACSTR, MAC2STR(sup.ap.mac));
        cJSON_AddStringToObject(ap, "mac", mac);
    }

    send_json_response(req, json, 200);
    cJSON_Delete(json);
    return ESP_OK;
}

/**
 * @brief POST /api/network/wifi/connect {"ssid": "..."} - join a saved network now
 */
static esp_err_t api_network_wifi_connect_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    if (!require_auth(req)) return ESP_OK;

    char *buf = NULL;
    if (read_body(req, &buf) != ESP_OK) {
        send_error_response(req, 400, "Request body missing or too large");
        return ESP_OK;
    }
    cJSON *doc = cJSON_Parse(buf);
    free(buf);
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(doc, "ssid");
    char name[33] = {0};
    if (cJSON_IsString(ssid) && ssid->valuestring[0] && strlen(ssid->valuestring) < sizeof(name)) {
        strcpy(name, ssid->valuestring);
    }
    cJSON_Delete(doc);
    if (!name[0]) {
        send_error_response(req, 400, "ssid required");
        return ESP_OK;
    }

    const config_t *cfg = config_get();
    bool saved = false;
    for (int i = 0; i < cfg->network.wifi_profile_count && i < CONFIG_WIFI_PROFILES_MAX; i++) {
        if (strcmp(cfg->network.wifi_profiles[i].ssid, name) == 0) saved = true;
    }
    if (!saved) {
        send_error_response(req, 404, "Network is not saved");
        return ESP_OK;
    }
    if (network_supervisor_connect_now(name) != ESP_OK) {
        send_error_response(req, 503, "Network supervisor busy, try again");
        return ESP_OK;
    }
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "status", "connecting");
    send_json_response(req, json, 202);
    cJSON_Delete(json);
    return ESP_OK;
}

/**
 * @brief POST /api/network/ap/off - switch off an AP kept up after joining a network
 */
static esp_err_t api_network_ap_off_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    if (!require_auth(req)) return ESP_OK;
    network_supervisor_ap_dismiss();
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "status", "ok");
    send_json_response(req, json, 200);
    cJSON_Delete(json);
    return ESP_OK;
}

/**
 * @brief GET /api/ports/status - Get all ports status
 */
static esp_err_t api_ports_status_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    if (!require_auth(req)) return ESP_OK;

    config_t *cfg = config_get();
    cJSON *json = cJSON_CreateArray();
    static const char *proto_names[] = {"artnet", "sacn", "artnet+sacn(artnet wins)",
                                        "artnet+sacn(sacn wins)", "artnet+sacn"};

    for (uint8_t p = 1; p <= CONFIG_DMX_PORTS; p++) {
        const port_config_t *pc = config_port(cfg, p);
        dmx_port_status_t st;
        if (dmx_handler_get_port_status(p, &st) != ESP_OK) continue;

        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "port", p);
        cJSON_AddBoolToObject(o, "active", st.is_active);
        cJSON_AddNumberToObject(o, "mode", st.mode);
        cJSON_AddNumberToObject(o, "universe", pc->universe_primary);
        cJSON_AddNumberToObject(o, "artnet_universe", pc->universe_primary);
        cJSON_AddNumberToObject(o, "sacn_universe", pc->sacn_universe);
        cJSON_AddStringToObject(o, "protocol",
                                pc->protocol_mode <= PROTOCOL_MERGE_BOTH ? proto_names[pc->protocol_mode] : "?");
        cJSON_AddNumberToObject(o, "frames_sent", st.stats.frames_sent);
        cJSON_AddNumberToObject(o, "frames_received", st.stats.frames_received);
        cJSON_AddNumberToObject(o, "errors", st.stats.error_count);
        cJSON_AddBoolToObject(o, "signal", st.stats.signal_present);
        cJSON_AddNumberToObject(o, "slots", st.stats.last_slot_count);
        cJSON_AddBoolToObject(o, "rdm_available", st.rdm_available);
        cJSON_AddBoolToObject(o, "rdm_discovering", st.rdm_discovering);
        cJSON_AddNumberToObject(o, "rdm_devices", st.rdm_device_count);
        cJSON_AddBoolToObject(o, "rdm_tod_valid", st.rdm_tod_valid);
        cJSON_AddBoolToObject(o, "output_active", merge_engine_is_output_active(p));
        merge_sync_t sync = merge_engine_get_sync_state(p);
        cJSON_AddStringToObject(o, "sync", sync == MERGE_SYNC_ARTNET ? "artnet" :
                                           sync == MERGE_SYNC_SACN ? "sacn" : "none");
        cJSON_AddNumberToObject(o, "scene", scene_active(p));
        cJSON_AddBoolToObject(o, "failsafe", merge_engine_in_failsafe(p));
        if (dmx_tester_enabled()) {
            cJSON_AddStringToObject(o, "tester_role", config_tester_role_name(cfg->tester.ports[p - 1].role));
        }
        dmx_output_timing_t tm;
        if (dmx_handler_get_output_timing(p, &tm) == ESP_OK) {
            cJSON *t = cJSON_AddObjectToObject(o, "timing");
            cJSON_AddNumberToObject(t, "rate_hz", tm.rate_hz);
            cJSON_AddNumberToObject(t, "break_us", tm.break_us);
            cJSON_AddNumberToObject(t, "mab_us", tm.mab_us);
            cJSON_AddNumberToObject(t, "slots", tm.slots);
        }
        dmx_test_t test;
        if (dmx_test_get(p, &test)) {
            cJSON *t = cJSON_AddObjectToObject(o, "test");
            web_test_to_json(t, &test);
        }
        dmx_input_analysis_t an;
        if (dmx_handler_get_analysis(p, &an) == ESP_OK) {
            web_analysis_to_json(cJSON_AddObjectToObject(o, "analysis"), &an);
        }

        merge_source_info_t src[MERGE_MAX_SOURCES];
        uint8_t n = merge_engine_get_sources(p, src, MERGE_MAX_SOURCES);
        cJSON *arr = cJSON_AddArrayToObject(o, "sources");
        for (uint8_t i = 0; arr && i < n; i++) {
            cJSON *so = cJSON_CreateObject();
            struct in_addr a = {.s_addr = src[i].source_ip};
            cJSON_AddStringToObject(so, "protocol",
                                    src[i].protocol == SOURCE_PROTOCOL_SACN ? "sACN" :
                                    src[i].protocol == SOURCE_PROTOCOL_ARTNET ? "Art-Net" : "DMX");
            cJSON_AddStringToObject(so, "ip", inet_ntoa(a));
            cJSON_AddStringToObject(so, "name", src[i].name);
            cJSON_AddNumberToObject(so, "priority", src[i].priority);
            cJSON_AddBoolToObject(so, "per_address_priority", src[i].uses_pap);
            cJSON_AddBoolToObject(so, "backup", src[i].backup);
            cJSON_AddBoolToObject(so, "contributing", src[i].contributing);
            cJSON_AddNumberToObject(so, "age_ms", src[i].age_ms);
            cJSON_AddNumberToObject(so, "packets", src[i].packets);
            cJSON_AddItemToArray(arr, so);
        }
        cJSON_AddItemToArray(json, o);
    }

    send_json_response(req, json, 200);
    cJSON_Delete(json);

    return ESP_OK;
}

/**
 * @brief GET /api/ports/{id}/config - Get port configuration
 */
static esp_err_t api_port_config_get_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    if (!require_auth(req)) return ESP_OK;

    int port = get_port_from_uri(req->uri);
    if (port < 1 || port > CONFIG_DMX_PORTS) {
        send_error_response(req, 400, "Invalid port number");
        return ESP_OK;
    }

    cJSON *json = config_port_to_json(config_port(config_get(), port));
    cJSON_AddNumberToObject(json, "port", port);
    
    send_json_response(req, json, 200);
    cJSON_Delete(json);
    
    return ESP_OK;
}

/**
 * @brief POST /api/ports/{id}/blackout - Blackout port
 */
static esp_err_t api_port_blackout_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    if (!require_auth(req)) return ESP_OK;

    int port = get_port_from_uri(req->uri);
    if (port < 1 || port > CONFIG_DMX_PORTS) {
        send_error_response(req, 400, "Invalid port number");
        return ESP_OK;
    }

    // Blackout the wire, not just the fallback buffer: drop network sources,
    // release any scene/local levels and zero the merged output. New network
    // data takes over again as soon as it arrives.
    scene_release(port, 0);
    esp_err_t ret = merge_engine_blackout(port);
    if (dmx_tester_enabled()) {
        dmx_tester_fill(port, 1, 512, 0);   /* tester: the levels it sends */
    }
    
    cJSON *json = cJSON_CreateObject();
    if (ret == ESP_OK) {
        cJSON_AddStringToObject(json, "status", "ok");
        cJSON_AddNumberToObject(json, "port", port);
        send_json_response(req, json, 200);
    } else {
        cJSON_AddStringToObject(json, "status", "error");
        cJSON_AddStringToObject(json, "message", "Blackout failed");
        send_json_response(req, json, 500);
    }
    
    cJSON_Delete(json);
    
    return ESP_OK;
}

/**
 * @brief GET /api/system/info - Get system information
 */
static esp_err_t api_system_info_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    if (!require_auth(req)) return ESP_OK;

    cJSON *json = cJSON_CreateObject();
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    cJSON_AddStringToObject(json, "firmware_version", fw_version());
    cJSON_AddStringToObject(json, "project", app->project_name);
    cJSON_AddStringToObject(json, "build_date", app->date);
    cJSON_AddStringToObject(json, "build_time", app->time);
    cJSON_AddStringToObject(json, "hardware", BOARD_NAME " (" WEB_CHIP_NAME ")");
    cJSON_AddStringToObject(json, "board", BOARD_NAME);
    cJSON_AddStringToObject(json, "chip", WEB_CHIP_NAME);
    cJSON_AddNumberToObject(json, "ports", CONFIG_DMX_PORTS);
    /* What this firmware was built with (menuconfig: ESP-NODE > Features);
     * the web UI hides what is missing */
    cJSON *feat = cJSON_AddObjectToObject(json, "features");
    if (feat) {
        cJSON_AddBoolToObject(feat, "ethernet", WEB_HAS(CONFIG_NODE_ETHERNET));
        cJSON_AddBoolToObject(feat, "sacn", WEB_HAS(CONFIG_NODE_SACN));
        cJSON_AddBoolToObject(feat, "rdm", WEB_HAS(CONFIG_NODE_RDM));
        cJSON_AddBoolToObject(feat, "scenes", WEB_HAS(CONFIG_NODE_SCENES));
        cJSON_AddBoolToObject(feat, "dmx_tools", WEB_HAS(CONFIG_NODE_DMX_TOOLS));
        cJSON_AddBoolToObject(feat, "web_log", WEB_HAS(CONFIG_NODE_WEB_LOG));
        cJSON_AddBoolToObject(feat, "mdns", WEB_HAS(CONFIG_NODE_MDNS));
        cJSON_AddBoolToObject(feat, "tester", WEB_HAS(CONFIG_NODE_TESTER));
    }
    cJSON *tst = cJSON_AddObjectToObject(json, "tester");
    if (tst) {
        cJSON_AddBoolToObject(tst, "available", WEB_HAS(CONFIG_NODE_TESTER));
        cJSON_AddBoolToObject(tst, "enabled", dmx_tester_enabled());          /* running now */
        cJSON_AddBoolToObject(tst, "next_boot", config_get()->tester.enabled);
    }
    cJSON_AddStringToObject(json, "idf_version", esp_get_idf_version());
    cJSON_AddStringToObject(json, "partition", running ? running->label : "?");
    cJSON_AddNumberToObject(json, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(json, "min_free_heap", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(json, "uptime_sec", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(json, "reset_reason", esp_reset_reason());
    
    send_json_response(req, json, 200);
    cJSON_Delete(json);
    
    return ESP_OK;
}

/**
 * @brief GET /api/system/stats - Get system statistics
 */
static esp_err_t api_system_stats_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    if (!require_auth(req)) return ESP_OK;

    cJSON *json = cJSON_CreateObject();
    
    // Art-Net stats
    artnet_stats_t artnet_stats;
    if (artnet_receiver_get_stats(&artnet_stats) == ESP_OK) {
        cJSON *artnet = cJSON_CreateObject();
        cJSON_AddNumberToObject(artnet, "packets", artnet_stats.packets_received);
        cJSON_AddNumberToObject(artnet, "dmx_packets", artnet_stats.dmx_packets);
        cJSON_AddNumberToObject(artnet, "poll_packets", artnet_stats.poll_packets);
        cJSON_AddNumberToObject(artnet, "poll_replies", artnet_stats.poll_replies_sent);
        cJSON_AddNumberToObject(artnet, "address_packets", artnet_stats.address_packets);
        cJSON_AddNumberToObject(artnet, "invalid_packets", artnet_stats.invalid_packets);
        cJSON_AddItemToObject(json, "artnet", artnet);
    }
    
    // sACN stats
    sacn_stats_t sacn_stats;
    if (sacn_receiver_get_stats(&sacn_stats) == ESP_OK) {
        cJSON *sacn = cJSON_CreateObject();
        cJSON_AddNumberToObject(sacn, "packets", sacn_stats.packets_received);
        cJSON_AddNumberToObject(sacn, "data_packets", sacn_stats.data_packets);
        cJSON_AddNumberToObject(sacn, "pap_packets", sacn_stats.pap_packets);
        cJSON_AddNumberToObject(sacn, "terminated", sacn_stats.terminated_packets);
        cJSON_AddNumberToObject(sacn, "invalid_packets", sacn_stats.invalid_packets);
        cJSON_AddNumberToObject(sacn, "subscriptions", sacn_receiver_get_subscription_count());
        cJSON_AddItemToObject(json, "sacn", sacn);
    }
    
    // Merge engine stats ("merge_port1", ...)
    for (int p = 1; p <= CONFIG_DMX_PORTS; p++) {
        merge_stats_t ms;
        if (merge_engine_get_stats(p, &ms) != ESP_OK) continue;
        cJSON *merge = cJSON_CreateObject();
        cJSON_AddNumberToObject(merge, "active_sources", ms.active_sources);
        cJSON_AddNumberToObject(merge, "total_merges", ms.total_merges);
        cJSON_AddNumberToObject(merge, "sequence_drops", ms.sequence_drops);
        cJSON_AddNumberToObject(merge, "source_timeouts", ms.source_timeouts);
        cJSON_AddNumberToObject(merge, "sources_rejected", ms.sources_rejected);
        cJSON_AddNumberToObject(merge, "backup_switches", ms.backup_switches);
        char key[16];
        snprintf(key, sizeof(key), "merge_port%d", p);
        cJSON_AddItemToObject(json, key, merge);
    }

    send_json_response(req, json, 200);
    cJSON_Delete(json);
    
    return ESP_OK;
}

/**
 * @brief POST /api/system/factory-reset - Reset to defaults and restart
 */
static esp_err_t api_system_factory_reset_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    if (!require_auth(req)) return ESP_OK;

    esp_err_t err = config_reset_to_defaults();   /* also saves */
    if (err != ESP_OK) {
        send_error_response(req, 500, "Failed to reset configuration");
        return ESP_OK;
    }

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "status", "ok");
    cJSON_AddStringToObject(json, "message", "Factory reset complete — restarting in 2 seconds");
    send_json_response(req, json, 200);
    cJSON_Delete(json);

    ESP_LOGI(TAG, "Factory reset initiated via API");
    restart_later(2000);

    return ESP_OK;
}

/**
 * @brief POST /api/system/firmware-upload - OTA firmware update
 *
 * Accepts a raw binary body (the .bin firmware image). Writes it to the
 * next OTA partition and reboots on success.
 */
static esp_err_t api_system_ota_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    if (!require_auth(req)) return ESP_OK;

    if (req->content_len == 0) {
        send_error_response(req, 400, "No firmware data received");
        return ESP_OK;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        send_error_response(req, 500, "No OTA partition: flash the new partition table over USB once");
        return ESP_OK;
    }
    if (req->content_len > update_partition->size) {
        send_error_response(req, 400, "Image larger than the OTA partition");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "OTA update: %d bytes -> %s", req->content_len, update_partition->label);

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        send_error_response(req, 500, "OTA begin failed");
        return ESP_OK;
    }

    char *buf = malloc(4096);
    if (!buf) {
        esp_ota_abort(ota_handle);
        send_error_response(req, 500, "Memory allocation failed");
        return ESP_OK;
    }

    int remaining = req->content_len;
    int timeouts = 0;
    bool write_error = false;
    bool header_checked = false;

    while (remaining > 0) {
        int to_read = (remaining < 4096) ? remaining : 4096;
        int received = httpd_req_recv(req, buf, to_read);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            /* slow WiFi links stall for a while; retry instead of failing */
            if (++timeouts > 10) {
                ESP_LOGE(TAG, "OTA receive timed out");
                write_error = true;
                break;
            }
            continue;
        }
        if (received <= 0) {
            ESP_LOGE(TAG, "OTA receive error %d", received);
            write_error = true;
            break;
        }
        timeouts = 0;

        if (!header_checked) {
            /* First byte of an ESP image is the magic 0xE9 */
            if ((uint8_t)buf[0] != 0xE9) {
                ESP_LOGE(TAG, "Not an ESP firmware image");
                write_error = true;
                break;
            }
            header_checked = true;
        }

        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write error: %s", esp_err_to_name(err));
            write_error = true;
            break;
        }
        remaining -= received;
    }

    free(buf);

    if (write_error) {
        esp_ota_abort(ota_handle);
        send_error_response(req, 400, "OTA upload failed (not a valid firmware .bin or connection lost)");
        return ESP_OK;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        send_error_response(req, 400, "Image validation failed - file may be corrupt or for another chip");
        return ESP_OK;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        send_error_response(req, 500, "Failed to set boot partition");
        return ESP_OK;
    }

    esp_app_desc_t new_desc;
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "status", "ok");
    if (esp_ota_get_partition_description(update_partition, &new_desc) == ESP_OK) {
        cJSON_AddStringToObject(json, "version", new_desc.version);
    }
    cJSON_AddStringToObject(json, "message", "Firmware written - restarting in 3 seconds");
    send_json_response(req, json, 200);
    cJSON_Delete(json);

    ESP_LOGI(TAG, "OTA complete - rebooting into %s", update_partition->label);
    restart_later(3000);

    return ESP_OK;
}

/**
 * @brief POST /api/rdm/discover - Trigger RDM discovery and return results
 *
 * Runs discovery synchronously on both ports (that are in RDM master mode)
 * and returns the combined device list. Discovery can take several seconds.
 */
static esp_err_t api_rdm_discover_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    if (!require_auth(req)) return ESP_OK;

    /* Start discovery on every RDM-capable port at once (they run in
     * parallel, interleaved with DMX output), then wait for them. */
    bool started[CONFIG_DMX_PORTS + 1] = {false};
    for (int p = 1; p <= CONFIG_DMX_PORTS; p++) {
        if (!dmx_handler_rdm_available(p)) continue;
        /* A background (incremental) pass may be running: let it finish,
         * then do the full discovery that was asked for */
        if (dmx_handler_rdm_is_discovering(p)) {
            dmx_handler_rdm_wait_discovery(p, 10000);
        }
        if (dmx_handler_rdm_discover(p) == ESP_OK) {
            started[p] = true;
        }
    }
    for (int p = 1; p <= CONFIG_DMX_PORTS; p++) {
        if (started[p] && dmx_handler_rdm_wait_discovery(p, 20000) != ESP_OK) {
            ESP_LOGW(TAG, "RDM discovery on port %d still running", p);
        }
    }
    return api_rdm_devices_handler(req);
}

static void add_rdm_devices(cJSON *json, int p)
{
    rdm_device_t *devices = malloc(sizeof(rdm_device_t) * DMX_MAX_DEVICES);
    if (!devices) return;
    size_t count = DMX_MAX_DEVICES;
    if (dmx_handler_get_rdm_devices(p, devices, &count) == ESP_OK) {
        for (size_t i = 0; i < count; i++) {
            cJSON *d = cJSON_CreateObject();
            char uid_str[32];
            snprintf(uid_str, sizeof(uid_str), "%04x:%08lx",
                     devices[i].uid.man_id, (unsigned long)devices[i].uid.dev_id);
            cJSON_AddNumberToObject(d, "port", p);
            cJSON_AddStringToObject(d, "uid", uid_str);
            cJSON_AddStringToObject(d, "label",
                devices[i].device_label[0] ? devices[i].device_label : "(none)");
            cJSON_AddNumberToObject(d, "dmx_address", devices[i].dmx_start_address);
            cJSON_AddStringToObject(d, "manufacturer",
                devices[i].manufacturer_label[0] ? devices[i].manufacturer_label : "(unknown)");
            cJSON_AddNumberToObject(d, "model", devices[i].device_model);
            cJSON_AddStringToObject(d, "model_description", devices[i].device_model_desc);
            cJSON_AddStringToObject(d, "software", devices[i].software_label);
            cJSON_AddNumberToObject(d, "footprint", devices[i].dmx_footprint);
            cJSON_AddNumberToObject(d, "personality", devices[i].current_personality);
            cJSON_AddNumberToObject(d, "personality_count", devices[i].personality_count);
            cJSON_AddItemToArray(json, d);
        }
    }
    free(devices);
}

/**
 * @brief GET /api/rdm/devices - Devices found by the last discovery
 */
static esp_err_t api_rdm_devices_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    if (!require_auth(req)) return ESP_OK;

    cJSON *json = cJSON_CreateArray();
    for (int p = 1; p <= CONFIG_DMX_PORTS; p++) {
        add_rdm_devices(json, p);
    }
    send_json_response(req, json, 200);
    cJSON_Delete(json);
    return ESP_OK;
}

static bool parse_uid(const char *s, rdm_uid_t *uid)
{
    unsigned int man_id = 0;
    unsigned long dev_id = 0;
    if (!s || sscanf(s, "%x:%lx", &man_id, &dev_id) != 2 || man_id > 0xFFFF) {
        return false;
    }
    memset(uid, 0, sizeof(*uid));
    uid->man_id = (uint16_t)man_id;
    uid->dev_id = (uint32_t)dev_id;
    return true;
}

/**
 * @brief POST /api/rdm/address - Set a device's DMX start address
 *
 * Body: {"port":1,"uid":"1234:56789abc","address":101}
 */
static esp_err_t api_rdm_address_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    if (!require_auth(req)) return ESP_OK;

    cJSON *body = read_json_body(req);
    if (!body) return ESP_OK;

    cJSON *jport = cJSON_GetObjectItem(body, "port");
    cJSON *juid  = cJSON_GetObjectItem(body, "uid");
    cJSON *jaddr = cJSON_GetObjectItem(body, "address");
    rdm_uid_t uid;
    if (!cJSON_IsNumber(jport) || !cJSON_IsNumber(jaddr) || !cJSON_IsString(juid) ||
        !parse_uid(juid->valuestring, &uid) || jaddr->valueint < 1 || jaddr->valueint > 512) {
        cJSON_Delete(body);
        send_error_response(req, 400, "port, uid and address (1-512) required");
        return ESP_OK;
    }
    int port = jport->valueint;
    uint16_t address = (uint16_t)jaddr->valueint;
    cJSON_Delete(body);

    esp_err_t err = (port >= 1 && port <= CONFIG_DMX_PORTS) ? dmx_handler_rdm_set_address(port, uid, address)
                                                           : ESP_ERR_INVALID_ARG;
    if (err == ESP_OK) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "status", "ok");
        send_json_response(req, resp, 200);
        cJSON_Delete(resp);
    } else {
        send_error_response(req, err == ESP_ERR_TIMEOUT ? 503 : 500,
                            err == ESP_ERR_TIMEOUT ? "Device did not respond" : "RDM SET DMX_START_ADDRESS failed");
    }
    return ESP_OK;
}

/**
 * @brief GET /api/ports/{id}/dmx[?count=N] - levels the port sends (output)
 *        or receives (input); first 8 channels unless count is given
 */
static esp_err_t api_port_dmx_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    if (!require_auth(req)) return ESP_OK;

    int port = get_port_from_uri(req->uri);
    if (port < 1 || port > CONFIG_DMX_PORTS) {
        send_error_response(req, 400, "Invalid port number");
        return ESP_OK;
    }

    uint8_t dmx_data[512];
    bool input = false;
    bool active = port_levels(port, dmx_data, &input);

    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "port", port);
    cJSON_AddBoolToObject(json, "active", active);
    cJSON_AddBoolToObject(json, "input", input);

    /* ?count=N (1-512) selects how many channels to return; default 8 */
    int count = 8;
    char query[32];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[8];
        if (httpd_query_key_value(query, "count", val, sizeof(val)) == ESP_OK) {
            count = atoi(val);
            if (count < 1) count = 1;
            if (count > 512) count = 512;
        }
    }
    cJSON *channels = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON_AddItemToArray(channels, cJSON_CreateNumber(dmx_data[i]));
    }
    cJSON_AddItemToObject(json, "channels", channels);

    send_json_response(req, json, 200);
    cJSON_Delete(json);

    return ESP_OK;
}

/**
 * @brief POST /api/system/restart - Restart device
 */
static esp_err_t api_system_restart_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    if (!require_auth(req)) return ESP_OK;

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "status", "ok");
    cJSON_AddStringToObject(json, "message", "Restarting in 2 seconds...");
    send_json_response(req, json, 200);
    cJSON_Delete(json);
    
    restart_later(2000);
    
    return ESP_OK;
}

/**
 * @brief WebSocket handler
 *
 * FIX 5: Each send path now uses its own local httpd_ws_frame_t rather than
 * reusing the receive ws_pkt. Reusing the receive struct caused ws_pkt.payload
 * and ws_pkt.len to carry stale receive-buffer values into the send call on
 * ping/close frames (where ws_pkt.len == 0 and no payload allocation occurs),
 * which could corrupt the outgoing frame or dereference freed memory.
 */
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket handshake for: %s", req->uri);
        if (!ws_token_ok(req)) {
            /* httpd has already answered the upgrade; failing here closes it */
            ESP_LOGW(TAG, "WebSocket rejected: missing or stale token");
            return ESP_FAIL;
        }
        
        // Add client to list
        xSemaphoreTake(server_state.ws_mutex, portMAX_DELAY);
        
        for (int i = 0; i < MAX_WS_CLIENTS; i++) {
            if (!server_state.ws_clients[i].active) {
                server_state.ws_clients[i].fd = httpd_req_to_sockfd(req);
                size_t ul = strnlen(req->uri, sizeof(server_state.ws_clients[i].path) - 1);
                memcpy(server_state.ws_clients[i].path, req->uri, ul);
                server_state.ws_clients[i].path[ul] = '\0';
                server_state.ws_clients[i].active = true;
                server_state.ws_client_count++;
                ESP_LOGI(TAG, "WebSocket client connected: %s (fd=%d)",
                         req->uri, server_state.ws_clients[i].fd);
                break;
            }
        }
        
        xSemaphoreGive(server_state.ws_mutex);
        
        return ESP_OK;
    }

    // --- Receive frame (two-pass: length probe then full read) ---
    httpd_ws_frame_t rx_pkt;
    memset(&rx_pkt, 0, sizeof(httpd_ws_frame_t));
    rx_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    // First pass: get frame length without a payload buffer
    esp_err_t ret = httpd_ws_recv_frame(req, &rx_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame (length probe) failed: %d", ret);
        return ret;
    }

    // Handle close/ping frames that carry no meaningful payload
    if (rx_pkt.type == HTTPD_WS_TYPE_CLOSE || rx_pkt.type == HTTPD_WS_TYPE_PING) {
        if (rx_pkt.type == HTTPD_WS_TYPE_CLOSE) {
            // Remove client from the active list
            ws_remove_fd(httpd_req_to_sockfd(req));
        }
        return ESP_OK;
    }

    if (rx_pkt.len == 0) {
        // Empty text/binary frame - nothing to process
        return ESP_OK;
    }
    if (rx_pkt.len > 2048) {
        ESP_LOGW(TAG, "WebSocket frame too large (%u bytes)", (unsigned)rx_pkt.len);
        return ESP_FAIL;
    }

    // Second pass: read the full payload
    uint8_t *buf = malloc(rx_pkt.len + 1);
    if (!buf) {
        ESP_LOGE(TAG, "WebSocket: out of memory for %zu-byte payload", rx_pkt.len);
        return ESP_ERR_NO_MEM;
    }
    rx_pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &rx_pkt, rx_pkt.len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame (payload) failed: %d", ret);
        free(buf);
        return ret;
    }
    buf[rx_pkt.len] = '\0';
    ESP_LOGD(TAG, "WebSocket received: %s", (char *)buf);

    // --- Parse and dispatch command ---
    cJSON *cmd = cJSON_Parse((char *)buf);
    free(buf);

    if (!cmd) {
        ESP_LOGW(TAG, "WebSocket: invalid JSON");
        return ESP_OK;
    }

    cJSON *cmd_type = cJSON_GetObjectItem(cmd, "command");
    if (!cmd_type || !cJSON_IsString(cmd_type)) {
        cJSON_Delete(cmd);
        return ESP_OK;
    }

    const char *command = cmd_type->valuestring;

    // Helper macro: send a JSON response over this WebSocket connection.
    // Uses its own local tx_pkt (FIX 5) - completely independent of rx_pkt.
    #define WS_SEND_JSON(json_obj) do { \
        char *_s = cJSON_PrintUnformatted(json_obj); \
        if (_s) { \
            httpd_ws_frame_t tx_pkt; \
            memset(&tx_pkt, 0, sizeof(tx_pkt)); \
            tx_pkt.type = HTTPD_WS_TYPE_TEXT; \
            tx_pkt.payload = (uint8_t *)_s; \
            tx_pkt.len = strlen(_s); \
            httpd_ws_send_frame(req, &tx_pkt); \
            free(_s); \
        } \
    } while (0)

    if (strcmp(command, "set_channel") == 0) {
        // {"command":"set_channel","port":1,"channel":1,"value":255}
        cJSON *port_obj    = cJSON_GetObjectItem(cmd, "port");
        cJSON *channel_obj = cJSON_GetObjectItem(cmd, "channel");
        cJSON *value_obj   = cJSON_GetObjectItem(cmd, "value");
        
        if (port_obj && channel_obj && value_obj &&
            cJSON_IsNumber(port_obj) && cJSON_IsNumber(channel_obj) &&
            cJSON_IsNumber(value_obj)) {
            
            int port    = port_obj->valueint;
            int channel = channel_obj->valueint;
            int value   = value_obj->valueint;
            
            if (port >= 1 && port <= DMX_MAX_PORTS &&
                channel >= 1 && channel <= DMX_MAX_CHANNELS &&
                value >= 0 && value <= DMX_MAX_VALUE) {
                
                ESP_LOGD(TAG, "WebSocket: set DMX port %d ch %d = %d",
                         port, channel, value);

                if (dmx_tester_enabled()) {
                    /* tester: the level the send port outputs */
                    uint8_t v = (uint8_t)value;
                    dmx_tester_set_levels((uint8_t)port, (uint16_t)channel, &v, 1);
                } else {
                    /* Local layer: HTP on top of network data, held until
                     * released, other channels untouched */
                    merge_engine_set_local_channel(port, (uint16_t)channel, (uint8_t)value);
                }
                
                cJSON *resp = cJSON_CreateObject();
                cJSON_AddStringToObject(resp, "status", "ok");
                cJSON_AddStringToObject(resp, "command", "set_channel");
                WS_SEND_JSON(resp);
                cJSON_Delete(resp);
            } else {
                ESP_LOGW(TAG, "WebSocket: set_channel parameters out of range");
            }
        }
    }
    else if (strcmp(command, "blackout") == 0) {
        // {"command":"blackout","port":1}
        cJSON *port_obj = cJSON_GetObjectItem(cmd, "port");
        if (port_obj && cJSON_IsNumber(port_obj)) {
            int port = port_obj->valueint;
            if (port >= 1 && port <= DMX_MAX_PORTS) {
                ESP_LOGI(TAG, "WebSocket: blackout port %d", port);
                scene_release(port, 0);
                merge_engine_blackout(port);
                if (dmx_tester_enabled()) dmx_tester_fill((uint8_t)port, 1, 512, 0);
                
                cJSON *resp = cJSON_CreateObject();
                cJSON_AddStringToObject(resp, "status", "ok");
                cJSON_AddStringToObject(resp, "command", "blackout");
                WS_SEND_JSON(resp);
                cJSON_Delete(resp);
            }
        }
    }
    else if (strcmp(command, "release") == 0) {
        // {"command":"release","port":1}  - drop manual/scene levels
        cJSON *port_obj = cJSON_GetObjectItem(cmd, "port");
        int port = cJSON_IsNumber(port_obj) ? port_obj->valueint : 0;
        if (port >= 0 && port <= DMX_MAX_PORTS) {
            scene_release((uint8_t)port, 0);
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddStringToObject(resp, "status", "ok");
            cJSON_AddStringToObject(resp, "command", "release");
            WS_SEND_JSON(resp);
            cJSON_Delete(resp);
        }
    }
    else if (strcmp(command, "get_status") == 0) {
        // {"command":"get_status"}
        ESP_LOGD(TAG, "WebSocket: get_status");
        
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "status", "ok");
        cJSON_AddStringToObject(resp, "command", "get_status");
        cJSON_AddNumberToObject(resp, "uptime",
                                (double)esp_timer_get_time() / 1000000.0);
        cJSON_AddNumberToObject(resp, "free_heap", esp_get_free_heap_size());
        WS_SEND_JSON(resp);
        cJSON_Delete(resp);
    }
    else {
        ESP_LOGW(TAG, "WebSocket: unknown command: %s", command);
    }

    #undef WS_SEND_JSON

    cJSON_Delete(cmd);
    return ESP_OK;
}

// ============================================================================
// New API Handlers
// ============================================================================

/**
 * @brief GET /api/network/wifi-scan — Scan for nearby WiFi networks
 */
static esp_err_t api_network_wifi_scan_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    if (!require_auth(req)) return ESP_OK;

    const uint16_t max_aps = 24;
    wifi_ap_record_t *ap_records = malloc(sizeof(wifi_ap_record_t) * max_aps);
    if (!ap_records) {
        send_error_response(req, 500, "Memory allocation failed");
        return ESP_OK;
    }
    /* Works in every mode (the station is enabled just for the scan). A
     * result younger than 8 s is reused, and while the node is joining a
     * network the last result is returned. */
    uint16_t ap_num = network_wifi_scan_cached(ap_records, max_aps, 8000);
    const config_t *cfg = config_get();

    /* One entry per network name (mesh systems have many access points),
     * keeping the strongest; the driver returns them strongest first */
    cJSON *json = cJSON_CreateArray();
    for (uint16_t i = 0; i < ap_num; i++) {
        const char *ssid = (const char *)ap_records[i].ssid;
        if (!ssid[0]) continue;
        bool seen = false;
        for (uint16_t j = 0; j < i; j++) {
            if (strcmp((const char *)ap_records[j].ssid, ssid) == 0) seen = true;
        }
        if (seen) continue;
        wifi_auth_mode_t auth = ap_records[i].authmode;
        bool saved = false;
        for (int k = 0; k < cfg->network.wifi_profile_count && k < CONFIG_WIFI_PROFILES_MAX; k++) {
            if (strcmp(cfg->network.wifi_profiles[k].ssid, ssid) == 0) saved = true;
        }
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", ssid);
        cJSON_AddNumberToObject(ap, "rssi", ap_records[i].rssi);
        cJSON_AddNumberToObject(ap, "channel", ap_records[i].primary);
        cJSON_AddNumberToObject(ap, "auth", auth);
        cJSON_AddBoolToObject(ap, "secure", auth != WIFI_AUTH_OPEN && auth != WIFI_AUTH_OWE);
        cJSON_AddBoolToObject(ap, "enterprise", auth == WIFI_AUTH_ENTERPRISE ||
                                                auth == WIFI_AUTH_WPA3_ENT_192 ||
                                                auth == WIFI_AUTH_WPA_ENTERPRISE);
        cJSON_AddBoolToObject(ap, "wep", auth == WIFI_AUTH_WEP);
        cJSON_AddBoolToObject(ap, "saved", saved);
        cJSON_AddItemToArray(json, ap);
    }
    free(ap_records);

    send_json_response(req, json, 200);
    cJSON_Delete(json);
    return ESP_OK;
}

/**
 * @brief POST /api/system/firmware-rollback — Roll back to previous OTA partition
 */
static esp_err_t api_system_firmware_rollback_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    if (!require_auth(req)) return ESP_OK;

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        send_error_response(req, 500, "Cannot determine running partition");
        return ESP_OK;
    }

    const esp_partition_t *next = esp_ota_get_next_update_partition(running);
    esp_app_desc_t other;
    if (!next || esp_ota_get_partition_description(next, &other) != ESP_OK) {
        send_error_response(req, 409, "No previous firmware to roll back to");
        return ESP_OK;
    }

    esp_err_t err = esp_ota_set_boot_partition(next);
    if (err != ESP_OK) {
        send_error_response(req, 500, "Previous firmware image is not valid");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Rolling back to %s (%s)", next->label, other.version);

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "status", "ok");
    cJSON_AddStringToObject(json, "message", "Rolling back — restarting in 2 seconds");
    send_json_response(req, json, 200);
    cJSON_Delete(json);

    ESP_LOGI(TAG, "Firmware rollback initiated");
    restart_later(2000);
    return ESP_OK;
}

/**
 * @brief POST /api/rdm/identify — Identify an RDM device
 *
 * Body: {"port":1,"uid":"1234:56789abc","enable":true}
 */
static esp_err_t api_rdm_identify_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    if (!require_auth(req)) return ESP_OK;

    cJSON *body = read_json_body(req);
    if (!body) return ESP_OK;

    cJSON *jport   = cJSON_GetObjectItem(body, "port");
    cJSON *juid    = cJSON_GetObjectItem(body, "uid");
    cJSON *jenable = cJSON_GetObjectItem(body, "enable");

    if (!cJSON_IsNumber(jport) || !cJSON_IsString(juid) || !cJSON_IsBool(jenable)) {
        cJSON_Delete(body);
        send_error_response(req, 400, "Missing/invalid fields: port, uid, enable required");
        return ESP_OK;
    }

    uint8_t port = (uint8_t)jport->valueint;
    bool enable  = cJSON_IsTrue(jenable);

    rdm_uid_t uid;
    if (!parse_uid(juid->valuestring, &uid)) {
        cJSON_Delete(body);
        send_error_response(req, 400, "Invalid UID format (expected XXXX:XXXXXXXX)");
        return ESP_OK;
    }
    cJSON_Delete(body);

    uint8_t enable_byte = enable ? 1 : 0;
    // RDM_PID_IDENTIFY_DEVICE = 0x1000
    esp_err_t err = dmx_handler_rdm_set(port, uid, 0x1000, &enable_byte, 1);

    cJSON *resp = cJSON_CreateObject();
    if (err == ESP_OK) {
        cJSON_AddStringToObject(resp, "status", "ok");
        send_json_response(req, resp, 200);
    } else {
        cJSON_AddStringToObject(resp, "status", "error");
        cJSON_AddStringToObject(resp, "message", "RDM SET IDENTIFY failed");
        send_json_response(req, resp, 500);
    }
    cJSON_Delete(resp);
    return ESP_OK;
}

/**
 * @brief GET /api/scenes — List all scenes
 */
static esp_err_t api_scenes_list_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    if (!require_auth(req)) return ESP_OK;

    dmx_scene_t *scenes = malloc(sizeof(dmx_scene_t) * SCENE_MAX_COUNT);
    if (!scenes) {
        send_error_response(req, 500, "Memory allocation failed");
        return ESP_OK;
    }
    size_t count = SCENE_MAX_COUNT;
    scene_list(scenes, &count);

    cJSON *json = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddNumberToObject(s, "scene_id",  scenes[i].scene_id);
        cJSON_AddStringToObject(s, "name",      scenes[i].name);
        cJSON_AddNumberToObject(s, "port",      scenes[i].port);
        cJSON_AddNumberToObject(s, "fade_ms",   scenes[i].fade_time_ms);
        cJSON_AddNumberToObject(s, "timestamp", scenes[i].timestamp);
        cJSON_AddBoolToObject(s, "active", scene_active(scenes[i].port) == scenes[i].scene_id);
        cJSON_AddItemToArray(json, s);
    }

    free(scenes);
    send_json_response(req, json, 200);
    cJSON_Delete(json);
    return ESP_OK;
}

/**
 * @brief POST /api/scenes/record — Record current DMX state as a scene
 *
 * Body: {"port":1,"name":"My Scene"}
 */
static esp_err_t api_scenes_record_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    if (!require_auth(req)) return ESP_OK;

    cJSON *body = read_json_body(req);
    if (!body) return ESP_OK;

    cJSON *jport = cJSON_GetObjectItem(body, "port");
    cJSON *jname = cJSON_GetObjectItem(body, "name");

    if (!cJSON_IsNumber(jport) || !cJSON_IsString(jname)) {
        cJSON_Delete(body);
        send_error_response(req, 400, "port and name are required");
        return ESP_OK;
    }

    uint8_t port     = (uint8_t)jport->valueint;
    const char *name = jname->valuestring;

    uint32_t scene_id = 0;
    esp_err_t err = scene_record(port, name, &scene_id);
    cJSON_Delete(body);

    cJSON *resp = cJSON_CreateObject();
    if (err == ESP_OK) {
        cJSON_AddStringToObject(resp, "status", "ok");
        cJSON_AddNumberToObject(resp, "scene_id", scene_id);
        send_json_response(req, resp, 200);
    } else {
        cJSON_AddStringToObject(resp, "status", "error");
        cJSON_AddStringToObject(resp, "message",
            err == ESP_ERR_NO_MEM ? "All scene slots are used" : "Failed to record scene");
        send_json_response(req, resp, err == ESP_ERR_NO_MEM ? 409 : 500);
    }
    cJSON_Delete(resp);
    return ESP_OK;
}

/**
 * @brief POST /api/scenes/playback — Play back a scene
 *
 * Body: {"scene_id":1,"fade_ms":500}
 */
static esp_err_t api_scenes_playback_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    if (!require_auth(req)) return ESP_OK;

    cJSON *body = read_json_body(req);
    if (!body) return ESP_OK;

    cJSON *jid   = cJSON_GetObjectItem(body, "scene_id");
    cJSON *jfade = cJSON_GetObjectItem(body, "fade_ms");

    if (!cJSON_IsNumber(jid)) {
        cJSON_Delete(body);
        send_error_response(req, 400, "scene_id is required");
        return ESP_OK;
    }

    uint32_t scene_id = (uint32_t)jid->valueint;
    int fade = cJSON_IsNumber(jfade) ? jfade->valueint : 0;
    uint16_t fade_ms  = (uint16_t)(fade < 0 ? 0 : (fade > 60000 ? 60000 : fade));
    cJSON_Delete(body);

    esp_err_t err = scene_playback(scene_id, fade_ms);

    cJSON *resp = cJSON_CreateObject();
    if (err == ESP_OK) {
        cJSON_AddStringToObject(resp, "status", "ok");
        send_json_response(req, resp, 200);
    } else if (err == ESP_ERR_NOT_FOUND) {
        cJSON_AddStringToObject(resp, "status", "error");
        cJSON_AddStringToObject(resp, "message", "Scene not found");
        send_json_response(req, resp, 404);
    } else {
        cJSON_AddStringToObject(resp, "status", "error");
        cJSON_AddStringToObject(resp, "message", "Playback failed");
        send_json_response(req, resp, 500);
    }
    cJSON_Delete(resp);
    return ESP_OK;
}

/**
 * @brief DELETE /api/scenes/{id} — Delete a scene
 */
static esp_err_t api_scenes_delete_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    if (!require_auth(req)) return ESP_OK;

    // Extract ID from the last path segment of req->uri
    const char *uri = req->uri;
    const char *last_slash = strrchr(uri, '/');
    if (!last_slash || *(last_slash + 1) == '\0') {
        send_error_response(req, 400, "Invalid scene URI");
        return ESP_OK;
    }

    uint32_t scene_id = (uint32_t)atoi(last_slash + 1);
    if (scene_id < 1 || scene_id > SCENE_MAX_COUNT) {
        send_error_response(req, 400, "Invalid scene ID");
        return ESP_OK;
    }

    esp_err_t err = scene_delete(scene_id);

    cJSON *resp = cJSON_CreateObject();
    if (err == ESP_OK) {
        cJSON_AddStringToObject(resp, "status", "ok");
        send_json_response(req, resp, 200);
    } else if (err == ESP_ERR_NOT_FOUND) {
        cJSON_AddStringToObject(resp, "status", "error");
        cJSON_AddStringToObject(resp, "message", "Scene not found");
        send_json_response(req, resp, 404);
    } else {
        cJSON_AddStringToObject(resp, "status", "error");
        cJSON_AddStringToObject(resp, "message", "Delete failed");
        send_json_response(req, resp, 500);
    }
    cJSON_Delete(resp);
    return ESP_OK;
}

/**
 * @brief POST /api/scenes/release - Fade out / release scene and manual levels
 *
 * Body (optional): {"port":1,"fade_ms":1000}  port 0 or missing = both ports
 */
static esp_err_t api_scenes_release_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    if (!require_auth(req)) return ESP_OK;

    int port = 0, fade = 0;
    if (req->content_len > 0) {
        cJSON *body = read_json_body(req);
        if (!body) return ESP_OK;
        cJSON *jp = cJSON_GetObjectItem(body, "port");
        cJSON *jf = cJSON_GetObjectItem(body, "fade_ms");
        if (cJSON_IsNumber(jp)) port = jp->valueint;
        if (cJSON_IsNumber(jf)) fade = jf->valueint;
        cJSON_Delete(body);
    }
    if (port < 0 || port > CONFIG_DMX_PORTS) {
        send_error_response(req, 400, "Invalid port");
        return ESP_OK;
    }
    if (fade < 0) fade = 0;
    if (fade > 60000) fade = 60000;

    esp_err_t err = scene_release((uint8_t)port, (uint16_t)fade);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", err == ESP_OK ? "ok" : "error");
    send_json_response(req, resp, err == ESP_OK ? 200 : 500);
    cJSON_Delete(resp);
    return ESP_OK;
}

// ============================================================================
// WebSocket DMX Streaming Task
// ============================================================================

/*
 * Pushes the DMX levels of every port to WebSocket clients, up to 10 times a
 * second, as one binary frame:
 *   [0] 0x02 (levels)  [1] bit n = port n+1 has live data
 *   [2] bit n = port n+1 is an input  [3] port count
 *   [4 ...] 512 levels per port
 * Output ports report what they send, input ports what they receive.
 * Sent when something changed, and at least once a second.
 */
#define WS_DMX_PORTS  CONFIG_DMX_PORTS
#define WS_DMX_HDR    4
static uint8_t s_ws_frame[WS_DMX_HDR + WS_DMX_PORTS * 512];
static uint8_t s_ws_last[sizeof(s_ws_frame)];

/*
 * Levels shown for @p port: the merged output of an output port, or the last
 * frame received by an input port. Returns true while the data is live (an
 * output has a source, an input has a DMX signal).
 */
static bool port_levels(int port, uint8_t data[512], bool *input)
{
    dmx_port_status_t st;
    bool ok = dmx_handler_get_port_status(port, &st) == ESP_OK;
    if (ok && (st.mode == DMX_MODE_INPUT || st.mode == DMX_MODE_RDM_RESPONDER)) {
        *input = true;
        /* after a signal loss the last frame stays visible, marked not live */
        if (!st.is_active || dmx_handler_read_dmx(port, data, 0) != ESP_OK) {
            memset(data, 0, 512);
        }
        return st.is_active && st.stats.signal_present;
    }
    *input = false;
    if ((dmx_test_active_mask() & (1u << (port - 1))) || dmx_tester_enabled()) {
        /* test pattern or tester: what is on the wire */
        dmx_handler_get_levels(port, data);
        return ok && st.is_active;
    }
    merge_engine_get_output(port, data);
    return merge_engine_is_output_active(port);
}

static void ws_dmx_stream_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(100);
    TickType_t last_wake    = xTaskGetTickCount();
    int64_t last_sent_us = 0;

    while (server_state.ws_stream_run) {
        if (server_state.running && web_server_ws_get_client_count(NULL) > 0) {
            uint8_t live = 0, inputs = 0;
            for (int p = 0; p < WS_DMX_PORTS; p++) {
                bool input = false;
                if (port_levels(p + 1, &s_ws_frame[WS_DMX_HDR + p * 512], &input)) {
                    live |= (uint8_t)(1u << p);
                }
                if (input) inputs |= (uint8_t)(1u << p);
            }
            s_ws_frame[0] = 0x02;
            s_ws_frame[1] = live;
            s_ws_frame[2] = inputs;
            s_ws_frame[3] = WS_DMX_PORTS;

            int64_t now = esp_timer_get_time();
            if (memcmp(s_ws_frame, s_ws_last, sizeof(s_ws_frame)) != 0 ||
                now - last_sent_us > 1000000) {
                web_server_ws_send("/ws", s_ws_frame, sizeof(s_ws_frame), true);
                memcpy(s_ws_last, s_ws_frame, sizeof(s_ws_frame));
                last_sent_us = now;
            }
        } else {
            last_sent_us = 0;   /* a new client gets a frame straight away */
        }

        vTaskDelayUntil(&last_wake, period);
    }
    xSemaphoreGive(server_state.ws_stream_done);
    vTaskDelete(NULL);
}

// ============================================================================
// Helper Functions
// ============================================================================

static const char *fw_version(void)
{
    return esp_app_get_description()->version;
}

static void restart_cb(void *arg)
{
    esp_restart();
}

/* Restart after the HTTP response has gone out, without blocking httpd */
static void restart_later(uint32_t ms)
{
    static esp_timer_handle_t t;
    if (!t) {
        const esp_timer_create_args_t args = {.callback = restart_cb, .name = "restart"};
        if (esp_timer_create(&args, &t) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(ms));
            esp_restart();
        }
    }
    esp_timer_start_once(t, (uint64_t)ms * 1000);
}

void web_restart_later(uint32_t ms)
{
    restart_later(ms);
}

/**
 * Read the whole request body (looping over partial reads and retrying
 * socket timeouts). The caller frees *out. Bodies over WEB_BODY_MAX are
 * rejected with ESP_ERR_INVALID_SIZE.
 */
static esp_err_t read_body(httpd_req_t *req, char **out)
{
    *out = NULL;
    size_t len = req->content_len;
    if (len == 0 || len > WEB_BODY_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *buf = malloc(len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    size_t got = 0;
    int timeouts = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 5) {
            continue;
        }
        if (r <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        got += r;
    }
    buf[got] = '\0';
    *out = buf;
    return ESP_OK;
}

/* read_body + cJSON_Parse; sends the error response itself and returns NULL on failure */
static cJSON *read_json_body(httpd_req_t *req)
{
    char *buf = NULL;
    esp_err_t err = read_body(req, &buf);
    if (err != ESP_OK) {
        send_error_response(req, err == ESP_ERR_INVALID_SIZE ? 400 : 500,
                            err == ESP_ERR_INVALID_SIZE ? "Request body missing or too large"
                                                        : "Failed to read request body");
        return NULL;
    }
    cJSON *json = cJSON_Parse(buf);
    free(buf);
    if (!json) {
        send_error_response(req, 400, "Invalid JSON");
    }
    return json;
}

static void ws_remove_fd(int fd)
{
    if (!server_state.ws_mutex) return;
    xSemaphoreTake(server_state.ws_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (server_state.ws_clients[i].active && server_state.ws_clients[i].fd == fd) {
            server_state.ws_clients[i].active = false;
            if (server_state.ws_client_count > 0) {
                server_state.ws_client_count--;
            }
            ESP_LOGI(TAG, "WebSocket client disconnected fd=%d", fd);
        }
    }
    xSemaphoreGive(server_state.ws_mutex);
}

/* httpd session close hook: we must close the socket ourselves */
static void ws_close_fn(httpd_handle_t hd, int sockfd)
{
    ws_remove_fd(sockfd);
    close(sockfd);
}

/**
 * @brief Send JSON response
 *
 * FIX 4: Use http_status_str() to map any status code correctly instead of
 * the original ternary that hardcoded "500 Internal Server Error" for all
 * non-200 responses.
 */
static void send_json_response(httpd_req_t *req, cJSON *json, int status)
{
    char *json_str = cJSON_PrintUnformatted(json);
    if (json_str) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, http_status_str(status));
        httpd_resp_send(req, json_str, strlen(json_str));
        free(json_str);
    } else {
        httpd_resp_send_500(req);
    }
}

/**
 * @brief Send error response
 */
static void send_error_response(httpd_req_t *req, int status, const char *message)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "error", message);
    cJSON_AddStringToObject(json, "message", message);
    send_json_response(req, json, status);
    cJSON_Delete(json);
}

/**
 * @brief Extract port number from URI
 */
static int get_port_from_uri(const char *uri)
{
    // Parse URI like "/api/ports/1/config" to extract port number
    const char *ptr = strstr(uri, "/ports/");
    if (ptr) {
        ptr += 7;  // Skip "/ports/"
        return atoi(ptr);
    }
    return 0;
}

// ============================================================================
// Authentication
//
// The UI is protected with HTTP Basic auth (user "admin"). Until first-run
// setup has stored an admin password, only the setup page and
// /api/auth/status + /api/setup are reachable.
//
// - Passwords are stored as salted PBKDF2-HMAC-SHA256 (config manager). The
//   SHA-256 of the last credential that verified is cached so the UI's
//   polling doesn't pay the PBKDF2 cost on every request.
// - Requests that change state must carry an X-Requested-With header.
//   Browsers can't add that header to a cross-site request without a CORS
//   preflight, which this server never answers, so another web page can't
//   use the browser's cached credentials against the node (CSRF).
// - The WebSocket only accepts a token handed out by an authenticated
//   /api/auth/status call (WebSockets are not covered by CORS).
// - Repeated wrong passwords from one address lock it out for a while.
// ============================================================================

#define AUTH_FAIL_SLOTS     8
#define AUTH_FAIL_LIMIT     10
#define AUTH_LOCK_US        (60LL * 1000000)
#define AUTH_FAIL_FORGET_US (300LL * 1000000)
#define AUTH_HDR_MAX        256

typedef enum {
    AUTH_OK = 0,
    AUTH_SETUP_REQUIRED,
    AUTH_MISSING,
    AUTH_BAD,
    AUTH_LOCKED,
    AUTH_CSRF,
} auth_result_t;

static struct {
    uint8_t good_digest[32];
    bool good_valid;
    char ws_token[33];
    struct {
        uint32_t ip;
        uint8_t fails;
        int64_t last_fail_us;
        int64_t locked_until_us;
    } fail[AUTH_FAIL_SLOTS];
} s_auth;

static void auth_new_ws_token(void)
{
    static const char digits[] = "0123456789abcdef";
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    for (int i = 0; i < 16; i++) {
        s_auth.ws_token[2 * i] = digits[rnd[i] >> 4];
        s_auth.ws_token[2 * i + 1] = digits[rnd[i] & 0x0F];
    }
    s_auth.ws_token[32] = '\0';
}

/* Forget the cached credential and invalidate WebSocket tokens */
static void auth_invalidate(void)
{
    memset(s_auth.good_digest, 0, sizeof(s_auth.good_digest));
    s_auth.good_valid = false;
    auth_new_ws_token();
}

static bool ct_equal(const void *a, const void *b, size_t len)
{
    const uint8_t *x = a, *y = b;
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) diff |= x[i] ^ y[i];
    return diff == 0;
}

/* IPv4 address of the client (0 if unknown); used as the lockout key */
static uint32_t client_ip(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (fd < 0 || getpeername(fd, (struct sockaddr *)&addr, &len) != 0) {
        return 0;
    }
    if (addr.ss_family == AF_INET) {
        return ((struct sockaddr_in *)&addr)->sin_addr.s_addr;
    }
#if CONFIG_LWIP_IPV6
    if (addr.ss_family == AF_INET6) {
        uint32_t v;   /* IPv4-mapped (::ffff:a.b.c.d) - last 4 bytes */
        memcpy(&v, &((struct sockaddr_in6 *)&addr)->sin6_addr.s6_addr[12], sizeof(v));
        return v;
    }
#endif
    return 0;
}

static int auth_slot(uint32_t ip, bool create, int64_t now)
{
    int oldest = 0;
    for (int i = 0; i < AUTH_FAIL_SLOTS; i++) {
        if (s_auth.fail[i].fails || s_auth.fail[i].locked_until_us) {
            if (s_auth.fail[i].ip == ip) return i;
        }
        if (s_auth.fail[i].last_fail_us < s_auth.fail[oldest].last_fail_us) oldest = i;
    }
    if (!create) return -1;
    /* Reuse an idle slot, else the one with the oldest failure */
    for (int i = 0; i < AUTH_FAIL_SLOTS; i++) {
        bool locked = s_auth.fail[i].locked_until_us > now;
        bool stale = now - s_auth.fail[i].last_fail_us > AUTH_FAIL_FORGET_US;
        if (!locked && (s_auth.fail[i].fails == 0 || stale)) {
            oldest = i;
            break;
        }
    }
    memset(&s_auth.fail[oldest], 0, sizeof(s_auth.fail[oldest]));
    s_auth.fail[oldest].ip = ip;
    return oldest;
}

/* Seconds left on a lockout for @p ip, 0 if none */
static uint32_t auth_locked_for(uint32_t ip, int64_t now)
{
    int i = auth_slot(ip, false, now);
    if (i < 0 || s_auth.fail[i].locked_until_us <= now) return 0;
    return (uint32_t)((s_auth.fail[i].locked_until_us - now) / 1000000) + 1;
}

static void auth_record_failure(uint32_t ip, int64_t now)
{
    int i = auth_slot(ip, true, now);
    if (now - s_auth.fail[i].last_fail_us > AUTH_FAIL_FORGET_US) {
        s_auth.fail[i].fails = 0;
    }
    s_auth.fail[i].last_fail_us = now;
    if (++s_auth.fail[i].fails >= AUTH_FAIL_LIMIT) {
        s_auth.fail[i].fails = 0;
        s_auth.fail[i].locked_until_us = now + AUTH_LOCK_US;
        const uint8_t *b = (const uint8_t *)&ip;
        ESP_LOGW(TAG, "Too many failed logins from %u.%u.%u.%u, locked for %d s",
                 b[0], b[1], b[2], b[3], (int)(AUTH_LOCK_US / 1000000));
    }
}

static void auth_record_success(uint32_t ip, int64_t now)
{
    int i = auth_slot(ip, false, now);
    if (i >= 0) memset(&s_auth.fail[i], 0, sizeof(s_auth.fail[i]));
}

/* Checks the request's credentials without sending anything */
static auth_result_t auth_evaluate(httpd_req_t *req)
{
    if (!config_setup_done()) {
        return AUTH_SETUP_REQUIRED;
    }
    if (!config_login_required()) {
        return AUTH_OK;   /* the owner chose to run without a password */
    }
    int64_t now = esp_timer_get_time();
    uint32_t ip = client_ip(req);
    if (auth_locked_for(ip, now)) {
        return AUTH_LOCKED;
    }

    size_t hlen = httpd_req_get_hdr_value_len(req, "Authorization");
    if (hlen == 0) {
        return AUTH_MISSING;
    }
    if (hlen >= AUTH_HDR_MAX) {
        auth_record_failure(ip, now);
        return AUTH_BAD;
    }
    char hdr[AUTH_HDR_MAX];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        return AUTH_MISSING;
    }
    if (strncasecmp(hdr, "Basic ", 6) != 0) {
        return AUTH_MISSING;
    }
    const char *b64 = hdr + 6;
    while (*b64 == ' ') b64++;

    unsigned char cred[AUTH_HDR_MAX];
    size_t cred_len = 0;
    if (mbedtls_base64_decode(cred, sizeof(cred) - 1, &cred_len,
                              (const unsigned char *)b64, strlen(b64)) != 0) {
        auth_record_failure(ip, now);
        return AUTH_BAD;
    }
    cred[cred_len] = '\0';

    auth_result_t result = AUTH_BAD;
    uint8_t digest[32];
    mbedtls_sha256(cred, cred_len, digest, 0);
    if (s_auth.good_valid && ct_equal(digest, s_auth.good_digest, sizeof(digest))) {
        result = AUTH_OK;
    } else {
        char *colon = strchr((char *)cred, ':');
        if (colon) {
            *colon = '\0';
            if (strcmp((char *)cred, CONFIG_ADMIN_USER) == 0 &&
                config_check_admin_password(colon + 1)) {
                memcpy(s_auth.good_digest, digest, sizeof(digest));
                s_auth.good_valid = true;
                result = AUTH_OK;
            }
        }
    }
    memset(cred, 0, sizeof(cred));
    memset(hdr, 0, sizeof(hdr));

    if (result == AUTH_OK) {
        auth_record_success(ip, now);
    } else {
        auth_record_failure(ip, now);
    }
    return result;
}

static void send_auth_error(httpd_req_t *req, auth_result_t r)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    switch (r) {
    case AUTH_SETUP_REQUIRED:
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "{\"error\":\"setup_required\","
                                "\"message\":\"Set an admin password first (first-run setup)\"}");
        break;
    case AUTH_LOCKED: {
        uint32_t secs = auth_locked_for(client_ip(req), esp_timer_get_time());
        char retry[12], body[128];
        snprintf(retry, sizeof(retry), "%lu", (unsigned long)(secs ? secs : 1));
        snprintf(body, sizeof(body),
                 "{\"error\":\"locked\",\"message\":\"Too many failed logins, try again in %lu s\"}",
                 (unsigned long)(secs ? secs : 1));
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_set_hdr(req, "Retry-After", retry);
        httpd_resp_sendstr(req, body);
        break;
    }
    case AUTH_CSRF:
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "{\"error\":\"forbidden\","
                                "\"message\":\"Missing X-Requested-With header\"}");
        break;
    default:
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP-NODE\", charset=\"UTF-8\"");
        httpd_resp_sendstr(req, "{\"error\":\"unauthorized\",\"message\":\"Sign in required\"}");
        break;
    }
}

/* True if the request carries the anti-CSRF header (any value) */
static bool has_xrw(httpd_req_t *req)
{
    return httpd_req_get_hdr_value_len(req, "X-Requested-With") > 0;
}

/**
 * Gate for every API handler. Returns true if the request may proceed;
 * otherwise the error response has been sent and the handler must return
 * ESP_OK straight away.
 */
static bool require_auth(httpd_req_t *req)
{
    auth_result_t r = auth_evaluate(req);
    if (r == AUTH_OK && req->method != HTTP_GET && !has_xrw(req)) {
        r = AUTH_CSRF;
    }
    if (r != AUTH_OK) {
        send_auth_error(req, r);
        return false;
    }
    return true;
}

/* Static UI files: open during first-run setup, Basic auth afterwards */
static bool require_auth_page(httpd_req_t *req)
{
    auth_result_t r = auth_evaluate(req);
    if (r == AUTH_OK || r == AUTH_SETUP_REQUIRED) {
        return true;
    }
    send_auth_error(req, r);
    return false;
}

/* WebSocket handshake: needs ?token=<ws_token> */
static bool ws_token_ok(httpd_req_t *req)
{
    if (!config_setup_done()) {
        return false;
    }
    char query[96], token[40];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "token", token, sizeof(token)) != ESP_OK) {
        return false;
    }
    return strlen(token) == 32 && ct_equal(token, s_auth.ws_token, 32);
}

static bool valid_password_len(const char *pw)
{
    size_t n = pw ? strlen(pw) : 0;
    return n >= 8 && n <= 63;
}


/**
 * GET /api/auth/status - never challenges.
 * {setup_required, login_required, authenticated, username,
 *  ws_token (when authenticated)}
 */
static esp_err_t api_auth_status_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    auth_result_t r = auth_evaluate(req);

    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "setup_required", r == AUTH_SETUP_REQUIRED);
    cJSON_AddBoolToObject(json, "login_required", config_login_required());
    cJSON_AddBoolToObject(json, "authenticated", r == AUTH_OK);
    cJSON_AddBoolToObject(json, "locked", r == AUTH_LOCKED);
    cJSON_AddStringToObject(json, "username", CONFIG_ADMIN_USER);
    if (r == AUTH_OK) {
        cJSON_AddStringToObject(json, "ws_token", s_auth.ws_token);
    }
    if (r == AUTH_SETUP_REQUIRED || r == AUTH_OK) {
        cJSON_AddStringToObject(json, "ap_ssid", config_get()->network.ap_ssid);
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    send_json_response(req, json, 200);
    cJSON_Delete(json);
    return ESP_OK;
}

/**
 * POST /api/setup {password | no_password:true, ap_password} - first-run
 * setup only. Stores the admin password (or turns sign-in off) and the new AP
 * password ("" = open network), then restarts so the access point comes back
 * with it.
 */
static esp_err_t api_setup_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    if (config_setup_done()) {
        send_error_response(req, 409, "Setup has already been completed");
        return ESP_OK;
    }
    if (!has_xrw(req)) {
        send_auth_error(req, AUTH_CSRF);
        return ESP_OK;
    }
    cJSON *json = read_json_body(req);
    if (!json) return ESP_OK;

    const cJSON *pw = cJSON_GetObjectItemCaseSensitive(json, "password");
    const cJSON *ap = cJSON_GetObjectItemCaseSensitive(json, "ap_password");
    bool no_login = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "no_password"));
    const char *problem = NULL;
    if (!no_login && (!cJSON_IsString(pw) || !valid_password_len(pw->valuestring))) {
        problem = "Admin password must be 8-63 characters";
    } else if (!cJSON_IsString(ap)) {
        problem = "AP password missing";
    } else {
        problem = config_ap_password_problem(ap->valuestring);
    }
    if (problem) {
        cJSON_Delete(json);
        send_error_response(req, 400, problem);
        return ESP_OK;
    }

    config_t *cfg = config_get();
    size_t n = strnlen(ap->valuestring, sizeof(cfg->network.ap_password) - 1);
    memcpy(cfg->network.ap_password, ap->valuestring, n);
    cfg->network.ap_password[n] = '\0';
    esp_err_t err = no_login ? config_set_no_login()               /* both save */
                             : config_set_admin_password(pw->valuestring);
    cJSON_Delete(json);
    if (err != ESP_OK) {
        send_error_response(req, 500, "Failed to save the password");
        return ESP_OK;
    }
    auth_invalidate();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    cJSON_AddStringToObject(resp, "message", no_login
                            ? "Setup complete - restarting."
                            : "Setup complete - restarting. Sign in as \"admin\".");
    send_json_response(req, resp, 200);
    cJSON_Delete(resp);
    ESP_LOGI(TAG, "First-run setup complete (%s), restarting",
             no_login ? "no password" : "password set");
    restart_later(3000);
    return ESP_OK;
}

/**
 * POST /api/auth/password {current_password, new_password | no_password:true}
 * Changes the admin password, turns sign-in on (new_password while it is
 * off) or off (no_password). current_password is needed only while sign-in
 * is on.
 */
static esp_err_t api_auth_password_handler(httpd_req_t *req)
{
    server_state.total_requests++;
    if (!require_auth(req)) return ESP_OK;
    cJSON *json = read_json_body(req);
    if (!json) return ESP_OK;

    const cJSON *cur = cJSON_GetObjectItemCaseSensitive(json, "current_password");
    const cJSON *pw = cJSON_GetObjectItemCaseSensitive(json, "new_password");
    bool no_login = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "no_password"));
    bool was_on = config_login_required();
    int64_t now = esp_timer_get_time();
    uint32_t ip = client_ip(req);
    if (was_on && (!cJSON_IsString(cur) || !config_check_admin_password(cur->valuestring))) {
        cJSON_Delete(json);
        auth_record_failure(ip, now);
        send_error_response(req, 403, "Current password is wrong");
        return ESP_OK;
    }
    if (!no_login && (!cJSON_IsString(pw) || !valid_password_len(pw->valuestring))) {
        cJSON_Delete(json);
        send_error_response(req, 400, "New password must be 8-63 characters");
        return ESP_OK;
    }
    esp_err_t err = no_login ? config_set_no_login() : config_set_admin_password(pw->valuestring);
    cJSON_Delete(json);
    if (err != ESP_OK) {
        send_error_response(req, 500, "Failed to save the password");
        return ESP_OK;
    }
    auth_invalidate();

    const char *msg = no_login ? "Sign-in turned off: anyone on the network can change settings"
                    : was_on   ? "Password changed - sign in again"
                               : "Sign-in turned on - sign in as \"admin\"";
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    cJSON_AddStringToObject(resp, "message", msg);
    send_json_response(req, resp, 200);
    cJSON_Delete(resp);
    ESP_LOGI(TAG, "%s", no_login ? "Web UI sign-in turned off"
                      : was_on   ? "Admin password changed" : "Web UI sign-in turned on");
    return ESP_OK;
}
