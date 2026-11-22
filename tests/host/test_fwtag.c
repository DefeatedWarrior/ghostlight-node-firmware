/* Unit tests: components/web_server/fw_tag.c (board check for uploads). */
#include "../../components/web_server/fw_tag.h"
#include <stdlib.h>
#include <string.h>
#include "test.h"

static const char PREFIX[] = "\x01GLBOARD\x02";
#define PLEN (sizeof(PREFIX) - 1)

/* An "image": noise with the marker at @p at, fed in @p chunk-byte pieces */
static bool scan_image(const char *id, size_t at, size_t len, size_t chunk, char *found)
{
    uint8_t *img = malloc(len);
    for (size_t i = 0; i < len; i++) img[i] = (uint8_t)(i * 37 + 11);
    if (id) {
        char tag[64];
        int n = snprintf(tag, sizeof(tag), "%s%s\x03", PREFIX, id);
        memcpy(img + at, tag, (size_t)n);
    }
    fw_tag_scan_t t;
    fw_tag_scan_init(&t, PREFIX, PLEN);
    bool ok = false;
    for (size_t o = 0; o < len && !ok; o += chunk) {
        size_t n = len - o < chunk ? len - o : chunk;
        ok = fw_tag_scan(&t, img + o, n);
    }
    free(img);
    if (ok) strcpy(found, t.id);
    return ok;
}

static void test_found_in_one_piece(void)
{
    char id[FW_TAG_ID_MAX];
    CHECK(scan_image("esp-node-2rdm", 5000, 20000, 20000, id));
    CHECK_STR(id, "esp-node-2rdm");
}

static void test_found_across_chunks(void)
{
    char id[FW_TAG_ID_MAX];
    /* every split point of the marker over a chunk boundary */
    for (size_t at = 4090; at < 4100 + 24; at++) {
        CHECK(scan_image("ghostlight-rev1", at, 9000, 4096, id));
        CHECK_STR(id, "ghostlight-rev1");
    }
    CHECK(scan_image("esp32-devkitc", 100, 1000, 1, id));    /* a byte at a time */
    CHECK_STR(id, "esp32-devkitc");
}

static void test_not_found(void)
{
    char id[FW_TAG_ID_MAX];
    CHECK(!scan_image(NULL, 0, 20000, 4096, id));
}

static void test_false_starts(void)
{
    /* a broken marker, then a real one */
    const char *s = "xx\x01GLBO\x01GLBOARD\x02\x01GLBOARD\x02\xff\x01GLBOARD\x02" "esp32s3-devkitc\x03";
    fw_tag_scan_t t;
    fw_tag_scan_init(&t, PREFIX, PLEN);
    CHECK(fw_tag_scan(&t, (const uint8_t *)s, strlen(s)));
    CHECK_STR(t.id, "esp32s3-devkitc");

    /* an ID that runs on too long is not one */
    char longid[80];
    memset(longid, 'a', sizeof(longid));
    fw_tag_scan_init(&t, PREFIX, PLEN);
    fw_tag_scan(&t, (const uint8_t *)PREFIX, PLEN);
    fw_tag_scan(&t, (const uint8_t *)longid, sizeof(longid));
    CHECK(!fw_tag_scan(&t, (const uint8_t *)"\x03", 1));

    /* an empty ID is not one */
    fw_tag_scan_init(&t, PREFIX, PLEN);
    CHECK(!fw_tag_scan(&t, (const uint8_t *)"\x01GLBOARD\x02\x03", PLEN + 1));
}

int main(void)
{
    RUN(test_found_in_one_piece);
    RUN(test_found_across_chunks);
    RUN(test_not_found);
    RUN(test_false_starts);
    return test_summary();
}
