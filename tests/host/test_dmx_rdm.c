/* esp_dmx end to end: the real send and receive paths, timer and UART
 * interrupts, RDM controller and RDM responder, on a simulated clock and wire.
 * Each test runs in a child process, so an assert, a bad access or a lock-up
 * in the driver fails that test only. */
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sanitizer/allocator_interface.h>

#include "esp_dmx.h"
#include "dmx/hal/uart.c"
#include "dmx/hal/timer.c"
#include "dmx/include/driver.h"
#include "dmx/include/parameter.h"
#include "dmx/sniffer.h"
#include "rdm/controller.h"
#include "rdm/controller/include/utils.h"
#include "rdm/responder.h"
#include "rdm/responder/include/sensor_parameter.h"
#include "esp_mac.h"

/* defined in rdm_info.c, declared in no header */
size_t rdm_get_supported_parameters(dmx_port_t dmx_num, uint16_t *pids, size_t size);

#define P 1
#define D (dmx_driver[P])
#define SLOT_US 44

/* ---- simulated hardware ------------------------------------------------ */

uart_dev_t host_uart[SOC_UART_NUM];
int host_critical;

static int64_t T = 1000000;
static int64_t s_wake_lat, s_tim_lat, s_uart_hold_until, s_preempt;
static int s_task, s_locks, s_mutex_fail, s_reg_fail, s_negative_writes, s_blocked_in_critical;
static bool s_pending;
static uint32_t s_value;
static int s_rts_at_notify;

static struct {
    bool made, enabled, running, armed, reload, fired, dead;
    int64_t base, count, alarm, fire_at;
    bool (*cb)(gptimer_handle_t, const gptimer_alarm_event_data_t *, void *);
    void *arg;
} tim;

#define WIRE_MAX 2000
static struct { int64_t t; bool brk; uint8_t v; } s_wire[WIRE_MAX];
static int s_wire_n, s_wire_at;
static int64_t s_rx_end;

static uint8_t s_tx[1024];
static int s_tx_n, s_breaks;
static int64_t s_tx_busy, s_tx_first, s_tx_end, s_break_start, s_break_end;
static bool s_tx_active, s_tx_low_due;
static uint32_t s_inv;
static void (*s_fixture)(void);

static uart_dev_t *U(void) { return &host_uart[P]; }

static void track(void)
{
    if (U()->conf0.txd_inv == s_inv) return;
    s_inv = U()->conf0.txd_inv;
    if (s_inv) {
        s_break_start = T;
        s_breaks++;
        s_tx_n = 0;
    } else {
        s_break_end = T;
    }
}

int host_tx_room(uart_dev_t *d)
{
    (void)d;
    int64_t left = s_tx_busy > T ? (s_tx_busy - T + SLOT_US - 1) / SLOT_US : 0;
    return 128 - (int)left;
}

void host_tx_write(uart_dev_t *d, const uint8_t *b, int n)
{
    (void)d;
    track();
    if (n < 0 || s_tx_n + n > (int)sizeof(s_tx)) {
        s_negative_writes++;
        return;
    }
    if (n == 0) return;
    if (s_tx_n == 0) s_tx_first = T;
    memcpy(s_tx + s_tx_n, b, (size_t)n);
    s_tx_n += n;
    if (s_tx_busy < T) s_tx_busy = T;
    s_tx_busy += n * SLOT_US;
    s_tx_active = s_tx_low_due = true;
}

int gptimer_new_timer(const gptimer_config_t *c, gptimer_handle_t *h)
{
    (void)c;
    if (tim.made) return ESP_FAIL;
    memset(&tim, 0, sizeof(tim));
    tim.made = true;
    *h = &tim;
    return ESP_OK;
}
int gptimer_register_event_callbacks(gptimer_handle_t h, const gptimer_event_callbacks_t *c, void *a)
{
    (void)h;
    if (s_reg_fail) return ESP_FAIL;
    tim.cb = c->on_alarm;
    tim.arg = a;
    return ESP_OK;
}
int gptimer_enable(gptimer_handle_t h) { (void)h; tim.enabled = true; return ESP_OK; }
int gptimer_disable(gptimer_handle_t h)
{
    (void)h;
    if (!tim.enabled) return ESP_FAIL;
    tim.enabled = false;
    return ESP_OK;
}
int gptimer_del_timer(gptimer_handle_t h)
{
    (void)h;
    if (tim.enabled) return ESP_FAIL;
    tim.made = false;
    return ESP_OK;
}
int gptimer_start(gptimer_handle_t h)
{
    (void)h;
    if (!tim.running) tim.base = T - tim.count;
    tim.running = true;
    return ESP_OK;
}
int gptimer_stop(gptimer_handle_t h)
{
    (void)h;
    if (tim.running) tim.count = T - tim.base;
    tim.running = false;
    return ESP_OK;
}
int gptimer_set_raw_count(gptimer_handle_t h, uint64_t v)
{
    (void)h;
    tim.count = (int64_t)v;
    tim.base = T - (int64_t)v;
    return ESP_OK;
}
int gptimer_set_alarm_action(gptimer_handle_t h, const gptimer_alarm_config_t *c)
{
    (void)h;
    tim.alarm = (int64_t)c->alarm_count;
    tim.reload = c->flags.auto_reload_on_alarm;
    tim.armed = true;
    return ESP_OK;
}

static int64_t alarm_at(void)
{
    if (!tim.running || !tim.armed || tim.fired || tim.dead) return INT64_MAX;
    int64_t t = tim.base + tim.alarm;
    return t > T ? t : T;
}

static int64_t next_event(void)
{
    int64_t t = alarm_at();
    if (tim.fired && tim.fire_at < t) t = tim.fire_at;
    if (s_wire_at < s_wire_n && s_wire[s_wire_at].t < t) t = s_wire[s_wire_at].t;
    if (s_tx_low_due && s_tx_busy - 8 * SLOT_US < t) t = s_tx_busy - 8 * SLOT_US;
    if (s_tx_active && s_tx_busy < t) t = s_tx_busy;
    if (s_uart_hold_until > T && s_uart_hold_until < t) t = s_uart_hold_until;
    return t < T ? T : t;
}

/* everything due now: hardware first, then the interrupts */
static void pump(void)
{
    for (int guard = 0; guard < 10000; guard++) {
        bool did = false;
        track();
        if (alarm_at() <= T) {
            tim.fired = did = true;
            tim.fire_at = T + (U()->conf0.txd_inv ? s_tim_lat : 0);
            if (tim.reload) tim.base = T;
            else tim.armed = false;
        }
        while (s_wire_at < s_wire_n && s_wire[s_wire_at].t <= T) {
            if (U()->conf0.sw_rts) {   /* receiving */
                if (s_wire[s_wire_at].brk) {
                    U()->intr |= UART_INTR_BRK_DET;
                } else if (U()->fifo_len < (int)sizeof(U()->fifo)) {
                    U()->fifo[U()->fifo_len++] = s_wire[s_wire_at].v;
                    U()->intr |= UART_INTR_RXFIFO_FULL;
                }
            }
            s_wire_at++;
            did = true;
            break;   /* the interrupt runs for each byte */
        }
        if (s_tx_low_due && s_tx_busy - 8 * SLOT_US <= T) {
            s_tx_low_due = false;
            U()->intr |= UART_INTR_TXFIFO_EMPTY;
            did = true;
        }
        if (s_tx_active && s_tx_busy <= T) {
            s_tx_active = false;
            s_tx_end = T;
            U()->intr |= UART_INTR_TX_DONE;
            did = true;
        }
        if (D && T >= s_uart_hold_until && (U()->intr & U()->ena)) {
            bool was_sending = D->dmx.status == DMX_STATUS_SENDING;
            dmx_uart_isr(D);
            track();
            if (was_sending && D->dmx.status != DMX_STATUS_SENDING && s_fixture) s_fixture();
            did = true;
        }
        if (tim.fired && tim.fire_at <= T) {
            tim.fired = false;
            if (tim.cb && tim.enabled) tim.cb(&tim, NULL, tim.arg);
            track();
            did = true;
        }
        if (!did) return;
    }
    fprintf(stderr, "  interrupts never settled\n");
    abort();
}

static void run_until(int64_t end)
{
    for (;;) {
        pump();
        int64_t t = next_event();
        if (t > end) break;
        T = t;
    }
    if (end > T) T = end;
    pump();
}
static void run_for(int64_t us) { run_until(T + us); }

/* ---- FreeRTOS and the rest -------------------------------------------- */

int64_t esp_timer_get_time(void) { return T; }
int esp_efuse_mac_get_default(uint8_t *m)
{
    for (int i = 0; i < 6; i++) m[i] = (uint8_t)(0x10 + i);
    return 0;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void) { return &s_task; }

BaseType_t xTaskNotifyFromISR(TaskHandle_t t, uint32_t v, eNotifyAction a, int *woken)
{
    (void)t; (void)woken;
    s_pending = true;
    if (a == eSetValueWithOverwrite) s_value = v;
    else s_rts_at_notify = (int)U()->conf0.sw_rts;
    return pdTRUE;
}

BaseType_t xTaskNotifyStateClear(TaskHandle_t t) { (void)t; s_pending = false; return pdTRUE; }

BaseType_t xTaskNotifyWait(uint32_t ci, uint32_t co, uint32_t *v, TickType_t ticks)
{
    (void)ci; (void)co;
    if (host_critical) s_blocked_in_critical++;
    /* as FreeRTOS: a wait of n ticks ends at the nth tick from now, so it can
     * be nearly a tick short of n ms */
    int64_t end = ticks ? (T / 1000 + (ticks > 10000 ? 10000 : (int64_t)ticks)) * 1000 : T;
    while (!s_pending) {
        pump();
        if (s_pending) break;
        int64_t t = next_event();
        if (t > end) {
            T = end;
            return pdFALSE;
        }
        T = t;
    }
    s_pending = false;
    if (v) *v = s_value;
    run_for(s_wake_lat);
    return pdTRUE;
}

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return s_mutex_fail ? NULL : &s_locks; }
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t w)
{
    (void)w;
    assert(s);
    if (s_preempt) {
        T += s_preempt;
        s_preempt = 0;
    }
    s_locks++;
    return pdTRUE;
}
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s)
{
    assert(s);
    if (!s_locks) return pdFALSE;
    s_locks--;
    return pdTRUE;
}
void vSemaphoreDelete(SemaphoreHandle_t s) { assert(s); s_locks = 0; }
void vTaskSetTimeOutState(TimeOut_t *t) { t->at = T; }
BaseType_t xTaskCheckForTimeOut(TimeOut_t *t, TickType_t *w)
{
    if (*w == portMAX_DELAY) return pdFALSE;
    int64_t gone = T / 1000 - t->at / 1000;
    if (gone >= *w) {
        *w = 0;
        return pdTRUE;
    }
    *w -= (TickType_t)gone;
    t->at = T;
    return pdFALSE;
}

static struct { rdm_pid_t pid; size_t size; uint8_t d[80]; } s_nvs[16];
static int s_nvs_n, s_nvs_writes;
void dmx_nvs_init(dmx_port_t p) { (void)p; }
size_t dmx_nvs_get(dmx_port_t p, rdm_sub_device_t s, rdm_pid_t pid, void *d, size_t n)
{
    (void)p; (void)s;
    for (int i = 0; i < s_nvs_n; i++) {
        if (s_nvs[i].pid == pid && s_nvs[i].size <= n) {
            memcpy(d, s_nvs[i].d, s_nvs[i].size);
            return s_nvs[i].size;
        }
    }
    return 0;
}
bool dmx_nvs_set(dmx_port_t p, rdm_sub_device_t s, rdm_pid_t pid, const void *d, size_t n)
{
    (void)p; (void)s;
    int i = 0;
    while (i < s_nvs_n && s_nvs[i].pid != pid) i++;
    if (i == s_nvs_n) s_nvs_n++;
    s_nvs[i].pid = pid;
    s_nvs[i].size = n;
    memcpy(s_nvs[i].d, d, n);
    s_nvs_writes++;
    return true;
}
bool dmx_sniffer_is_enabled(dmx_port_t p) { (void)p; return false; }
bool dmx_sniffer_disable(dmx_port_t p) { (void)p; return true; }

/* ---- helpers ----------------------------------------------------------- */

#define FAIL(...) do {                                  \
        fprintf(stderr, "  %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__);                    \
        fputc('\n', stderr);                             \
        _exit(1);                                        \
    } while (0)
#define CHECK(c) do { if (!(c)) FAIL("CHECK(%s) failed", #c); } while (0)
#define CHECK_EQ(a, b) do {                              \
        long long a_ = (long long)(a), b_ = (long long)(b); \
        if (a_ != b_) FAIL("CHECK_EQ(%s, %s): %lld != %lld", #a, #b, a_, b_); \
    } while (0)

static const rdm_uid_t CONSOLE = {0x7A70, 0x01020304};
static const rdm_uid_t FIXTURE = {0x1234, 0x00000042};
static const rdm_uid_t OTHER = {0x1234, 0x00000099};

/* as dmx_handler.c's install_driver() */
static bool try_install(bool responder)
{
    dmx_config_t c = DMX_CONFIG_DEFAULT;
    c.product_category = RDM_PRODUCT_CATEGORY_DATA;
    c.software_version_label = "ESP-NODE-2RDM";
    dmx_personality_t ps[] = {{512, "Full universe"}};
    return dmx_driver_install(P, &c, responder ? ps : NULL, responder ? 1 : 0);
}
static void install(bool responder)
{
    CHECK(try_install(responder));
    T += 10000;
}

static rdm_uid_t me(void) { return *rdm_uid_get(P); }

static void put_uid(uint8_t *d, rdm_uid_t u)
{
    d[0] = u.man_id >> 8; d[1] = (uint8_t)u.man_id;
    d[2] = u.dev_id >> 24; d[3] = u.dev_id >> 16; d[4] = u.dev_id >> 8; d[5] = (uint8_t)u.dev_id;
}

static int packet(uint8_t *b, rdm_uid_t to, rdm_uid_t from, uint8_t tn, uint8_t type, uint16_t sub,
                  uint8_t cc, uint16_t pid, const void *pd, int pdl)
{
    b[0] = 0xCC; b[1] = 0x01; b[2] = (uint8_t)(24 + pdl);
    put_uid(b + 3, to);
    put_uid(b + 9, from);
    b[15] = tn; b[16] = type; b[17] = 0;
    b[18] = sub >> 8; b[19] = (uint8_t)sub; b[20] = cc;
    b[21] = pid >> 8; b[22] = (uint8_t)pid; b[23] = (uint8_t)pdl;
    if (pdl) memcpy(b + 24, pd, (size_t)pdl);
    uint16_t sum = 0;
    for (int i = 0; i < 24 + pdl; i++) sum += b[i];
    b[24 + pdl] = sum >> 8; b[25 + pdl] = (uint8_t)sum;
    return 26 + pdl;
}

static void on_wire(int64_t at, const uint8_t *b, int n, bool brk)
{
    if (s_wire_at == s_wire_n) s_wire_at = s_wire_n = 0;
    CHECK(s_wire_n + n + 1 <= WIRE_MAX);
    if (brk) {
        at += 176;
        s_wire[s_wire_n].t = at;
        s_wire[s_wire_n].brk = true;
        s_wire[s_wire_n++].v = 0;
        at += 12;
    }
    for (int i = 0; i < n; i++) {
        at += SLOT_US;
        s_wire[s_wire_n].t = at;
        s_wire[s_wire_n].brk = false;
        s_wire[s_wire_n++].v = b[i];
    }
    s_rx_end = at;
}

static bool sum_ok(const uint8_t *b, int n)
{
    if (n < 26 || n != b[2] + 2) return false;
    uint16_t sum = 0;
    for (int i = 0; i < b[2]; i++) sum += b[i];
    return sum == ((b[n - 2] << 8) | b[n - 1]);
}

/* as input_task(): an empty return just means go round again */
static size_t receive(dmx_packet_t *pkt)
{
    size_t n = 0;
    for (int i = 0; i < 2 && n == 0; i++) n = dmx_receive(P, pkt, 200);
    return n;
}

/* a console's packet arrives; returns what the node sent back */
static int serve(const uint8_t *req, int len)
{
    run_for(3000);
    on_wire(T + 50, req, len, true);
    s_tx_n = 0;
    dmx_packet_t pkt;
    if (receive(&pkt) && pkt.is_rdm && pkt.err == DMX_OK) rdm_send_response(P);
    run_for(100);
    return s_tx_n;
}

static int ask_to(rdm_uid_t to, uint8_t cc, uint16_t pid, uint16_t sub, const void *pd, int pdl)
{
    uint8_t b[300];
    return serve(b, packet(b, to, CONSOLE, 7, 1, sub, cc, pid, pd, pdl));
}
static int ask(uint8_t cc, uint16_t pid, uint16_t sub, const void *pd, int pdl)
{
    return ask_to(me(), cc, pid, sub, pd, pdl);
}

static void want_ack(int n, uint16_t pid, int pdl)
{
    if (!sum_ok(s_tx, n)) FAIL("no valid reply to PID %04x: %d bytes %02x %02x %02x", pid, n, s_tx[0], s_tx[1], s_tx[2]);
    CHECK_EQ(s_tx[16], RDM_RESPONSE_TYPE_ACK);
    CHECK_EQ((s_tx[21] << 8) | s_tx[22], pid);
    CHECK_EQ(s_tx[23], pdl);
}
static void want_nack(int n, int reason)
{
    CHECK(sum_ok(s_tx, n));
    CHECK_EQ(s_tx[16], RDM_RESPONSE_TYPE_NACK_REASON);
    CHECK_EQ(s_tx[23], 2);
    CHECK_EQ((s_tx[24] << 8) | s_tx[25], reason);
}

#define GET RDM_CC_GET_COMMAND
#define SET RDM_CC_SET_COMMAND
#define DISC RDM_CC_DISC_COMMAND
#define ROOT RDM_SUB_DEVICE_ROOT

static uint16_t start_address(void)
{
    uint16_t a = 0;
    CHECK(rdm_get_dmx_start_address(P, &a));
    return a;
}

static bool boot_loader_flag(void)
{
    int n = ask(DISC, RDM_PID_DISC_UN_MUTE, ROOT, NULL, 0);
    CHECK(sum_ok(s_tx, n));
    return (s_tx[25] & 0x04) != 0;
}

/* the fixture at the other end of a controller port */
static struct {
    bool silent, dub, collide;
    int delay, pdl, len_override;
    uint8_t type, cc, pd[232];
    uint16_t pid;
    rdm_uid_t src, dest;
    bool set_src, set_dest;
    int tn_off;
} fx;
static int s_fx_at;

static void fixture(void)
{
    if (fx.silent || s_tx_n < 26 || s_tx[0] != 0xCC || s_breaks == s_fx_at) return;
    s_fx_at = s_breaks;
    uint8_t b[300];
    int64_t at = T + (fx.delay ? fx.delay : 200);
    if (s_tx[20] == DISC && s_tx[22] == RDM_PID_DISC_UNIQUE_BRANCH) {
        if (!fx.dub) return;
        uint8_t u[6];
        put_uid(u, fx.set_src ? fx.src : FIXTURE);
        memset(b, RDM_PREAMBLE, 7);
        b[7] = RDM_DELIMITER;
        uint16_t sum = 0;
        for (int i = 0; i < 6; i++) {
            b[8 + 2 * i] = u[i] | 0xAA;
            b[9 + 2 * i] = u[i] | 0x55;
            sum += b[8 + 2 * i] + b[9 + 2 * i];
        }
        b[20] = (sum >> 8) | 0xAA; b[21] = (sum >> 8) | 0x55;
        b[22] = (sum & 0xFF) | 0xAA; b[23] = (sum & 0xFF) | 0x55;
        if (fx.collide) {
            for (int i = 10; i < 24; i++) b[i] ^= 0x5A;
        }
        on_wire(at, b, 24, false);
        return;
    }
    if (s_tx[5] == 0xFF && s_tx[6] == 0xFF && s_tx[7] == 0xFF && s_tx[8] == 0xFF) return;
    rdm_uid_t to = {(uint16_t)(s_tx[9] << 8 | s_tx[10]),
                    (uint32_t)s_tx[11] << 24 | (uint32_t)s_tx[12] << 16 | s_tx[13] << 8 | s_tx[14]};
    rdm_uid_t from = {(uint16_t)(s_tx[3] << 8 | s_tx[4]),
                      (uint32_t)s_tx[5] << 24 | (uint32_t)s_tx[6] << 16 | s_tx[7] << 8 | s_tx[8]};
    int n = packet(b, fx.set_dest ? fx.dest : to, fx.set_src ? fx.src : from,
                   (uint8_t)(s_tx[15] + fx.tn_off), fx.type, (uint16_t)(s_tx[18] << 8 | s_tx[19]),
                   fx.cc ? fx.cc : (uint8_t)(s_tx[20] + 1),
                   fx.pid ? fx.pid : (uint16_t)(s_tx[21] << 8 | s_tx[22]), fx.pd, fx.pdl);
    if (fx.len_override) n = fx.len_override;
    on_wire(at, b, n, true);
}

static void controller(void)
{
    install(false);
    s_fixture = fixture;
}

static size_t get(uint16_t pid, void *pd, size_t size, rdm_ack_t *ack)
{
    const rdm_request_t r = {.dest_uid = &FIXTURE, .sub_device = ROOT, .cc = GET, .pid = pid};
    return rdm_send_request(P, &r, "b", pd, size, ack);
}

static void dmx_frame(void)
{
    uint8_t f[513] = {0};
    dmx_write(P, f, sizeof(f));
    CHECK(dmx_send_num(P, sizeof(f)) > 0);
}

/* ---- responder --------------------------------------------------------- */

static void test_resp_device_info(void)
{
    install(true);
    int n = ask(GET, RDM_PID_DEVICE_INFO, ROOT, NULL, 0);
    want_ack(n, RDM_PID_DEVICE_INFO, 19);
    CHECK(s_break_start - s_rx_end >= 176);
    CHECK(s_break_start - s_rx_end <= 2000);
}

static void test_resp_queued_message(void)   /* esp_dmx #229, grandMA3 */
{
    install(true);
    uint8_t st = RDM_STATUS_ADVISORY;
    int n = ask(GET, RDM_PID_QUEUED_MESSAGE, ROOT, &st, 1);
    want_ack(n, RDM_PID_STATUS_MESSAGE, 0);
    n = ask(GET, RDM_PID_QUEUED_MESSAGE, ROOT, NULL, 0);
    want_nack(n, RDM_NR_FORMAT_ERROR);
}

static void test_resp_queue_last_message(void)
{
    install(true);
    uint8_t any = RDM_STATUS_ERROR, last = RDM_STATUS_GET_LAST_MESSAGE;
    CHECK(rdm_set_device_label(P, "first", 6));
    CHECK(rdm_set_dmx_start_address(P, 1));
    want_ack(ask(GET, RDM_PID_QUEUED_MESSAGE, ROOT, &any, 1), RDM_PID_DEVICE_LABEL, 5);
    want_ack(ask(GET, RDM_PID_QUEUED_MESSAGE, ROOT, &last, 1), RDM_PID_DEVICE_LABEL, 5);
    want_ack(ask(GET, RDM_PID_QUEUED_MESSAGE, ROOT, &any, 1), RDM_PID_DMX_START_ADDRESS, 2);
}

static void test_resp_queue_wrap(void)
{
    install(true);
    for (int i = 0; i < 32; i++) {
        CHECK(rdm_queue_push(P, (rdm_pid_t)(0x8000 + i % 3)));
        if (i < 31) rdm_queue_pop(P);
    }
    for (int i = 0; i < 70; i++) {
        rdm_queue_push(P, (rdm_pid_t)(0x9000 + i));
        if (i & 1) rdm_queue_pop(P);
    }
}

static void test_resp_sub_devices(void)      /* esp_dmx #163 */
{
    install(true);
    uint8_t on = 1;
    uint16_t subs[] = {3, 512, 513, 600, 0xFFFE, 0xFFFF};
    for (size_t i = 0; i < sizeof(subs) / sizeof(subs[0]); i++) {
        want_nack(ask(GET, RDM_PID_DEVICE_INFO, subs[i], NULL, 0), RDM_NR_SUB_DEVICE_OUT_OF_RANGE);
        CHECK_EQ((s_tx[18] << 8) | s_tx[19], subs[i]);
        want_nack(ask(SET, RDM_PID_IDENTIFY_DEVICE, subs[i], &on, 1), RDM_NR_SUB_DEVICE_OUT_OF_RANGE);
        CHECK_EQ(ask_to(RDM_UID_BROADCAST_ALL, SET, RDM_PID_IDENTIFY_DEVICE, subs[i], &on, 1), 0);
        CHECK_EQ(ask(DISC, RDM_PID_DISC_MUTE, subs[i], NULL, 0), 0);
    }
    bool ident = true;
    CHECK(rdm_get_identify_device(P, &ident));
    CHECK(!ident);
    CHECK(!boot_loader_flag());
}

static void test_resp_pid_zero(void)
{
    install(true);
    want_nack(ask(GET, 0, ROOT, NULL, 0), RDM_NR_UNKNOWN_PID);
    want_nack(ask(SET, 0, ROOT, NULL, 0), RDM_NR_UNKNOWN_PID);
}

static void test_resp_ignores_non_requests(void)
{
    install(true);
    uint8_t cc[] = {0x00, 0x01, 0x11, 0x21, 0x31, 0x40, 0x41, 0x50, 0xF0};
    for (size_t i = 0; i < sizeof(cc); i++) {
        CHECK_EQ(ask(cc[i], RDM_PID_DEVICE_INFO, ROOT, NULL, 0), 0);
        CHECK_EQ(ask_to(RDM_UID_BROADCAST_ALL, cc[i], RDM_PID_IDENTIFY_DEVICE, ROOT, NULL, 0), 0);
    }
}

static void test_resp_command_classes(void)
{
    install(true);
    CHECK_EQ(ask(DISC, RDM_PID_DEVICE_INFO, ROOT, NULL, 0), 0);      /* no NACK to discovery */
    CHECK_EQ(ask(DISC, 0x8123, ROOT, NULL, 0), 0);
    CHECK_EQ(ask(DISC, RDM_PID_DISC_UNIQUE_BRANCH, ROOT, NULL, 0), 0);
    CHECK(!boot_loader_flag());
    want_nack(ask(GET, RDM_PID_DISC_MUTE, ROOT, NULL, 0), RDM_NR_UNSUPPORTED_COMMAND_CLASS);
    CHECK_EQ(ask_to(RDM_UID_BROADCAST_ALL, GET, RDM_PID_DISC_UNIQUE_BRANCH, ROOT, NULL, 0), 0);
}

static void test_resp_discovery(void)
{
    install(true);
    uint8_t range[12];
    put_uid(range, (rdm_uid_t){0, 0});
    put_uid(range + 6, RDM_UID_MAX);
    int n = ask_to(RDM_UID_BROADCAST_ALL, DISC, RDM_PID_DISC_UNIQUE_BRANCH, ROOT, range, 12);
    CHECK_EQ(n, 24);
    CHECK_EQ(s_tx[0], RDM_PREAMBLE);
    CHECK_EQ(ask_to(RDM_UID_BROADCAST_ALL, DISC, RDM_PID_DISC_UNIQUE_BRANCH, ROOT, range, 3), 0);
    want_ack(ask(DISC, RDM_PID_DISC_MUTE, ROOT, NULL, 0), RDM_PID_DISC_MUTE, 2);
    CHECK_EQ(s_tx[24] | s_tx[25], 0);          /* no flags, reserved bits clear */
    CHECK_EQ(ask_to(RDM_UID_BROADCAST_ALL, DISC, RDM_PID_DISC_UNIQUE_BRANCH, ROOT, range, 12), 0);
}

static void test_resp_length_mismatch(void)
{
    install(true);
    uint8_t b[300], pd[4] = {0};
    int n = packet(b, me(), CONSOLE, 7, 1, ROOT, SET, RDM_PID_DEVICE_LABEL, pd, 4);
    b[23] = 200;                               /* claims more data than it carries */
    uint16_t sum = 0;
    for (int i = 0; i < n - 2; i++) sum += b[i];
    b[n - 2] = sum >> 8; b[n - 1] = (uint8_t)sum;
    want_nack(serve(b, n), RDM_NR_FORMAT_ERROR);
}

static void test_resp_personality_description(void)  /* esp_dmx #224 */
{
    install(true);
    uint8_t one = 1;
    static const uint8_t want[] = "\x01\x02\x00" "Full universe";
    want_ack(ask(GET, RDM_PID_DMX_PERSONALITY_DESCRIPTION, ROOT, &one, 1),
             RDM_PID_DMX_PERSONALITY_DESCRIPTION, sizeof(want) - 1);
    CHECK(!memcmp(s_tx + 24, want, sizeof(want) - 1));
}

static void test_resp_set_validation(void)
{
    install(true);
    uint8_t v[4] = {0, 0, 0, 0};
    want_nack(ask(SET, RDM_PID_DMX_START_ADDRESS, ROOT, v, 2), RDM_NR_DATA_OUT_OF_RANGE);
    v[0] = 0x02; v[1] = 0x01;                  /* 513 */
    want_nack(ask(SET, RDM_PID_DMX_START_ADDRESS, ROOT, v, 2), RDM_NR_DATA_OUT_OF_RANGE);
    want_nack(ask(SET, RDM_PID_DMX_START_ADDRESS, ROOT, v, 1), RDM_NR_FORMAT_ERROR);
    want_nack(ask(SET, RDM_PID_DMX_START_ADDRESS, ROOT, NULL, 0), RDM_NR_FORMAT_ERROR);
    v[0] = 2;
    want_nack(ask(SET, RDM_PID_IDENTIFY_DEVICE, ROOT, v, 1), RDM_NR_DATA_OUT_OF_RANGE);
    CHECK_EQ(start_address(), 1);
    v[0] = 0; v[1] = 9;
    want_ack(ask(SET, RDM_PID_DMX_START_ADDRESS, ROOT, v, 2), RDM_PID_DMX_START_ADDRESS, 0);
    CHECK_EQ(start_address(), 9);
}

static void test_resp_label(void)
{
    install(true);
    char big[40];
    memset(big, 'x', sizeof(big));
    want_ack(ask(SET, RDM_PID_DEVICE_LABEL, ROOT, "Stage left long", 15), RDM_PID_DEVICE_LABEL, 0);
    want_ack(ask(SET, RDM_PID_DEVICE_LABEL, ROOT, "abc", 3), RDM_PID_DEVICE_LABEL, 0);
    want_ack(ask(GET, RDM_PID_DEVICE_LABEL, ROOT, NULL, 0), RDM_PID_DEVICE_LABEL, 3);
    want_ack(ask(SET, RDM_PID_DEVICE_LABEL, ROOT, NULL, 0), RDM_PID_DEVICE_LABEL, 0);
    want_ack(ask(GET, RDM_PID_DEVICE_LABEL, ROOT, NULL, 0), RDM_PID_DEVICE_LABEL, 0);
    want_ack(ask(SET, RDM_PID_DEVICE_LABEL, ROOT, big, 32), RDM_PID_DEVICE_LABEL, 0);
    want_ack(ask(GET, RDM_PID_DEVICE_LABEL, ROOT, NULL, 0), RDM_PID_DEVICE_LABEL, 32);
    want_nack(ask(SET, RDM_PID_DEVICE_LABEL, ROOT, big, 40), RDM_NR_FORMAT_ERROR);
}

static void test_resp_label_survives_restart(void)
{
    install(true);
    ask(SET, RDM_PID_DEVICE_LABEL, ROOT, "House", 5);
    while (dmx_parameter_commit(P)) {}
    s_locks = 0;                               /* not what is tested here */
    CHECK(dmx_driver_delete(P));
    install(true);
    char got[33] = {0};
    rdm_get_device_label(P, got, sizeof(got));
    CHECK(!strcmp(got, "House"));
}

static void test_resp_saved_on_delete(void)
{
    install(true);
    ask(SET, RDM_PID_DEVICE_LABEL, ROOT, "Truss", 5);
    CHECK(dmx_driver_delete(P));
    CHECK_EQ(s_nvs_writes, 1);
}

static void test_resp_identify_not_saved(void)
{
    install(true);
    uint8_t on = 1;
    ask(SET, RDM_PID_IDENTIFY_DEVICE, ROOT, &on, 1);
    while (dmx_parameter_commit(P)) {}
    CHECK_EQ(s_nvs_writes, 0);
}

static void test_resp_releases_lock(void)
{
    install(true);
    for (int i = 0; i < 3; i++) {
        want_ack(ask(GET, RDM_PID_DEVICE_INFO, ROOT, NULL, 0), RDM_PID_DEVICE_INFO, 19);
        CHECK_EQ(s_locks, 0);
    }
    ask_to(RDM_UID_BROADCAST_ALL, DISC, RDM_PID_DISC_UN_MUTE, ROOT, NULL, 0);
    CHECK_EQ(s_locks, 0);
}

static void test_resp_keeps_frame_size(void)
{
    install(true);
    ask(GET, RDM_PID_DEVICE_INFO, ROOT, NULL, 0);
    uint8_t f[513];
    memset(f, 0x33, sizeof(f));
    f[0] = 0;
    uint32_t seq0 = D->dmx.rx_frame_seq;
    on_wire(T + 200, f, sizeof(f), true);
    on_wire(s_rx_end + 100, f, 1, true);       /* the break that follows */
    dmx_packet_t pkt;
    CHECK_EQ(receive(&pkt), 513);
    run_for(2000);
    CHECK_EQ(D->dmx.rx_frame_seq - seq0, 1);
}

static void test_resp_next_receive_waits(void)
{
    install(true);
    ask(GET, RDM_PID_DEVICE_INFO, ROOT, NULL, 0);
    dmx_packet_t pkt;
    dmx_receive(P, &pkt, 50);
    CHECK_EQ(pkt.err, DMX_ERR_TIMEOUT);        /* not an empty packet marked OK */
}

static void test_resp_late_is_not_boot_loader(void)
{
    install(true);
    uint8_t b[300];
    int n = packet(b, me(), CONSOLE, 7, 1, ROOT, GET, RDM_PID_DEVICE_INFO, NULL, 0);
    on_wire(T + 50, b, n, true);
    dmx_packet_t pkt;
    CHECK(dmx_receive(P, &pkt, 200) > 0);
    T += 4000;                                 /* the task was held up */
    s_tx_n = 0;
    rdm_send_response(P);
    CHECK_EQ(s_tx_n, 0);
    CHECK(!boot_loader_flag());
}

static void test_resp_supported_parameters(void)
{
    install(true);
    for (int i = 0; i < 64; i++) dmx_parameter_add(P, ROOT, (rdm_pid_t)(0x8000 + i), DMX_PARAMETER_TYPE_NULL, NULL, 0);
    int n = ask(GET, RDM_PID_SUPPORTED_PARAMETERS, ROOT, NULL, 0);
    CHECK(sum_ok(s_tx, n));
    CHECK_EQ(dmx_parameter_at(P, ROOT, 32), 0);

    uint16_t pids[40];
    size_t got = rdm_get_supported_parameters(P, pids, sizeof(pids)) / 2;
    CHECK(got >= 20 && got <= 32);
    for (size_t i = 1; i < got; i++) CHECK(pids[i] != pids[i - 1]);
}

static void test_resp_sensors(void)
{
    install(true);
    CHECK(rdm_register_sensor_value(P, 2, NULL, NULL));
    CHECK(rdm_register_sensor_definition(P, NULL, NULL));
    CHECK(rdm_register_record_sensors(P, NULL, NULL));
    uint8_t num = 2, all = 0xFF;
    want_nack(ask(GET, RDM_PID_SENSOR_VALUE, ROOT, &num, 1), RDM_NR_DATA_OUT_OF_RANGE);
    want_nack(ask(GET, RDM_PID_SENSOR_DEFINITION, ROOT, &num, 1), RDM_NR_DATA_OUT_OF_RANGE);
    want_ack(ask(SET, RDM_PID_RECORD_SENSORS, ROOT, &all, 1), RDM_PID_RECORD_SENSORS, 0);
    CHECK(rdm_sensor_reset(P, ROOT, 0xFF));
    rdm_sensor_definition_t def = {.num = 1};
    memcpy(def.description, "Board", 6);
    CHECK(rdm_sensor_definition_add(P, ROOT, &def));
    num = 1;
    CHECK(sum_ok(s_tx, ask(GET, RDM_PID_SENSOR_DEFINITION, ROOT, &num, 1)));
    CHECK_EQ(s_tx[16], RDM_RESPONSE_TYPE_ACK);
}

static void test_resp_language(void)
{
    install(true);
    CHECK(rdm_register_language(P, "en", NULL, NULL));
    CHECK(rdm_set_language(P, "de"));
    char got[3];
    CHECK(rdm_get_language(P, got));
    CHECK(!strcmp(got, "de"));
}

/* no request may crash the node, hang it or leave the driver locked */
static void test_resp_any_request(void)
{
    install(true);
    static const uint16_t pids[] = {0, 0x0001, 0x0002, 0x0003, 0x0020, 0x0030, 0x0050, 0x0051, 0x0060,
                                    0x0081, 0x0082, 0x00B0, 0x00C0, 0x00E0, 0x00E1, 0x00F0, 0x1000,
                                    0x8000, 0xFFFF};
    static const uint8_t ccs[] = {0x00, 0x10, 0x11, 0x20, 0x21, 0x30, 0x31, 0x40, 0xFF};
    static const uint16_t subs[] = {0, 1, 512, 513, 0xFFFF};
    static const int pdls[] = {0, 1, 2, 3, 12, 33, 229, 230, 231};
    static const uint8_t fill[] = {0x00, 0x01, 0xFF};
    int cases = 0, replies = 0;
    for (size_t a = 0; a < sizeof(pids) / 2; a++)
    for (size_t c = 0; c < sizeof(ccs); c++)
    for (size_t s = 0; s < sizeof(subs) / 2; s++)
    for (size_t l = 0; l < sizeof(pdls) / sizeof(int); l++)
    for (size_t f = 0; f < sizeof(fill); f++)
    for (int all = 0; all < 2; all++) {
        uint8_t b[300], pd[232];
        memset(pd, fill[f], sizeof(pd));
        int n = packet(b, all ? RDM_UID_BROADCAST_ALL : me(), CONSOLE, 1, 1, subs[s], ccs[c], pids[a],
                       pd, pdls[l]);
        memcpy(D->dmx.data, b, (size_t)n);
        D->dmx.controller_eop_timestamp = T - 400;
        s_tx_n = 0;
        rdm_send_response(P);
        run_for(13000);
        cases++;
        replies += s_tx_n > 0;
        if (s_tx_n && !(all && ccs[c] == DISC)) {
            if (!sum_ok(s_tx, s_tx_n) || all)
                FAIL("bad reply: pid %04x cc %02x sub %u pdl %d all %d", pids[a], ccs[c], subs[s], pdls[l], all);
        }
        if (s_locks)
            FAIL("lock left held: pid %04x cc %02x sub %u pdl %d", pids[a], ccs[c], subs[s], pdls[l]);
    }
    printf("     %d requests, %d answered\n", cases, replies);
    fflush(stdout);
    CHECK(replies > 5000);                     /* it really was asking */
}

/* ---- controller -------------------------------------------------------- */

static void test_ctrl_get(void)
{
    controller();
    fx.pdl = 3; memcpy(fx.pd, "\x07\x08\x09", 3);
    uint8_t pd[8] = {0};
    rdm_ack_t ack;
    CHECK_EQ(get(0x8000, pd, sizeof(pd), &ack), 3);
    CHECK_EQ(ack.type, RDM_RESPONSE_TYPE_ACK);
    CHECK(!memcmp(pd, "\x07\x08\x09", 3));
    CHECK_EQ(s_locks, 0);
}

static void test_ctrl_pd_without_ack(void)
{
    controller();
    fx.pdl = 2; fx.pd[0] = 0x55; fx.pd[1] = 0x66;
    uint8_t pd[4] = {0};
    CHECK_EQ(get(0x8000, pd, sizeof(pd), NULL), 2);
    CHECK_EQ(pd[0], 0x55);
}

static void test_ctrl_gap_after_reply(void)  /* esp_dmx #177 */
{
    controller();
    rdm_ack_t ack;
    uint8_t pd[8];
    for (int kind = 0; kind < 3; kind++) {
        if (kind == 0) get(0x8000, pd, sizeof(pd), &ack);
        else rdm_send_disc_mute(P, &FIXTURE, NULL, &ack);
        CHECK_EQ(ack.type, RDM_RESPONSE_TYPE_ACK);
        int64_t end = s_rx_end;
        s_fixture = NULL;
        if (kind == 2) dmx_frame();
        else get(0x8000, pd, sizeof(pd), &ack);
        run_for(25000);
        s_fixture = fixture;
        if (s_break_start - end < 176) FAIL("only %lld us after the reply", (long long)(s_break_start - end));
    }
}

static void test_ctrl_gap_lost_reply(void)
{
    controller();
    fx.silent = true;
    rdm_ack_t ack;
    uint8_t pd[8];
    get(0x8000, pd, sizeof(pd), &ack);
    CHECK_EQ(ack.type, RDM_RESPONSE_TYPE_NONE);
    int64_t end = s_tx_end;
    dmx_frame();
    track();
    CHECK(s_break_start - end >= 3000);
}

static void test_ctrl_gap_broadcast(void)
{
    controller();
    rdm_send_disc_un_mute(P, &RDM_UID_BROADCAST_ALL, NULL, NULL);
    int64_t end = s_tx_end;
    dmx_frame();
    track();
    CHECK(s_break_start - end >= 176);
    CHECK(s_break_start - end < 400);
}

static void test_ctrl_gap_discovery(void)
{
    controller();
    rdm_disc_unique_branch_t br = {{0, 0}, RDM_UID_MAX};
    for (int kind = 0; kind < 3; kind++) {     /* nobody, one fixture, a collision */
        fx.dub = kind > 0;
        fx.collide = kind == 2;
        rdm_ack_t ack;
        run_for(8000);
        rdm_send_disc_unique_branch(P, &br, &ack);
        if (kind == 0) CHECK_EQ(ack.type, RDM_RESPONSE_TYPE_NONE);
        if (kind == 1) {
            CHECK_EQ(ack.type, RDM_RESPONSE_TYPE_ACK);
            CHECK(ack.src_uid.man_id == FIXTURE.man_id && ack.src_uid.dev_id == FIXTURE.dev_id);
        }
        if (kind == 2) CHECK_EQ(ack.type, RDM_RESPONSE_TYPE_INVALID);
        int64_t end = s_tx_end;
        dmx_frame();
        track();
        if (s_break_start - end < 5800) FAIL("case %d: %lld us after the search", kind, (long long)(s_break_start - end));
    }
}

static void test_ctrl_reply_matching(void)
{
    static const struct { int what; int want; } cases[] = {
        {0, RDM_RESPONSE_TYPE_ACK}, {1, RDM_RESPONSE_TYPE_INVALID}, {2, RDM_RESPONSE_TYPE_INVALID},
        {3, RDM_RESPONSE_TYPE_INVALID}, {4, RDM_RESPONSE_TYPE_INVALID}, {5, RDM_RESPONSE_TYPE_ACK},
        {6, RDM_RESPONSE_TYPE_INVALID}, {7, RDM_RESPONSE_TYPE_INVALID},
    };
    controller();
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        memset(&fx, 0, sizeof(fx));
        fx.pdl = 1; fx.pd[0] = 0x99;
        switch (cases[i].what) {
        case 1: fx.set_src = true; fx.src = OTHER; break;
        case 2: fx.set_dest = true; fx.dest = OTHER; break;
        case 3: fx.cc = RDM_CC_SET_COMMAND_RESPONSE; break;
        case 4: fx.pid = 0x8001; break;
        case 5: fx.tn_off = 3; break;          /* only logged */
        case 6: fx.len_override = 2; break;    /* the rest of the buffer is our own request */
        case 7: fx.cc = GET; break;
        }
        run_for(5000);
        uint8_t pd[4] = {0};
        rdm_ack_t ack;
        get(0x8000, pd, sizeof(pd), &ack);
        if ((int)ack.type != cases[i].want) FAIL("case %d: type %d", cases[i].what, (int)ack.type);
        if (cases[i].want != RDM_RESPONSE_TYPE_ACK) CHECK_EQ(pd[0], 0);
    }
    memset(&fx, 0, sizeof(fx));                /* a queued message names its own PID */
    fx.pid = RDM_PID_DEVICE_LABEL;
    uint8_t st = RDM_STATUS_ERROR;
    const rdm_request_t q = {.dest_uid = &FIXTURE, .cc = GET, .pid = RDM_PID_QUEUED_MESSAGE,
                             .format = "b$", .pd = &st, .pdl = 1};
    rdm_ack_t ack;
    run_for(5000);
    rdm_send_request(P, &q, NULL, NULL, 0, &ack);
    CHECK_EQ(ack.type, RDM_RESPONSE_TYPE_ACK);
}

static void test_ctrl_ack_never_stale(void)
{
    controller();
    dmx_frame();
    tim.dead = true;                           /* the frame can't finish yet */
    rdm_ack_t ack;
    memset(&ack, 0, sizeof(ack));
    uint8_t pd[4];
    CHECK_EQ(get(0x8000, pd, sizeof(pd), &ack), 0);
    CHECK_EQ(ack.type, RDM_RESPONSE_TYPE_NONE);
    CHECK_EQ(s_locks, 0);
}

static void test_ctrl_ack_timer_units(void)
{
    controller();
    fx.type = RDM_RESPONSE_TYPE_ACK_TIMER;
    fx.pdl = 2; fx.pd[1] = 3;
    rdm_ack_t ack;
    uint8_t pd[4];
    get(0x8000, pd, sizeof(pd), &ack);
    CHECK_EQ(ack.type, RDM_RESPONSE_TYPE_ACK_TIMER);
    CHECK_EQ(ack.timer, 300);
    fx.pd[0] = fx.pd[1] = 0xFF;
    run_for(5000);
    get(0x8000, pd, sizeof(pd), &ack);
    CHECK_EQ(ack.timer, 6553500);
}

static void test_ctrl_malformed_reason(void)
{
    controller();
    static const uint8_t types[] = {RDM_RESPONSE_TYPE_NACK_REASON, RDM_RESPONSE_TYPE_ACK_TIMER};
    for (int t = 0; t < 2; t++) {
        for (int pdl = 0; pdl < 4; pdl++) {
            fx.type = types[t];
            fx.pdl = pdl;
            run_for(5000);
            rdm_ack_t ack;
            uint8_t pd[4];
            get(0x8000, pd, sizeof(pd), &ack);
            if (pdl != 2) CHECK_EQ(ack.type, RDM_RESPONSE_TYPE_INVALID);
        }
    }
}

static void test_ctrl_largest_packets(void)
{
    controller();
    uint8_t big[231], got[240];
    for (int i = 0; i < 231; i++) big[i] = (uint8_t)i;
    const rdm_request_t set = {.dest_uid = &FIXTURE, .cc = SET, .pid = 0x8000,
                               .format = "b", .pd = big, .pdl = 231};
    rdm_ack_t ack;
    rdm_send_request(P, &set, NULL, NULL, 0, &ack);
    CHECK_EQ(ack.type, RDM_RESPONSE_TYPE_ACK);
    CHECK_EQ(s_tx_n, 257);
    CHECK(sum_ok(s_tx, s_tx_n));
    CHECK(!memcmp(s_tx + 24, big, 231));
    fx.pdl = 231; memcpy(fx.pd, big, 231);
    run_for(5000);
    CHECK_EQ(get(0x8000, got, sizeof(got), &ack), 231);
    CHECK(!memcmp(got, big, 231));
}

static void test_ctrl_late_task(void)
{
    controller();
    fx.pdl = 100; fx.pd[0] = 0x42;
    s_wake_lat = 3500;                         /* wakes while the reply is arriving */
    uint8_t pd[128];
    rdm_ack_t ack;
    CHECK_EQ(get(0x8000, pd, sizeof(pd), &ack), 100);
    CHECK_EQ(ack.type, RDM_RESPONSE_TYPE_ACK);
}

static void test_ctrl_turns_round_before_waking(void)
{
    controller();
    rdm_ack_t ack;
    uint8_t pd[4];
    s_rts_at_notify = -1;
    get(0x8000, pd, sizeof(pd), &ack);
    CHECK_EQ(s_rts_at_notify, 1);
}

/* ---- sending, waiting, set-up ------------------------------------------- */

static void test_mab_late_interrupt(void)
{
    install(false);
    static const int lat[] = {0, 4, 8, 12, 20, 50, 150};
    for (size_t i = 0; i < sizeof(lat) / sizeof(lat[0]); i++) {
        run_for(30000);
        s_tim_lat = lat[i];
        dmx_frame();
        run_for(30000);
        int64_t mab = s_tx_first - s_break_end;
        if (mab < 12) FAIL("interrupt %d us late: MAB %lld us", lat[i], (long long)mab);
        CHECK(s_break_end - s_break_start >= 176);
    }
}

static void test_tx_size_below_head(void)
{
    install(false);
    dmx_frame();
    run_for(1000);
    CHECK(D->dmx.head > 40);
    D->dmx.size = 40;
    run_for(30000);
    CHECK_EQ(s_negative_writes, 0);
    CHECK(D->dmx.status != DMX_STATUS_SENDING);
}

static void test_tx_interrupt_held_up(void)
{
    install(false);
    uint8_t f[513];
    for (int i = 0; i < 513; i++) f[i] = (uint8_t)i;
    f[0] = 0;
    dmx_write(P, f, sizeof(f));
    CHECK(dmx_send_num(P, sizeof(f)));
    run_for(1000);
    s_uart_hold_until = T + 7000;              /* long enough to drain the FIFO */
    CHECK(dmx_wait_sent(P, 80));
    CHECK_EQ(s_tx_n, 513);
    CHECK(T >= s_tx_busy);
    CHECK(!memcmp(s_tx, f, sizeof(f)));
}

static void test_send_timer_never_fires(void)
{
    install(false);
    D->dmx.controller_eop_timestamp = T;
    D->dmx.last_controller_pid = 0;
    tim.dead = true;
    uint8_t f[513] = {0};
    dmx_write(P, f, sizeof(f));
    const int64_t from = T;
    /* no reboot: it either gives up or sends once the time really is up */
    if (dmx_send_num(P, sizeof(f)) > 0) {
        track();
        CHECK(s_break_start - from >= 176);
    }
    CHECK_EQ(s_locks, 0);
    CHECK(D->task_waiting == NULL);
}

static void test_send_stuck_recovers(void)
{
    install(false);
    tim.dead = true;
    dmx_frame();
    run_for(1000);
    tim.dead = false;
    tim.armed = false;                         /* that interrupt is lost for good */
    uint8_t f[513] = {0};
    bool back = false;
    for (int i = 0; i < 5 && !back; i++) {
        dmx_wait_sent(P, 50);
        dmx_write(P, f, sizeof(f));
        back = dmx_send_num(P, sizeof(f)) > 0;
    }
    CHECK(back);
    CHECK(dmx_wait_sent(P, 50));
}

static void test_stale_notify_wait_sent(void)
{
    install(false);
    dmx_frame();
    s_pending = true;
    CHECK(dmx_wait_sent(P, 50));
    CHECK(D->dmx.status != DMX_STATUS_SENDING);
}

static void test_stale_notify_send(void)
{
    install(false);
    D->dmx.controller_eop_timestamp = T;
    int64_t end = T;
    s_pending = true;
    dmx_frame();
    track();
    CHECK(s_break_start - end >= 176);
}

static void test_stale_notify_receive(void)
{
    install(true);
    uint8_t b[300];
    int n = packet(b, me(), CONSOLE, 7, 1, ROOT, GET, RDM_PID_DEVICE_INFO, NULL, 0);
    on_wire(T + 100, b, n, true);
    s_pending = true;
    dmx_packet_t pkt;
    CHECK_EQ(dmx_receive(P, &pkt, 200), n);
}

static void test_receive_early_out_clears_waiter(void)
{
    controller();
    fx.silent = true;
    rdm_ack_t ack;
    uint8_t pd[4];
    s_wake_lat = 3500;
    get(0x8000, pd, sizeof(pd), &ack);
    CHECK(D->task_waiting == NULL);
}

static void test_receive_timeout_releases_lock(void)
{
    install(false);
    s_preempt = 25000;
    dmx_packet_t pkt;
    CHECK_EQ(dmx_receive(P, &pkt, 23), 0);
    CHECK_EQ(s_locks, 0);
}

static void test_wait_sent_timeout_releases_lock(void)
{
    install(false);
    s_preempt = 25000;
    dmx_wait_sent(P, 23);
    CHECK_EQ(s_locks, 0);
}

static void test_reinstall_frees_memory(void)
{
    size_t after_two = 0;
    for (int i = 0; i < 12; i++) {
        install(true);
        CHECK(dmx_driver_delete(P));
        if (i == 1) after_two = __sanitizer_get_current_allocated_bytes();
    }
    CHECK_EQ(__sanitizer_get_current_allocated_bytes(), after_two);
}

static void test_install_no_timer_interrupt(void)
{
    s_reg_fail = 1;
    CHECK(!try_install(false));
    CHECK(!dmx_driver_is_installed(P));
    s_reg_fail = 0;
    CHECK(try_install(false));
}

static void test_install_no_mutex(void)
{
    s_mutex_fail = 1;
    CHECK(!try_install(false));
    CHECK(!dmx_driver_is_installed(P));
}

static void test_delete_mid_break(void)
{
    install(false);
    dmx_frame();
    run_for(50);
    CHECK_EQ(U()->conf0.txd_inv, 1);
    CHECK(dmx_driver_delete(P));
    CHECK_EQ(U()->conf0.txd_inv, 0);           /* line not left low */
}

/* everything at once: frames and RDM mixed, interrupts and the task late at random */
/* discovery gets the bus just after the output task has started a frame */
static void test_ctrl_request_after_frame(void)
{
    static const uint16_t timing[][2] = {{176, 12}, {176, 88}, {352, 88}};
    controller();
    int asked = 0, failed = 0;
    for (int t = 0; t < 3; t++) {
        dmx_set_break_len(P, timing[t][0]);
        dmx_set_mab_len(P, timing[t][1]);
        for (int phase = 0; phase < 1000; phase += 25) {
            run_for(30000);
            run_until((T / 1000 + 1) * 1000 + phase);
            dmx_frame();
            run_for(20);
            rdm_ack_t ack = {.type = RDM_RESPONSE_TYPE_NONE};
            get(0x8000, NULL, 0, &ack);
            asked++;
            failed += ack.type != RDM_RESPONSE_TYPE_ACK;
        }
    }
    if (failed) FAIL("%d of %d requests just after a frame got nowhere", failed, asked);
}

/* both packets full and a responder that takes 2.5 ms */
static void test_ctrl_slow_full_exchange(void)
{
    controller();
    uint8_t out[231] = {1}, in[231];
    fx.pdl = 231;
    fx.delay = 2500;
    const rdm_request_t r = {.dest_uid = &FIXTURE, .cc = GET, .pid = 0x8000,
                             .format = "b", .pd = out, .pdl = sizeof(out)};
    for (int phase = 0; phase < 1000; phase += 100) {
        run_for(30000);
        run_until((T / 1000 + 1) * 1000 + phase);
        rdm_ack_t ack;
        CHECK_EQ(rdm_send_request(P, &r, "b", in, sizeof(in), &ack), 231);
        CHECK_EQ(ack.type, RDM_RESPONSE_TYPE_ACK);
    }
}

static void test_ctrl_label_32(void)
{
    controller();
    fx.pdl = 32;
    memcpy(fx.pd, "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345", 32);
    const rdm_request_t r = {.dest_uid = &FIXTURE, .cc = GET, .pid = RDM_PID_DEVICE_LABEL};
    char label[33];
    rdm_ack_t ack;
    CHECK_EQ(rdm_send_request(P, &r, "a$", label, sizeof(label), &ack), 32);
    CHECK(!memcmp(label, fx.pd, 32));
    CHECK_EQ(label[32], 0);
}

static void test_ctrl_queued_set_response(void)
{
    controller();
    fx.cc = RDM_CC_SET_COMMAND_RESPONSE;
    fx.pid = RDM_PID_DMX_START_ADDRESS;
    rdm_ack_t ack;
    get(RDM_PID_QUEUED_MESSAGE, NULL, 0, &ack);
    CHECK_EQ(ack.type, RDM_RESPONSE_TYPE_ACK);
    CHECK_EQ(ack.pid, RDM_PID_DMX_START_ADDRESS);
}

static void test_mixed_traffic(void)
{
    controller();
    srand(1);
    uint8_t f[513];
    for (int i = 0; i < 513; i++) f[i] = (uint8_t)(i * 7);
    f[0] = 0;
    int64_t last_end = 0, need = 0;
    int answered = 0, asked = 0;
    for (int i = 0; i < 4000; i++) {
        memset(&fx, 0, sizeof(fx));
        s_tim_lat = rand() % 40;
        s_wake_lat = rand() % 4 ? 0 : rand() % 1500;
        if (rand() % 5 == 0) s_pending = true;
        if (rand() % 7 == 0) s_uart_hold_until = T + rand() % 400;
        const int kind = rand() % 6;
        if (kind < 3) {
            CHECK(dmx_wait_sent(P, 50));
            dmx_write(P, f, sizeof(f));
            CHECK_EQ(dmx_send_num(P, sizeof(f)), sizeof(f));
            track();
            if (s_break_start - last_end < need)
                FAIL("step %d: only %lld us before a frame", i, (long long)(s_break_start - last_end));
            CHECK(dmx_wait_sent(P, 50));
            CHECK_EQ(s_tx_n, 513);
            CHECK(!memcmp(s_tx, f, sizeof(f)));
            CHECK(s_tx_first - s_break_end >= 12);
            last_end = s_tx_end;
            need = 0;
        } else {
            rdm_ack_t ack;
            uint8_t pd[232];
            fx.silent = kind == 5;
            fx.dub = true;
            fx.pdl = rand() % 60;
            fx.delay = 176 + rand() % 1500;
            if (kind == 4) {
                rdm_disc_unique_branch_t br = {{0, 0}, RDM_UID_MAX};
                rdm_send_disc_unique_branch(P, &br, &ack);
            } else {
                get(0x8000, pd, sizeof(pd), &ack);
            }
            if (!fx.silent) {
                asked++;
                answered += ack.type == RDM_RESPONSE_TYPE_ACK;
                last_end = s_rx_end;
                need = 176;
            } else {
                last_end = s_tx_end;
                need = 3000;
            }
        }
        CHECK_EQ(s_locks, 0);
    }
    if (answered != asked) FAIL("%d of %d replies got through", answered, asked);
}

/* ------------------------------------------------------------------------ */

static int s_run, s_failed;

static void run(const char *name, void (*fn)(void))
{
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        alarm(strcmp(name, "test_resp_any_request") && strcmp(name, "test_mixed_traffic") ? 10 : 300);
        fn();
        if (host_critical || s_blocked_in_critical) {
            fprintf(stderr, "  critical section unbalanced, or waited inside one\n");
            _exit(1);
        }
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    s_run++;
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
        printf("ok   %s\n", name);
    } else {
        s_failed++;
        if (WIFSIGNALED(st)) printf("FAIL %s (signal %d)\n", name, WTERMSIG(st));
        else printf("FAIL %s\n", name);
    }
}
#define RUN(fn) run(#fn, fn)

int main(void)
{
    RUN(test_resp_device_info);
    RUN(test_resp_queued_message);
    RUN(test_resp_queue_last_message);
    RUN(test_resp_queue_wrap);
    RUN(test_resp_sub_devices);
    RUN(test_resp_pid_zero);
    RUN(test_resp_ignores_non_requests);
    RUN(test_resp_command_classes);
    RUN(test_resp_discovery);
    RUN(test_resp_length_mismatch);
    RUN(test_resp_personality_description);
    RUN(test_resp_set_validation);
    RUN(test_resp_label);
    RUN(test_resp_label_survives_restart);
    RUN(test_resp_saved_on_delete);
    RUN(test_resp_identify_not_saved);
    RUN(test_resp_releases_lock);
    RUN(test_resp_keeps_frame_size);
    RUN(test_resp_next_receive_waits);
    RUN(test_resp_late_is_not_boot_loader);
    RUN(test_resp_supported_parameters);
    RUN(test_resp_sensors);
    RUN(test_resp_language);
    RUN(test_resp_any_request);
    RUN(test_ctrl_get);
    RUN(test_ctrl_pd_without_ack);
    RUN(test_ctrl_gap_after_reply);
    RUN(test_ctrl_gap_lost_reply);
    RUN(test_ctrl_gap_broadcast);
    RUN(test_ctrl_gap_discovery);
    RUN(test_ctrl_reply_matching);
    RUN(test_ctrl_ack_never_stale);
    RUN(test_ctrl_ack_timer_units);
    RUN(test_ctrl_malformed_reason);
    RUN(test_ctrl_largest_packets);
    RUN(test_ctrl_late_task);
    RUN(test_ctrl_turns_round_before_waking);
    RUN(test_mab_late_interrupt);
    RUN(test_tx_size_below_head);
    RUN(test_tx_interrupt_held_up);
    RUN(test_send_timer_never_fires);
    RUN(test_send_stuck_recovers);
    RUN(test_stale_notify_wait_sent);
    RUN(test_stale_notify_send);
    RUN(test_stale_notify_receive);
    RUN(test_receive_early_out_clears_waiter);
    RUN(test_receive_timeout_releases_lock);
    RUN(test_wait_sent_timeout_releases_lock);
    RUN(test_reinstall_frees_memory);
    RUN(test_install_no_timer_interrupt);
    RUN(test_install_no_mutex);
    RUN(test_delete_mid_break);
    RUN(test_ctrl_request_after_frame);
    RUN(test_ctrl_slow_full_exchange);
    RUN(test_ctrl_label_32);
    RUN(test_ctrl_queued_set_response);
    RUN(test_mixed_traffic);
    printf("%d tests, %d failed\n", s_run, s_failed);
    return s_failed ? 1 : 0;
}
