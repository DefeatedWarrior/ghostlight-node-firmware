/* Board check for firmware uploads: see fw_tag.h */
#include "fw_tag.h"
#include <string.h>

#define FW_TAG_END 0x03

void fw_tag_scan_init(fw_tag_scan_t *t, const char *prefix, size_t prefix_len)
{
    memset(t, 0, sizeof(*t));
    t->prefix = prefix;
    t->prefix_len = prefix_len;
}

bool fw_tag_scan(fw_tag_scan_t *t, const uint8_t *data, size_t n)
{
    if (!t->prefix || !t->prefix_len) return false;
    /* volatile: compare against the bytes in memory, so the compiler can't
     * fold the pattern into constants of its own (a second copy) */
    const volatile uint8_t *pre = (const volatile uint8_t *)t->prefix;
    for (size_t i = 0; i < n && !t->found; i++) {
        uint8_t c = data[i];
        if (t->matched < t->prefix_len) {
            /* The prefix starts with a byte it never repeats, so on a
             * mismatch the match can only restart at this byte */
            if (c == pre[t->matched]) {
                t->matched++;
            } else {
                t->matched = (c == pre[0]) ? 1 : 0;
            }
            continue;
        }
        if (c == FW_TAG_END && t->id_len > 0) {
            t->id[t->id_len] = '\0';
            t->found = true;
        } else if (c >= 0x20 && c < 0x7F && t->id_len < FW_TAG_ID_MAX - 1) {
            t->id[t->id_len++] = (char)c;
        } else {
            /* not a marker after all */
            t->id_len = 0;
            t->matched = (c == pre[0]) ? 1 : 0;
        }
    }
    return t->found;
}
