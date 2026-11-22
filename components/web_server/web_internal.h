/**
 * @file web_internal.h
 * @brief Helpers shared by web_server.c and web_tools.c (not public API)
 */
#ifndef WEB_INTERNAL_H
#define WEB_INTERNAL_H

#include <stdbool.h>
#include "esp_http_server.h"
#include "cJSON.h"
#include "web_server.h"
#include "dmx_handler.h"
#include "dmx_tools.h"

/** Counts the request and checks the login (sends the error itself) */
bool web_require_auth(httpd_req_t *req);
void web_send_json(httpd_req_t *req, cJSON *json, int status);
void web_send_error(httpd_req_t *req, int status, const char *message);
/** Parsed JSON body; NULL after an error response was sent */
cJSON *web_read_json(httpd_req_t *req);
const char *web_status_line(int status);
const web_server_hooks_t *web_hooks(void);

/** Register the diagnostics / DMX tools routes (web_tools.c) */
void web_tools_register(httpd_handle_t server);
/** Number of routes web_tools_register() adds */
#define WEB_TOOLS_ROUTES 13

/** Register the signal check route and the captive portal 404 handler (web_signal.c) */
void web_signal_register(httpd_handle_t server);
#define WEB_SIGNAL_ROUTES 2

/** IPv4 address of the client, network byte order (0 if unknown) */
uint32_t web_client_ip(httpd_req_t *req);

/** Register the DMX tester routes (web_tester.c) */
void web_tester_register(httpd_handle_t server);
/** Number of routes web_tester_register() adds */
#define WEB_TESTER_ROUTES 6

/** Restart the node after @p ms, once the HTTP response has gone out */
void web_restart_later(uint32_t ms);

/** 1 if a menuconfig bool is set, else 0 (a switched-off bool is undefined) */
#define WEB_HAS_(x) (#x[0] == '1' && #x[1] == '\0')
#define WEB_HAS(x)  WEB_HAS_(x)

void web_test_to_json(cJSON *o, const dmx_test_t *t);
void web_analysis_to_json(cJSON *o, const dmx_input_analysis_t *a);

#endif // WEB_INTERNAL_H
