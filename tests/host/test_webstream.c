/* web_stream.c: the responses it writes must be the JSON cJSON wrote */
#include "web_stream.h"
#include "cJSON.h"
#include "test.h"
#include <stdlib.h>

static httpd_req_t s_req;
static char s_out[32768];
static size_t s_len, s_biggest;
static int s_ended, s_chunks, s_fail_at;

esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type)
{
    (void)req; (void)type;
    return ESP_OK;
}

esp_err_t httpd_resp_send_500(httpd_req_t *req)
{
    (void)req;
    return ESP_OK;
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *req, const char *buf, ssize_t len)
{
    (void)req;
    if (len == 0) {
        s_ended++;
        return ESP_OK;
    }
    if (++s_chunks == s_fail_at) return ESP_FAIL;
    memcpy(s_out + s_len, buf, (size_t)len);
    s_len += (size_t)len;
    s_out[s_len] = '\0';
    if ((size_t)len > s_biggest) s_biggest = (size_t)len;
    return ESP_OK;
}

static void reset(void)
{
    s_len = s_biggest = 0;
    s_ended = s_chunks = s_fail_at = 0;
    s_out[0] = '\0';
}

static void test_level_arrays_match_cjson(void)
{
    static const int counts[] = { 1, 8, 255, 256, 257, 511, 512 };
    uint8_t v[512];
    for (int i = 0; i < 512; i++) v[i] = (uint8_t)(i * 7 + 3);
    for (size_t c = 0; c < sizeof(counts) / sizeof(counts[0]); c++) {
        reset();
        cJSON *a = cJSON_CreateArray();
        for (int i = 0; i < counts[c]; i++) cJSON_AddItemToArray(a, cJSON_CreateNumber(v[i]));
        char *want = cJSON_PrintUnformatted(a);
        web_stream_t *w = web_stream_begin(&s_req);
        web_stream_u8_array(w, v, (size_t)counts[c]);
        esp_err_t err = web_stream_end(w);
        int same = !strcmp(want, s_out);
        free(want);
        cJSON_Delete(a);
        CHECK_EQ(err, ESP_OK);
        CHECK(same);
        CHECK_EQ(s_ended, 1);
        CHECK(s_biggest <= WEB_STREAM_BUF);
    }
}

static void test_u32_array(void)
{
    uint32_t v[512];
    for (int i = 0; i < 512; i++) v[i] = 4294967295u - (uint32_t)i;
    web_stream_t *w = web_stream_begin(&s_req);
    web_stream_u32_array(w, v, 512);
    CHECK_EQ(web_stream_end(w), ESP_OK);
    cJSON *a = cJSON_Parse(s_out);
    int ok = a && cJSON_GetArraySize(a) == 512;
    for (int i = 0; ok && i < 512; i++) ok = cJSON_GetArrayItem(a, i)->valuedouble == v[i];
    cJSON_Delete(a);
    CHECK(ok);
}

/* every byte, at every position against the buffer's edge */
static void test_strings_match_cjson(void)
{
    char s[3000];
    for (int off = 0; off < 300; off++) {
        reset();
        size_t n = 0;
        for (int i = 0; i < off * 4; i++) s[n++] = 'x';
        for (int c = 1; c < 256; c++) s[n++] = (char)c;
        s[n] = '\0';
        char *want;
        cJSON *j = cJSON_CreateString(s);
        want = cJSON_PrintUnformatted(j);
        web_stream_t *w = web_stream_begin(&s_req);
        web_stream_string(w, s);
        esp_err_t err = web_stream_end(w);
        int same = !strcmp(want, s_out);
        free(want);
        cJSON_Delete(j);
        CHECK_EQ(err, ESP_OK);
        CHECK(same);
        CHECK(s_biggest <= WEB_STREAM_BUF);
    }
}

static void test_failed_send_stops(void)
{
    uint8_t v[512] = { 0 };
    s_fail_at = 1;
    web_stream_t *w = web_stream_begin(&s_req);
    web_stream_u8_array(w, v, 512);
    web_stream_u8_array(w, v, 512);
    CHECK(web_stream_end(w) != ESP_OK);
    CHECK_EQ(s_chunks, 1);
    CHECK_EQ(s_ended, 0);
}

#undef TEST_SETUP
#define TEST_SETUP() reset()

int main(void)
{
    RUN(test_level_arrays_match_cjson);
    RUN(test_u32_array);
    RUN(test_strings_match_cjson);
    RUN(test_failed_send_stops);
    return test_summary();
}
