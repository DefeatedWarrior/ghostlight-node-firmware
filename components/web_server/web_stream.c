/**
 * @file web_stream.c
 * @brief Write a large JSON response in pieces (see web_stream.h)
 */
#include "web_stream.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

struct web_stream {
    httpd_req_t *req;
    esp_err_t err;      /* the first failure; nothing is sent after it */
    size_t len;
    char buf[WEB_STREAM_BUF];
};

web_stream_t *web_stream_begin(httpd_req_t *req)
{
    web_stream_t *w = malloc(sizeof(*w));
    if (!w) {
        httpd_resp_send_500(req);
        return NULL;
    }
    w->req = req;
    w->err = ESP_OK;
    w->len = 0;
    httpd_resp_set_type(req, "application/json");
    return w;
}

static void flush(web_stream_t *w)
{
    /* a chunk of length 0 would end the response */
    if (w->len > 0 && w->err == ESP_OK) {
        w->err = httpd_resp_send_chunk(w->req, w->buf, w->len);
    }
    w->len = 0;
}

static void put(web_stream_t *w, char c)
{
    if (w->len == sizeof(w->buf)) flush(w);
    w->buf[w->len++] = c;
}

void web_stream_printf(web_stream_t *w, const char *fmt, ...)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        size_t room = sizeof(w->buf) - w->len;
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(w->buf + w->len, room, fmt, ap);
        va_end(ap);
        if (n >= 0 && (size_t)n < room) {
            w->len += (size_t)n;
            return;
        }
        flush(w);
    }
    if (w->err == ESP_OK) w->err = ESP_FAIL;    /* too long for one call */
}

void web_stream_string(web_stream_t *w, const char *s)
{
    put(w, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char esc = 0;
        switch (c) {
        case '"':  esc = '"';  break;
        case '\\': esc = '\\'; break;
        case '\n': esc = 'n';  break;
        case '\r': esc = 'r';  break;
        case '\t': esc = 't';  break;
        case '\b': esc = 'b';  break;
        case '\f': esc = 'f';  break;
        default: break;
        }
        if (esc) {
            put(w, '\\');
            put(w, esc);
        } else if (c < 0x20) {
            web_stream_printf(w, "\\u%04x", c);
        } else {
            put(w, (char)c);
        }
    }
    put(w, '"');
}

void web_stream_u8_array(web_stream_t *w, const uint8_t *v, size_t n)
{
    put(w, '[');
    for (size_t i = 0; i < n; i++) {
        web_stream_printf(w, i ? ",%u" : "%u", (unsigned)v[i]);
    }
    put(w, ']');
}

void web_stream_u32_array(web_stream_t *w, const uint32_t *v, size_t n)
{
    put(w, '[');
    for (size_t i = 0; i < n; i++) {
        web_stream_printf(w, i ? ",%" PRIu32 : "%" PRIu32, v[i]);
    }
    put(w, ']');
}

esp_err_t web_stream_end(web_stream_t *w)
{
    flush(w);
    esp_err_t err = w->err;
    if (err == ESP_OK) err = httpd_resp_send_chunk(w->req, NULL, 0);
    free(w);
    return err;
}
