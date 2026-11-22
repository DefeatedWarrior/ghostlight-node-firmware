/* Unit tests: the captive portal DNS answers (components/network_manager/dns_reply.c). */
#include "captive_dns.h"
#include "test.h"
#include <stdint.h>

#define IP(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
static const uint32_t AP = IP(192, 168, 4, 1);

static uint8_t q[600], r[DNS_REPLY_MAX];

/* A query for @p name (dotted) with type and class; returns its length */
static size_t query(const char *name, uint16_t type, uint16_t cls, uint16_t flags)
{
    memset(q, 0, sizeof(q));
    q[0] = 0xBE; q[1] = 0xEF;                /* id */
    q[2] = (uint8_t)(flags >> 8); q[3] = (uint8_t)flags;
    q[5] = 1;                                /* one question */
    size_t pos = 12;
    const char *s = name;
    while (*s) {
        const char *dot = strchr(s, '.');
        size_t n = dot ? (size_t)(dot - s) : strlen(s);
        q[pos++] = (uint8_t)n;
        memcpy(&q[pos], s, n);
        pos += n;
        s += n + (dot ? 1 : 0);
    }
    q[pos++] = 0;
    q[pos++] = (uint8_t)(type >> 8); q[pos++] = (uint8_t)type;
    q[pos++] = (uint8_t)(cls >> 8); q[pos++] = (uint8_t)cls;
    return pos;
}

static int rcode(void) { return r[3] & 0x0F; }
static int answers(void) { return (r[6] << 8) | r[7]; }

static void test_a_query_answered(void)
{
    size_t n = query("captive.apple.com", 1, 1, 0x0100);   /* RD */
    size_t len = dns_captive_reply(q, n, r, sizeof(r), AP, true);
    CHECK_EQ(len, n + 16);
    CHECK_EQ(r[0], 0xBE);
    CHECK_EQ(r[1], 0xEF);
    CHECK_EQ(r[2], 0x85);                    /* response, authoritative, RD copied */
    CHECK_EQ(rcode(), 0);
    CHECK_EQ(answers(), 1);
    CHECK_MEM(&r[12], &q[12], n - 12);       /* the question repeated */
    const uint8_t want[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 10, 0, 4, 192, 168, 4, 1};
    CHECK_MEM(&r[n], want, sizeof(want));
}

static void test_other_types_empty(void)
{
    size_t n = query("connectivitycheck.gstatic.com", 28, 1, 0);   /* AAAA */
    CHECK_EQ(dns_captive_reply(q, n, r, sizeof(r), AP, true), n);
    CHECK_EQ(rcode(), 0);
    CHECK_EQ(answers(), 0);
    n = query("x", 255, 1, 0);                                       /* ANY */
    CHECK_EQ(dns_captive_reply(q, n, r, sizeof(r), AP, true), n + 16);
    n = query("x", 1, 3, 0);                                         /* class CH */
    CHECK_EQ(dns_captive_reply(q, n, r, sizeof(r), AP, true), n);
    CHECK_EQ(answers(), 0);
}

static void test_off_is_nxdomain(void)
{
    size_t n = query("captive.apple.com", 1, 1, 0);
    CHECK_EQ(dns_captive_reply(q, n, r, sizeof(r), AP, false), n);
    CHECK_EQ(rcode(), 3);
    CHECK_EQ(answers(), 0);
}

static void test_edns_dropped(void)
{
    size_t n = query("a.b", 1, 1, 0);
    q[11] = 1;                               /* one additional record (EDNS OPT) */
    const uint8_t opt[] = {0, 0, 41, 0x10, 0, 0, 0, 0, 0, 0, 0};
    memcpy(&q[n], opt, sizeof(opt));
    size_t len = dns_captive_reply(q, n + sizeof(opt), r, sizeof(r), AP, true);
    CHECK_EQ(len, n + 16);
    CHECK_EQ(r[11], 0);
}

static void test_malformed(void)
{
    size_t n = query("a.b", 1, 1, 0);
    CHECK_EQ(dns_captive_reply(q, 11, r, sizeof(r), AP, true), 0);          /* no header */
    q[2] = 0x80;                                                             /* a response */
    CHECK_EQ(dns_captive_reply(q, n, r, sizeof(r), AP, true), 0);
    query("a.b", 1, 1, 0x2800);                                              /* opcode 5 (update) */
    CHECK_EQ(dns_captive_reply(q, n, r, sizeof(r), AP, true), 12);
    CHECK_EQ(rcode(), 4);
    query("a.b", 1, 1, 0);
    for (size_t cut = 12; cut < n; cut++) {                                  /* every truncation */
        CHECK_EQ(dns_captive_reply(q, cut, r, sizeof(r), AP, true), 12);
        CHECK_EQ(rcode(), 1);
        CHECK_EQ((r[4] << 8) | r[5], 0);
    }
    query("a.b", 1, 1, 0);
    q[12] = 0xC0;                                                            /* a pointer */
    CHECK_EQ(dns_captive_reply(q, n, r, sizeof(r), AP, true), 12);
    CHECK_EQ(rcode(), 1);
    query("a.b", 1, 1, 0);
    q[5] = 2;                                                                /* two questions */
    CHECK_EQ(dns_captive_reply(q, n, r, sizeof(r), AP, true), 12);
    CHECK_EQ(rcode(), 1);
    /* a name longer than 255 bytes */
    memset(q, 0, sizeof(q));
    q[5] = 1;
    size_t pos = 12;
    for (int i = 0; i < 5; i++) { q[pos++] = 60; memset(&q[pos], 'a', 60); pos += 60; }
    q[pos++] = 0;
    q[pos++] = 0; q[pos++] = 1; q[pos++] = 0; q[pos++] = 1;
    CHECK_EQ(dns_captive_reply(q, pos, r, sizeof(r), AP, true), 12);
    CHECK_EQ(rcode(), 1);
    /* too small an output buffer: nothing */
    n = query("a.b", 1, 1, 0);
    CHECK_EQ(dns_captive_reply(q, n, r, n + 15, AP, true), 0);
    CHECK_EQ(dns_captive_reply(NULL, n, r, sizeof(r), AP, true), 0);
}

static void test_longest_name_fits(void)
{
    /* 253 characters: 3 x 63 + 61 with dots */
    char name[260];
    memset(name, 'a', 253);
    name[63] = name[127] = name[191] = '.';
    name[253] = '\0';
    size_t n = query(name, 1, 1, 0);
    CHECK_EQ(n, 12 + 255 + 4);
    CHECK_EQ(dns_captive_reply(q, n, r, sizeof(r), AP, true), n + 16);
}

int main(void)
{
    RUN(test_a_query_answered);
    RUN(test_other_types_empty);
    RUN(test_off_is_nxdomain);
    RUN(test_edns_dropped);
    RUN(test_malformed);
    RUN(test_longest_name_fits);
    return test_summary();
}
