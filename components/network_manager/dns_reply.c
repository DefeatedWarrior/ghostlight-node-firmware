/*
 * Captive portal DNS answers (see captive_dns.h). Plain C so the host tests
 * can check it.
 */
#include <string.h>
#include "captive_dns.h"

#define HDR        12
#define TYPE_A     1
#define TYPE_ANY   255
#define CLASS_IN   1
#define CLASS_ANY  255
#define RCODE_FORMERR  1
#define RCODE_NXDOMAIN 3
#define RCODE_NOTIMP   4
#define TTL_S      10

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* End of the one question (just past type and class), 0 if malformed */
static size_t question_end(const uint8_t *q, size_t len)
{
    size_t pos = HDR, name = 0;
    for (;;) {
        if (pos >= len) return 0;
        uint8_t l = q[pos++];
        if (l == 0) break;
        if (l & 0xC0) return 0;            /* no compression in a question */
        name += 1u + l;
        if (name > 254 || pos + l > len) return 0;
        pos += l;
    }
    return pos + 4 <= len ? pos + 4 : 0;
}

size_t dns_captive_reply(const uint8_t *query, size_t len, uint8_t *out, size_t out_size,
                         uint32_t ip, bool captive)
{
    if (!query || !out || len < HDR || out_size < HDR) return 0;
    uint16_t flags = be16(&query[2]);
    if (flags & 0x8000) return 0;          /* a response: never answer those */
    uint8_t opcode = (flags >> 11) & 0x0F;

    size_t qend = 0;
    if (opcode == 0 && be16(&query[4]) == 1) qend = question_end(query, len);

    uint8_t rcode = 0;
    bool answer = false;
    if (opcode != 0) {
        rcode = RCODE_NOTIMP;
    } else if (!qend) {
        rcode = RCODE_FORMERR;
    } else if (!captive) {
        rcode = RCODE_NXDOMAIN;
    } else {
        uint16_t type = be16(&query[qend - 4]), cls = be16(&query[qend - 2]);
        answer = (type == TYPE_A || type == TYPE_ANY) && (cls == CLASS_IN || cls == CLASS_ANY);
    }

    size_t n = qend ? qend : HDR;
    if (n + (answer ? 16 : 0) > out_size) return 0;
    memcpy(out, query, n);                 /* id and the question */
    put16(&out[2], (uint16_t)(0x8000 | (opcode << 11) | 0x0400 | (flags & 0x0100) | rcode));
    put16(&out[4], qend ? 1 : 0);          /* questions */
    put16(&out[6], answer ? 1 : 0);        /* answers */
    put16(&out[8], 0);
    put16(&out[10], 0);                    /* EDNS and the rest are dropped */
    if (answer) {
        uint8_t *a = &out[n];
        put16(&a[0], 0xC00C);              /* the name in the question */
        put16(&a[2], TYPE_A);
        put16(&a[4], CLASS_IN);
        put16(&a[6], 0);
        put16(&a[8], TTL_S);
        put16(&a[10], 4);
        memcpy(&a[12], &ip, 4);            /* already network byte order */
        n += 16;
    }
    return n;
}
