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
#define WEB_TOOLS_ROUTES 12

void web_test_to_json(cJSON *o, const dmx_test_t *t);
void web_analysis_to_json(cJSON *o, const dmx_input_analysis_t *a);

#endif // WEB_INTERNAL_H
