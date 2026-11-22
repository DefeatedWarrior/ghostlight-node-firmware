/**
 * @file dmx_handler.c
 * @brief DMX512 / RDM port driver — see dmx_handler.h
 *
 * Concurrency model (per port):
 *   - bus lock   : serialises everything that touches the line (DMX frame
 *                  write+send, every RDM transaction). esp_dmx's dmx_write()
 *                  is not protected by the driver mutex, so without this lock
 *                  an output frame could overwrite an in-flight RDM request.
 *   - data_mutex : protects the 512-slot buffer and the device list.
 *   - tasks stop cooperatively (run flag + "done" semaphore); they are never
 *     deleted from outside while they might hold a lock.
 */

#include "dmx_handler.h"
#include "esp_dmx.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "board_gpio.h"
#include "dmx/sniffer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "rdm/controller.h"
#include "rdm/responder.h"
#include "dmx/include/parameter.h"          /* dmx_parameter_commit() */
#include "rdm/include/driver.h"             /* rdm_uid_get() */
#include "rdm/include/uid.h"
#include "rdm/controller/include/utils.h"   /* rdm_send_request(), rdm_request_t */

static const char *TAG = "dmx_handler";

/* Hardware mapping (board.h) */
static const int8_t k_uart[DMX_PORT_MAX]    = BOARD_DMX_UART;
static const int8_t k_tx_pin[DMX_PORT_MAX]  = BOARD_DMX_TX_GPIO;
static const int8_t k_rx_pin[DMX_PORT_MAX]  = BOARD_DMX_RX_GPIO;
static const int8_t k_dir_pin[DMX_PORT_MAX] = BOARD_DMX_DIR_GPIO;
static const int8_t k_bias_pin[DMX_PORT_MAX] = BOARD_DMX_BIAS_GPIO;

/* Output tasks pull from the merge engine, which also logs there (all
 * sources lost, sync): 4 KB left 496 bytes free on the S3 */
#define DMX_OUT_STACK_SIZE    6144
/* Input tasks run the forwarding path too (Art-Net and sACN sends, merging
 * into local outputs, loss callbacks and their logging), next to two frame
 * buffers: 4 KB left under 900 bytes free on the S3 */
#define DMX_IN_STACK_SIZE     6144
#define DMX_TASK_PRIORITY     12          /* above network rx (6) */
#define DMX_TASK_CORE         BOARD_CORE_DMX   /* board.h: 1, or 0 on single-core chips */
#define DMX_DISC_STACK_SIZE   6144
#define DMX_DISC_PRIORITY     4

/* The task that installs a port's driver, see install_on_dmx_core() */
#define DMX_INSTALL_STACK_SIZE 6144
#define DMX_INSTALL_PRIORITY  6

#define DMX_RX_POLL_MS        200
#define RDM_SAVE_MS           2000        /* responder: save what RDM set this often */
#define DMX_TIMING_MAX_S      600         /* break/MAB measurement auto-off */

/* RDM (E1.20) controller timing window */
#define RDM_BREAK_MIN_US      176
#define RDM_BREAK_MAX_US      352
#define RDM_MAB_MAX_US        88
#define DMX_TASK_STOP_MS      1500
/* A raw request and its response: both full, 2.8 ms apart, take 26.3 ms, more
 * with gaps between slots. Without a response the driver gives up after 2.8 ms. */
#define RDM_EXCHANGE_MS       50
/* A fixture may ask for time (ACK_TIMER): most we wait per fixture, in all */
#define RDM_ACK_TIMER_MAX_MS  1000
/* Background discovery takes a fixture off the list after this many misses */
#define DISC_MISSES_MAX       2

#define DISC_IDLE_BIT         BIT0

typedef struct {
    uint8_t port_num;
    dmx_port_t dmx_num;
    int tx_pin, rx_pin, dir_pin;

    dmx_mode_t mode;
    uint16_t universe;
    bool rdm_enabled;
    bool is_configured;
    volatile bool is_active;
    volatile bool run;
    bool driver_installed;

    SemaphoreHandle_t bus;
    atomic_int rdm_waiting;         /* RDM requests waiting for the bus */
    SemaphoreHandle_t data_mutex;
    SemaphoreHandle_t out_done;
    SemaphoreHandle_t in_done;
    SemaphoreHandle_t trigger;      /* "send the next frame now" (sync) */
    EventGroupHandle_t disc_events;
    TaskHandle_t out_task;
    TaskHandle_t in_task;

    uint8_t buffer[DMX_CHANNEL_COUNT];
    dmx_frame_source_t source;
    void *source_ctx;

    dmx_port_stats_t stats;

    dmx_output_timing_t timing;     /* as requested */
    volatile bool timing_dirty;     /* output task re-applies break/MAB */

    dmx_input_analysis_t an;        /* input analyser (data_mutex) */
    uint32_t an_win_start;
    uint16_t an_win_frames, an_win_packets;
    uint32_t an_last_frame_ms;
    volatile uint32_t timing_until_ms;  /* break/MAB measurement end (0 = off) */
    bool sniffer_on;

    rdm_device_t *devices;          /* device_count long (data_mutex) */
    uint8_t dev_misses[DMX_MAX_DEVICES];    /* background passes that missed it */
    uint8_t device_count;
    volatile bool discovering;
    volatile bool disc_abort;
    dmx_disc_mode_t disc_mode;
    volatile bool tod_valid;        /* a discovery has completed since start */
    bool identify;

    dmx_rx_callback_t rx_cb;
    void *rx_ctx;
    dmx_loss_callback_t loss_cb;
    void *loss_ctx;
    rdm_discovery_callback_t disc_cb;
    void *disc_ctx;
    dmx_identify_callback_t ident_cb;
    void *ident_ctx;
} port_t;

static struct {
    bool initialized;
    SemaphoreHandle_t state_mutex;
    port_t ports[DMX_PORT_MAX];
} s_dmx;

static port_t *get_port(uint8_t port)
{
    if (!s_dmx.initialized || port < 1 || port > DMX_PORT_MAX) {
        return NULL;
    }
    return &s_dmx.ports[port - 1];
}

static inline bool is_output_mode(dmx_mode_t m)
{
    return m == DMX_MODE_OUTPUT || m == DMX_MODE_RDM_MASTER;
}

static inline bool is_input_mode(dmx_mode_t m)
{
    return m == DMX_MODE_INPUT || m == DMX_MODE_RDM_RESPONDER;
}

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ============================================================================
// Driver install / tasks
// ============================================================================

static esp_err_t install_driver(port_t *p)
{
    dmx_config_t config = DMX_CONFIG_DEFAULT;
    config.product_category = RDM_PRODUCT_CATEGORY_DATA;
    config.software_version_label = "ESP-NODE-2RDM";

    /* A personality is needed for RDM responder mode (DMX_START_ADDRESS etc.) */
    dmx_personality_t personalities[] = {
        {512, "Full universe"},
    };
    int personality_count = (p->mode == DMX_MODE_RDM_RESPONDER) ? 1 : 0;

    if (!dmx_driver_install(p->dmx_num, &config,
                            personality_count ? personalities : NULL,
                            personality_count)) {
        ESP_LOGE(TAG, "Port %u: dmx_driver_install failed", p->port_num);
        return ESP_FAIL;
    }
    if (!dmx_set_pin(p->dmx_num, p->tx_pin, p->rx_pin, p->dir_pin)) {
        ESP_LOGE(TAG, "Port %u: dmx_set_pin failed", p->port_num);
        dmx_driver_delete(p->dmx_num);
        return ESP_FAIL;
    }
    p->driver_installed = true;
    p->timing_dirty = true;
    const rdm_uid_t *uid = rdm_uid_get(p->dmx_num);
    if (uid) {
        ESP_LOGI(TAG, "Port %u: driver installed (UART%d, UID " UIDSTR ")",
                 p->port_num, (int)p->dmx_num, UID2STR(*uid));
    }
    return ESP_OK;
}

static bool uninstall_driver(port_t *p)
{
    if (!p->driver_installed) return true;
    /* It refuses while a task is inside it, which is over within a packet */
    for (int i = 0; i < 10; i++) {
        if (!dmx_driver_is_installed(p->dmx_num) ||
            dmx_driver_delete(p->dmx_num)) {    /* also removes the sniffer ISR */
            p->driver_installed = false;
            p->sniffer_on = false;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGE(TAG, "Port %u: the driver is busy and wasn't removed", p->port_num);
    return false;
}

typedef struct {
    port_t *p;
    esp_err_t err;
    SemaphoreHandle_t done;
} install_job_t;

static void install_task(void *arg)
{
    install_job_t *job = (install_job_t *)arg;
    job->err = install_driver(job->p);
    xSemaphoreGive(job->done);
    vTaskDelete(NULL);
}

/*
 * ESP-IDF puts an interrupt on the core that asks for it. Ports are started
 * from the network side, which put the DMX timer and UART interrupts on the
 * Wi-Fi core, so install from a task pinned to the DMX core. (Checking the
 * caller's core wouldn't do: one that isn't pinned can move half-way.)
 */
static esp_err_t install_on_dmx_core(port_t *p)
{
    install_job_t job = {.p = p, .err = ESP_FAIL, .done = xSemaphoreCreateBinary()};
    if (job.done) {
        if (xTaskCreatePinnedToCore(install_task, "dmx_install", DMX_INSTALL_STACK_SIZE, &job,
                                    DMX_INSTALL_PRIORITY, NULL, DMX_TASK_CORE) == pdPASS) {
            xSemaphoreTake(job.done, portMAX_DELAY);
            vSemaphoreDelete(job.done);
            return job.err;
        }
        vSemaphoreDelete(job.done);
    }
    ESP_LOGW(TAG, "Port %u: no memory for the install task, interrupts stay on this core",
             p->port_num);
    return install_driver(p);
}

static bool port_has_rdm(const port_t *p)
{
    return p->mode == DMX_MODE_RDM_MASTER || (p->mode == DMX_MODE_OUTPUT && p->rdm_enabled);
}

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Effective timing for the port's mode (RDM needs a tighter window) */
static dmx_output_timing_t effective_timing(const port_t *p)
{
    dmx_output_timing_t t = p->timing;
    t.rate_hz  = (uint8_t)clamp_u32(t.rate_hz, CONFIG_DMX_RATE_MIN, CONFIG_DMX_RATE_MAX);
    t.slots    = (uint16_t)clamp_u32(t.slots, CONFIG_DMX_SLOTS_MIN, CONFIG_DMX_SLOTS_MAX);
    if (port_has_rdm(p)) {
        t.break_us = (uint16_t)clamp_u32(t.break_us, RDM_BREAK_MIN_US, RDM_BREAK_MAX_US);
        t.mab_us   = (uint16_t)clamp_u32(t.mab_us, CONFIG_DMX_MAB_MIN, RDM_MAB_MAX_US);
    } else {
        t.break_us = (uint16_t)clamp_u32(t.break_us, CONFIG_DMX_BREAK_MIN, CONFIG_DMX_BREAK_MAX);
        t.mab_us   = (uint16_t)clamp_u32(t.mab_us, CONFIG_DMX_MAB_MIN, CONFIG_DMX_MAB_MAX);
    }
    return t;
}

/* Frame period: the requested rate, but never shorter than the frame itself
 * takes on the wire (break + MAB + 44 us per slot incl. start code), so the
 * default 44 Hz / 512 slots gives 23 ms frames with a short idle gap */
static TickType_t period_ticks(const dmx_output_timing_t *t)
{
    uint32_t ms = 1000u / (t->rate_hz ? t->rate_hz : CONFIG_DMX_RATE_DEFAULT);
    uint32_t wire_us = (uint32_t)t->break_us + t->mab_us + ((uint32_t)t->slots + 1u) * 44u;
    uint32_t wire_ms = (wire_us + 999u) / 1000u;
    if (ms < wire_ms) ms = wire_ms;
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return ticks ? ticks : 1;
}

static void output_task(void *arg)
{
    port_t *p = (port_t *)arg;
    uint8_t frame[DMX_FRAME_SIZE];
    frame[0] = 0x00;   /* NULL start code */
    memset(&frame[1], 0, DMX_CHANNEL_COUNT);

    dmx_output_timing_t tm = effective_timing(p);
    TickType_t period = period_ticks(&tm);
    TickType_t next = xTaskGetTickCount() + period;
    xSemaphoreTake(p->trigger, 0);
    esp_task_wdt_add(NULL);

    ESP_LOGI(TAG, "Port %u: output task running", p->port_num);

    while (p->run) {
        esp_task_wdt_reset();
        if (p->timing_dirty) {
            p->timing_dirty = false;
            tm = effective_timing(p);
            period = period_ticks(&tm);
            xSemaphoreTake(p->bus, portMAX_DELAY);
            dmx_wait_sent(p->dmx_num, pdMS_TO_TICKS(50));
            dmx_set_break_len(p->dmx_num, tm.break_us);
            dmx_set_mab_len(p->dmx_num, tm.mab_us);
            xSemaphoreGive(p->bus);
            ESP_LOGI(TAG, "Port %u: output %u Hz, break %u us, MAB %u us, %u slots",
                     p->port_num, tm.rate_hz, tm.break_us, tm.mab_us, tm.slots);
        }
        dmx_frame_source_t src = p->source;
        if (src) {
            src(p->port_num, &frame[1], p->source_ctx);
        } else {
            xSemaphoreTake(p->data_mutex, portMAX_DELAY);
            memcpy(&frame[1], p->buffer, DMX_CHANNEL_COUNT);
            xSemaphoreGive(p->data_mutex);
        }

        xSemaphoreTake(p->bus, portMAX_DELAY);
        dmx_wait_sent(p->dmx_num, pdMS_TO_TICKS(50));
        dmx_write(p->dmx_num, frame, (size_t)tm.slots + 1);
        size_t sent = dmx_send_num(p->dmx_num, (size_t)tm.slots + 1);
        xSemaphoreGive(p->bus);
        /* An RDM request is waiting: step aside so it gets the bus before
         * the next frame. Otherwise, when this task is catching up after a
         * late frame, it takes the bus straight back (higher priority) and
         * RDM can wait seconds. */
        if (atomic_load(&p->rdm_waiting) > 0) {
            vTaskDelay(1);
        }

        if (sent) {
            p->stats.frames_sent++;
            p->stats.last_frame_time_ms = now_ms();
        }
        if (!src) {
            /* keep the buffer readable for status pages */
        } else {
            xSemaphoreTake(p->data_mutex, portMAX_DELAY);
            memcpy(p->buffer, &frame[1], DMX_CHANNEL_COUNT);
            xSemaphoreGive(p->data_mutex);
        }
        /* Wait for the next frame slot, or less if a sync trigger arrives
         * (the frame then goes out as soon as the current one has left) */
        TickType_t now = xTaskGetTickCount();
        TickType_t wait = ((int32_t)(next - now) > 0) ? (next - now) : 0;
        if (xSemaphoreTake(p->trigger, wait) == pdTRUE) {
            next = xTaskGetTickCount() + period;
        } else {
            next += period;
            now = xTaskGetTickCount();
            if ((int32_t)(now - next) > (int32_t)period) {
                next = now + period;   /* fell behind (long RDM transaction) */
            }
        }
    }

    ESP_LOGI(TAG, "Port %u: output task stopped", p->port_num);
    esp_task_wdt_delete(NULL);
    xSemaphoreGive(p->out_done);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Input analyser (runs in the input task)
// ---------------------------------------------------------------------------

static void analysis_clear(port_t *p)
{
    bool on = p->an.timing_on;
    memset(&p->an, 0, sizeof(p->an));
    p->an.timing_on = on;
    p->an_win_start = now_ms();
    p->an_win_frames = p->an_win_packets = 0;
    p->an_last_frame_ms = 0;
}

/* Close the 1 s rate window */
static void analysis_window(port_t *p, uint32_t t)
{
    uint32_t el = t - p->an_win_start;
    if (el < 1000) return;
    xSemaphoreTake(p->data_mutex, portMAX_DELAY);
    p->an.refresh_hz = (uint16_t)((p->an_win_frames * 1000u + el / 2) / el);
    p->an.packets_per_s = (uint16_t)((p->an_win_packets * 1000u + el / 2) / el);
    xSemaphoreGive(p->data_mutex);
    p->an_win_frames = p->an_win_packets = 0;
    p->an_win_start = t;
}

static void analysis_packet(port_t *p, const dmx_packet_t *pkt)
{
    xSemaphoreTake(p->data_mutex, portMAX_DELAY);
    p->an_win_packets++;
    switch (pkt->err) {
    case DMX_ERR_IMPROPER_SLOT:    p->an.err_framing++; break;
    case DMX_ERR_UART_OVERFLOW:    p->an.err_overflow++; break;
    case DMX_ERR_NOT_ENOUGH_SLOTS: p->an.err_short++; break;
    default: break;
    }
    if (pkt->is_rdm) {
        p->an.rdm_packets++;
    } else if (pkt->err == DMX_OK && pkt->sc > 0x00 && pkt->sc <= 0xFF) {
        p->an.alt_start_codes++;
        p->an.last_alt_start_code = (uint8_t)pkt->sc;
    }
    xSemaphoreGive(p->data_mutex);
}

static void analysis_frame(port_t *p, uint16_t slots, uint32_t t)
{
    xSemaphoreTake(p->data_mutex, portMAX_DELAY);
    p->an_win_frames++;
    p->an.frames++;
    p->an.slots_last = slots;
    if (p->an.slots_min == 0 || slots < p->an.slots_min) p->an.slots_min = slots;
    if (slots > p->an.slots_max) p->an.slots_max = slots;
    if (p->an_last_frame_ms) {
        uint32_t iv = t - p->an_last_frame_ms;
        if (iv < DMX_INPUT_LOSS_MS) {   /* ignore gaps from signal loss */
            if (p->an.interval_min_ms == 0 || iv < p->an.interval_min_ms) p->an.interval_min_ms = iv;
            if (iv > p->an.interval_max_ms) p->an.interval_max_ms = iv;
        }
    }
    p->an_last_frame_ms = t;
    if (p->sniffer_on) {
        dmx_metadata_t md;
        if (dmx_sniffer_get_data(p->dmx_num, &md)) {
            p->an.break_us = md.break_len;
            p->an.mab_us = md.mab_len;
        }
    }
    xSemaphoreGive(p->data_mutex);
}

/* Switch the break/MAB sniffer on or off as requested */
static void sniffer_service(port_t *p)
{
    uint32_t until = p->timing_until_ms;
    bool want = until && (int32_t)(until - now_ms()) > 0;
    if (want && !p->sniffer_on) {
        esp_err_t r = board_gpio_isr_service();
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "Port %u: GPIO ISR service failed (%d)", p->port_num, r);
            p->timing_until_ms = 0;
            return;
        }
        if (dmx_sniffer_enable(p->dmx_num, p->rx_pin)) {
            p->sniffer_on = true;
            ESP_LOGI(TAG, "Port %u: break/MAB measurement on", p->port_num);
        } else {
            p->timing_until_ms = 0;
        }
    } else if (!want && p->sniffer_on) {
        dmx_sniffer_disable(p->dmx_num);
        p->sniffer_on = false;
        p->timing_until_ms = 0;
        ESP_LOGI(TAG, "Port %u: break/MAB measurement off", p->port_num);
    }
    p->an.timing_on = p->sniffer_on;
}

static void input_task(void *arg)
{
    port_t *p = (port_t *)arg;
    uint8_t frame[DMX_FRAME_SIZE];
    uint8_t slots_buf[DMX_CHANNEL_COUNT];   /* the last frame forwarded */
    uint32_t last_seq = 0;   /* the driver's number of the last frame used */
    bool holding = false;    /* slots_buf holds a frame from the current signal */
    uint32_t saved_ms = now_ms();
    memset(slots_buf, 0, sizeof(slots_buf));

    ESP_LOGI(TAG, "Port %u: input task running (%s)", p->port_num,
             p->mode == DMX_MODE_RDM_RESPONDER ? "RDM responder" : "DMX input");
    esp_task_wdt_add(NULL);
    p->an_win_start = now_ms();

    while (p->run) {
        esp_task_wdt_reset();
        sniffer_service(p);

        dmx_packet_t pkt = { .err = DMX_ERR_TIMEOUT };
        size_t size = dmx_receive(p->dmx_num, &pkt, pdMS_TO_TICKS(DMX_RX_POLL_MS));
        /* Level frames come from the copy the driver's interrupt saves as
         * each frame ends, not from what dmx_receive() reports: the next
         * frame, or a stray break, starts overwriting the driver's buffer
         * well under a millisecond later, so by the time this task looks
         * that buffer can hold a mix of two frames, or nothing (a break
         * right after a frame made dmx_receive() report it as empty) */
        uint32_t seq = 0;
        size_t frame_len = dmx_read_received(p->dmx_num, frame, DMX_FRAME_SIZE, &seq);
        bool fresh = seq != 0 && seq != last_seq && frame_len >= 2;
        uint32_t t = now_ms();
        analysis_window(p, t);

        /* size is 0 when the next break got in first, which is always so
         * for a frame cut short: count those by the frame that was saved */
        if (size > 0 || (fresh && pkt.err != DMX_ERR_TIMEOUT)) {
            analysis_packet(p, &pkt);
        }
        if (size > 0) {
            if (pkt.is_rdm) {
                if (p->mode == DMX_MODE_RDM_RESPONDER && pkt.err == DMX_OK) {
                    rdm_send_response(p->dmx_num);
                    p->stats.rdm_responder_requests++;
                    bool ident = false;
                    if (rdm_get_identify_device(p->dmx_num, &ident) && ident != p->identify) {
                        p->identify = ident;
                        ESP_LOGI(TAG, "Port %u: RDM identify %s", p->port_num, ident ? "ON" : "OFF");
                        dmx_identify_callback_t icb = p->ident_cb;
                        if (icb) icb(p->port_num, ident, p->ident_ctx);
                    }
                }
            } else if (pkt.err != DMX_OK && pkt.err != DMX_ERR_NOT_ENOUGH_SLOTS) {
                p->stats.error_count++;
            }
        }
        /* The driver only saves what RDM set when a receive times out, which
         * never happens while DMX keeps arriving */
        if (p->mode == DMX_MODE_RDM_RESPONDER && p->run && t - saved_ms >= RDM_SAVE_MS) {
            saved_ms = t;
            while (dmx_parameter_commit(p->dmx_num)) {
            }
        }

        if (!fresh) {
            if (p->stats.signal_present &&
                (t - p->stats.last_frame_time_ms) > DMX_INPUT_LOSS_MS) {
                p->stats.signal_present = false;
                holding = false;
                ESP_LOGW(TAG, "Port %u: DMX input signal lost", p->port_num);
                dmx_loss_callback_t lcb = p->loss_cb;
                if (lcb) lcb(p->port_num, p->loss_ctx);
            }
            continue;
        }

        if (last_seq != 0 && seq - last_seq > 1) {
            /* more than a frame late: the newer frame replaced the others */
            p->an.dropped_late += seq - last_seq - 1;
        }
        last_seq = seq;
        uint16_t slots = (uint16_t)(frame_len - 1);

        /* Slots a short frame leaves out keep their last level, as fixtures
         * do, so a frame cut short (a glitch, a cable pulled mid-frame)
         * doesn't flash the channels above it to zero. After a signal loss
         * they start from zero, for whatever source comes next. */
        memcpy(slots_buf, &frame[1], slots);
        if (slots < DMX_CHANNEL_COUNT && !holding) {
            memset(slots_buf + slots, 0, DMX_CHANNEL_COUNT - slots);
        }
        holding = true;

        xSemaphoreTake(p->data_mutex, portMAX_DELAY);
        memcpy(p->buffer, slots_buf, DMX_CHANNEL_COUNT);
        xSemaphoreGive(p->data_mutex);

        if (!p->stats.signal_present) {
            ESP_LOGI(TAG, "Port %u: DMX input signal present (%u slots)", p->port_num, slots);
        }
        p->stats.frames_received++;
        p->stats.last_frame_time_ms = t;
        p->stats.last_slot_count = slots;
        p->stats.signal_present = true;
        analysis_frame(p, slots, t);

        dmx_rx_callback_t cb = p->rx_cb;
        if (cb) {
            cb(p->port_num, slots_buf, DMX_CHANNEL_COUNT, p->rx_ctx);
        }
    }

    ESP_LOGI(TAG, "Port %u: input task stopped", p->port_num);
    esp_task_wdt_delete(NULL);
    xSemaphoreGive(p->in_done);
    vTaskDelete(NULL);
}

/* Line bias (boards with a bias switch): on while the port drives the bus.
 * E1.20 has the controller bias the line, so RDM response windows (nobody
 * driving) read as idle instead of noise. */
static void set_bias(const port_t *p, bool on)
{
    int pin = k_bias_pin[p->port_num - 1];
    if (pin >= 0) {
        gpio_set_level(pin, on ? 1 : 0);
    }
}

/* Caller holds state_mutex */
static void stop_port_locked(port_t *p)
{
    if (!p->is_active) {
        return;
    }
    set_bias(p, false);

    /* Discovery, the DMX tasks and RDM requests all use the driver. Ask
     * discovery to stop, then stop the DMX task first: a busy output task
     * can keep discovery off the bus. */
    if (p->discovering) {
        p->disc_abort = true;
    }

    p->run = false;
    if (p->out_task) {
        if (xSemaphoreTake(p->out_done, pdMS_TO_TICKS(DMX_TASK_STOP_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "Port %u: output task did not stop", p->port_num);
        }
        p->out_task = NULL;
    }
    if (p->in_task) {
        if (xSemaphoreTake(p->in_done, pdMS_TO_TICKS(DMX_TASK_STOP_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "Port %u: input task did not stop", p->port_num);
        }
        p->in_task = NULL;
    }

    /* Never remove the driver under a running discovery. With the DMX task
     * gone it ends within one RDM transaction. */
    while (!(xEventGroupWaitBits(p->disc_events, DISC_IDLE_BIT, pdFALSE, pdTRUE,
                                 pdMS_TO_TICKS(2000)) & DISC_IDLE_BIT)) {
        ESP_LOGW(TAG, "Port %u: waiting for RDM discovery to stop", p->port_num);
    }

    /* Holding the bus: no RDM request is half-way through (they check
     * rdm_bus_ok() once they have it) */
    xSemaphoreTake(p->bus, portMAX_DELAY);
    uninstall_driver(p);
    p->is_active = false;
    xSemaphoreGive(p->bus);
    p->tod_valid = false;
    p->stats.signal_present = false;
    ESP_LOGI(TAG, "Port %u stopped", p->port_num);
}

// ============================================================================
// Init / configuration
// ============================================================================

esp_err_t dmx_handler_init(void)
{
    if (s_dmx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_dmx, 0, sizeof(s_dmx));
    s_dmx.state_mutex = xSemaphoreCreateMutex();
    if (!s_dmx.state_mutex) {
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < DMX_PORT_MAX; i++) {
        port_t *p = &s_dmx.ports[i];
        p->port_num = i + 1;
        p->dmx_num = (dmx_port_t)k_uart[i];
        p->tx_pin = k_tx_pin[i];
        p->rx_pin = k_rx_pin[i];
        p->dir_pin = k_dir_pin[i];
        p->mode = DMX_MODE_DISABLED;
        p->timing = (dmx_output_timing_t){
            .rate_hz = CONFIG_DMX_RATE_DEFAULT, .break_us = CONFIG_DMX_BREAK_DEFAULT,
            .mab_us = CONFIG_DMX_MAB_DEFAULT, .slots = CONFIG_DMX_SLOTS_MAX,
        };
        p->bus = xSemaphoreCreateMutex();
        p->data_mutex = xSemaphoreCreateMutex();
        p->out_done = xSemaphoreCreateBinary();
        p->in_done = xSemaphoreCreateBinary();
        p->trigger = xSemaphoreCreateBinary();
        p->disc_events = xEventGroupCreate();
        if (!p->bus || !p->data_mutex || !p->out_done || !p->in_done || !p->trigger ||
            !p->disc_events) {
            return ESP_ERR_NO_MEM;
        }
        xEventGroupSetBits(p->disc_events, DISC_IDLE_BIT);
        if (k_bias_pin[i] >= 0) {
            gpio_reset_pin(k_bias_pin[i]);
            gpio_set_direction(k_bias_pin[i], GPIO_MODE_OUTPUT);
            gpio_set_level(k_bias_pin[i], 0);   /* no bias until a port drives the bus */
        }
    }

    s_dmx.initialized = true;
    for (int i = 0; i < DMX_PORT_MAX; i++) {
        ESP_LOGI(TAG, "Port %d: UART%d TX%d RX%d DIR%d", i + 1, k_uart[i], k_tx_pin[i],
                 k_rx_pin[i], k_dir_pin[i]);
    }
    return ESP_OK;
}

esp_err_t dmx_handler_deinit(void)
{
    if (!s_dmx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    for (int i = 0; i < DMX_PORT_MAX; i++) {
        dmx_handler_stop_port(i + 1);
    }
    for (int i = 0; i < DMX_PORT_MAX; i++) {
        port_t *p = &s_dmx.ports[i];
        free(p->devices);
        vSemaphoreDelete(p->bus);
        vSemaphoreDelete(p->data_mutex);
        vSemaphoreDelete(p->out_done);
        vSemaphoreDelete(p->in_done);
        vSemaphoreDelete(p->trigger);
        vEventGroupDelete(p->disc_events);
    }
    vSemaphoreDelete(s_dmx.state_mutex);
    s_dmx.initialized = false;
    return ESP_OK;
}

/* Caller holds data_mutex */
static void devices_clear(port_t *p)
{
    free(p->devices);
    p->devices = NULL;
    p->device_count = 0;
}

esp_err_t dmx_handler_configure_port(uint8_t port, const port_config_t *config)
{
    port_t *p = get_port(port);
    if (!p || !config) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->mode > DMX_MODE_RDM_RESPONDER) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_dmx.state_mutex, portMAX_DELAY);
    stop_port_locked(p);
    p->mode = config->mode;
    p->universe = config->universe_primary;
    p->rdm_enabled = config->rdm_enabled;
    p->is_configured = true;
    p->timing = (dmx_output_timing_t){
        .rate_hz = config->out_rate_hz, .break_us = config->break_us,
        .mab_us = config->mab_us, .slots = config->out_slots,
    };
    p->timing_until_ms = 0;

    xSemaphoreTake(p->data_mutex, portMAX_DELAY);
    memset(p->buffer, 0, DMX_CHANNEL_COUNT);
    devices_clear(p);
    p->tod_valid = false;
    xSemaphoreGive(p->data_mutex);
    xSemaphoreGive(s_dmx.state_mutex);

    ESP_LOGI(TAG, "Port %u configured: mode %d, RDM %s", port, config->mode,
             config->rdm_enabled ? "on" : "off");
    return ESP_OK;
}

esp_err_t dmx_handler_start_port(uint8_t port)
{
    port_t *p = get_port(port);
    if (!p) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!p->is_configured) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_dmx.state_mutex, portMAX_DELAY);
    if (p->is_active || p->mode == DMX_MODE_DISABLED) {
        xSemaphoreGive(s_dmx.state_mutex);
        return ESP_OK;
    }

    /* Still there if the last stop couldn't remove it */
    xSemaphoreTake(p->bus, portMAX_DELAY);
    esp_err_t ret = uninstall_driver(p) ? install_on_dmx_core(p) : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(p->bus);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_dmx.state_mutex);
        return ret;
    }

    xSemaphoreTake(p->out_done, 0);
    xSemaphoreTake(p->in_done, 0);
    p->run = true;
    p->stats.signal_present = false;
    analysis_clear(p);
    p->an.timing_on = false;

    char name[16];
    BaseType_t ok;
    if (is_output_mode(p->mode)) {
        snprintf(name, sizeof(name), "dmx_out_%u", port);
        ok = xTaskCreatePinnedToCore(output_task, name, DMX_OUT_STACK_SIZE, p,
                                     DMX_TASK_PRIORITY, &p->out_task, DMX_TASK_CORE);
    } else {
        snprintf(name, sizeof(name), "dmx_in_%u", port);
        ok = xTaskCreatePinnedToCore(input_task, name, DMX_IN_STACK_SIZE, p,
                                     DMX_TASK_PRIORITY, &p->in_task, DMX_TASK_CORE);
    }
    if (ok != pdPASS) {
        p->run = false;
        p->out_task = NULL;
        p->in_task = NULL;
        uninstall_driver(p);
        xSemaphoreGive(s_dmx.state_mutex);
        ESP_LOGE(TAG, "Port %u: task create failed", port);
        return ESP_ERR_NO_MEM;
    }

    p->is_active = true;
    set_bias(p, is_output_mode(p->mode));
    xSemaphoreGive(s_dmx.state_mutex);
    ESP_LOGI(TAG, "Port %u started", port);
    return ESP_OK;
}

esp_err_t dmx_handler_stop_port(uint8_t port)
{
    port_t *p = get_port(port);
    if (!p) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_dmx.state_mutex, portMAX_DELAY);
    stop_port_locked(p);
    xSemaphoreGive(s_dmx.state_mutex);
    return ESP_OK;
}

// ============================================================================
// Data
// ============================================================================

esp_err_t dmx_handler_set_output_timing(uint8_t port, const dmx_output_timing_t *timing)
{
    port_t *p = get_port(port);
    if (!p || !timing) return ESP_ERR_INVALID_ARG;
    p->timing = *timing;
    p->timing_dirty = true;
    return ESP_OK;
}

esp_err_t dmx_handler_get_output_timing(uint8_t port, dmx_output_timing_t *timing)
{
    port_t *p = get_port(port);
    if (!p || !timing) return ESP_ERR_INVALID_ARG;
    *timing = effective_timing(p);
    return ESP_OK;
}

esp_err_t dmx_handler_get_analysis(uint8_t port, dmx_input_analysis_t *out)
{
    port_t *p = get_port(port);
    if (!p || !out) return ESP_ERR_INVALID_ARG;
    if (!is_input_mode(p->mode) || !p->is_active) return ESP_ERR_INVALID_STATE;
    uint32_t t = now_ms();
    xSemaphoreTake(p->data_mutex, portMAX_DELAY);
    *out = p->an;
    xSemaphoreGive(p->data_mutex);
    /* Rates decay to zero when the signal stops (the window only closes
     * when the input task wakes up) */
    if (!p->stats.signal_present || (t - p->stats.last_frame_time_ms) > DMX_INPUT_LOSS_MS) {
        out->refresh_hz = 0;
        out->packets_per_s = 0;
    }
    uint32_t until = p->timing_until_ms;
    out->timing_left_s = (out->timing_on && until && (int32_t)(until - t) > 0)
                             ? (until - t + 999) / 1000 : 0;
    return ESP_OK;
}

esp_err_t dmx_handler_reset_analysis(uint8_t port)
{
    port_t *p = get_port(port);
    if (!p) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(p->data_mutex, portMAX_DELAY);
    analysis_clear(p);
    xSemaphoreGive(p->data_mutex);
    return ESP_OK;
}

esp_err_t dmx_handler_measure_timing(uint8_t port, uint32_t seconds)
{
    port_t *p = get_port(port);
    if (!p) return ESP_ERR_INVALID_ARG;
    if (!is_input_mode(p->mode) || !p->is_active) return ESP_ERR_INVALID_STATE;
    if (seconds > DMX_TIMING_MAX_S) seconds = DMX_TIMING_MAX_S;
    uint32_t until = seconds ? now_ms() + seconds * 1000u : 0;
    if (seconds && until == 0) until = 1;
    p->timing_until_ms = until;   /* the input task switches the sniffer */
    return ESP_OK;
}

esp_err_t dmx_handler_get_levels(uint8_t port, uint8_t data[DMX_CHANNEL_COUNT])
{
    port_t *p = get_port(port);
    if (!p || !data) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(p->data_mutex, portMAX_DELAY);
    memcpy(data, p->buffer, DMX_CHANNEL_COUNT);
    xSemaphoreGive(p->data_mutex);
    return ESP_OK;
}

esp_err_t dmx_handler_send_dmx(uint8_t port, const uint8_t *data)
{
    port_t *p = get_port(port);
    if (!p || !data) return ESP_ERR_INVALID_ARG;
    if (!is_output_mode(p->mode)) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(p->data_mutex, portMAX_DELAY);
    memcpy(p->buffer, data, DMX_CHANNEL_COUNT);
    xSemaphoreGive(p->data_mutex);
    return ESP_OK;
}

esp_err_t dmx_handler_set_frame_source(uint8_t port, dmx_frame_source_t source, void *user_data)
{
    port_t *p = get_port(port);
    if (!p) return ESP_ERR_INVALID_ARG;
    p->source = NULL;
    p->source_ctx = user_data;
    p->source = source;
    return ESP_OK;
}

esp_err_t dmx_handler_trigger_output(uint8_t port)
{
    port_t *p = get_port(port);
    if (!p) return ESP_ERR_INVALID_ARG;
    if (!p->is_active || !is_output_mode(p->mode)) return ESP_ERR_INVALID_STATE;
    xSemaphoreGive(p->trigger);
    return ESP_OK;
}

esp_err_t dmx_handler_read_dmx(uint8_t port, uint8_t *data, uint32_t timeout_ms)
{
    port_t *p = get_port(port);
    if (!p || !data) return ESP_ERR_INVALID_ARG;
    if (!is_input_mode(p->mode)) return ESP_ERR_INVALID_STATE;
    if (timeout_ms > 0 && (now_ms() - p->stats.last_frame_time_ms) > timeout_ms) {
        return ESP_ERR_TIMEOUT;
    }
    xSemaphoreTake(p->data_mutex, portMAX_DELAY);
    memcpy(data, p->buffer, DMX_CHANNEL_COUNT);
    xSemaphoreGive(p->data_mutex);
    return ESP_OK;
}

esp_err_t dmx_handler_set_channel(uint8_t port, uint16_t channel, uint8_t value)
{
    port_t *p = get_port(port);
    if (!p || channel < 1 || channel > DMX_CHANNEL_COUNT) return ESP_ERR_INVALID_ARG;
    if (!is_output_mode(p->mode)) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(p->data_mutex, portMAX_DELAY);
    p->buffer[channel - 1] = value;
    xSemaphoreGive(p->data_mutex);
    return ESP_OK;
}

esp_err_t dmx_handler_set_channels(uint8_t port, uint16_t start_channel,
                                   const uint8_t *data, uint16_t length)
{
    port_t *p = get_port(port);
    if (!p || !data || length == 0 || start_channel < 1 ||
        length > DMX_CHANNEL_COUNT ||
        start_channel > (DMX_CHANNEL_COUNT - length + 1)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!is_output_mode(p->mode)) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(p->data_mutex, portMAX_DELAY);
    memcpy(&p->buffer[start_channel - 1], data, length);
    xSemaphoreGive(p->data_mutex);
    return ESP_OK;
}

esp_err_t dmx_handler_blackout(uint8_t port)
{
    port_t *p = get_port(port);
    if (!p) return ESP_ERR_INVALID_ARG;
    if (!is_output_mode(p->mode)) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(p->data_mutex, portMAX_DELAY);
    memset(p->buffer, 0, DMX_CHANNEL_COUNT);
    xSemaphoreGive(p->data_mutex);
    return ESP_OK;
}

// ============================================================================
// RDM controller
// ============================================================================

bool dmx_handler_rdm_available(uint8_t port)
{
    port_t *p = get_port(port);
    if (!p || !p->is_active) return false;
    return p->mode == DMX_MODE_RDM_MASTER ||
           (p->mode == DMX_MODE_OUTPUT && p->rdm_enabled);
}

/* esp_dmx leaves the ack untouched on early failures, and ACK == 0, so a
 * zero-initialised ack would read as success. Always start from "no response". */
#define ACK_INIT ((rdm_ack_t){.err = DMX_OK, .type = RDM_RESPONSE_TYPE_NONE})

/* Hard cap on discovery transactions so a misbehaving responder can never
 * keep discovery running forever */
#define DISC_MAX_TRANSACTIONS 4000

/* The port can send an RDM request now. Checked with the bus held:
 * stop_port_locked() takes the bus before it removes the driver, and the
 * port may have been reconfigured since the caller's first check. */
static bool rdm_bus_ok(const port_t *p)
{
    return p->is_active && p->driver_installed && port_has_rdm(p);
}

/* Take the bus for an RDM request (the output task lets it in between frames) */
static void rdm_bus_take(port_t *p)
{
    atomic_fetch_add(&p->rdm_waiting, 1);
    xSemaphoreTake(p->bus, portMAX_DELAY);
    atomic_fetch_sub(&p->rdm_waiting, 1);
}

/* One locked RDM transaction (0 / false, ack untouched, if the port can no
 * longer send RDM) */
#define RDM_LOCKED(p, expr) ({                          \
        rdm_bus_take(p);                                \
        __typeof__(expr) _r = 0;                        \
        if (rdm_bus_ok(p)) {                            \
            _r = (expr);                                \
            (p)->stats.rdm_requests_sent++;             \
        }                                               \
        xSemaphoreGive((p)->bus);                       \
        _r; })

static bool disc_mute(port_t *p, const rdm_uid_t *uid)
{
    for (int attempt = 0; attempt < 3 && !p->disc_abort; attempt++) {
        rdm_disc_mute_t mute = {0};
        rdm_ack_t ack = ACK_INIT;
        (void)RDM_LOCKED(p, rdm_send_disc_mute(p->dmx_num, uid, &mute, &ack));
        if (ack.type == RDM_RESPONSE_TYPE_ACK && rdm_uid_is_eq(&ack.src_uid, uid)) {
            p->stats.rdm_responses_rx++;
            return true;
        }
    }
    return false;
}

static bool is_found(const rdm_uid_t *found, int count, const rdm_uid_t *uid)
{
    for (int i = 0; i < count; i++) {
        if (rdm_uid_is_eq(&found[i], uid)) return true;
    }
    return false;
}

static void add_found(rdm_uid_t *found, int *count, const rdm_uid_t *uid)
{
    if (!is_found(found, *count, uid) && *count < DMX_MAX_DEVICES) {
        found[(*count)++] = *uid;
    }
}

typedef struct {
    int transactions;   /* DISC_UNIQUE_BRANCH requests sent */
    int answered;       /* ... that got any reply */
    int garbled;        /* ... whose reply wasn't a clean single UID */
    int ghosts;         /* leaf UIDs that answered a branch but not DISC_MUTE */
} disc_diag_t;

/*
 * Binary-tree discovery (E1.20 7.x) with one RDM transaction per bus-lock
 * hold, so DMX frames keep flowing between transactions.
 */
static int run_discovery(port_t *p, rdm_uid_t *found, bool unmute_all, disc_diag_t *diag)
{
    int count = 0;
    rdm_disc_unique_branch_t *stack = malloc(sizeof(rdm_disc_unique_branch_t) * 64);
    if (!stack) {
        return 0;
    }

    rdm_uid_t bcast = RDM_UID_BROADCAST_ALL;
    for (int i = 0; unmute_all && i < 2; i++) {
        (void)RDM_LOCKED(p, rdm_send_disc_un_mute(p->dmx_num, &bcast, NULL, NULL));
    }

    int transactions = 0;
    int sp = 0;
    stack[sp].lower_bound = (rdm_uid_t){0, 0};
    stack[sp].upper_bound = RDM_UID_MAX;
    sp++;

    while (sp > 0 && !p->disc_abort && count < DMX_MAX_DEVICES) {    /* until the list is full */
        if (++transactions > DISC_MAX_TRANSACTIONS) {
            ESP_LOGW(TAG, "Port %u: discovery transaction limit reached", p->port_num);
            break;
        }
        rdm_disc_unique_branch_t br = stack[--sp];

        if (rdm_uid_is_eq(&br.lower_bound, &br.upper_bound)) {
            if (disc_mute(p, &br.lower_bound)) {
                add_found(found, &count, &br.lower_bound);
            } else {
                diag->ghosts++;
                ESP_LOGW(TAG, "Port %u: " UIDSTR " answers discovery but not DISC_MUTE",
                         p->port_num, UID2STR(br.lower_bound));
            }
            continue;
        }

        rdm_ack_t ack = ACK_INIT;
        int attempts = 0;
        do {
            ack = ACK_INIT;
            diag->transactions++;
            (void)RDM_LOCKED(p, rdm_send_disc_unique_branch(p->dmx_num, &br, &ack));
        } while (ack.type == RDM_RESPONSE_TYPE_NONE && ++attempts < 3 && !p->disc_abort);

        if (ack.type == RDM_RESPONSE_TYPE_NONE) {
            continue;   /* nobody in this branch */
        }
        diag->answered++;
        if (diag->answered <= 6) {
            ESP_LOGD(TAG, "Port %u: DUB " UIDSTR ".." UIDSTR " -> type %d err %d size %u from " UIDSTR,
                     p->port_num, UID2STR(br.lower_bound), UID2STR(br.upper_bound),
                     (int)ack.type, (int)ack.err, (unsigned)ack.size, UID2STR(ack.src_uid));
        }

        /* Clean single response: mute it and search the same branch again in
         * case more devices are behind it. A UID we already muted answering
         * again means it ignores mute, so fall through and split instead
         * (guarantees termination). */
        if (ack.type == RDM_RESPONSE_TYPE_ACK &&
            !is_found(found, count, &ack.src_uid)) {
            rdm_uid_t uid = ack.src_uid;
            if (disc_mute(p, &uid)) {
                add_found(found, &count, &uid);
                if (sp < 64) stack[sp++] = br;
                continue;
            }
        }

        /* Collision (or unverifiable response): split the branch */
        diag->garbled++;
        uint64_t lo = ((uint64_t)br.lower_bound.man_id << 32) | br.lower_bound.dev_id;
        uint64_t hi = ((uint64_t)br.upper_bound.man_id << 32) | br.upper_bound.dev_id;
        uint64_t mid = lo + (hi - lo) / 2;
        if (sp + 2 > 64) {
            ESP_LOGW(TAG, "Port %u: discovery stack full", p->port_num);
            continue;
        }
        rdm_disc_unique_branch_t upper = {
            .lower_bound = {.man_id = (uint16_t)((mid + 1) >> 32), .dev_id = (uint32_t)(mid + 1)},
            .upper_bound = br.upper_bound,
        };
        rdm_disc_unique_branch_t lower = {
            .lower_bound = br.lower_bound,
            .upper_bound = {.man_id = (uint16_t)(mid >> 32), .dev_id = (uint32_t)mid},
        };
        stack[sp++] = upper;
        stack[sp++] = lower;
    }

    free(stack);
    return count;
}

/*
 * The fixture answered "ask again in a while" (ACK_TIMER): wait, off the bus,
 * and say whether to ask again. @p left is what this fixture may still cost,
 * and every wait takes some of it.
 */
static bool ack_timer_wait(const port_t *p, const rdm_ack_t *ack, uint32_t *left)
{
    if (ack->type != RDM_RESPONSE_TYPE_ACK_TIMER || *left == 0) return false;
    uint32_t ms = clamp_u32((uint32_t)ack->timer * portTICK_PERIOD_MS, 20, RDM_ACK_TIMER_MAX_MS);
    if (ms > *left) ms = *left;
    *left -= ms;
    while (ms && !p->disc_abort) {
        uint32_t step = ms < 50 ? ms : 50;
        vTaskDelay(pdMS_TO_TICKS(step) ? pdMS_TO_TICKS(step) : 1);
        ms -= step;
    }
    return !p->disc_abort;
}

static void read_device_info(port_t *p, rdm_device_t *dev)
{
    rdm_ack_t ack;
    rdm_device_info_t info;
    uint32_t left = RDM_ACK_TIMER_MAX_MS;
    size_t got;

    memset(&info, 0, sizeof(info));
    do {
        ack = ACK_INIT;
        got = RDM_LOCKED(p, rdm_send_get_device_info(p->dmx_num, &dev->uid,
                                                     RDM_SUB_DEVICE_ROOT, &info, &ack));
    } while (ack_timer_wait(p, &ack, &left));
    if (got > 0 && ack.type == RDM_RESPONSE_TYPE_ACK) {
        p->stats.rdm_responses_rx++;
        dev->device_model = info.model_id;
        dev->dmx_footprint = info.footprint;
        dev->dmx_start_address = info.dmx_start_address;
        dev->personality_count = info.personality.count;
        dev->current_personality = info.personality.current;
        dev->software_version = info.software_version_id;
        dev->sub_device_count = info.sub_device_count;
    }

    struct { rdm_pid_t pid; char *dst; } labels[] = {
        {RDM_PID_MANUFACTURER_LABEL,       dev->manufacturer_label},
        {RDM_PID_DEVICE_LABEL,             dev->device_label},
        {RDM_PID_DEVICE_MODEL_DESCRIPTION, dev->device_model_desc},
    };
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]) && !p->disc_abort; i++) {
        const rdm_request_t req = {
            .dest_uid = &dev->uid,
            .sub_device = RDM_SUB_DEVICE_ROOT,
            .cc = RDM_CC_GET_COMMAND,
            .pid = labels[i].pid,
        };
        char tmp[33] = {0};     /* 32 characters and the null */
        do {
            ack = ACK_INIT;
            got = RDM_LOCKED(p, rdm_send_request(p->dmx_num, &req, "a$", tmp, sizeof(tmp), &ack));
        } while (ack_timer_wait(p, &ack, &left));
        if (got > 0 && ack.type == RDM_RESPONSE_TYPE_ACK) {
            p->stats.rdm_responses_rx++;
            memcpy(labels[i].dst, tmp, 32);
            labels[i].dst[32] = '\0';
        }
    }

    char sw[33] = {0};
    do {
        ack = ACK_INIT;
        got = RDM_LOCKED(p, rdm_send_get_software_version_label(
                                p->dmx_num, &dev->uid, RDM_SUB_DEVICE_ROOT, sw, sizeof(sw), &ack));
    } while (ack_timer_wait(p, &ack, &left));
    if (got > 0 && ack.type == RDM_RESPONSE_TYPE_ACK) {
        memcpy(dev->software_label, sw, 32);
        dev->software_label[32] = '\0';
    }
}

static void log_device(const port_t *p, int i, const rdm_device_t *d)
{
    ESP_LOGI(TAG, "Port %u: [%d] " UIDSTR " addr %u fp %u \"%s\" / \"%s\"",
             p->port_num, i, UID2STR(d->uid), d->dmx_start_address,
             d->dmx_footprint, d->manufacturer_label, d->device_label);
}

static int dev_index(const rdm_device_t *devs, int n, const rdm_uid_t *uid)
{
    for (int i = 0; i < n; i++) {
        if (rdm_uid_is_eq(&devs[i].uid, uid)) return i;
    }
    return -1;
}

/* Device lists go in PSRAM where there is some */
static void *list_alloc(size_t count, size_t size)
{
#if CONFIG_SPIRAM
    void *mem = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (mem) return mem;
#endif
    return calloc(count, size);
}

/* Make @p devs the port's list, in a block of just that size */
static bool devices_store(port_t *p, const rdm_device_t *devs, const uint8_t *misses, int n)
{
    rdm_device_t *list = NULL;
    if (n) {
        list = list_alloc(n, sizeof(rdm_device_t));
        if (!list) return false;
        memcpy(list, devs, sizeof(rdm_device_t) * n);
    }
    xSemaphoreTake(p->data_mutex, portMAX_DELAY);
    rdm_device_t *old = p->devices;
    p->devices = list;
    p->device_count = (uint8_t)n;
    memcpy(p->dev_misses, misses, n);
    p->tod_valid = true;
    xSemaphoreGive(p->data_mutex);
    free(old);
    return true;
}

/*
 * Full discovery: un-mute everything, search the whole UID space and read
 * the details of every device.
 * Incremental discovery (Art-Net background discovery): un-mute everything,
 * mute each known device to confirm it is still there, then search, so only
 * the others answer. Without the un-mute, a fixture that was muted but has
 * left the list (a lost reply, a cable out for a moment) never answers a
 * search again until it is power-cycled. A known device is dropped when
 * DISC_MISSES_MAX passes in a row neither mute nor find it.
 * Only new devices have their details read.
 */
static void discovery_task(void *arg)
{
    port_t *p = (port_t *)arg;
    int64_t t0 = esp_timer_get_time();
    bool incremental = p->disc_mode == DMX_DISC_INCREMENTAL && p->tod_valid;

    rdm_uid_t *found = calloc(DMX_MAX_DEVICES, sizeof(rdm_uid_t));
    uint8_t *misses = calloc(DMX_MAX_DEVICES, 1);
    rdm_device_t *devs = list_alloc(DMX_MAX_DEVICES, sizeof(rdm_device_t));
    int n = 0, added = 0, lost = 0;
    disc_diag_t diag = {0};

    if (found && devs && misses) {
        if (incremental) {
            xSemaphoreTake(p->data_mutex, portMAX_DELAY);
            n = p->device_count;
            if (n) {
                memcpy(devs, p->devices, sizeof(rdm_device_t) * n);
                memcpy(misses, p->dev_misses, n);
            }
            xSemaphoreGive(p->data_mutex);
            rdm_uid_t all = RDM_UID_BROADCAST_ALL;
            for (int i = 0; i < 2 && !p->disc_abort; i++) {
                (void)RDM_LOCKED(p, rdm_send_disc_un_mute(p->dmx_num, &all, NULL, NULL));
            }
            for (int i = 0; i < n && !p->disc_abort; i++) {
                misses[i] = disc_mute(p, &devs[i].uid) ? 0 : misses[i] + 1;
            }
        }
        int nf = p->disc_abort ? 0 : run_discovery(p, found, !incremental, &diag);
        if (incremental && !p->disc_abort) {
            for (int i = 0; i < nf; i++) {
                int at = dev_index(devs, n, &found[i]);
                if (at >= 0) misses[at] = 0;
            }
            int keep = 0;
            for (int i = 0; i < n; i++) {
                if (misses[i] >= DISC_MISSES_MAX) {
                    ESP_LOGI(TAG, "Port %u: " UIDSTR " is gone", p->port_num, UID2STR(devs[i].uid));
                    lost++;
                } else {
                    devs[keep] = devs[i];
                    misses[keep++] = misses[i];
                }
            }
            n = keep;
        }
        for (int i = 0; i < nf && n < DMX_MAX_DEVICES && !p->disc_abort; i++) {
            if (dev_index(devs, n, &found[i]) >= 0) continue;
            memset(&devs[n], 0, sizeof(devs[n]));
            misses[n] = 0;
            devs[n].uid = found[i];
            read_device_info(p, &devs[n]);
            log_device(p, n, &devs[n]);
            n++;
            added++;
        }
        if (!p->disc_abort) {
            if (!devices_store(p, devs, misses, n)) {
                ESP_LOGE(TAG, "Port %u: no memory for %d devices, the list is unchanged",
                         p->port_num, n);
            } else if (n == DMX_MAX_DEVICES && added) {
                ESP_LOGW(TAG, "Port %u: the RDM list is full (%d): any more fixtures aren't listed",
                         p->port_num, n);
            }
        }
    } else {
        ESP_LOGE(TAG, "Port %u: discovery out of memory", p->port_num);
    }
    free(found);
    free(misses);
    free(devs);

    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    if (incremental) {
        if (added || lost || p->disc_abort) {
            ESP_LOGI(TAG, "Port %u: incremental discovery %s: %d device(s), +%d -%d, %lld ms",
                     p->port_num, p->disc_abort ? "aborted" : "done", n, added, lost,
                     (long long)ms);
        }
    } else {
        ESP_LOGI(TAG, "Port %u: RDM discovery %s, %d device(s) in %lld ms "
                 "(%d branch requests, %d answered, %d collisions)", p->port_num,
                 p->disc_abort ? "aborted" : "finished", n, (long long)ms,
                 diag.transactions, diag.answered, diag.garbled);
    }

    bool aborted = p->disc_abort;
    rdm_discovery_callback_t cb = p->disc_cb;
    void *ctx = p->disc_ctx;
    p->discovering = false;
    p->disc_abort = false;
    xEventGroupSetBits(p->disc_events, DISC_IDLE_BIT);

    if (cb) {
        cb(p->port_num, (uint8_t)n, aborted, ctx);
    }
    vTaskDelete(NULL);
}

esp_err_t dmx_handler_rdm_discover(uint8_t port)
{
    return dmx_handler_rdm_discover_mode(port, DMX_DISC_FULL);
}

esp_err_t dmx_handler_rdm_discover_mode(uint8_t port, dmx_disc_mode_t mode)
{
    port_t *p = get_port(port);
    if (!p) return ESP_ERR_INVALID_ARG;
    if (!dmx_handler_rdm_available(port)) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_dmx.state_mutex, portMAX_DELAY);
    /* Already running (callers can just wait), or the port was stopped or
     * changed mode while this call waited for the lock */
    if (p->discovering || !dmx_handler_rdm_available(port)) {
        xSemaphoreGive(s_dmx.state_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (mode == DMX_DISC_FLUSH) {
        xSemaphoreTake(p->data_mutex, portMAX_DELAY);
        devices_clear(p);
        p->tod_valid = false;
        xSemaphoreGive(p->data_mutex);
        mode = DMX_DISC_FULL;
    }
    p->disc_mode = mode;
    p->discovering = true;
    p->disc_abort = false;
    xEventGroupClearBits(p->disc_events, DISC_IDLE_BIT);

    char name[16];
    snprintf(name, sizeof(name), "rdm_disc_%u", port);
    if (xTaskCreatePinnedToCore(discovery_task, name, DMX_DISC_STACK_SIZE, p,
                                DMX_DISC_PRIORITY, NULL, DMX_TASK_CORE) != pdPASS) {
        p->discovering = false;
        xEventGroupSetBits(p->disc_events, DISC_IDLE_BIT);
        xSemaphoreGive(s_dmx.state_mutex);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_dmx.state_mutex);
    if (mode == DMX_DISC_FULL) {
        ESP_LOGI(TAG, "Port %u: RDM discovery started", port);
    }
    return ESP_OK;
}

esp_err_t dmx_handler_rdm_abort_discovery(uint8_t port)
{
    port_t *p = get_port(port);
    if (!p) return ESP_ERR_INVALID_ARG;
    if (p->discovering) {
        p->disc_abort = true;
    }
    return ESP_OK;
}

bool dmx_handler_rdm_tod_valid(uint8_t port)
{
    port_t *p = get_port(port);
    return p ? p->tod_valid : false;
}

size_t dmx_handler_get_rdm_uids(uint8_t port, rdm_uid_t *uids, size_t max)
{
    port_t *p = get_port(port);
    if (!p || !uids) return 0;
    xSemaphoreTake(p->data_mutex, portMAX_DELAY);
    size_t n = p->device_count < max ? p->device_count : max;
    for (size_t i = 0; i < n; i++) {
        uids[i] = p->devices[i].uid;
    }
    xSemaphoreGive(p->data_mutex);
    return n;
}

/*
 * Is @p resp the answer to @p req? From the fixture asked, to whoever asked
 * (the console's UID, not ours), for that PID. A queued message comes with
 * its own PID and the command class that goes with it.
 */
static bool rdm_reply_matches(const uint8_t *req, const uint8_t *resp)
{
    uint16_t pid = ((uint16_t)req[21] << 8) | req[22];
    if (resp[2] != 24 + resp[23] || memcmp(&resp[3], &req[9], 6) != 0 ||
        memcmp(&resp[9], &req[3], 6) != 0) {
        return false;
    }
    if (pid == RDM_PID_QUEUED_MESSAGE && req[20] == RDM_CC_GET_COMMAND) {
        return resp[20] == RDM_CC_GET_COMMAND_RESPONSE || resp[20] == RDM_CC_SET_COMMAND_RESPONSE;
    }
    return resp[20] == req[20] + 1 && resp[21] == req[21] && resp[22] == req[22];
}

/*
 * Raw RDM pass-through (Art-Net ArtRdm). @p req is a complete request with
 * start code and checksum, built by the controller (its own source UID and
 * transaction number are kept). The response, if any, is checked and copied
 * to @p resp including start code and checksum.
 */
esp_err_t dmx_handler_rdm_transaction(uint8_t port, const uint8_t *req, size_t req_len,
                                      uint8_t *resp, size_t resp_size, size_t *resp_len)
{
    port_t *p = get_port(port);
    if (resp_len) *resp_len = 0;
    if (!p || !req || !resp || !resp_len) return ESP_ERR_INVALID_ARG;
    if (!dmx_handler_rdm_available(port)) return ESP_ERR_INVALID_STATE;
    if (req_len < 26 || req[0] != RDM_SC || req[1] != RDM_SUB_SC ||
        req[2] < 24 || req_len < (size_t)req[2] + 2) {
        return ESP_ERR_INVALID_ARG;
    }
    if (req[20] != RDM_CC_GET_COMMAND && req[20] != RDM_CC_SET_COMMAND) {
        return ESP_ERR_NOT_SUPPORTED;   /* discovery is the node's job, and only requests go out */
    }
    size_t len = (size_t)req[2] + 2;
    /* All-devices or manufacturer broadcast: nobody answers */
    bool broadcast = req[5] == 0xFF && req[6] == 0xFF && req[7] == 0xFF && req[8] == 0xFF;

    size_t got = 0;
    dmx_packet_t pkt = {0};
    rdm_bus_take(p);
    if (!rdm_bus_ok(p)) {
        xSemaphoreGive(p->bus);
        return ESP_ERR_INVALID_STATE;
    }
    p->stats.rdm_requests_sent++;
    if (dmx_wait_sent(p->dmx_num, pdMS_TO_TICKS(50))) {
        dmx_write(p->dmx_num, req, len);
        if (dmx_send_num(p->dmx_num, len) > 0) {
            if (broadcast) {
                dmx_wait_sent(p->dmx_num, pdMS_TO_TICKS(50));
            } else {
                got = dmx_receive(p->dmx_num, &pkt, pdMS_TO_TICKS(RDM_EXCHANGE_MS));
                if (got > 0) {
                    if (got > resp_size) got = resp_size;
                    dmx_read(p->dmx_num, resp, got);
                }
            }
        }
    }
    xSemaphoreGive(p->bus);

    if (broadcast) return ESP_OK;
    if (got == 0) return ESP_ERR_TIMEOUT;
    if (pkt.err != DMX_OK || got < 26 || resp[0] != RDM_SC || resp[1] != RDM_SUB_SC ||
        resp[2] < 24 || got < (size_t)resp[2] + 2) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    size_t ml = resp[2];
    uint16_t sum = 0;
    for (size_t i = 0; i < ml; i++) sum += resp[i];
    if (sum != (((uint16_t)resp[ml] << 8) | resp[ml + 1])) {
        return ESP_ERR_INVALID_CRC;
    }
    if (!rdm_reply_matches(req, resp)) {
        return ESP_ERR_INVALID_RESPONSE;    /* another fixture's, a late one or our own echo */
    }
    p->stats.rdm_responses_rx++;
    *resp_len = ml + 2;
    return ESP_OK;
}

esp_err_t dmx_handler_rdm_wait_discovery(uint8_t port, uint32_t timeout_ms)
{
    port_t *p = get_port(port);
    if (!p) return ESP_ERR_INVALID_ARG;
    EventBits_t bits = xEventGroupWaitBits(p->disc_events, DISC_IDLE_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & DISC_IDLE_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool dmx_handler_rdm_is_discovering(uint8_t port)
{
    port_t *p = get_port(port);
    return p ? p->discovering : false;
}

esp_err_t dmx_handler_get_rdm_devices(uint8_t port, rdm_device_t *devices, size_t *count)
{
    port_t *p = get_port(port);
    if (!p || !devices || !count) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(p->data_mutex, portMAX_DELAY);
    size_t n = p->device_count < *count ? p->device_count : *count;
    if (n) memcpy(devices, p->devices, n * sizeof(rdm_device_t));
    *count = n;
    xSemaphoreGive(p->data_mutex);
    return ESP_OK;
}

esp_err_t dmx_handler_rdm_get(uint8_t port, const rdm_uid_t uid, uint16_t pid,
                              uint8_t *response_data, size_t *response_size)
{
    port_t *p = get_port(port);
    if (!p || !response_data || !response_size) return ESP_ERR_INVALID_ARG;
    if (!dmx_handler_rdm_available(port)) return ESP_ERR_INVALID_STATE;

    if (*response_size == 0) return ESP_ERR_INVALID_SIZE;
    rdm_ack_t ack = ACK_INIT;
    const rdm_request_t req = {
        .dest_uid = &uid,
        .sub_device = RDM_SUB_DEVICE_ROOT,
        .cc = RDM_CC_GET_COMMAND,
        .pid = (rdm_pid_t)pid,
    };
    /* Unterminated "b" = repeated raw bytes ("b$" would read a single byte) */
    size_t received = RDM_LOCKED(p, rdm_send_request(p->dmx_num, &req, "b",
                                                     response_data, *response_size, &ack));
    if (ack.type != RDM_RESPONSE_TYPE_ACK) {
        ESP_LOGW(TAG, "RDM GET 0x%04x to " UIDSTR " failed (type %d, err %d)",
                 pid, UID2STR(uid), ack.type, ack.err);
        return ack.type == RDM_RESPONSE_TYPE_NONE ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }
    p->stats.rdm_responses_rx++;
    *response_size = (ack.pdl < *response_size) ? ack.pdl : *response_size;
    (void)received;
    return ESP_OK;
}

esp_err_t dmx_handler_rdm_set(uint8_t port, const rdm_uid_t uid, uint16_t pid,
                              const uint8_t *data, size_t size)
{
    port_t *p = get_port(port);
    if (!p || (!data && size)) return ESP_ERR_INVALID_ARG;
    if (!dmx_handler_rdm_available(port)) return ESP_ERR_INVALID_STATE;

    if (size > 231) return ESP_ERR_INVALID_SIZE;
    rdm_ack_t ack = ACK_INIT;
    const rdm_request_t req = {
        .dest_uid = &uid,
        .sub_device = RDM_SUB_DEVICE_ROOT,
        .cc = RDM_CC_SET_COMMAND,
        .pid = (rdm_pid_t)pid,
        .format = size ? "b" : NULL,
        .pd = size ? data : NULL,
        .pdl = size,
    };
    (void)RDM_LOCKED(p, rdm_send_request(p->dmx_num, &req, NULL, NULL, 0, &ack));
    bool broadcast = rdm_uid_is_broadcast(&uid);
    if (!broadcast && ack.type != RDM_RESPONSE_TYPE_ACK) {
        ESP_LOGW(TAG, "RDM SET 0x%04x to " UIDSTR " failed (type %d, err %d)",
                 pid, UID2STR(uid), ack.type, ack.err);
        return ack.type == RDM_RESPONSE_TYPE_NONE ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }
    if (!broadcast) p->stats.rdm_responses_rx++;
    return ESP_OK;
}

esp_err_t dmx_handler_rdm_set_address(uint8_t port, const rdm_uid_t uid, uint16_t address)
{
    port_t *p = get_port(port);
    if (!p || address < 1 || address > 512) return ESP_ERR_INVALID_ARG;
    if (!dmx_handler_rdm_available(port)) return ESP_ERR_INVALID_STATE;

    rdm_ack_t ack = ACK_INIT;
    (void)RDM_LOCKED(p, rdm_send_set_dmx_start_address(p->dmx_num, &uid, RDM_SUB_DEVICE_ROOT,
                                                 address, &ack));
    if (ack.type != RDM_RESPONSE_TYPE_ACK) {
        return ack.type == RDM_RESPONSE_TYPE_NONE ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }
    xSemaphoreTake(p->data_mutex, portMAX_DELAY);
    for (int i = 0; i < p->device_count; i++) {
        if (rdm_uid_is_eq(&p->devices[i].uid, &uid)) {
            p->devices[i].dmx_start_address = address;
        }
    }
    xSemaphoreGive(p->data_mutex);
    return ESP_OK;
}

esp_err_t dmx_handler_get_uid(uint8_t port, uint8_t uid[6])
{
    port_t *p = get_port(port);
    if (!p || !uid) return ESP_ERR_INVALID_ARG;
    memset(uid, 0, 6);
    if (!p->driver_installed) return ESP_ERR_INVALID_STATE;
    const rdm_uid_t *u = rdm_uid_get(p->dmx_num);
    if (!u) return ESP_FAIL;
    uid[0] = u->man_id >> 8;
    uid[1] = u->man_id & 0xFF;
    uid[2] = u->dev_id >> 24;
    uid[3] = (u->dev_id >> 16) & 0xFF;
    uid[4] = (u->dev_id >> 8) & 0xFF;
    uid[5] = u->dev_id & 0xFF;
    return ESP_OK;
}

// ============================================================================
// Status / callbacks
// ============================================================================

esp_err_t dmx_handler_get_port_status(uint8_t port, dmx_port_status_t *status)
{
    port_t *p = get_port(port);
    if (!p || !status) return ESP_ERR_INVALID_ARG;
    status->mode = p->mode;
    status->is_active = p->is_active;
    status->universe = p->universe;
    status->stats = p->stats;
    status->rdm_device_count = p->device_count;
    status->rdm_list_full = p->device_count >= DMX_MAX_DEVICES;
    status->rdm_available = dmx_handler_rdm_available(port);
    status->rdm_discovering = p->discovering;
    status->rdm_tod_valid = p->tod_valid;
    if (is_input_mode(p->mode) && p->stats.signal_present &&
        (now_ms() - p->stats.last_frame_time_ms) > DMX_INPUT_LOSS_MS) {
        status->stats.signal_present = false;
    }
    return ESP_OK;
}

esp_err_t dmx_handler_register_rx_callback(uint8_t port, dmx_rx_callback_t callback,
                                           void *user_data)
{
    port_t *p = get_port(port);
    if (!p) return ESP_ERR_INVALID_ARG;
    p->rx_cb = NULL;
    p->rx_ctx = user_data;
    p->rx_cb = callback;
    return ESP_OK;
}

esp_err_t dmx_handler_register_loss_callback(uint8_t port, dmx_loss_callback_t callback,
                                             void *user_data)
{
    port_t *p = get_port(port);
    if (!p) return ESP_ERR_INVALID_ARG;
    p->loss_cb = NULL;
    p->loss_ctx = user_data;
    p->loss_cb = callback;
    return ESP_OK;
}

esp_err_t dmx_handler_register_discovery_callback(uint8_t port,
                                                  rdm_discovery_callback_t callback,
                                                  void *user_data)
{
    port_t *p = get_port(port);
    if (!p) return ESP_ERR_INVALID_ARG;
    p->disc_cb = NULL;
    p->disc_ctx = user_data;
    p->disc_cb = callback;
    return ESP_OK;
}

esp_err_t dmx_handler_register_identify_callback(uint8_t port,
                                                 dmx_identify_callback_t callback,
                                                 void *user_data)
{
    port_t *p = get_port(port);
    if (!p) return ESP_ERR_INVALID_ARG;
    p->ident_cb = NULL;
    p->ident_ctx = user_data;
    p->ident_cb = callback;
    return ESP_OK;
}
