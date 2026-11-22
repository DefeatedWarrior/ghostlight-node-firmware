/**
 * @file web_stream.h
 * @brief Write a large JSON response in pieces
 *
 * cJSON costs about 40 bytes of internal RAM per number (small allocations
 * always land there, PSRAM or not): 512 levels took 22 KB, the tester's
 * channel table would take about 80 KB. Responses like those, and long
 * strings, go through this with one 1 KB buffer instead.
 */
#ifndef WEB_STREAM_H
#define WEB_STREAM_H

#include <stddef.h>
#include <stdint.h>
#include "esp_http_server.h"

#define WEB_STREAM_BUF 1024

typedef struct web_stream web_stream_t;

/** Start a JSON response; NULL after sending a 500 (out of memory) */
web_stream_t *web_stream_begin(httpd_req_t *req);
/** Append formatted text; one call must stay under WEB_STREAM_BUF bytes */
void web_stream_printf(web_stream_t *w, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
/** Append a quoted, escaped JSON string of any length */
void web_stream_string(web_stream_t *w, const char *s);
/** Append a JSON array of @p n numbers */
void web_stream_u8_array(web_stream_t *w, const uint8_t *v, size_t n);
void web_stream_u32_array(web_stream_t *w, const uint32_t *v, size_t n);
/** Send the rest and free @p w; not ESP_OK if sending failed */
esp_err_t web_stream_end(web_stream_t *w);

#endif // WEB_STREAM_H
