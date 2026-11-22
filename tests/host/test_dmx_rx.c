/* The vendored esp_dmx receive path: the real interrupt handler
 * (dmx/hal/uart.c) and the real dmx_receive() / dmx_read_received()
 * (dmx/io.c), fed by a simulated UART (stubs_dmx/).
 *
 * What the hardware does at a break isn't documented to the microsecond, so
 * every test runs four ways: framing error and break flagged together or
 * the error first, and a receiving task that wakes at once or only after
 * the break that follows. This proves the logic, not the timing on a chip. */
#include "dmx/hal/uart.c"
#include "dmx/include/driver.h"
#include "test.h"

#define FRAME_MAX DMX_PACKET_SIZE_MAX

uart_dev_t host_uart[SOC_UART_NUM];
const char *TAG = "dmx";
dmx_driver_t *dmx_driver[DMX_NUM_MAX];
static dmx_driver_t drv;

typedef enum { EV_BYTE, EV_BAD_BYTE, EV_FRM, EV_BRK, EV_FRM_BRK } kind_t;
static struct { kind_t kind; uint8_t v; } s_ev[200000];
static int s_n, s_at;

static bool s_err_first, s_late;
static bool s_pending;
static uint32_t s_value;
static int s_task;

#define GOT_MAX 128
static struct { uint8_t b[FRAME_MAX]; int len; } s_got[GOT_MAX];
static int s_got_n, s_short_errs, s_seq_gaps;

static void reset(void)
{
    memset(&drv, 0, sizeof(drv));
    memset(host_uart, 0, sizeof(host_uart));
    memset(s_got, 0, sizeof(s_got));
    drv.is_enabled = true;
    drv.dmx.size = DMX_PACKET_SIZE_MAX;
    drv.dmx.head = DMX_HEAD_WAITING_FOR_BREAK;
    dmx_driver[0] = &drv;
    host_uart[0].conf0.sw_rts = 1;
    s_n = s_at = s_got_n = s_short_errs = s_seq_gaps = 0;
    s_pending = false;
}

/* ---- the wire ---------------------------------------------------------- */

static void ev(kind_t k, uint8_t v)
{
    s_ev[s_n].kind = k;
    s_ev[s_n++].v = v;
}

static void brk(void)
{
    if (s_err_first) {
        ev(EV_FRM, 0);
        ev(EV_BRK, 0);
    } else {
        ev(EV_FRM_BRK, 0);
    }
}

static uint8_t slot(int seed, int i)
{
    return (uint8_t)(1 + (seed * 17 + i) % 250);
}

static void body(uint8_t sc, int slots, int seed)
{
    ev(EV_BYTE, sc);
    for (int i = 1; i <= slots; i++) ev(EV_BYTE, slot(seed, i));
}

static void packet(uint8_t sc, int slots, int seed)
{
    brk();
    body(sc, slots, seed);
}

static void step(void)
{
    uart_dev_t *u = &host_uart[0];
    kind_t k = s_ev[s_at].kind;
    uint8_t v = s_ev[s_at++].v;

    /* a break arrives as a null byte with a framing error */
    if (k != EV_BRK && u->fifo_len < (int)sizeof(u->fifo)) u->fifo[u->fifo_len++] = v;
    if (k != EV_BRK) u->intr |= UART_INTR_RXFIFO_FULL;
    if (k == EV_FRM || k == EV_FRM_BRK || k == EV_BAD_BYTE) u->intr |= UART_INTR_FRAM_ERR;
    if (k == EV_BRK || k == EV_FRM_BRK) u->intr |= UART_INTR_BRK_DET;
    dmx_uart_isr(&drv);
}

/* ---- FreeRTOS and the rest of esp_dmx ---------------------------------- */

TaskHandle_t xTaskGetCurrentTaskHandle(void) { return &s_task; }

BaseType_t xTaskNotifyFromISR(TaskHandle_t task, uint32_t value, eNotifyAction a, int *woken)
{
    (void)task; (void)woken;
    s_pending = true;
    if (a == eSetValueWithOverwrite) s_value = value;
    return pdTRUE;
}

BaseType_t xTaskNotifyStateClear(TaskHandle_t task)
{
    (void)task;
    s_pending = false;
    return pdTRUE;
}

/* The task sleeps while the wire runs, until the interrupt wakes it */
BaseType_t xTaskNotifyWait(uint32_t clr_in, uint32_t clr_out, uint32_t *value, TickType_t ticks)
{
    (void)clr_in; (void)clr_out; (void)ticks;
    while (!s_pending && s_at < s_n) step();
    if (!s_pending) return pdFALSE;
    /* late: whatever break follows directly has already been handled */
    while (s_late && s_at < s_n && s_ev[s_at].kind >= EV_FRM) step();
    s_pending = false;
    if (value) *value = s_value;
    return pdTRUE;
}

int64_t esp_timer_get_time(void) { return 0; }
int64_t dmx_timer_get_micros_since_boot(void) { return 0; }
void dmx_timer_stop(dmx_port_t n) { (void)n; }
void dmx_timer_start(dmx_port_t n) { (void)n; }
void dmx_timer_set_counter(dmx_port_t n, uint64_t c) { (void)n; (void)c; }
void dmx_timer_set_alarm(dmx_port_t n, uint64_t a, bool r) { (void)n; (void)a; (void)r; }
bool dmx_driver_is_installed(dmx_port_t n) { return dmx_driver[n] != NULL; }
bool dmx_driver_is_enabled(dmx_port_t n) { return dmx_driver[n]->is_enabled; }
rdm_pid_t dmx_parameter_commit(dmx_port_t n) { (void)n; return 0; }
bool rdm_read_header(dmx_port_t n, rdm_header_t *h) { (void)n; (void)h; return true; }

/* ---- the receiving task, reading as dmx_handler.c's input task does ---- */

static void run(void)
{
    uint32_t last = 0;
    for (;;) {
        dmx_packet_t pkt = { .err = DMX_ERR_TIMEOUT };
        uint8_t b[FRAME_MAX];
        uint32_t seq = 0;
        bool end = s_at >= s_n;
        dmx_receive(0, &pkt, 200);
        size_t n = dmx_read_received(0, b, sizeof(b), &seq);
        if (seq && seq != last) {
            if (seq - last > 1) s_seq_gaps += (int)(seq - last - 1);
            if (pkt.err == DMX_ERR_NOT_ENOUGH_SLOTS) s_short_errs++;
            if (s_got_n < GOT_MAX) {
                memcpy(s_got[s_got_n].b, b, n);
                s_got[s_got_n++].len = (int)n;
            }
            last = seq;
        }
        if (end) return;
    }
}

static int times_got(int slots, int seed)
{
    uint8_t want[FRAME_MAX] = { 0 };
    int found = 0;
    for (int i = 1; i <= slots; i++) want[i] = slot(seed, i);
    for (int g = 0; g < s_got_n; g++) {
        if (s_got[g].len == slots + 1 && !memcmp(s_got[g].b, want, (size_t)slots + 1)) found++;
    }
    return found;
}

/* ---- tests ------------------------------------------------------------- */

static void test_full_frames(void)
{
    for (int i = 0; i < 20; i++) packet(0, 512, i);
    brk();
    run();
    for (int i = 0; i < 20; i++) CHECK_EQ(times_got(512, i), 1);
    CHECK_EQ(s_got_n, 20);
    CHECK_EQ(s_seq_gaps, 0);
    CHECK_EQ(s_short_errs, 0);
}

static void test_constant_short_frames(void)
{
    for (int i = 0; i < 20; i++) packet(0, 24, i);
    brk();
    run();
    for (int i = 0; i < 20; i++) CHECK_EQ(times_got(24, i), 1);
    CHECK_EQ(s_got_n, 20);
    CHECK_EQ(drv.dmx.size, 25);
    CHECK(s_short_errs <= 2);
}

static void test_short_frames_of_varying_length(void)
{
    static const int len[] = { 24, 100, 50, 200, 24, 300, 128, 64, 500, 2 };
    for (int i = 0; i < 10; i++) packet(0, len[i], i);
    brk();
    run();
    for (int i = 0; i < 10; i++) CHECK_EQ(times_got(len[i], i), 1);
    CHECK_EQ(drv.dmx.size, 513);
}

static void test_one_cut_frame(void)
{
    packet(0, 512, 1);
    packet(0, 200, 2);      /* cable pulled, or a glitch before a break */
    for (int i = 3; i < 8; i++) packet(0, 512, i);
    brk();
    run();
    CHECK_EQ(times_got(512, 1), 1);
    CHECK_EQ(times_got(200, 2), 1);
    for (int i = 3; i < 8; i++) CHECK_EQ(times_got(512, i), 1);
    CHECK_EQ(drv.dmx.size, 513);
    CHECK_EQ(s_short_errs, 1);
}

static void test_grows_back(void)
{
    for (int i = 0; i < 4; i++) packet(0, 24, i);
    for (int i = 4; i < 8; i++) packet(0, 512, i);
    brk();
    run();
    for (int i = 0; i < 4; i++) CHECK_EQ(times_got(24, i), 1);
    for (int i = 4; i < 8; i++) CHECK_EQ(times_got(512, i), 1);
    CHECK_EQ(drv.dmx.size, 513);
}

/* two short, one long, over and over: the long one's top slots must arrive */
static void test_repeating_mixed_lengths(void)
{
    for (int i = 0; i < 12; i++) packet(0, i % 3 == 2 ? 512 : 24, i);
    brk();
    run();
    for (int i = 0; i < 12; i++) CHECK(times_got(i % 3 == 2 ? 512 : 24, i) >= 1);
}

static void test_stray_break_after_frame(void)
{
    for (int i = 0; i < 6; i++) {
        packet(0, 512, i);
        brk();
    }
    brk();
    run();
    for (int i = 0; i < 6; i++) CHECK_EQ(times_got(512, i), 1);
    CHECK_EQ(s_got_n, 6);
    CHECK_EQ(drv.dmx.size, 513);
}

/* SIP and text packets are short and must not set the DMX frame's size */
static void test_other_start_codes(void)
{
    packet(0, 512, 1);
    packet(0xCF, 25, 2);
    packet(0xCF, 25, 3);
    packet(0, 512, 4);
    packet(0x17, 40, 5);
    packet(0, 512, 6);
    brk();
    run();
    CHECK_EQ(s_got_n, 3);
    CHECK_EQ(times_got(512, 4), 1);
    CHECK_EQ(times_got(512, 6), 1);
    CHECK_EQ(drv.dmx.size, 513);
}

/* a start code and nothing else is no frame, and never a size */
static void test_start_code_only(void)
{
    packet(0, 512, 1);
    packet(0, 0, 2);
    packet(0, 0, 3);
    packet(0, 512, 4);
    brk();
    run();
    CHECK_EQ(s_got_n, 2);
    CHECK_EQ(times_got(512, 4), 1);
    CHECK_EQ(drv.dmx.size, 513);
    CHECK_EQ(s_seq_gaps, 0);
}

static void test_bad_slot_mid_frame(void)
{
    packet(0, 512, 1);
    brk();
    ev(EV_BYTE, 0);
    for (int i = 1; i <= 512; i++) ev(i == 100 ? EV_BAD_BYTE : EV_BYTE, slot(2, i));
    packet(0, 512, 3);
    brk();
    run();
    CHECK_EQ(s_got_n, 2);   /* the damaged frame is not forwarded */
    CHECK_EQ(times_got(512, 3), 1);
    CHECK_EQ(drv.dmx.size, 513);
}

static void test_rdm_between_short_frames(void)
{
    for (int i = 0; i < 8; i++) {
        packet(0, 24, i);
        if (i >= 3) {
            brk();
            uint8_t rdm[40] = { 0xCC, 0x01, 38 };
            rdm[20] = 0x20;     /* GET */
            for (int b = 0; b < 40; b++) ev(EV_BYTE, rdm[b]);
        }
    }
    brk();
    run();
    for (int i = 0; i < 8; i++) CHECK_EQ(times_got(24, i), 1);
    CHECK_EQ(s_got_n, 8);
    CHECK_EQ(drv.dmx.size, 25);
}

#undef TEST_SETUP
#define TEST_SETUP() reset()

int main(void)
{
    for (int v = 0; v < 4; v++) {
        s_err_first = v & 1;
        s_late = v & 2;
        printf("-- %s, task %s\n", s_err_first ? "error before break" : "error with break",
               s_late ? "wakes after the next break" : "wakes at once");
        RUN(test_full_frames);
        RUN(test_constant_short_frames);
        RUN(test_short_frames_of_varying_length);
        RUN(test_one_cut_frame);
        RUN(test_grows_back);
        RUN(test_repeating_mixed_lengths);
        RUN(test_stray_break_after_frame);
        RUN(test_other_start_codes);
        RUN(test_start_code_only);
        RUN(test_bad_slot_mid_frame);
        RUN(test_rdm_between_short_frames);
    }
    return test_summary();
}
