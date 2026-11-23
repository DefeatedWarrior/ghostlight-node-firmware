/* Unit tests: components/merge_engine (public API only). */
#include "merge_engine.h"
#include "fakes.h"
#include <stdlib.h>

static void reset(void);
#define TEST_SETUP() reset()
#include "test.h"

#define IP(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define IP_A IP(2, 0, 0, 1)
#define IP_B IP(2, 0, 0, 2)
#define IP_C IP(2, 0, 0, 3)

static const uint8_t CID_A[16] = {0xA};
static const uint8_t CID_B[16] = {0xB};

static uint8_t out[512];

static void reset(void)
{
    host_fakes_reset();
    merge_engine_deinit();
    merge_engine_init();
}

/* A full frame with every slot at @p v */
static uint8_t frame[512];
static const uint8_t *fill(uint8_t v)
{
    memset(frame, v, sizeof(frame));
    return frame;
}

static esp_err_t push_artnet(uint8_t port, uint32_t ip, const uint8_t *data, uint16_t len)
{
    merge_input_t in = {
        .protocol = SOURCE_PROTOCOL_ARTNET, .source_ip = ip,
        .priority = MERGE_ARTNET_PRIORITY, .start_code = 0x00,
        .data = data, .length = len,
    };
    return merge_engine_push(port, &in);
}

static esp_err_t push_sacn(uint8_t port, const uint8_t *cid, uint8_t prio, uint8_t seq,
                           const uint8_t *data, uint16_t len)
{
    merge_input_t in = {
        .protocol = SOURCE_PROTOCOL_SACN, .source_ip = IP(2, 0, 0, 100) + cid[0],
        .cid = cid, .priority = prio, .sequence = seq, .check_sequence = true,
        .start_code = 0x00, .data = data, .length = len,
    };
    return merge_engine_push(port, &in);
}

static esp_err_t push_sacn_pap(uint8_t port, const uint8_t *cid, uint8_t seq, const uint8_t *pap)
{
    merge_input_t in = {
        .protocol = SOURCE_PROTOCOL_SACN, .cid = cid, .priority = 100,
        .sequence = seq, .check_sequence = true, .start_code = 0xDD,
        .data = pap, .length = 512,
    };
    return merge_engine_push(port, &in);
}

static void get(uint8_t port)
{
    merge_engine_get_output(port, out);
}

/* ---- argument checks -------------------------------------------------------- */

static void test_invalid_args(void)
{
    uint8_t d[4] = {0};
    CHECK_EQ(push_artnet(0, IP_A, d, 4), ESP_ERR_INVALID_ARG);
    CHECK_EQ(push_artnet(CONFIG_DMX_PORTS + 1, IP_A, d, 4), ESP_ERR_INVALID_ARG);
    CHECK_EQ(push_artnet(1, IP_A, d, 0), ESP_ERR_INVALID_ARG);
    CHECK_EQ(push_artnet(1, IP_A, d, 513), ESP_ERR_INVALID_ARG);
    CHECK_EQ(push_artnet(1, IP_A, NULL, 4), ESP_ERR_INVALID_ARG);
    CHECK_EQ(merge_engine_push(1, NULL), ESP_ERR_INVALID_ARG);
    CHECK_EQ(merge_engine_config(1, (merge_mode_t)99, 1000), ESP_ERR_INVALID_ARG);
    CHECK_EQ(merge_engine_get_output(3, out), ESP_ERR_INVALID_ARG);
    CHECK_EQ(merge_engine_set_local_channel(1, 0, 1), ESP_ERR_INVALID_ARG);
    CHECK_EQ(merge_engine_set_local_channel(1, 513, 1), ESP_ERR_INVALID_ARG);
    CHECK_EQ(merge_engine_set_loss_behavior(1, (loss_behavior_t)9, NULL), ESP_ERR_INVALID_ARG);
}

static void test_no_source_is_zero(void)
{
    memset(out, 0xEE, sizeof(out));
    get(1);
    for (int i = 0; i < 512; i++) CHECK_EQ(out[i], 0);
    CHECK(!merge_engine_is_output_active(1));
    CHECK(!merge_engine_in_failsafe(1));
}

static void test_short_frame_zero_fills(void)
{
    push_artnet(1, IP_A, fill(200), 512);
    get(1);
    CHECK_EQ(out[511], 200);
    uint8_t d[3] = {1, 2, 3};
    push_artnet(1, IP_A, d, 3);
    get(1);
    CHECK_EQ(out[0], 1);
    CHECK_EQ(out[2], 3);
    CHECK_EQ(out[3], 0);
    CHECK_EQ(out[511], 0);
}

static void test_alternate_start_code_ignored(void)
{
    merge_input_t in = {
        .protocol = SOURCE_PROTOCOL_ARTNET, .source_ip = IP_A, .priority = 100,
        .start_code = 0xCC, .data = fill(99), .length = 512,
    };
    CHECK_EQ(merge_engine_push(1, &in), ESP_OK);
    get(1);
    CHECK_EQ(out[0], 0);
    CHECK(!merge_engine_is_output_active(1));
}

/* ---- HTP ------------------------------------------------------------------- */

static void test_htp_per_slot_max(void)
{
    uint8_t a[512] = {0}, b[512] = {0};
    a[0] = 100; a[1] = 10;  a[2] = 255;
    b[0] = 50;  b[1] = 200; b[3] = 7;
    push_artnet(1, IP_A, a, 512);
    push_artnet(1, IP_B, b, 512);
    get(1);
    CHECK_EQ(out[0], 100);
    CHECK_EQ(out[1], 200);
    CHECK_EQ(out[2], 255);
    CHECK_EQ(out[3], 7);
    CHECK_EQ(out[4], 0);
    merge_source_info_t info[MERGE_MAX_SOURCES];
    CHECK_EQ(merge_engine_get_sources(1, info, MERGE_MAX_SOURCES), 2);
    CHECK(info[0].contributing && info[1].contributing);
    /* ports are independent */
    get(2);
    CHECK_EQ(out[0], 0);
}

static void test_htp_priority_wins(void)
{
    push_sacn(1, CID_A, 100, 1, fill(255), 512);
    get(1);
    host_advance_ms(1600);          /* past the new-source hold-back below */
    push_sacn(1, CID_A, 100, 2, fill(255), 512);
    push_sacn(1, CID_B, 150, 1, fill(10), 512);
    host_advance_ms(1600);
    push_sacn(1, CID_A, 100, 3, fill(255), 512);
    push_sacn(1, CID_B, 150, 2, fill(10), 512);
    get(1);
    CHECK_EQ(out[0], 10);           /* higher priority, lower level, still wins */
    CHECK_EQ(out[511], 10);
}

static void test_htp_artnet_with_sacn_default_priority(void)
{
    push_artnet(1, IP_A, fill(30), 512);
    push_sacn(1, CID_A, 100, 1, fill(60), 512);
    host_advance_ms(1600);          /* the new sACN source is held back first */
    push_artnet(1, IP_A, fill(30), 512);
    push_sacn(1, CID_A, 100, 2, fill(60), 512);
    get(1);
    CHECK_EQ(out[0], 60);           /* same priority: HTP across protocols */
}

static void test_new_sacn_source_held_back(void)
{
    push_artnet(1, IP_A, fill(30), 512);
    push_sacn(1, CID_A, 100, 1, fill(90), 512);
    get(1);
    CHECK_EQ(out[0], 30);           /* waits for a possible 0xDD packet */
    host_advance_ms(1000);
    push_artnet(1, IP_A, fill(30), 512);
    push_sacn(1, CID_A, 100, 2, fill(90), 512);
    get(1);
    CHECK_EQ(out[0], 30);
    host_advance_ms(600);
    push_artnet(1, IP_A, fill(30), 512);
    push_sacn(1, CID_A, 100, 3, fill(90), 512);
    get(1);
    CHECK_EQ(out[0], 90);
}

static void test_per_address_priority(void)
{
    uint8_t pap_a[512], pap_b[512], a[512], b[512];
    memset(pap_a, 0, 512); memset(pap_b, 0, 512);
    memset(a, 200, 512);   memset(b, 50, 512);
    pap_a[0] = 150; pap_b[0] = 100;  /* slot 1: A by priority */
    pap_a[1] = 100; pap_b[1] = 150;  /* slot 2: B by priority, lower level */
    pap_a[2] = 0;   pap_b[2] = 100;  /* slot 3: only B sources it */
                                     /* slot 4: nobody sources it -> 0 */
    push_sacn_pap(1, CID_A, 1, pap_a);
    push_sacn(1, CID_A, 100, 2, a, 512);
    push_sacn_pap(1, CID_B, 1, pap_b);
    push_sacn(1, CID_B, 100, 2, b, 512);
    get(1);
    CHECK_EQ(out[0], 200);
    CHECK_EQ(out[1], 50);
    CHECK_EQ(out[2], 50);
    CHECK_EQ(out[3], 0);

    /* 0xDD stops: after 2.5 s each source falls back to its packet priority */
    for (int t = 0; t < 3; t++) {
        host_advance_ms(1000);
        push_sacn(1, CID_A, 100, (uint8_t)(3 + t), a, 512);
        push_sacn(1, CID_B, 100, (uint8_t)(3 + t), b, 512);
    }
    get(1);
    CHECK_EQ(out[1], 200);
    CHECK_EQ(out[3], 200);
}

/* ---- LTP ------------------------------------------------------------------- */

static void test_ltp_latest_change_wins(void)
{
    merge_engine_config(1, MERGE_MODE_LTP, 2500);
    uint8_t a[512] = {0}, b[512] = {0};
    a[0] = 200; a[1] = 200;
    push_artnet(1, IP_A, a, 512);
    b[0] = 10; b[1] = 10;
    push_artnet(1, IP_B, b, 512);   /* B is new: it takes every slot */
    get(1);
    CHECK_EQ(out[0], 10);
    CHECK_EQ(out[1], 10);

    a[1] = 201;                     /* A changes slot 2 only */
    push_artnet(1, IP_A, a, 512);
    push_artnet(1, IP_B, b, 512);   /* B repeats itself: no change */
    get(1);
    CHECK_EQ(out[0], 10);
    CHECK_EQ(out[1], 201);

    b[0] = 0;                       /* lower values win too, if they are the latest */
    push_artnet(1, IP_B, b, 512);
    get(1);
    CHECK_EQ(out[0], 0);
    merge_stats_t st;
    merge_engine_get_stats(1, &st);
    CHECK(st.ltp_merges >= 3);
}

static void test_ltp_owner_lost_falls_back_to_htp(void)
{
    merge_engine_config(1, MERGE_MODE_LTP, 1000);
    uint8_t a[512] = {0}, b[512] = {0};
    a[0] = 100;
    b[0] = 40;
    push_artnet(1, IP_A, a, 512);
    push_artnet(1, IP_B, b, 512);
    get(1);
    CHECK_EQ(out[0], 40);
    /* B goes away: its slots are nobody's, the rest show A */
    host_advance_ms(600);
    push_artnet(1, IP_A, a, 512);
    host_advance_ms(600);
    get(1);
    CHECK_EQ(out[0], 100);
}

/* ---- LAST / BACKUP / DISABLE ------------------------------------------------- */

static void test_last_source_owns_frame(void)
{
    merge_engine_config(1, MERGE_MODE_LAST, 2500);
    uint8_t a[512] = {0}, b[512] = {0};
    a[0] = 200; a[1] = 0;
    b[0] = 10;  b[1] = 99;
    push_artnet(1, IP_A, a, 512);
    host_advance_ms(10);
    push_artnet(1, IP_B, b, 512);
    get(1);
    CHECK_EQ(out[0], 10);           /* whole frame from B, no per-slot merge */
    CHECK_EQ(out[1], 99);
    host_advance_ms(10);
    push_artnet(1, IP_A, a, 512);
    get(1);
    CHECK_EQ(out[0], 200);
    CHECK_EQ(out[1], 0);
}

static void test_backup_failover(void)
{
    merge_engine_config(1, MERGE_MODE_BACKUP, 1000);
    merge_input_t primary = {
        .protocol = SOURCE_PROTOCOL_ARTNET, .source_ip = IP_A, .priority = 100,
        .data = NULL, .length = 512,
    };
    merge_input_t backup = primary;
    backup.source_ip = IP_B;
    backup.backup = true;
    uint8_t p[512], bk[512];
    memset(p, 100, 512);
    memset(bk, 50, 512);
    primary.data = p;
    backup.data = bk;

    merge_engine_push(1, &backup);
    merge_engine_push(1, &primary);
    get(1);
    CHECK_EQ(out[0], 100);          /* the primary hides the backup */

    for (int i = 0; i < 3; i++) {   /* primary lost, backup keeps sending */
        host_advance_ms(500);
        merge_engine_push(1, &backup);
    }
    get(1);
    CHECK_EQ(out[0], 50);

    merge_engine_push(1, &primary); /* primary back: it takes over again */
    get(1);
    CHECK_EQ(out[0], 100);
    merge_stats_t st;
    merge_engine_get_stats(1, &st);
    /* Only the switch back counts: a primary that times out clears the
     * owner before the backup is picked */
    CHECK_EQ(st.backup_switches, 1);
}

static void test_disable_first_source_keeps_port(void)
{
    merge_engine_config(1, MERGE_MODE_DISABLE, 1000);
    push_artnet(1, IP_A, fill(11), 512);
    get(1);
    host_advance_ms(10);
    push_artnet(1, IP_B, fill(22), 512);
    get(1);
    CHECK_EQ(out[0], 11);           /* no merge, the first source stays */
    for (int i = 0; i < 3; i++) {   /* A stops; B takes over once A times out */
        host_advance_ms(500);
        push_artnet(1, IP_B, fill(22), 512);
    }
    get(1);
    CHECK_EQ(out[0], 22);
    push_artnet(1, IP_A, fill(11), 512);   /* A back: B now owns the port */
    get(1);
    CHECK_EQ(out[0], 22);
}

static void test_owner_modes_respect_priority(void)
{
    merge_engine_config(1, MERGE_MODE_DISABLE, 2500);
    push_sacn(1, CID_A, 100, 1, fill(11), 512);
    get(1);
    host_advance_ms(1600);
    push_sacn(1, CID_A, 100, 2, fill(11), 512);
    push_sacn(1, CID_B, 120, 1, fill(22), 512);
    host_advance_ms(1600);
    push_sacn(1, CID_A, 100, 3, fill(11), 512);
    push_sacn(1, CID_B, 120, 2, fill(22), 512);
    get(1);
    CHECK_EQ(out[0], 22);
}

/* ---- timeouts, termination, table -------------------------------------------- */

static void test_source_timeout(void)
{
    merge_engine_config(1, MERGE_MODE_HTP, 1000);
    push_artnet(1, IP_A, fill(10), 512);
    push_artnet(1, IP_B, fill(20), 512);
    host_advance_ms(600);
    push_artnet(1, IP_A, fill(10), 512);
    host_advance_ms(500);           /* B silent for 1.1 s */
    get(1);
    CHECK_EQ(out[0], 10);
    merge_stats_t st;
    merge_engine_get_stats(1, &st);
    CHECK_EQ(st.source_timeouts, 1);
    CHECK_EQ(st.active_sources, 1);
    host_advance_ms(1000);
    get(1);
    CHECK(!merge_engine_is_output_active(1));
}

static void test_timeout_clamped(void)
{
    merge_engine_config(1, MERGE_MODE_HTP, 1);      /* raised to 500 ms */
    push_artnet(1, IP_A, fill(10), 512);
    host_advance_ms(400);
    get(1);
    CHECK(merge_engine_is_output_active(1));
    host_advance_ms(200);
    get(1);
    CHECK(!merge_engine_is_output_active(1));
}

static void test_stream_terminated_drops_source(void)
{
    push_sacn(1, CID_A, 100, 1, fill(80), 512);
    get(1);
    CHECK_EQ(out[0], 80);
    merge_input_t term = {
        .protocol = SOURCE_PROTOCOL_SACN, .cid = CID_A, .priority = 100,
        .sequence = 2, .check_sequence = true, .data = fill(80), .length = 512,
        .terminated = true,
    };
    merge_engine_push(1, &term);
    get(1);
    CHECK(!merge_engine_is_output_active(1));
    merge_source_info_t info[MERGE_MAX_SOURCES];
    CHECK_EQ(merge_engine_get_sources(1, info, MERGE_MAX_SOURCES), 0);
}

/* E1.31 6.2.6: the property values of a Stream_Terminated packet are
 * ignored, so a sender may terminate with the start code alone (an sACN
 * packet with property count 1 reaches the merge engine with length 0). */
static void test_stream_terminated_without_slots(void)
{
    push_sacn(1, CID_A, 100, 1, fill(80), 512);
    get(1);
    merge_input_t term = {
        .protocol = SOURCE_PROTOCOL_SACN, .cid = CID_A, .priority = 100,
        .sequence = 2, .check_sequence = true, .data = frame, .length = 0,
        .terminated = true,
    };
    CHECK_EQ(merge_engine_push(1, &term), ESP_OK);
    get(1);
    CHECK(!merge_engine_is_output_active(1));
}

static void test_source_table_full(void)
{
    for (int i = 0; i < MERGE_MAX_SOURCES; i++) {
        CHECK_EQ(push_artnet(1, IP(2, 0, 0, 10 + i), fill(1), 512), ESP_OK);
    }
    CHECK_EQ(push_artnet(1, IP(2, 0, 0, 99), fill(1), 512), ESP_ERR_NO_MEM);
    merge_stats_t st;
    merge_engine_get_stats(1, &st);
    CHECK_EQ(st.sources_rejected, 1);
    /* once the others have timed out there is room again */
    host_advance_ms(3000);
    CHECK_EQ(push_artnet(1, IP(2, 0, 0, 99), fill(1), 512), ESP_OK);
}

/* ---- sequence numbers -------------------------------------------------------- */

static void test_sequence_window(void)
{
    push_sacn(1, CID_A, 100, 10, fill(10), 512);
    push_sacn(1, CID_A, 100, 10, fill(99), 512);   /* duplicate */
    push_sacn(1, CID_A, 100, 9, fill(99), 512);    /* late */
    push_sacn(1, CID_A, 100, 247, fill(99), 512);  /* 247 - 10 = -19: late */
    get(1);
    CHECK_EQ(out[0], 10);
    merge_stats_t st;
    merge_engine_get_stats(1, &st);
    CHECK_EQ(st.sequence_drops, 3);
    push_sacn(1, CID_A, 100, 11, fill(11), 512);
    get(1);
    CHECK_EQ(out[0], 11);
    push_sacn(1, CID_A, 100, 246, fill(46), 512);  /* -21: taken (source restarted) */
    get(1);
    CHECK_EQ(out[0], 46);
    push_sacn(1, CID_A, 100, 0, fill(1), 512);     /* wraps 246 -> 0 */
    get(1);
    CHECK_EQ(out[0], 1);
}

static void test_artnet_sequence_zero_not_checked(void)
{
    merge_input_t in = {
        .protocol = SOURCE_PROTOCOL_ARTNET, .source_ip = IP_A, .priority = 100,
        .sequence = 0, .check_sequence = false, .data = fill(5), .length = 512,
    };
    merge_engine_push(1, &in);
    in.data = fill(6);
    merge_engine_push(1, &in);
    get(1);
    CHECK_EQ(out[0], 6);
}

/* ---- protocol filter --------------------------------------------------------- */

static void test_protocol_filter(void)
{
    merge_engine_set_protocol_mode(1, PROTOCOL_ARTNET_ONLY);
    push_sacn(1, CID_A, 100, 1, fill(90), 512);
    push_artnet(1, IP_A, fill(30), 512);
    get(1);
    CHECK_EQ(out[0], 30);
    merge_stats_t st;
    merge_engine_get_stats(1, &st);
    CHECK_EQ(st.packets_filtered, 1);

    merge_engine_set_protocol_mode(1, PROTOCOL_SACN_ONLY);   /* drops the Art-Net source */
    get(1);
    CHECK(!merge_engine_is_output_active(1));
}

/* Another port's DMX input (main/app_routing.c feed_local_outputs): the
 * input port number stands in for the address */
static esp_err_t push_dmx_in(uint8_t port, uint8_t from_port, const uint8_t *data, bool terminated)
{
    merge_input_t in = {
        .protocol = SOURCE_PROTOCOL_DMX_IN, .source_ip = from_port, .name = "Port 2 DMX in",
        .priority = MERGE_ARTNET_PRIORITY, .start_code = 0x00,
        .data = data, .length = terminated ? 0 : 512, .terminated = terminated,
    };
    return merge_engine_push(port, &in);
}

static void test_local_dmx_input(void)
{
    /* passes every protocol filter, HTP-merges with the network */
    merge_engine_set_protocol_mode(1, PROTOCOL_SACN_ONLY);
    CHECK_EQ(push_dmx_in(1, 2, fill(40), false), ESP_OK);
    get(1);
    CHECK_EQ(out[0], 40);
    merge_engine_set_protocol_mode(1, PROTOCOL_ARTNET_ONLY);
    get(1);
    CHECK_EQ(out[0], 40);
    uint8_t net[512] = {0};
    net[0] = 90;
    push_artnet(1, IP_A, net, 512);
    get(1);
    CHECK_EQ(out[0], 90);
    CHECK_EQ(out[1], 40);

    /* not mistaken for an Art-Net sender at "address" 2 */
    push_artnet(1, 2, fill(10), 512);
    merge_source_info_t src[MERGE_MAX_SOURCES];
    CHECK_EQ(merge_engine_get_sources(1, src, MERGE_MAX_SOURCES), 3);

    /* a second input is a source of its own */
    uint8_t second[512] = {0};
    second[2] = 200;
    CHECK_EQ(push_dmx_in(1, 3, second, false), ESP_OK);
    CHECK_EQ(merge_engine_get_sources(1, src, MERGE_MAX_SOURCES), 4);
    get(1);
    CHECK_EQ(out[1], 40);
    CHECK_EQ(out[2], 200);

    /* each dropped at once when the port stops taking it */
    CHECK_EQ(push_dmx_in(1, 2, frame, true), ESP_OK);
    CHECK_EQ(push_dmx_in(1, 3, frame, true), ESP_OK);
    CHECK_EQ(merge_engine_get_sources(1, src, MERGE_MAX_SOURCES), 2);
    get(1);
    CHECK_EQ(out[0], 90);
    CHECK_EQ(out[1], 10);
    CHECK_EQ(out[2], 10);
}

static void test_protocol_priority_modes(void)
{
    merge_engine_set_protocol_mode(1, PROTOCOL_SACN_PRIORITY);
    push_artnet(1, IP_A, fill(200), 512);
    get(1);
    CHECK_EQ(out[0], 200);          /* Art-Net is used while there is no sACN */
    push_sacn(1, CID_A, 100, 1, fill(20), 512);
    host_advance_ms(1600);
    push_artnet(1, IP_A, fill(200), 512);
    push_sacn(1, CID_A, 100, 2, fill(20), 512);
    get(1);
    CHECK_EQ(out[0], 20);           /* sACN hides Art-Net */

    merge_engine_set_protocol_mode(1, PROTOCOL_ARTNET_PRIORITY);
    get(1);
    CHECK_EQ(out[0], 200);
}

/* ---- failsafe (loss behaviour) ------------------------------------------------ */

static void lose_sources(void)
{
    host_advance_ms(3000);
    get(1);
}

static void test_failsafe_hold_last_look(void)
{
    merge_engine_set_loss_behavior(1, LOSS_HOLD_LAST_LOOK, NULL);
    push_artnet(1, IP_A, fill(77), 512);
    get(1);
    CHECK(!merge_engine_in_failsafe(1));
    lose_sources();
    CHECK_EQ(out[0], 77);
    CHECK(merge_engine_in_failsafe(1));
    CHECK(!merge_engine_is_output_active(1));
    push_artnet(1, IP_A, fill(5), 512);   /* data returns: failsafe ends */
    get(1);
    CHECK_EQ(out[0], 5);
    CHECK(!merge_engine_in_failsafe(1));
}

static void test_failsafe_blackout(void)
{
    merge_engine_set_hold_last_look(1, false);
    push_artnet(1, IP_A, fill(77), 512);
    get(1);
    lose_sources();
    CHECK_EQ(out[0], 0);
    CHECK_EQ(out[511], 0);
    CHECK(merge_engine_in_failsafe(1));
}

static void test_failsafe_full(void)
{
    merge_engine_set_loss_behavior(1, LOSS_FULL, NULL);
    push_artnet(1, IP_A, fill(77), 512);
    get(1);
    lose_sources();
    CHECK_EQ(out[0], 255);
    CHECK_EQ(out[511], 255);
}

static void test_failsafe_scene(void)
{
    uint8_t scene[512];
    for (int i = 0; i < 512; i++) scene[i] = (uint8_t)i;
    merge_engine_set_loss_behavior(1, LOSS_SCENE, scene);
    push_artnet(1, IP_A, fill(77), 512);
    get(1);
    lose_sources();
    CHECK_MEM(out, scene, 512);
    merge_engine_set_loss_behavior(1, LOSS_SCENE, NULL);   /* no scene: zero */
    get(1);
    CHECK_EQ(out[5], 0);
}

static void test_failsafe_stop_output(void)
{
    merge_engine_set_loss_behavior(1, LOSS_STOP_OUTPUT, NULL);
    push_artnet(1, IP_A, fill(77), 512);
    get(1);
    CHECK(!merge_engine_output_stopped(1));
    lose_sources();
    CHECK(merge_engine_output_stopped(1));      /* the frame source sends nothing */
    CHECK_EQ(out[0], 77);                       /* the last look stays readable */
    merge_engine_set_local_channel(1, 1, 9);    /* a scene or the web console */
    get(1);
    CHECK(!merge_engine_output_stopped(1));
    merge_engine_clear_local(1);
    get(1);
    CHECK(merge_engine_output_stopped(1));
    push_artnet(1, IP_A, fill(5), 512);         /* data returns: output again */
    get(1);
    CHECK(!merge_engine_output_stopped(1));
    CHECK_EQ(out[0], 5);
    CHECK(!merge_engine_output_stopped(0));     /* no such port */
}

static void test_failsafe_not_before_first_source(void)
{
    merge_engine_set_loss_behavior(1, LOSS_FULL, NULL);
    get(1);
    CHECK_EQ(out[0], 0);            /* never had data: nothing to fail over from */
    CHECK(!merge_engine_in_failsafe(1));
}

/* ---- local layer, sync, blackout ---------------------------------------------- */

static void test_local_layer_htp_on_top(void)
{
    uint8_t a[512] = {0};
    a[0] = 100; a[1] = 100;
    push_artnet(1, IP_A, a, 512);
    merge_engine_set_local_channel(1, 1, 50);
    merge_engine_set_local_channel(1, 2, 150);
    merge_engine_set_local_channel(1, 512, 9);
    get(1);
    CHECK_EQ(out[0], 100);
    CHECK_EQ(out[1], 150);
    CHECK_EQ(out[511], 9);
    lose_sources();                 /* the local layer never times out */
    CHECK(merge_engine_is_output_active(1));
    CHECK_EQ(out[511], 9);
    merge_engine_clear_local(1);
    get(1);
    CHECK(!merge_engine_is_output_active(1));
}

static void test_artnet_sync_latches(void)
{
    push_artnet(1, IP_A, fill(10), 512);
    get(1);
    CHECK_EQ(merge_engine_artnet_sync(IP_B), 0);   /* not from the ArtDmx sender */
    CHECK_EQ(merge_engine_artnet_sync(IP_A), 0x01);
    CHECK_EQ(merge_engine_get_sync_state(1), MERGE_SYNC_ARTNET);
    push_artnet(1, IP_A, fill(20), 512);
    get(1);
    CHECK_EQ(out[0], 10);           /* held until the next ArtSync */
    merge_engine_artnet_sync(IP_A);
    get(1);
    CHECK_EQ(out[0], 20);
    /* 4 s without ArtSync: back to free-running output */
    for (int i = 0; i < 5; i++) {
        host_advance_ms(1000);
        push_artnet(1, IP_A, fill(30), 512);
    }
    get(1);
    CHECK_EQ(merge_engine_get_sync_state(1), MERGE_SYNC_NONE);
    CHECK_EQ(out[0], 30);
}

static void test_artnet_sync_ignored_while_merging(void)
{
    push_artnet(1, IP_A, fill(10), 512);
    push_artnet(1, IP_B, fill(20), 512);
    get(1);
    CHECK_EQ(merge_engine_artnet_sync(IP_A), 0);
}

static void test_sacn_sync_latches(void)
{
    merge_input_t in = {
        .protocol = SOURCE_PROTOCOL_SACN, .cid = CID_A, .priority = 100,
        .sequence = 1, .check_sequence = true, .data = fill(10), .length = 512,
        .sync_address = 7,
    };
    merge_engine_push(1, &in);
    get(1);
    uint16_t su[4];
    CHECK_EQ(merge_engine_sync_universes(1, su, 4), 1);
    CHECK_EQ(su[0], 7);
    CHECK_EQ(merge_engine_sacn_sync(8), 0);
    CHECK_EQ(merge_engine_sacn_sync(7), 0x01);
    in.sequence = 2;
    in.data = fill(20);
    merge_engine_push(1, &in);
    get(1);
    CHECK_EQ(out[0], 10);
    merge_engine_sacn_sync(7);
    get(1);
    CHECK_EQ(out[0], 20);
}

static void test_blackout(void)
{
    push_artnet(1, IP_A, fill(10), 512);
    merge_engine_set_local_channel(1, 1, 50);
    get(1);
    merge_engine_blackout(1);
    get(1);
    CHECK_EQ(out[0], 0);
    CHECK(!merge_engine_is_output_active(1));
}

static void test_cancel_merge(void)
{
    merge_engine_config(1, MERGE_MODE_HTP, 1000);
    push_artnet(1, IP_A, fill(200), 512);
    push_artnet(1, IP_B, fill(50), 512);
    get(1);
    CHECK_EQ(out[0], 200);
    merge_engine_cancel_merge(1);
    get(1);
    CHECK_EQ(out[0], 200);          /* nothing until the next ArtDmx */

    push_artnet(1, IP_B, fill(50), 512);
    get(1);
    CHECK_EQ(out[0], 50);
    push_artnet(1, IP_A, fill(220), 512);
    push_artnet(1, IP_C, fill(230), 512);
    get(1);
    CHECK_EQ(out[0], 50);           /* other senders are discarded */
    merge_source_info_t s[MERGE_MAX_SOURCES];
    CHECK_EQ(merge_engine_get_sources(1, s, MERGE_MAX_SOURCES), 1);

    push_sacn(1, CID_A, 120, 1, fill(90), 512);
    host_advance_ms(900);
    push_artnet(1, IP_B, fill(50), 512);
    host_advance_ms(700);           /* sACN's 1.5 s hold-back is over */
    push_artnet(1, IP_B, fill(50), 512);
    push_sacn(1, CID_A, 120, 2, fill(90), 512);
    get(1);
    CHECK_EQ(out[0], 90);           /* sACN isn't part of it */
}

static void test_cancel_merge_needs_a_merge(void)
{
    push_artnet(1, IP_A, fill(200), 512);
    merge_engine_cancel_merge(1);
    push_artnet(1, IP_B, fill(50), 512);
    push_artnet(1, IP_A, fill(200), 512);
    get(1);
    CHECK_EQ(out[0], 200);
    merge_source_info_t s[MERGE_MAX_SOURCES];
    CHECK_EQ(merge_engine_get_sources(1, s, MERGE_MAX_SOURCES), 2);
}

static void cancelled(void)
{
    merge_engine_config(1, MERGE_MODE_HTP, 1000);
    push_artnet(1, IP_A, fill(200), 512);
    push_artnet(1, IP_B, fill(50), 512);
    merge_engine_cancel_merge(1);
    push_artnet(1, IP_B, fill(50), 512);
}

static void test_cancel_merge_ends_when_sender_stops(void)
{
    cancelled();
    host_advance_ms(1100);
    push_artnet(1, IP_A, fill(200), 512);   /* before any output was computed */
    get(1);
    CHECK_EQ(out[0], 200);
}

static void test_cancel_merge_undone(void)
{
    cancelled();
    merge_engine_resume_merge(1);
    push_artnet(1, IP_A, fill(200), 512);
    get(1);
    CHECK_EQ(out[0], 200);

    cancelled();
    merge_engine_config(1, MERGE_MODE_LTP, 1000);
    push_artnet(1, IP_A, fill(201), 512);
    get(1);
    CHECK_EQ(out[0], 201);

    cancelled();
    merge_engine_blackout(1);
    push_artnet(1, IP_A, fill(202), 512);
    get(1);
    CHECK_EQ(out[0], 202);
}

static void test_cancel_merge_request_expires(void)
{
    merge_engine_config(1, MERGE_MODE_HTP, 1000);
    push_artnet(1, IP_A, fill(200), 512);
    push_artnet(1, IP_B, fill(50), 512);
    merge_engine_cancel_merge(1);
    host_advance_ms(1100);
    push_artnet(1, IP_B, fill(50), 512);
    push_artnet(1, IP_A, fill(200), 512);
    get(1);
    CHECK_EQ(out[0], 200);
}

static void test_cancel_merge_again(void)
{
    cancelled();
    merge_engine_cancel_merge(1);   /* not merging now: nothing changes */
    push_artnet(1, IP_A, fill(200), 512);
    get(1);
    CHECK_EQ(out[0], 50);
    merge_engine_cancel_merge(2);
    push_artnet(2, IP_A, fill(7), 512);
    get(2);
    CHECK_EQ(out[0], 7);            /* the other port is its own */
}

int main(void)
{
    RUN(test_invalid_args);
    RUN(test_no_source_is_zero);
    RUN(test_short_frame_zero_fills);
    RUN(test_alternate_start_code_ignored);
    RUN(test_htp_per_slot_max);
    RUN(test_htp_priority_wins);
    RUN(test_htp_artnet_with_sacn_default_priority);
    RUN(test_new_sacn_source_held_back);
    RUN(test_per_address_priority);
    RUN(test_ltp_latest_change_wins);
    RUN(test_ltp_owner_lost_falls_back_to_htp);
    RUN(test_last_source_owns_frame);
    RUN(test_backup_failover);
    RUN(test_disable_first_source_keeps_port);
    RUN(test_owner_modes_respect_priority);
    RUN(test_source_timeout);
    RUN(test_timeout_clamped);
    RUN(test_stream_terminated_drops_source);
    RUN(test_stream_terminated_without_slots);
    RUN(test_source_table_full);
    RUN(test_sequence_window);
    RUN(test_artnet_sequence_zero_not_checked);
    RUN(test_protocol_filter);
    RUN(test_local_dmx_input);
    RUN(test_protocol_priority_modes);
    RUN(test_failsafe_hold_last_look);
    RUN(test_failsafe_blackout);
    RUN(test_failsafe_full);
    RUN(test_failsafe_scene);
    RUN(test_failsafe_stop_output);
    RUN(test_failsafe_not_before_first_source);
    RUN(test_local_layer_htp_on_top);
    RUN(test_artnet_sync_latches);
    RUN(test_artnet_sync_ignored_while_merging);
    RUN(test_sacn_sync_latches);
    RUN(test_blackout);
    RUN(test_cancel_merge);
    RUN(test_cancel_merge_needs_a_merge);
    RUN(test_cancel_merge_ends_when_sender_stops);
    RUN(test_cancel_merge_undone);
    RUN(test_cancel_merge_request_expires);
    RUN(test_cancel_merge_again);
    return test_summary();
}
