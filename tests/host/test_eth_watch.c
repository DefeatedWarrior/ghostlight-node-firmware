/* Unit tests: when a deaf W5500 is set up again (components/network_manager/eth_watch.c). */
#include "eth_watch.h"
#include "test.h"

static eth_watch_t w;
static uint32_t frames;

/* Step once a second for @p s seconds; the number of set-ups asked for */
static int run(uint32_t *now, int s, bool link, bool talking)
{
    int n = 0;
    for (int i = 0; i < s; i++) {
        *now += 1000;
        if (talking) frames++;
        n += eth_watch_step(&w, link, frames, *now);
    }
    return n;
}

static void test_traffic_never_resets(void)
{
    uint32_t now = 5000;
    eth_watch_init(&w, frames, now);
    CHECK_EQ(run(&now, 3600, true, true), 0);
    CHECK_EQ(w.tries, 0);
}

static void test_no_link_never_resets(void)
{
    uint32_t now = 0;
    eth_watch_init(&w, frames, now);
    CHECK_EQ(run(&now, 3600, false, false), 0);   /* unplugged: nothing to fix */
}

static void test_silence_resets_after_a_minute(void)
{
    uint32_t now = 0;
    eth_watch_init(&w, frames, now);
    CHECK_EQ(run(&now, ETH_QUIET_S - 1, true, false), 0);
    CHECK_EQ(run(&now, 1, true, false), 1);
    CHECK_EQ(w.tries, 1);
}

static void test_silent_network_backs_off(void)
{
    uint32_t now = 0;
    eth_watch_init(&w, frames, now);
    /* 60 + 120 + 240 + 480 + 960 s, then every 960 s */
    CHECK_EQ(run(&now, 60 + 120 + 240 + 480 + 960, true, false), 5);
    CHECK_EQ(w.limit_s, ETH_QUIET_MAX_S);
    CHECK_EQ(run(&now, 959, true, false), 0);
    CHECK_EQ(run(&now, 1, true, false), 1);
    CHECK_EQ(run(&now, 2 * ETH_QUIET_MAX_S, true, false), 2);
}

static void test_recovery_restores_the_short_wait(void)
{
    uint32_t now = 0;
    eth_watch_init(&w, frames, now);
    CHECK_EQ(run(&now, 60 + 120, true, false), 2);
    CHECK_EQ(run(&now, 5, true, true), 0);        /* it hears again */
    CHECK_EQ(w.limit_s, ETH_QUIET_S);
    CHECK_EQ(run(&now, ETH_QUIET_S, true, false), 1);
}

static void test_link_flap_restarts_the_wait(void)
{
    uint32_t now = 0;
    eth_watch_init(&w, frames, now);
    CHECK_EQ(run(&now, 50, true, false), 0);
    CHECK_EQ(run(&now, 2, false, false), 0);      /* cable out and back */
    CHECK_EQ(run(&now, ETH_QUIET_S - 1, true, false), 0);
    CHECK_EQ(run(&now, 1, true, false), 1);
}

static void test_clock_wraps(void)
{
    uint32_t now = 0xFFFFFFFFu - 30000;           /* 49.7 days in */
    eth_watch_init(&w, frames, now);
    CHECK_EQ(run(&now, ETH_QUIET_S - 1, true, false), 0);
    CHECK_EQ(run(&now, 1, true, false), 1);
}

int main(void)
{
    RUN(test_traffic_never_resets);
    RUN(test_no_link_never_resets);
    RUN(test_silence_resets_after_a_minute);
    RUN(test_silent_network_backs_off);
    RUN(test_recovery_restores_the_short_wait);
    RUN(test_link_flap_restarts_the_wait);
    RUN(test_clock_wraps);
    return test_summary();
}
