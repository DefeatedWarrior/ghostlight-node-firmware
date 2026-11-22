/* Unit tests: components/source_monitor (the signal check's sender table). */
#include <stdio.h>
#include "source_monitor.h"
#include "fakes.h"

static void reset(void);
#define TEST_SETUP() reset()
#include "test.h"

#define IP(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

static const uint8_t CID_A[16] = {0xA0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
static const uint8_t CID_B[16] = {0xB0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
static srcmon_source_t rows[SRCMON_MAX_SOURCES];

static void reset(void)
{
    host_fakes_reset();
    srcmon_reset();
}

static void artnet(uint32_t ip, uint16_t universe, uint8_t seq)
{
    srcmon_packet_t p = { .proto = SRCMON_ARTNET, .ip = ip, .universe = universe,
                          .sequence = seq, .slots = 512 };
    srcmon_note(&p);
}

static void sacn(const uint8_t *cid, uint16_t universe, uint8_t seq, uint8_t start_code)
{
    srcmon_packet_t p = { .proto = SRCMON_SACN, .ip = IP(10, 0, 0, 5), .cid = cid,
                          .universe = universe, .name = "Desk\x01 A", .priority = 120,
                          .start_code = start_code, .sequence = seq, .slots = 24 };
    srcmon_note(&p);
}

static int get(void)
{
    return srcmon_get_sources(rows, SRCMON_MAX_SOURCES);
}

static void test_one_artnet_sender(void)
{
    for (int i = 0; i < 45; i++) {
        artnet(IP(2, 0, 0, 1), 3, (uint8_t)(i % 255 + 1));
        host_advance_ms(25);   /* 40 per second */
    }
    CHECK_EQ(get(), 1);
    CHECK_EQ(rows[0].proto, SRCMON_ARTNET);
    CHECK_EQ(rows[0].ip, IP(2, 0, 0, 1));
    CHECK_EQ(rows[0].universe, 3);
    CHECK_EQ(rows[0].packets, 45);
    CHECK_EQ(rows[0].lost, 0);
    CHECK_EQ(rows[0].late, 0);
    CHECK_EQ(rows[0].slots, 512);
    CHECK(rows[0].sequenced);
    CHECK_EQ(rows[0].rate_x10, 400);
    CHECK_EQ(rows[0].max_gap_ms, 25);
    CHECK_EQ(rows[0].age_ms, 25);
    CHECK_STR(rows[0].name, "");
}

static void test_rows_per_universe_and_sender(void)
{
    artnet(IP(2, 0, 0, 1), 0, 0);
    artnet(IP(2, 0, 0, 1), 1, 0);
    artnet(IP(2, 0, 0, 2), 1, 0);
    sacn(CID_A, 1, 1, 0);
    sacn(CID_B, 1, 1, 0);       /* same IP, another sACN source */
    artnet(IP(2, 0, 0, 1), 0, 0);
    CHECK_EQ(get(), 5);
    host_advance_ms(10);
    artnet(IP(2, 0, 0, 2), 1, 0);
    CHECK_EQ(get(), 5);
    CHECK_EQ(rows[0].ip, IP(2, 0, 0, 2));    /* newest first */
    CHECK_EQ(rows[0].age_ms, 0);
    CHECK(!rows[0].sequenced);               /* Art-Net sequence 0 = not used */
}

static void test_artnet_sequence(void)
{
    uint8_t seq[] = {253, 254, 255, 1, 2, 5, 4, 5, 6};   /* wraps 255 -> 1; 3-4 lost; 4 late; 5 repeat */
    for (size_t i = 0; i < sizeof(seq); i++) {
        artnet(IP(2, 0, 0, 1), 0, seq[i]);
        host_advance_ms(20);
    }
    CHECK_EQ(get(), 1);
    CHECK_EQ(rows[0].lost, 2);
    CHECK_EQ(rows[0].late, 2);
    CHECK_EQ(rows[0].lost_recent, 2);
}

static void test_sacn_sequence_and_pap(void)
{
    uint8_t seq[] = {254, 255, 0, 1, 3, 3, 250, 4};   /* wraps 255 -> 0; 2 lost; 3 repeat; 250 late */
    for (size_t i = 0; i < sizeof(seq); i++) {
        sacn(CID_A, 7, seq[i], 0x00);
        host_advance_ms(20);
    }
    sacn(CID_A, 7, 5, 0xDD);
    CHECK_EQ(get(), 1);
    CHECK_EQ(rows[0].proto, SRCMON_SACN);
    CHECK_EQ(rows[0].lost, 1);
    CHECK_EQ(rows[0].late, 2);
    CHECK_EQ(rows[0].priority, 120);
    CHECK_STR(rows[0].name, "Desk? A");      /* JSON-safe */
    CHECK(rows[0].pap);
    CHECK_EQ(rows[0].slots, 24);             /* from the level packets, not 0xDD */
    host_advance_ms(3000);
    CHECK_EQ(get(), 1);
    CHECK(!rows[0].pap);                     /* no 0xDD for a while */
}

static void test_restart_and_pause(void)
{
    sacn(CID_A, 1, 10, 0);
    host_advance_ms(20);
    sacn(CID_A, 1, 200, 0);                  /* too far back to be late: a restart */
    host_advance_ms(20);
    sacn(CID_A, 1, 201, 0);
    CHECK_EQ(get(), 1);
    CHECK_EQ(rows[0].lost, 0);
    CHECK_EQ(rows[0].late, 0);
    host_advance_ms(SRCMON_RESYNC_MS + 1);
    sacn(CID_A, 1, 50, 0);                   /* after a pause: a new start, not 104 lost */
    CHECK_EQ(get(), 1);
    CHECK_EQ(rows[0].lost, 0);
    CHECK_EQ(rows[0].max_gap_ms, SRCMON_RESYNC_MS + 1);
}

static void test_recent_counters_age_out(void)
{
    artnet(IP(2, 0, 0, 1), 0, 1);
    host_advance_ms(100);
    artnet(IP(2, 0, 0, 1), 0, 4);            /* 2 lost, 100 ms gap */
    CHECK_EQ(get(), 1);
    CHECK_EQ(rows[0].lost_recent, 2);
    CHECK_EQ(rows[0].max_gap_ms, 100);
    for (int i = 0; i < 12; i++) {           /* 12 s of steady packets */
        host_advance_ms(1000);
        artnet(IP(2, 0, 0, 1), 0, (uint8_t)(5 + i));
    }
    CHECK_EQ(get(), 1);
    CHECK_EQ(rows[0].lost_recent, 2);        /* the previous epoch still counts */
    CHECK_EQ(rows[0].max_gap_ms, 1000);
    host_advance_ms(9000);
    artnet(IP(2, 0, 0, 1), 0, 17);
    host_advance_ms(9000);
    CHECK_EQ(get(), 1);
    CHECK_EQ(rows[0].lost_recent, 0);
    CHECK_EQ(rows[0].lost, 2);               /* all time */
}

static void test_rate_and_silence(void)
{
    for (int i = 0; i < 11; i++) {
        artnet(IP(2, 0, 0, 1), 0, 0);
        host_advance_ms(100);
    }
    CHECK_EQ(get(), 1);
    CHECK_EQ(rows[0].rate_x10, 100);         /* 10 per second */
    host_advance_ms(5100);
    CHECK_EQ(get(), 1);
    CHECK_EQ(rows[0].rate_x10, 0);           /* silent: no rate */
    host_advance_ms(SRCMON_FORGET_MS);
    CHECK_EQ(get(), 0);                      /* and forgotten */
}

static void test_full_table_reuses_the_oldest(void)
{
    for (int i = 0; i < SRCMON_MAX_SOURCES; i++) {
        artnet(IP(2, 0, 0, 1), (uint16_t)i, 0);
        host_advance_ms(1);
    }
    artnet(IP(2, 0, 0, 9), 500, 0);
    CHECK_EQ(get(), SRCMON_MAX_SOURCES);
    bool zero = false, new_one = false;
    for (int i = 0; i < SRCMON_MAX_SOURCES; i++) {
        if (rows[i].universe == 0) zero = true;
        if (rows[i].universe == 500) new_one = true;
    }
    CHECK(!zero);
    CHECK(new_one);
    CHECK_EQ(srcmon_get_sources(rows, 3), 3);
    CHECK_EQ(srcmon_get_sources(rows, 0), 0);
    CHECK_EQ(srcmon_get_sources(NULL, 3), 0);
}

static void test_bad_input_ignored(void)
{
    srcmon_note(NULL);
    srcmon_packet_t p = { .proto = 9, .ip = 1, .universe = 1 };
    srcmon_note(&p);
    srcmon_packet_t q = { .proto = SRCMON_SACN, .ip = 1, .universe = 1 };   /* no CID, no name */
    srcmon_note(&q);
    CHECK_EQ(get(), 1);
    CHECK_STR(rows[0].name, "");
}

static void put_be(uint8_t *b, const uint16_t *u, int n)
{
    for (int i = 0; i < n; i++) {
        b[2 * i] = (uint8_t)(u[i] >> 8);
        b[2 * i + 1] = (uint8_t)u[i];
    }
}

static void test_adverts(void)
{
    srcmon_advert_t a[SRCMON_MAX_ADVERTS];
    uint16_t u[20];
    uint8_t be[40];
    for (int i = 0; i < 20; i++) u[i] = (uint16_t)(i + 1);
    put_be(be, u, 20);
    srcmon_note_discovery(CID_A, IP(10, 0, 0, 5), "Desk A", 0, 1, be, 12);
    srcmon_note_discovery(CID_A, IP(10, 0, 0, 5), "Desk A", 1, 1, be + 24, 8);
    CHECK_EQ(srcmon_get_adverts(a, SRCMON_MAX_ADVERTS), 1);
    CHECK_EQ(a[0].count, 20);
    CHECK_EQ(a[0].stored, SRCMON_ADVERT_UNIVERSES);
    CHECK_EQ(a[0].universes[0], 1);
    CHECK_EQ(a[0].universes[15], 16);
    CHECK_STR(a[0].name, "Desk A");
    /* page 1 again without a new page 0: not counted twice */
    srcmon_note_discovery(CID_A, IP(10, 0, 0, 5), "Desk A", 1, 1, be + 24, 8);
    CHECK_EQ(srcmon_get_adverts(a, SRCMON_MAX_ADVERTS), 1);
    CHECK_EQ(a[0].count, 20);
    /* the next round starts over */
    srcmon_note_discovery(CID_A, IP(10, 0, 0, 5), "Desk A", 0, 0, be, 2);
    srcmon_note_discovery(CID_B, IP(10, 0, 0, 6), "Desk B", 0, 0, be, 0);
    CHECK_EQ(srcmon_get_adverts(a, SRCMON_MAX_ADVERTS), 2);
    CHECK_EQ(a[0].count + a[1].count, 2);
    host_advance_ms(31000);
    CHECK_EQ(srcmon_get_adverts(a, SRCMON_MAX_ADVERTS), 0);
    srcmon_note_discovery(NULL, 1, "x", 0, 0, be, 1);
    srcmon_note_discovery(CID_A, 1, "x", 0, 0, NULL, 1);
    CHECK_EQ(srcmon_get_adverts(a, SRCMON_MAX_ADVERTS), 0);
}

/* ---- other Art-Net nodes ------------------------------------------------ */

static srcmon_node_t nodes[SRCMON_MAX_NODES];

static void node_reply(uint32_t ip, uint8_t mac_last, uint8_t bind, const char *name)
{
    artnet_poll_reply_t r = { .ip = ip, .from_ip = ip, .bind_index = bind, .num_ports = 1 };
    r.mac[0] = 0x02;
    r.mac[5] = mac_last;
    snprintf(r.short_name, sizeof(r.short_name), "%s", name);
    snprintf(r.long_name, sizeof(r.long_name), "%s rack", name);
    r.ports[0].type = 0x80;
    r.ports[0].out_address = bind;
    srcmon_note_node(&r, false);
}

static void test_nodes(void)
{
    node_reply(IP(2, 0, 0, 30), 3, 1, "C");
    node_reply(IP(2, 0, 0, 20), 2, 2, "B\x7f\x01");   /* not ASCII-safe */
    node_reply(IP(2, 0, 0, 20), 2, 1, "B");
    node_reply(IP(10, 0, 0, 1), 9, 1, "Far");
    node_reply(IP(2, 0, 0, 20), 2, 1, "B");            /* again: same row */
    int n = srcmon_get_nodes(nodes, SRCMON_MAX_NODES);
    CHECK_EQ(n, 4);
    /* by address, then BindIndex */
    CHECK_EQ(nodes[0].reply.ip, IP(2, 0, 0, 20));
    CHECK_EQ(nodes[0].reply.bind_index, 1);
    CHECK_EQ(nodes[0].replies, 2);
    CHECK_EQ(nodes[1].reply.bind_index, 2);
    CHECK_STR(nodes[1].reply.short_name, "B??");
    CHECK_EQ(nodes[2].reply.ip, IP(2, 0, 0, 30));
    CHECK_EQ(nodes[3].reply.ip, IP(10, 0, 0, 1));
    CHECK_STR(nodes[3].reply.long_name, "Far rack");

    /* same address, another device: its own row */
    node_reply(IP(2, 0, 0, 30), 4, 1, "Clash");
    CHECK_EQ(srcmon_get_nodes(nodes, SRCMON_MAX_NODES), 5);
    CHECK_EQ(srcmon_get_nodes(nodes, 2), 2);            /* max respected */

    /* forgotten once they stop answering */
    host_advance_ms(SRCMON_NODE_FORGET_MS - 1000);
    node_reply(IP(2, 0, 0, 30), 3, 1, "C");
    host_advance_ms(1001);
    n = srcmon_get_nodes(nodes, SRCMON_MAX_NODES);
    CHECK_EQ(n, 1);
    CHECK_EQ(nodes[0].age_ms, 1001);
    CHECK_STR(nodes[0].reply.short_name, "C");
}

static void test_output_nodes(void)
{
    uint32_t ips[8];
    CHECK_EQ(srcmon_output_nodes(1, ips, 8), 0);
    node_reply(IP(2, 0, 0, 30), 3, 1, "C");      /* outputs Port-Address 1 */
    node_reply(IP(2, 0, 0, 20), 2, 2, "B");      /* outputs 2 */
    node_reply(IP(2, 0, 0, 20), 2, 1, "B");      /* the same node, 1 too */
    node_reply(IP(10, 0, 0, 1), 9, 1, "Far");
    int n = srcmon_output_nodes(1, ips, 8);
    CHECK_EQ(n, 3);                              /* 2.0.0.20 once */
    int found20 = 0;
    for (int i = 0; i < n; i++) found20 += ips[i] == IP(2, 0, 0, 20);
    CHECK_EQ(found20, 1);
    CHECK_EQ(srcmon_output_nodes(2, ips, 8), 1);
    CHECK_EQ(ips[0], IP(2, 0, 0, 20));
    CHECK_EQ(srcmon_output_nodes(1, ips, 2), 2);   /* max respected */

    /* an input-only port, or a node using this node's address, doesn't count */
    artnet_poll_reply_t r = { .ip = IP(2, 0, 0, 40), .from_ip = IP(2, 0, 0, 40), .num_ports = 1 };
    r.ports[0].type = 0x40;
    r.ports[0].out_address = 7;
    srcmon_note_node(&r, false);
    CHECK_EQ(srcmon_output_nodes(7, ips, 8), 0);
    r.ip = r.from_ip = IP(2, 0, 0, 41);
    r.ports[0].type = 0x80;
    srcmon_note_node(&r, true);
    CHECK_EQ(srcmon_output_nodes(7, ips, 8), 0);

    host_advance_ms(SRCMON_NODE_FORGET_MS + 1);
    CHECK_EQ(srcmon_output_nodes(1, ips, 8), 0);   /* forgotten */
}

static void test_nodes_table_full(void)
{
    for (int i = 0; i < SRCMON_MAX_NODES + 5; i++) {
        node_reply(IP(2, 0, 1, i), (uint8_t)i, 1, "N");
        host_advance_ms(10);
    }
    int n = srcmon_get_nodes(nodes, SRCMON_MAX_NODES);
    CHECK_EQ(n, SRCMON_MAX_NODES);
    CHECK_EQ(nodes[0].reply.ip, IP(2, 0, 1, 5));        /* the oldest five went */
    CHECK(srcmon_get_nodes(NULL, 4) == 0);
    srcmon_note_node(NULL, true);                       /* ignored */
}

int main(void)
{
    RUN(test_one_artnet_sender);
    RUN(test_rows_per_universe_and_sender);
    RUN(test_artnet_sequence);
    RUN(test_sacn_sequence_and_pap);
    RUN(test_restart_and_pause);
    RUN(test_recent_counters_age_out);
    RUN(test_rate_and_silence);
    RUN(test_full_table_reuses_the_oldest);
    RUN(test_bad_input_ignored);
    RUN(test_adverts);
    RUN(test_nodes);
    RUN(test_output_nodes);
    RUN(test_nodes_table_full);
    return test_summary();
}
