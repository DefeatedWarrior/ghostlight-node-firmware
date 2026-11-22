/* Unit tests: components/port_rules (per-port patch, limit and park). */
#include "port_rules.h"
#include "fakes.h"

static void reset(void);
#define TEST_SETUP() reset()
#include "test.h"

static uint8_t lv[512];

static void ramp(void)
{
    for (int i = 0; i < 512; i++) lv[i] = (uint8_t)(i + 1);   /* channel n = n (mod 256) */
}

static void reset(void)
{
    host_fakes_reset();
    for (int p = 1; p <= CONFIG_DMX_PORTS; p++) port_rules_set((uint8_t)p, NULL, 0);
    ramp();
}

#define PATCH(c, n, f) { .type = PORT_RULE_PATCH, .channel = (c), .count = (n), .from = (f) }
#define LIMIT(c, n, l) { .type = PORT_RULE_LIMIT, .channel = (c), .count = (n), .level = (l) }
#define PARK(c, n, l)  { .type = PORT_RULE_PARK,  .channel = (c), .count = (n), .level = (l) }

static void test_no_rules_changes_nothing(void)
{
    uint8_t want[512];
    memcpy(want, lv, sizeof(lv));
    port_rules_apply_list(NULL, 0, lv);
    port_rules_apply(1, lv);
    CHECK_MEM(lv, want, sizeof(lv));
    CHECK(!port_rules_active(1));
}

static void test_patch(void)
{
    port_rule_t r[] = { PATCH(101, 10, 1) };
    port_rules_apply_list(r, 1, lv);
    for (int i = 0; i < 10; i++) CHECK_EQ(lv[100 + i], i + 1);
    CHECK_EQ(lv[99], 100);                   /* around it: untouched */
    CHECK_EQ(lv[110], 111);
    CHECK_EQ(lv[0], 1);                      /* the source channels keep their own */
}

static void test_patches_read_the_incoming_levels(void)
{
    /* swap 1-4 and 5-8, and a patch that overlaps another's target */
    port_rule_t r[] = { PATCH(1, 4, 5), PATCH(5, 4, 1), PATCH(3, 4, 1) };
    port_rules_apply_list(r, 3, lv);
    CHECK_EQ(lv[0], 5);
    CHECK_EQ(lv[1], 6);
    CHECK_EQ(lv[2], 1);                      /* the later patch wins its channels */
    CHECK_EQ(lv[5], 4);
    CHECK_EQ(lv[4], 3);
    CHECK_EQ(lv[6], 3);
    CHECK_EQ(lv[7], 4);
}

static void test_order_patch_limit_park(void)
{
    /* listed park, limit, patch: applied patch, then limit, then park */
    port_rule_t r[] = { PARK(2, 1, 77), LIMIT(1, 3, 40), PATCH(1, 3, 200) };
    port_rules_apply_list(r, 3, lv);
    CHECK_EQ(lv[0], 40);                     /* 200 patched in, limited to 40 */
    CHECK_EQ(lv[1], 77);                     /* parked above the limit */
    CHECK_EQ(lv[2], 40);
    CHECK_EQ(lv[3], 4);
}

static void test_limit_and_park_edges(void)
{
    memset(lv, 255, sizeof(lv));
    port_rule_t r[] = { LIMIT(1, 512, 128), PARK(512, 1, 0), PARK(1, 1, 255) };
    port_rules_apply_list(r, 3, lv);
    CHECK_EQ(lv[0], 255);
    CHECK_EQ(lv[1], 128);
    CHECK_EQ(lv[510], 128);
    CHECK_EQ(lv[511], 0);
}

static void test_bad_rules_skipped(void)
{
    uint8_t want[512];
    memcpy(want, lv, sizeof(lv));
    port_rule_t r[] = { PATCH(500, 20, 1), LIMIT(0, 1, 0), PARK(1, 0, 0), { .type = 9, .channel = 1, .count = 1 },
                        PATCH(1, 1, 513) };
    port_rules_apply_list(r, 5, lv);
    CHECK_MEM(lv, want, sizeof(lv));
}

static void test_per_port(void)
{
    port_rule_t r[] = { PARK(1, 2, 9) };
    port_rules_set(1, r, 1);
    CHECK(port_rules_active(1));
    CHECK(!port_rules_active(2));
    r[0].level = 1;                          /* the port keeps its own copy */
    port_rules_apply(1, lv);
    CHECK_EQ(lv[0], 9);
    ramp();
    port_rules_apply(2, lv);
    CHECK_EQ(lv[0], 1);
    port_rules_set(1, NULL, 0);
    CHECK(!port_rules_active(1));
    port_rules_set(0, r, 1);                 /* out of range: ignored */
    port_rules_set(CONFIG_DMX_PORTS + 1, r, 1);
    port_rules_apply(CONFIG_DMX_PORTS + 1, lv);
    CHECK(!port_rules_active(0));
}

static void test_too_many(void)
{
    port_rule_t r[CONFIG_PORT_RULES_MAX + 2];
    for (int i = 0; i < CONFIG_PORT_RULES_MAX + 2; i++) r[i] = (port_rule_t)PARK(i + 1, 1, 0);
    port_rules_set(1, r, CONFIG_PORT_RULES_MAX + 2);
    port_rules_apply(1, lv);
    CHECK_EQ(lv[CONFIG_PORT_RULES_MAX - 1], 0);
    CHECK_EQ(lv[CONFIG_PORT_RULES_MAX], CONFIG_PORT_RULES_MAX + 1);   /* only the first 8 */
}

int main(void)
{
    RUN(test_no_rules_changes_nothing);
    RUN(test_patch);
    RUN(test_patches_read_the_incoming_levels);
    RUN(test_order_patch_limit_park);
    RUN(test_limit_and_park_edges);
    RUN(test_bad_rules_skipped);
    RUN(test_per_port);
    RUN(test_too_many);
    return test_summary();
}
