/* Unit tests: E1.31 (sACN) parsing in components/sacn_receiver.
 *
 * The component source is included so the tests can call its static packet
 * parser (process_packet) directly, the way the receive task does. */
#include "../../components/sacn_receiver/sacn_receiver.c"
#include "fakes.h"
#include "packets.h"

static void reset(void);
#define TEST_SETUP() reset()
#include "test.h"

static __typeof__(s_sacn) s_sacn_pristine;

static const uint8_t CID_A[16] = {0xA0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
static const uint8_t CID_B[16] = {0xB0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

static struct {
    int calls;
    sacn_packet_info_t info;   /* pointers are only valid during the call */
    uint8_t cid[16];
    char name[65];
    uint8_t data[600];
} rx;

static void on_data(const sacn_packet_info_t *pkt, void *user_data)
{
    (void)user_data;
    rx.calls++;
    rx.info = *pkt;
    memcpy(rx.cid, pkt->cid, 16);
    snprintf(rx.name, sizeof(rx.name), "%s", pkt->source_name);
    memcpy(rx.data, pkt->data, pkt->length);   /* ASan flags a length past the packet */
}

static struct { int calls; uint16_t universe; uint8_t cid[16]; uint32_t ip; } sync_rx;
static void on_sync(uint16_t universe, const uint8_t *cid, uint32_t ip, void *user_data)
{
    (void)user_data;
    sync_rx.calls++;
    sync_rx.universe = universe;
    memcpy(sync_rx.cid, cid, 16);
    sync_rx.ip = ip;
}

static void reset(void)
{
    host_fakes_reset();
    s_sacn = s_sacn_pristine;
    memset(&rx, 0, sizeof(rx));
    memset(&sync_rx, 0, sizeof(sync_rx));
    sacn_receiver_init();
    sacn_receiver_set_callback(on_data, NULL);
    sacn_receiver_set_sync_callback(on_sync, NULL);
}

#define IP(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

/* Hand @p len bytes to the parser in a receive buffer like the task's */
static void feed(const uint8_t *b, int len)
{
    uint8_t *copy = host_rx_buffer(b, (size_t)len, SACN_RX_BUF_SIZE);
    struct sockaddr_in from = { .sin_family = AF_INET, .sin_port = htons(5568) };
    from.sin_addr.s_addr = IP(2, 0, 0, 99);
    s_sacn.stats.packets_received++;
    process_packet(copy, len, &from);
    host_rx_free(copy, SACN_RX_BUF_SIZE);
}

static pk_sacn_t src(void)
{
    return (pk_sacn_t){ .cid = CID_A, .name = "Console A", .priority = 100,
                        .sequence = 1, .universe = 1, .start_code = 0x00 };
}

static uint8_t levels[512];
static uint8_t pkt[700];

/* ---- data packets ------------------------------------------------------- */

static void test_data_valid(void)
{
    for (int i = 0; i < 512; i++) levels[i] = (uint8_t)(255 - i);
    pk_sacn_t s = src();
    s.universe = 63999;
    s.sync_address = 7;
    s.sequence = 200;
    int n = pk_sacn_data(pkt, &s, levels, 512);
    CHECK_EQ(n, 638);
    feed(pkt, n);
    CHECK_EQ(rx.calls, 1);
    CHECK_EQ(rx.info.universe, 63999);
    CHECK_MEM(rx.cid, CID_A, 16);
    CHECK_STR(rx.name, "Console A");
    CHECK_EQ(rx.info.priority, 100);
    CHECK_EQ(rx.info.sequence, 200);
    CHECK_EQ(rx.info.sync_address, 7);
    CHECK_EQ(rx.info.start_code, 0x00);
    CHECK_EQ(rx.info.length, 512);
    CHECK_MEM(rx.data, levels, 512);
    CHECK_EQ(rx.info.source_ip, IP(2, 0, 0, 99));
    CHECK(!rx.info.preview && !rx.info.terminated);
    CHECK_EQ(s_sacn.stats.data_packets, 1);
    CHECK_EQ(s_sacn.stats.invalid_packets, 0);
}

static void test_data_short_frame_and_start_code_only(void)
{
    pk_sacn_t s = src();
    levels[0] = 77;
    feed(pkt, pk_sacn_data(pkt, &s, levels, 1));
    CHECK_EQ(rx.calls, 1);
    CHECK_EQ(rx.info.length, 1);
    CHECK_EQ(rx.data[0], 77);
    feed(pkt, pk_sacn_data(pkt, &s, levels, 0));   /* start code, no slots */
    CHECK_EQ(rx.calls, 2);
    CHECK_EQ(rx.info.length, 0);
}

static void test_priority(void)
{
    pk_sacn_t s = src();
    s.priority = 0;
    feed(pkt, pk_sacn_data(pkt, &s, levels, 4));
    CHECK_EQ(rx.info.priority, 0);
    s.priority = 200;
    feed(pkt, pk_sacn_data(pkt, &s, levels, 4));
    CHECK_EQ(rx.info.priority, 200);
    s.priority = 201;               /* out of range: clamped to 200 */
    feed(pkt, pk_sacn_data(pkt, &s, levels, 4));
    CHECK_EQ(rx.info.priority, 200);
    s.priority = 255;
    feed(pkt, pk_sacn_data(pkt, &s, levels, 4));
    CHECK_EQ(rx.info.priority, 200);
}

static void test_per_address_priority_and_alt_start_codes(void)
{
    pk_sacn_t s = src();
    s.start_code = 0xDD;
    feed(pkt, pk_sacn_data(pkt, &s, levels, 512));
    CHECK_EQ(rx.calls, 1);
    CHECK_EQ(rx.info.start_code, 0xDD);
    CHECK_EQ(s_sacn.stats.pap_packets, 1);
    CHECK_EQ(s_sacn.stats.data_packets, 0);

    s.start_code = 0xCC;            /* RDM, text, ...: not for the merge */
    feed(pkt, pk_sacn_data(pkt, &s, levels, 24));
    s.start_code = 0x17;
    feed(pkt, pk_sacn_data(pkt, &s, levels, 24));
    CHECK_EQ(rx.calls, 1);
    CHECK_EQ(s_sacn.stats.invalid_packets, 0);
}

static void test_sequence_passed_through(void)
{
    /* The receiver hands every sequence number on; the merge engine applies
     * the E1.31 6.7.2 window per source (see test_merge.c) */
    pk_sacn_t s = src();
    uint8_t seqs[] = {5, 5, 4, 250, 6};
    for (unsigned i = 0; i < sizeof(seqs); i++) {
        s.sequence = seqs[i];
        feed(pkt, pk_sacn_data(pkt, &s, levels, 8));
        CHECK_EQ(rx.info.sequence, seqs[i]);
    }
    CHECK_EQ(rx.calls, 5);
}

static void test_preview_and_terminated_flags(void)
{
    pk_sacn_t s = src();
    s.options = SACN_OPT_PREVIEW;
    feed(pkt, pk_sacn_data(pkt, &s, levels, 8));
    CHECK(rx.info.preview && !rx.info.terminated);
    s.options = SACN_OPT_STREAM_TERM;
    feed(pkt, pk_sacn_data(pkt, &s, levels, 8));
    CHECK(!rx.info.preview && rx.info.terminated);
    s.options = SACN_OPT_PREVIEW | SACN_OPT_STREAM_TERM | SACN_OPT_FORCE_SYNC;
    feed(pkt, pk_sacn_data(pkt, &s, levels, 8));
    CHECK(rx.info.preview && rx.info.terminated);
    CHECK_EQ(rx.info.options & SACN_OPT_FORCE_SYNC, SACN_OPT_FORCE_SYNC);
    CHECK_EQ(s_sacn.stats.preview_packets, 2);
    CHECK_EQ(s_sacn.stats.terminated_packets, 2);
    CHECK_EQ(rx.calls, 3);
}

static void test_source_name_not_terminated(void)
{
    pk_sacn_t s = src();
    int n = pk_sacn_data(pkt, &s, levels, 8);
    memset(&pkt[44], 'N', 64);      /* 64 bytes, no NUL */
    feed(pkt, n);
    CHECK_EQ(rx.calls, 1);
    CHECK_EQ(strlen(rx.name), 64);
}

/* One corrupted field per case; each packet must be counted invalid */
static void test_bad_headers(void)
{
    static const struct { int off; uint8_t val; } bad[] = {
        {1, 0x11},      /* preamble size != 0x0010 */
        {3, 0x01},      /* postamble size != 0 */
        {4, 'B'},       /* ACN packet identifier */
        {15, 0x01},
        {21, 0x05},     /* root vector neither DATA nor EXTENDED */
        {43, 0x03},     /* framing vector != DATA */
        {117, 0x01},    /* DMP vector != SET_PROPERTY */
        {118, 0xA0},    /* address & data type != 0xA1 */
        {120, 0x01},    /* first property address != 0 */
        {122, 0x02},    /* address increment != 1 */
        {113, 0x00},    /* universe 0 (with byte 114 cleared below) */
    };
    pk_sacn_t s = src();
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        int n = pk_sacn_data(pkt, &s, levels, 16);
        pkt[bad[i].off] = bad[i].val;
        if (bad[i].off == 113) pkt[114] = 0;
        feed(pkt, n);
        CHECK_EQ(rx.calls, 0);
        CHECK_EQ(s_sacn.stats.invalid_packets, i + 1);
    }
}

static void test_bad_universe(void)
{
    pk_sacn_t s = src();
    s.universe = 64000;
    feed(pkt, pk_sacn_data(pkt, &s, levels, 8));
    s.universe = 0xFFFF;
    feed(pkt, pk_sacn_data(pkt, &s, levels, 8));
    CHECK_EQ(rx.calls, 0);
    CHECK_EQ(s_sacn.stats.invalid_packets, 2);
}

static void test_bad_property_count(void)
{
    pk_sacn_t s = src();
    int n = pk_sacn_data(pkt, &s, levels, 512);
    pk_be16(&pkt[123], 0);          /* no start code */
    feed(pkt, n);
    pk_be16(&pkt[123], 514);        /* more than 512 slots */
    feed(pkt, n + 1);
    pk_be16(&pkt[123], 513);
    feed(pkt, n - 1);               /* count larger than the packet */
    CHECK_EQ(rx.calls, 0);
    CHECK_EQ(s_sacn.stats.invalid_packets, 3);
}

static void test_every_truncation(void)
{
    pk_sacn_t s = src();
    memset(levels, 0x5A, sizeof(levels));
    int n = pk_sacn_data(pkt, &s, levels, 512);
    for (int len = 0; len < n; len++) {
        feed(pkt, len);
    }
    CHECK_EQ(rx.calls, 0);
    CHECK_EQ(s_sacn.stats.invalid_packets, n);
    feed(pkt, n);
    CHECK_EQ(rx.calls, 1);
    feed(pkt, n + 40 < (int)sizeof(pkt) ? n + 40 : n);   /* trailing bytes are fine */
    CHECK_EQ(rx.calls, 2);
}

static void test_own_cid_ignored(void)
{
    sacn_receiver_set_local_cid(CID_A);
    pk_sacn_t s = src();
    feed(pkt, pk_sacn_data(pkt, &s, levels, 8));
    CHECK_EQ(rx.calls, 0);
    CHECK_EQ(s_sacn.stats.own_packets, 1);
    s.cid = CID_B;
    feed(pkt, pk_sacn_data(pkt, &s, levels, 8));
    CHECK_EQ(rx.calls, 1);
    sacn_receiver_set_local_cid(NULL);
    s.cid = CID_A;
    feed(pkt, pk_sacn_data(pkt, &s, levels, 8));
    CHECK_EQ(rx.calls, 2);
}

/* ---- synchronization / discovery packets ---------------------------------- */

static void test_sync_valid(void)
{
    feed(pkt, pk_sacn_sync(pkt, CID_A, 1, 4000));
    CHECK_EQ(sync_rx.calls, 1);
    CHECK_EQ(sync_rx.universe, 4000);
    CHECK_MEM(sync_rx.cid, CID_A, 16);
    CHECK_EQ(sync_rx.ip, IP(2, 0, 0, 99));
    CHECK_EQ(s_sacn.stats.sync_packets, 1);
}

static void test_sync_sequence(void)
{
    feed(pkt, pk_sacn_sync(pkt, CID_A, 10, 1));
    feed(pkt, pk_sacn_sync(pkt, CID_A, 10, 1));   /* duplicate */
    feed(pkt, pk_sacn_sync(pkt, CID_A, 9, 1));    /* late */
    feed(pkt, pk_sacn_sync(pkt, CID_A, 247, 1));  /* -19: late */
    CHECK_EQ(sync_rx.calls, 1);
    feed(pkt, pk_sacn_sync(pkt, CID_A, 246, 1));  /* -20: a restarted source */
    CHECK_EQ(sync_rx.calls, 2);
    feed(pkt, pk_sacn_sync(pkt, CID_A, 247, 1));
    CHECK_EQ(sync_rx.calls, 3);
    /* other sources and other sync universes have their own sequence */
    feed(pkt, pk_sacn_sync(pkt, CID_B, 0, 1));
    feed(pkt, pk_sacn_sync(pkt, CID_A, 0, 2));
    CHECK_EQ(sync_rx.calls, 5);
    /* after 2.5 s of silence any sequence is taken */
    host_advance_ms(2600);
    feed(pkt, pk_sacn_sync(pkt, CID_A, 200, 1));
    CHECK_EQ(sync_rx.calls, 6);
}

static void test_sync_bad(void)
{
    int n = pk_sacn_sync(pkt, CID_A, 1, 0);       /* universe 0 */
    feed(pkt, n);
    pk_sacn_sync(pkt, CID_A, 1, 64000);
    feed(pkt, n);
    pk_sacn_sync(pkt, CID_A, 1, 5);
    for (int len = 38; len < 49; len++) feed(pkt, len);
    CHECK_EQ(sync_rx.calls, 0);
    CHECK_EQ(s_sacn.stats.invalid_packets, 2 + 11);
    sacn_receiver_set_local_cid(CID_A);
    feed(pkt, n);
    CHECK_EQ(sync_rx.calls, 0);
    CHECK_EQ(s_sacn.stats.own_packets, 1);
}

static void test_discovery_ignored(void)
{
    int n = pk_sacn_sync(pkt, CID_A, 1, 1);
    pk_be32(&pkt[40], SACN_EXT_VECTOR_DISCOVERY);
    feed(pkt, n);
    CHECK_EQ(sync_rx.calls, 0);
    CHECK_EQ(rx.calls, 0);
    CHECK_EQ(s_sacn.stats.extended_packets, 1);
    CHECK_EQ(s_sacn.stats.invalid_packets, 0);
}

/* ---- subscriptions (no socket: bookkeeping only) -------------------------- */

static void test_subscriptions(void)
{
    CHECK_EQ(sacn_receiver_subscribe_universe(0), ESP_ERR_INVALID_ARG);
    CHECK_EQ(sacn_receiver_subscribe_universe(64000), ESP_ERR_INVALID_ARG);
    for (int u = 1; u <= SACN_MAX_UNIVERSES; u++) {
        CHECK_EQ(sacn_receiver_subscribe_universe((uint16_t)u), ESP_OK);
    }
    CHECK_EQ(sacn_receiver_subscribe_universe(100), ESP_ERR_NO_MEM);
    CHECK_EQ(sacn_receiver_subscribe_universe(3), ESP_OK);   /* already there */
    CHECK_EQ(sacn_receiver_unsubscribe_universe(3), ESP_OK);
    CHECK_EQ(sacn_receiver_unsubscribe_universe(3), ESP_ERR_NOT_FOUND);
    uint16_t want[] = {7, 0, 9};
    CHECK_EQ(sacn_receiver_set_universes(want, 3), ESP_OK);
    int n = 0;
    for (int i = 0; i < SACN_MAX_UNIVERSES; i++) {
        if (s_sacn.subs[i].universe) {
            n++;
            CHECK(s_sacn.subs[i].universe == 7 || s_sacn.subs[i].universe == 9);
        }
    }
    CHECK_EQ(n, 2);
}

int main(void)
{
    s_sacn_pristine = s_sacn;
    RUN(test_data_valid);
    RUN(test_data_short_frame_and_start_code_only);
    RUN(test_priority);
    RUN(test_per_address_priority_and_alt_start_codes);
    RUN(test_sequence_passed_through);
    RUN(test_preview_and_terminated_flags);
    RUN(test_source_name_not_terminated);
    RUN(test_bad_headers);
    RUN(test_bad_universe);
    RUN(test_bad_property_count);
    RUN(test_every_truncation);
    RUN(test_own_cid_ignored);
    RUN(test_sync_valid);
    RUN(test_sync_sequence);
    RUN(test_sync_bad);
    RUN(test_discovery_ignored);
    RUN(test_subscriptions);
    return test_summary();
}
