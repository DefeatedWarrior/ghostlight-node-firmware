/* dmx_handler.c on the host: RDM discovery, the raw RDM relay, saving and
 * driver installs, against fake fixtures. esp_dmx is faked here (its own
 * tests are test_dmx_rdm.c and test_dmx_rx.c). Includes the .c to run its
 * tasks by hand. */
#include "dmx_handler.c"

static void reset(void);
#define TEST_SETUP() reset()
#include "test.h"

#define P1 (&s_dmx.ports[0])
#define FX_MAX (DMX_MAX_DEVICES + 12)

int host_log_enabled;

/* ---- FreeRTOS: one task, and the others run as they are created -------- */

static int64_t T;
static int s_core, s_deadlocks, s_blocked_holding_bus, s_create_fail;
static void (*s_on_delay)(void);

int64_t esp_timer_get_time(void) { return T; }
TickType_t xTaskGetTickCount(void) { return (TickType_t)(T / 1000); }
void vTaskDelete(TaskHandle_t t) { (void)t; }

static SemaphoreHandle_t sem(int count)
{
    int *s = malloc(sizeof(int));
    *s = count;
    return s;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return sem(1); }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return sem(0); }
void vSemaphoreDelete(SemaphoreHandle_t s) { free(s); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait)
{
    int *c = s;
    if (*c > 0) {
        --*c;
        return pdTRUE;
    }
    if (wait == portMAX_DELAY) s_deadlocks++;
    else T += (int64_t)wait * 1000;
    return pdFALSE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    *(int *)s = 1;
    return pdTRUE;
}

EventGroupHandle_t xEventGroupCreate(void) { return calloc(1, sizeof(EventBits_t)); }
void vEventGroupDelete(EventGroupHandle_t e) { free(e); }
EventBits_t xEventGroupSetBits(EventGroupHandle_t e, EventBits_t b) { return *(EventBits_t *)e |= b; }
EventBits_t xEventGroupClearBits(EventGroupHandle_t e, EventBits_t b) { return *(EventBits_t *)e &= ~b; }
EventBits_t xEventGroupWaitBits(EventGroupHandle_t e, EventBits_t b, BaseType_t c, BaseType_t a,
                                TickType_t w)
{
    (void)b; (void)c; (void)a; (void)w;
    return *(EventBits_t *)e;
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                   int priority, TaskHandle_t *out, int core)
{
    (void)stack; (void)priority;
    if (s_create_fail && !strncmp(name, "dmx_install", 11)) return pdFALSE;
    if (out) *out = arg;
    if (fn == output_task || fn == input_task) return pdPASS;   /* the test runs those */
    int was = s_core;
    s_core = core;
    fn(arg);
    s_core = was;
    return pdPASS;
}

void vTaskDelay(TickType_t ticks)
{
    if (s_dmx.initialized && P1->is_active && *(int *)P1->bus == 0) s_blocked_holding_bus++;
    T += (int64_t)ticks * 1000;
    if (s_on_delay) s_on_delay();
}

int gpio_reset_pin(int p) { (void)p; return 0; }
int gpio_set_direction(int p, int m) { (void)p; (void)m; return 0; }
int gpio_set_level(int p, uint32_t v) { (void)p; (void)v; return 0; }
esp_err_t board_gpio_isr_service(void) { return ESP_OK; }

/* ---- esp_dmx ----------------------------------------------------------- */

static struct {
    bool installed[4];
    int install_core, installs, install_fail, delete_refuse;
    int staged, commits, commit_after_delete, rx_left;
} drv;

bool dmx_driver_install(dmx_port_t n, const dmx_config_t *c, const dmx_personality_t *p, int count)
{
    (void)c; (void)p; (void)count;
    if (drv.install_fail || drv.installed[n]) return false;
    drv.installed[n] = true;
    drv.install_core = s_core;
    drv.installs++;
    return true;
}
bool dmx_driver_delete(dmx_port_t n)
{
    if (!drv.installed[n]) return false;
    if (drv.delete_refuse) {
        drv.delete_refuse--;
        return false;
    }
    drv.installed[n] = false;
    return true;
}
bool dmx_driver_is_installed(dmx_port_t n) { return drv.installed[n]; }
bool dmx_set_pin(dmx_port_t n, int tx, int rx, int en) { (void)n; (void)tx; (void)rx; (void)en; return true; }
uint32_t dmx_set_break_len(dmx_port_t n, uint32_t v) { (void)n; return v; }
uint32_t dmx_set_mab_len(dmx_port_t n, uint32_t v) { (void)n; return v; }
bool dmx_sniffer_enable(dmx_port_t n, int pin) { (void)n; (void)pin; return true; }
bool dmx_sniffer_disable(dmx_port_t n) { (void)n; return true; }
bool dmx_sniffer_get_data(dmx_port_t n, dmx_metadata_t *m) { (void)n; (void)m; return false; }
rdm_pid_t dmx_parameter_commit(dmx_port_t n)
{
    if (!drv.installed[n]) drv.commit_after_delete++;
    if (!drv.staged) return 0;
    drv.staged--;
    drv.commits++;
    return 0xF0;
}
const rdm_uid_t *rdm_uid_get(dmx_port_t n)
{
    static const rdm_uid_t me = {0x05E0, 0x00000001};
    (void)n;
    return &me;
}
bool rdm_send_response(dmx_port_t n) { (void)n; return true; }
size_t rdm_get_identify_device(dmx_port_t n, bool *i) { (void)n; *i = false; return 1; }

/* the raw relay's line */
static struct { uint8_t sent[300], reply[300]; size_t sent_len, reply_len; TickType_t wait; int sends; } raw;

bool dmx_wait_sent(dmx_port_t n, TickType_t w) { (void)n; (void)w; return true; }
size_t dmx_write(dmx_port_t n, const void *s, size_t len)
{
    (void)n;
    if (len > sizeof(raw.sent)) return 0;
    memcpy(raw.sent, s, len);
    raw.sent_len = len;
    return len;
}
size_t dmx_send_num(dmx_port_t n, size_t len) { (void)n; raw.sends++; return len; }
size_t dmx_receive(dmx_port_t n, dmx_packet_t *pkt, TickType_t w)
{
    (void)n;
    raw.wait = w;
    if (drv.rx_left) {                  /* DMX that never stops arriving */
        if (--drv.rx_left == 0) P1->run = false;
        T += 23000;
        *pkt = (dmx_packet_t){.err = DMX_OK, .sc = 0, .size = DMX_FRAME_SIZE};
        return DMX_FRAME_SIZE;
    }
    if (pkt) *pkt = (dmx_packet_t){.err = raw.reply_len ? DMX_OK : DMX_ERR_TIMEOUT, .size = raw.reply_len};
    return raw.reply_len;
}
size_t dmx_read(dmx_port_t n, void *d, size_t len)
{
    (void)n;
    memcpy(d, raw.reply, len);
    return len;
}
size_t dmx_read_received(dmx_port_t n, void *d, size_t len, uint32_t *seq)
{
    (void)n;
    memset(d, 0, len);
    *seq = (uint32_t)(T / 23000) + 1;
    return len;
}

/* fake fixtures */
static struct {
    rdm_uid_t u;
    bool muted, cut;
    int drop;           /* mute replies still to lose */
    int busy;           /* ACK_TIMERs still to send */
    int timer_ms;
} fx[FX_MAX];
static int s_nf, s_lookups;
static const char LABEL32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";

static uint64_t U(rdm_uid_t u) { return (uint64_t)u.man_id << 32 | u.dev_id; }
static int fixture(const rdm_uid_t *u)
{
    for (int i = 0; i < s_nf; i++) {
        if (U(fx[i].u) == U(*u) && !fx[i].cut) return i;
    }
    return -1;
}

bool rdm_send_disc_mute(dmx_port_t n, const rdm_uid_t *u, rdm_disc_mute_t *m, rdm_ack_t *ack)
{
    (void)n; (void)m;
    int i = fixture(u);
    if (i < 0) return false;
    fx[i].muted = true;
    if (fx[i].drop) {
        fx[i].drop--;
        return false;
    }
    ack->type = RDM_RESPONSE_TYPE_ACK;
    ack->src_uid = *u;
    return true;
}
bool rdm_send_disc_un_mute(dmx_port_t n, const rdm_uid_t *u, rdm_disc_mute_t *m, rdm_ack_t *ack)
{
    (void)n; (void)u; (void)m; (void)ack;
    for (int i = 0; i < s_nf; i++) {
        if (!fx[i].cut) fx[i].muted = false;
    }
    return true;
}
bool rdm_send_disc_unique_branch(dmx_port_t n, const rdm_disc_unique_branch_t *b, rdm_ack_t *ack)
{
    (void)n;
    int c = 0;
    for (int i = 0; i < s_nf; i++) {
        if (fx[i].cut || fx[i].muted || U(fx[i].u) < U(b->lower_bound) || U(fx[i].u) > U(b->upper_bound))
            continue;
        c++;
        ack->src_uid = fx[i].u;
    }
    ack->type = c == 0 ? RDM_RESPONSE_TYPE_NONE : c == 1 ? RDM_RESPONSE_TYPE_ACK : RDM_RESPONSE_TYPE_INVALID;
    return c == 1;
}

static bool answer(const rdm_uid_t *u, rdm_ack_t *ack)
{
    int i = fixture(u);
    s_lookups++;
    if (i < 0) return false;
    ack->src_uid = *u;
    if (fx[i].busy) {
        fx[i].busy--;
        ack->type = RDM_RESPONSE_TYPE_ACK_TIMER;
        ack->timer = (TickType_t)fx[i].timer_ms;
        return false;
    }
    ack->type = RDM_RESPONSE_TYPE_ACK;
    return true;
}
/* bounded as the driver's codec is: the null needs room too */
static void put_label(char *d, size_t size)
{
    size_t n = size - 1 < 32 ? size - 1 : 32;
    memcpy(d, LABEL32, n);
    d[n] = '\0';
}
size_t rdm_send_get_device_info(dmx_port_t n, const rdm_uid_t *u, rdm_sub_device_t s,
                                rdm_device_info_t *d, rdm_ack_t *ack)
{
    (void)n; (void)s;
    if (!answer(u, ack)) return 0;
    d->dmx_start_address = 101;
    d->footprint = 16;
    return 19;
}
size_t rdm_send_request(dmx_port_t n, const rdm_request_t *r, const char *f, void *pd, size_t size,
                        rdm_ack_t *ack)
{
    (void)n; (void)f;
    if (!answer(r->dest_uid, ack)) return 0;
    if (pd && size) put_label(pd, size);
    return 32;
}
size_t rdm_send_get_software_version_label(dmx_port_t n, const rdm_uid_t *u, rdm_sub_device_t s,
                                           char *d, size_t size, rdm_ack_t *ack)
{
    (void)n; (void)s;
    if (!answer(u, ack)) return 0;
    put_label(d, size);
    return 32;
}
bool rdm_send_set_dmx_start_address(dmx_port_t n, const rdm_uid_t *u, rdm_sub_device_t s,
                                    const uint16_t a, rdm_ack_t *ack)
{
    (void)n; (void)s; (void)a;
    return answer(u, ack);
}

/* ---- helpers ----------------------------------------------------------- */

static void configure(dmx_mode_t mode)
{
    port_config_t c = {.mode = mode, .rdm_enabled = true, .out_rate_hz = CONFIG_DMX_RATE_DEFAULT,
                       .break_us = CONFIG_DMX_BREAK_DEFAULT, .mab_us = CONFIG_DMX_MAB_DEFAULT,
                       .out_slots = CONFIG_DMX_SLOTS_MAX};
    xSemaphoreGive(P1->out_done);       /* the task we don't run */
    xSemaphoreGive(P1->in_done);
    dmx_handler_configure_port(1, &c);
}

static void reset(void)
{
    if (s_dmx.initialized) {
        for (int i = 0; i < DMX_PORT_MAX; i++) {
            xSemaphoreGive(s_dmx.ports[i].out_done);
            xSemaphoreGive(s_dmx.ports[i].in_done);
        }
        drv.delete_refuse = 0;
        dmx_handler_deinit();
    }
    memset(&drv, 0, sizeof(drv));
    memset(&raw, 0, sizeof(raw));
    memset(fx, 0, sizeof(fx));
    s_nf = s_lookups = s_deadlocks = s_blocked_holding_bus = s_create_fail = 0;
    s_on_delay = NULL;
    s_core = BOARD_CORE_NET;
    T = 1000000;
    dmx_handler_init();
    configure(DMX_MODE_RDM_MASTER);
    dmx_handler_start_port(1);
}

static void fixtures(int n)
{
    s_nf = n;
    for (int i = 0; i < n; i++) fx[i].u = (rdm_uid_t){0x4C44, 0x1000u + (uint32_t)i * 7919u};
}
static void full(void) { dmx_handler_rdm_discover(1); }
static void background(int passes)
{
    while (passes--) dmx_handler_rdm_discover_mode(1, DMX_DISC_INCREMENTAL);
}
static int listed(void)
{
    rdm_uid_t u[FX_MAX];
    return (int)dmx_handler_get_rdm_uids(1, u, FX_MAX);
}
static bool is_listed(int i)
{
    rdm_uid_t u[FX_MAX];
    size_t n = dmx_handler_get_rdm_uids(1, u, FX_MAX);
    for (size_t k = 0; k < n; k++) {
        if (U(u[k]) == U(fx[i].u)) return true;
    }
    return false;
}

/* ---- discovery --------------------------------------------------------- */

static void test_discovery_full(void)
{
    fixtures(6);
    full();
    CHECK_EQ(listed(), 6);
    CHECK(dmx_handler_rdm_tod_valid(1));
    background(120);
    CHECK_EQ(listed(), 6);
    CHECK_EQ(s_deadlocks, 0);
}

static void test_discovery_lost_mute_replies(void)
{
    fixtures(6);
    full();
    fx[2].drop = 3;                 /* all three tries */
    background(1);
    CHECK_EQ(listed(), 6);          /* one miss is no loss */
    background(120);
    CHECK_EQ(listed(), 6);
}

static void test_discovery_cable_out_for_a_moment(void)
{
    fixtures(6);
    full();
    for (int i = 2; i < 6; i++) fx[i].cut = true;
    background(1);
    CHECK_EQ(listed(), 6);
    for (int i = 2; i < 6; i++) fx[i].cut = false;
    background(1);
    CHECK_EQ(listed(), 6);
}

static void test_discovery_gone_then_back(void)
{
    fixtures(6);
    full();
    fx[4].cut = true;
    background(1);
    CHECK(is_listed(4));
    background(1);
    CHECK(!is_listed(4));
    CHECK_EQ(listed(), 5);
    fx[4].cut = false;              /* still muted, as it was never powered off */
    background(1);
    CHECK(is_listed(4));
    CHECK_EQ(listed(), 6);
}

static void test_discovery_misses_in_a_row(void)
{
    fixtures(3);
    full();
    for (int i = 0; i < 5; i++) {   /* every other pass missed */
        fx[1].cut = true;
        background(1);
        fx[1].cut = false;
        background(1);
        CHECK_EQ(listed(), 3);
    }
}

static void test_discovery_all_gone(void)
{
    fixtures(4);
    full();
    for (int i = 0; i < 4; i++) fx[i].cut = true;
    background(2);
    CHECK_EQ(listed(), 0);
    CHECK(dmx_handler_rdm_tod_valid(1));
    rdm_device_t d[4];
    size_t n = 4;
    CHECK_EQ(dmx_handler_get_rdm_devices(1, d, &n), ESP_OK);
    CHECK_EQ(n, 0);
}

static void test_discovery_forty(void)
{
    fixtures(40);
    full();
    CHECK_EQ(listed(), 40);
    background(5);
    CHECK_EQ(listed(), 40);
}

static void test_discovery_full_list(void)
{
    fixtures(DMX_MAX_DEVICES + 10);
    full();
    CHECK_EQ(listed(), DMX_MAX_DEVICES);
    dmx_port_status_t st;
    CHECK_EQ(dmx_handler_get_port_status(1, &st), ESP_OK);
    CHECK(st.rdm_list_full);
    background(3);
    CHECK_EQ(listed(), DMX_MAX_DEVICES);
    for (int i = 0; i < 20; i++) fx[i].cut = true;      /* room comes free */
    background(3);
    CHECK_EQ(listed(), DMX_MAX_DEVICES - 10);
    dmx_handler_get_port_status(1, &st);
    CHECK(!st.rdm_list_full);
}

static void test_discovery_flush(void)
{
    fixtures(5);
    full();
    s_nf = 2;
    dmx_handler_rdm_discover_mode(1, DMX_DISC_FLUSH);
    CHECK_EQ(listed(), 2);
}

static void test_discovery_random_faults(void)
{
    srand(7);
    fixtures(24);
    full();
    for (int round = 0; round < 2000; round++) {
        for (int i = 0; i < s_nf; i++) {
            fx[i].cut = rand() % 7 == 0;
            fx[i].drop = rand() % 11 == 0 ? rand() % 5 : 0;
        }
        background(1 + rand() % 3);
        for (int i = 0; i < s_nf; i++) {
            fx[i].cut = false;
            fx[i].drop = 0;
        }
        background(1);              /* one clean pass and everything is there */
        CHECK_EQ(listed(), s_nf);
        for (int i = 0; i < s_nf; i++) CHECK(is_listed(i));
    }
    CHECK_EQ(s_deadlocks, 0);
}

static void test_discovery_labels_whole(void)
{
    fixtures(1);
    full();
    rdm_device_t d;
    size_t n = 1;
    CHECK_EQ(dmx_handler_get_rdm_devices(1, &d, &n), ESP_OK);
    CHECK_STR(d.manufacturer_label, LABEL32);
    CHECK_STR(d.device_label, LABEL32);
    CHECK_STR(d.software_label, LABEL32);
}

static void test_discovery_ack_timer(void)
{
    fixtures(1);
    fx[0].busy = 1;
    fx[0].timer_ms = 200;
    int64_t t0 = T;
    full();
    rdm_device_t d;
    size_t n = 1;
    dmx_handler_get_rdm_devices(1, &d, &n);
    CHECK_EQ(d.dmx_start_address, 101);
    CHECK(T - t0 >= 200000);
    CHECK_EQ(s_blocked_holding_bus, 0);
}

static void test_discovery_ack_timer_bounded(void)
{
    fixtures(1);
    fx[0].busy = 1000000;
    fx[0].timer_ms = 0;             /* "now", for ever */
    int64_t t0 = T;
    full();
    CHECK(T - t0 <= 1100000);
    CHECK(s_lookups < 80);
    CHECK_EQ(listed(), 1);
    fx[0].timer_ms = 6553500;
    t0 = T;
    full();
    CHECK(T - t0 <= 1100000);
}

static void abort_now(void) { dmx_handler_rdm_abort_discovery(1); }

static void test_discovery_abort_keeps_list(void)
{
    fixtures(3);
    full();
    fixtures(4);
    fx[3].busy = 1;
    fx[3].timer_ms = 900;
    s_on_delay = abort_now;
    int64_t t0 = T;
    background(1);
    CHECK(T - t0 <= 100000);        /* didn't sit out the 900 ms */
    CHECK_EQ(listed(), 3);
    CHECK(!dmx_handler_rdm_is_discovering(1));
}

/* ---- raw relay --------------------------------------------------------- */

static const rdm_uid_t CONSOLE = {0x7A70, 0x01020304};
static const rdm_uid_t FIXTURE = {0x1234, 0x00000042};
static const rdm_uid_t OTHER = {0x1234, 0x00000099};

static void put_uid(uint8_t *d, rdm_uid_t u)
{
    d[0] = u.man_id >> 8; d[1] = (uint8_t)u.man_id;
    d[2] = u.dev_id >> 24; d[3] = u.dev_id >> 16; d[4] = u.dev_id >> 8; d[5] = (uint8_t)u.dev_id;
}
static size_t packet(uint8_t *b, rdm_uid_t to, rdm_uid_t from, uint8_t cc, uint16_t pid, int pdl)
{
    memset(b, 0, 300);
    b[0] = 0xCC; b[1] = 0x01; b[2] = (uint8_t)(24 + pdl);
    put_uid(b + 3, to);
    put_uid(b + 9, from);
    b[15] = 9; b[20] = cc;
    b[21] = pid >> 8; b[22] = (uint8_t)pid; b[23] = (uint8_t)pdl;
    uint16_t sum = 0;
    for (int i = 0; i < 24 + pdl; i++) sum += b[i];
    b[24 + pdl] = sum >> 8; b[25 + pdl] = (uint8_t)sum;
    return 26 + (size_t)pdl;
}
static esp_err_t relay(uint8_t cc, uint16_t pid, rdm_uid_t to)
{
    uint8_t req[300], resp[257];
    size_t got;
    size_t n = packet(req, to, CONSOLE, cc, pid, 0);
    return dmx_handler_rdm_transaction(1, req, n, resp, sizeof(resp), &got);
}
static esp_err_t reply_with(rdm_uid_t to, rdm_uid_t from, uint8_t cc, uint16_t pid)
{
    raw.reply_len = packet(raw.reply, to, from, cc, pid, 4);
    return relay(0x20, 0x0060, FIXTURE);
}

static void test_relay_takes_the_answer(void)
{
    CHECK_EQ(reply_with(CONSOLE, FIXTURE, 0x21, 0x0060), ESP_OK);
    CHECK(raw.wait >= 27);          /* two full packets and 2.8 ms */
}

static void test_relay_refuses_others(void)
{
    CHECK_EQ(reply_with(CONSOLE, OTHER, 0x21, 0x0060), ESP_ERR_INVALID_RESPONSE);
    CHECK_EQ(reply_with(OTHER, FIXTURE, 0x21, 0x0060), ESP_ERR_INVALID_RESPONSE);
    CHECK_EQ(reply_with(CONSOLE, FIXTURE, 0x31, 0x0060), ESP_ERR_INVALID_RESPONSE);
    CHECK_EQ(reply_with(CONSOLE, FIXTURE, 0x21, 0x0082), ESP_ERR_INVALID_RESPONSE);
    raw.reply_len = packet(raw.reply, FIXTURE, CONSOLE, 0x20, 0x0060, 0);   /* our own echo */
    CHECK_EQ(relay(0x20, 0x0060, FIXTURE), ESP_ERR_INVALID_RESPONSE);

    raw.reply_len = packet(raw.reply, CONSOLE, FIXTURE, 0x21, 0x0060, 4);
    raw.reply[23] = 9;              /* lengths disagree, checksum right */
    uint16_t sum = 0;
    for (int i = 0; i < 28; i++) sum += raw.reply[i];
    raw.reply[28] = sum >> 8; raw.reply[29] = (uint8_t)sum;
    CHECK_EQ(relay(0x20, 0x0060, FIXTURE), ESP_ERR_INVALID_RESPONSE);
}

static void test_relay_queued_message(void)
{
    raw.reply_len = packet(raw.reply, CONSOLE, FIXTURE, 0x21, 0x0082, 33);
    CHECK_EQ(relay(0x20, 0x0020, FIXTURE), ESP_OK);
    raw.reply_len = packet(raw.reply, CONSOLE, FIXTURE, 0x31, 0x00F0, 0);
    CHECK_EQ(relay(0x20, 0x0020, FIXTURE), ESP_OK);
    raw.reply_len = packet(raw.reply, CONSOLE, OTHER, 0x21, 0x0082, 0);
    CHECK_EQ(relay(0x20, 0x0020, FIXTURE), ESP_ERR_INVALID_RESPONSE);
}

static void test_relay_only_requests(void)
{
    static const uint8_t cc[] = {0x00, 0x10, 0x11, 0x21, 0x31, 0x40, 0xFF};
    for (size_t i = 0; i < sizeof(cc); i++) CHECK_EQ(relay(cc[i], 0x0060, FIXTURE), ESP_ERR_NOT_SUPPORTED);
    CHECK_EQ(raw.sends, 0);
    CHECK_EQ(relay(0x30, 0x00F0, RDM_UID_BROADCAST_ALL), ESP_OK);
    CHECK_EQ(raw.sends, 1);
}

/* ---- driver installs and saving ---------------------------------------- */

static void test_install_on_dmx_core(void)
{
    CHECK_EQ(drv.installs, 1);
    CHECK_EQ(drv.install_core, BOARD_CORE_DMX);
    CHECK_EQ(s_deadlocks, 0);
}

static void test_install_without_the_task(void)
{
    configure(DMX_MODE_OUTPUT);
    s_create_fail = 1;
    CHECK_EQ(dmx_handler_start_port(1), ESP_OK);
    CHECK_EQ(drv.installs, 2);
}

static void test_install_fails(void)
{
    configure(DMX_MODE_OUTPUT);
    drv.install_fail = 1;
    CHECK(dmx_handler_start_port(1) != ESP_OK);
    CHECK(!dmx_handler_rdm_available(1));
    CHECK_EQ(*(int *)P1->bus, 1);
}

static void test_uninstall_busy_driver(void)
{
    drv.delete_refuse = 3;          /* busy for a moment */
    configure(DMX_MODE_OUTPUT);
    CHECK_EQ(dmx_handler_start_port(1), ESP_OK);
    CHECK_EQ(drv.installs, 2);

    drv.delete_refuse = 1000;       /* stuck */
    configure(DMX_MODE_OUTPUT);
    CHECK(P1->driver_installed);
    CHECK(!rdm_bus_ok(P1));
    CHECK(dmx_handler_start_port(1) != ESP_OK);
    drv.delete_refuse = 0;          /* free again */
    CHECK_EQ(dmx_handler_start_port(1), ESP_OK);
    CHECK_EQ(drv.installs, 3);
}

static void test_saved_while_dmx_runs(void)
{
    configure(DMX_MODE_RDM_RESPONDER);
    CHECK_EQ(dmx_handler_start_port(1), ESP_OK);
    drv.staged = 2;
    drv.rx_left = 100;              /* 2.3 s */
    input_task(P1);
    CHECK_EQ(drv.commits, 2);
    CHECK_EQ(drv.commit_after_delete, 0);
}

static void test_plain_input_saves_nothing(void)
{
    configure(DMX_MODE_INPUT);
    CHECK_EQ(dmx_handler_start_port(1), ESP_OK);
    drv.staged = 1;
    drv.rx_left = 100;
    input_task(P1);
    CHECK_EQ(drv.commits, 0);
}

int main(void)
{
    RUN(test_discovery_full);
    RUN(test_discovery_lost_mute_replies);
    RUN(test_discovery_cable_out_for_a_moment);
    RUN(test_discovery_gone_then_back);
    RUN(test_discovery_misses_in_a_row);
    RUN(test_discovery_all_gone);
    RUN(test_discovery_forty);
    RUN(test_discovery_full_list);
    RUN(test_discovery_flush);
    RUN(test_discovery_random_faults);
    RUN(test_discovery_labels_whole);
    RUN(test_discovery_ack_timer);
    RUN(test_discovery_ack_timer_bounded);
    RUN(test_discovery_abort_keeps_list);
    RUN(test_relay_takes_the_answer);
    RUN(test_relay_refuses_others);
    RUN(test_relay_queued_message);
    RUN(test_relay_only_requests);
    RUN(test_install_on_dmx_core);
    RUN(test_install_without_the_task);
    RUN(test_install_fails);
    RUN(test_uninstall_busy_driver);
    RUN(test_saved_while_dmx_runs);
    RUN(test_plain_input_saves_nothing);
    return test_summary();
}
