/* Host stub: only what web_stream.c uses. The test collects what is sent. */
#pragma once
#include <sys/types.h>
#include "esp_err.h"

typedef struct { int unused; } httpd_req_t;
esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type);
esp_err_t httpd_resp_send_chunk(httpd_req_t *req, const char *buf, ssize_t len);
esp_err_t httpd_resp_send_500(httpd_req_t *req);
