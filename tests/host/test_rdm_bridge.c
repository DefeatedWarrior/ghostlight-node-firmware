/* rdm_bridge.c on the host: its request queue, GET retries and age limit,
 * against a fake dmx_handler and Art-Net sender. Includes the .c to run its
 * task one message at a time. */
#include "rdm_bridge.c"

static void reset(void);
#define TEST_SETUP() reset()
#include "test.h"

int host_log_enabled;
const char *esp_err_to_name(esp_err_t e) { (void)e; return "error"; }

/* ---- clock, tasks ------------------------------------------------------ */

static int64_t T;
int64_t esp_timer_get_time(void) { return T; }
TickType_t xTaskGetTickCount(void) { return (TickType_t)(T / 1000); }
void vTaskDelay(TickType_t ticks) { T += (int64_t)ticks * 1000; }
void vTaskDelete(TaskHandle_t t) { (void)t; }
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                   int priority, TaskHandle_t *out, int core)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)priority; (void)core;
    if (out) *out = (TaskHandle_t)1;
    return pdPASS;              /* the test runs bridge_step() itself */
}

/* ---- ring buffer: FIFO, 8-byte header and 4-byte rounding per item ----- */

#define RB_MAX 1024
static struct {
    size_t cap, used;
    int head, n;
    struct { void *p; size_t size; } items[RB_MAX];
} rb;
static size_t cost(size_t size) { return 8 + ((size + 3) & ~(size_t)3); }
RingbufHandle_t xRingbufferCreate(size_t size, RingbufferType_t type)
{
    (void)type;
    memset(&rb, 0, sizeof(rb));
    rb.cap = size;
    return &rb;
}
RingbufHandle_t xRingbufferCreateWithCaps(size_t size, RingbufferType_t type, unsigned caps)
{
    (void)caps;
    return xRingbufferCreate(size, type);
}
BaseType_t xRingbufferSend(RingbufHandle_t h, const void *item, size_t size, TickType_t wait)
{
    (void)h; (void)wait;
    if (rb.used + cost(size) > rb.cap || rb.n == RB_MAX) return pdFALSE;
    void *p = malloc(size);
    memcpy(p, item, size);
    int at = (rb.head + rb.n++) % RB_MAX;
    rb.items[at].p = p;
    rb.items[at].size = size;
    rb.used += cost(size);
    return pdTRUE;
}
void *xRingbufferReceive(RingbufHandle_t h, size_t *size, TickType_t wait)
{
    (void)h; (void)wait;
    if (!rb.n) return NULL;
    *size = rb.items[rb.head].size;
    return rb.items[rb.head].p;  /* stays counted until returned */
}
void vRingbufferReturnItem(RingbufHandle_t h, void *item)
{
    (void)h;
    if (!rb.n || rb.items[rb.head].p != item) abort();
    rb.used -= cost(rb.items[rb.head].size);
    free(item);
    rb.head = (rb.head + 1) % RB_MAX;
    rb.n--;
}
void vRingbufferDelete(RingbufHandle_t h) { (void)h; }
void vRingbufferDeleteWithCaps(RingbufHandle_t h) { (void)h; }

/* ---- dmx_handler and Art-Net ------------------------------------------- */

static struct {
    int transactions;
    esp_err_t script[8];        /* what each transaction returns, then ESP_OK */
    int tod_sent, rdm_sent, notified;
    uint16_t last_pid;
} fk;
static rdm_discovery_callback_t s_disc_cb;
static artnet_rdm_callback_t s_rdm_cb;

bool dmx_handler_rdm_available(uint8_t port) { return port == 1; }
bool dmx_handler_rdm_tod_valid(uint8_t port) { return port == 1; }
bool dmx_handler_rdm_is_discovering(uint8_t port) { (void)port; return false; }
esp_err_t dmx_handler_rdm_discover_mode(uint8_t port, dmx_disc_mode_t mode) { (void)port; (void)mode; return ESP_OK; }
esp_err_t dmx_handler_rdm_abort_discovery(uint8_t port) { (void)port; return ESP_OK; }
esp_err_t dmx_handler_rdm_wait_discovery(uint8_t port, uint32_t ms) { (void)port; (void)ms; return ESP_OK; }
size_t dmx_handler_get_rdm_uids(uint8_t port, rdm_uid_t *uids, size_t max)
{
    (void)port;
    if (max) uids[0] = (rdm_uid_t){0x4C44, 0x1234};
    return max ? 1 : 0;
}
esp_err_t dmx_handler_register_discovery_callback(uint8_t port, rdm_discovery_callback_t cb, void *user)
{
    (void)port; (void)user;
    s_disc_cb = cb;
    return ESP_OK;
}
esp_err_t dmx_handler_rdm_transaction(uint8_t port, const uint8_t *req, size_t req_len,
                                      uint8_t *resp, size_t resp_size, size_t *resp_len)
{
    (void)port; (void)req_len;
    fk.last_pid = (uint16_t)(req[21] << 8 | req[22]);
    esp_err_t r = fk.transactions < 8 ? fk.script[fk.transactions] : ESP_OK;
    fk.transactions++;
    if (r == ESP_OK) {
        memset(resp, 0, resp_size);
        *resp_len = 26;
    }
    return r;
}
esp_err_t artnet_receiver_set_rdm_callback(artnet_rdm_callback_t cb, void *user) { (void)user; s_rdm_cb = cb; return ESP_OK; }
void artnet_receiver_notify_change(bool urgent) { (void)urgent; fk.notified++; }
esp_err_t artnet_receiver_send_tod_data(uint32_t ip, uint8_t bind, uint16_t pa, bool nak,
                                        const uint8_t (*uids)[6], uint16_t count)
{
    (void)ip; (void)bind; (void)pa; (void)nak; (void)uids; (void)count;
    fk.tod_sent++;
    return ESP_OK;
}
esp_err_t artnet_receiver_send_rdm(uint32_t ip, uint8_t bind, uint16_t pa, const uint8_t *rdm, uint16_t len)
{
    (void)ip; (void)bind; (void)pa; (void)rdm; (void)len;
    fk.rdm_sent++;
    return ESP_OK;
}

/* ---- helpers ----------------------------------------------------------- */

#define PA 0x0001               /* port 1's Port-Address */

static void reset(void)
{
    for (int i = 0; i < rb.n; i++) free(rb.items[(rb.head + i) % RB_MAX].p);
    memset(&s_br, 0, sizeof(s_br));
    memset(&fk, 0, sizeof(fk));
    T = 1000000;
    rdm_bridge_start();
    rdm_bridge_set_port_address(1, PA);
    supervise();                /* port 1 becomes an RDM gateway */
}

/* An ArtRdm request as the receiver hands it over: from the sub-start code,
 * no checksum */
static void request(uint8_t cc, uint16_t pid, uint8_t pdl)
{
    static uint8_t r[RDM_MAX_PACKET];
    memset(r, 0, sizeof(r));
    r[0] = RDM_SUB_SC;
    r[1] = (uint8_t)(24 + pdl);
    r[2] = 0x4C; r[3] = 0x44; r[6] = 0x12; r[7] = 0x34;
    r[15] = 1;
    r[19] = cc;
    r[20] = (uint8_t)(pid >> 8);
    r[21] = (uint8_t)pid;
    r[22] = pdl;
    artnet_rdm_event_t ev = {.type = ARTNET_RDM_EV_RDM, .source_ip = 0x0A00000A, .port_address = PA,
                             .rdm = r, .rdm_len = (uint16_t)(23 + pdl)};
    s_rdm_cb(&ev, NULL);
}
static void run_queue(void) { while (rb.n) bridge_step(0); }

/* ---- the queue --------------------------------------------------------- */

static void test_burst_of_gets_all_answered(void)
{
    for (int i = 0; i < 30; i++) request(RDM_CC_GET_COMMAND, 0x0082, 0);  /* the old queue took 8 */
    CHECK_EQ(s_br.stats.rdm_queue_full, 0);
    run_queue();
    CHECK_EQ(fk.transactions, 30);
    CHECK_EQ(fk.rdm_sent, 30);
    CHECK_EQ(rb.used, 0);
}

static void test_full_queue_counted(void)
{
    /* the largest requests: 24-byte header + 257 = 292 with the ring's own */
    int fit = 0;
    for (int i = 0; i < 20; i++) {
        uint32_t before = s_br.stats.rdm_queue_full;
        request(RDM_CC_SET_COMMAND, 0x1000, 231);
        if (s_br.stats.rdm_queue_full == before) fit++;
    }
    CHECK_EQ(fit, BRIDGE_QUEUE_BYTES / 292);
    CHECK_EQ(s_br.stats.rdm_queue_full, 20 - fit);
    run_queue();
    CHECK_EQ(fk.transactions, fit);
}

static void test_old_request_dropped(void)
{
    request(RDM_CC_GET_COMMAND, 0x0082, 0);
    T += 3500000;
    run_queue();
    CHECK_EQ(fk.transactions, 0);
    CHECK_EQ(s_br.stats.rdm_expired, 1);
    request(RDM_CC_GET_COMMAND, 0x0082, 0);
    T += 2500000;
    run_queue();
    CHECK_EQ(fk.transactions, 1);
}

static void test_tod_request_addresses_kept(void)
{
    artnet_rdm_event_t ev = {.type = ARTNET_RDM_EV_TOD_REQUEST, .source_ip = 0x0A00000A,
                             .address_count = 3, .addresses = {0x0105, 0x0002, PA}};
    s_rdm_cb(&ev, NULL);
    run_queue();
    CHECK_EQ(fk.tod_sent, 1);
    ev.address_count = 2;       /* PA not among them */
    s_rdm_cb(&ev, NULL);
    run_queue();
    CHECK_EQ(fk.tod_sent, 1);
}

static void test_discovery_done_announced(void)
{
    artnet_rdm_event_t ev = {.type = ARTNET_RDM_EV_TOD_REQUEST, .source_ip = 0x0A00000A,
                             .address_count = 1, .addresses = {PA}};
    s_rdm_cb(&ev, NULL);        /* the controller becomes a subscriber */
    run_queue();
    int sent = fk.tod_sent;
    s_disc_cb(1, 1, false, NULL);
    run_queue();
    CHECK(fk.tod_sent > sent);  /* the TOD changed from empty */
    CHECK(fk.notified > 0);
}

/* ---- asking again ------------------------------------------------------ */

static void test_lost_get_asked_again(void)
{
    fk.script[0] = ESP_ERR_TIMEOUT;
    request(RDM_CC_GET_COMMAND, 0x0082, 0);
    run_queue();
    CHECK_EQ(fk.transactions, 2);
    CHECK_EQ(fk.rdm_sent, 1);
    CHECK_EQ(s_br.stats.rdm_retries, 1);
    CHECK_EQ(s_br.stats.rdm_timeouts, 0);
}

static void test_garbled_reply_asked_again(void)
{
    fk.script[0] = ESP_ERR_INVALID_CRC;
    request(RDM_CC_GET_COMMAND, 0x00E0, 0);
    run_queue();
    CHECK_EQ(fk.transactions, 2);
    CHECK_EQ(fk.rdm_sent, 1);
}

static void test_lost_get_gives_up(void)
{
    for (int i = 0; i < 8; i++) fk.script[i] = ESP_ERR_TIMEOUT;
    request(RDM_CC_GET_COMMAND, 0x0082, 0);
    run_queue();
    CHECK_EQ(fk.transactions, RDM_GET_ATTEMPTS);
    CHECK_EQ(fk.rdm_sent, 0);
    CHECK_EQ(s_br.stats.rdm_timeouts, 1);
}

static void test_set_not_asked_again(void)
{
    fk.script[0] = ESP_ERR_TIMEOUT;
    request(RDM_CC_SET_COMMAND, 0x00F0, 2);
    run_queue();
    CHECK_EQ(fk.transactions, 1);
    CHECK_EQ(fk.last_pid, 0x00F0);
}

static void test_message_gets_not_asked_again(void)
{
    fk.script[0] = ESP_ERR_TIMEOUT;
    fk.script[1] = ESP_ERR_TIMEOUT;
    request(RDM_CC_GET_COMMAND, RDM_PID_QUEUED_MESSAGE, 0);
    request(RDM_CC_GET_COMMAND, RDM_PID_STATUS_MESSAGE, 0);
    run_queue();
    CHECK_EQ(fk.transactions, 2);
    CHECK_EQ(s_br.stats.rdm_retries, 0);
}

static void test_other_failures_not_asked_again(void)
{
    fk.script[0] = ESP_ERR_INVALID_RESPONSE;    /* another fixture's: the bus is busy */
    request(RDM_CC_GET_COMMAND, 0x0082, 0);
    run_queue();
    CHECK_EQ(fk.transactions, 1);
}

int main(void)
{
    RUN(test_burst_of_gets_all_answered);
    RUN(test_full_queue_counted);
    RUN(test_old_request_dropped);
    RUN(test_tod_request_addresses_kept);
    RUN(test_discovery_done_announced);
    RUN(test_lost_get_asked_again);
    RUN(test_garbled_reply_asked_again);
    RUN(test_lost_get_gives_up);
    RUN(test_set_not_asked_again);
    RUN(test_message_gets_not_asked_again);
    RUN(test_other_failures_not_asked_again);
    return test_summary();
}
