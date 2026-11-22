#ifndef DMX_HANDLER_H
#define DMX_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "config_manager.h"
#include "board.h"
#include "esp_dmx.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief DMX512 / RDM port driver (built on esp_dmx)
 *
 * The number of ports and their UARTs / pins come from board.h
 * (ESP-NODE-2RDM: port 1 UART1 TX17 RX16 DIR39, port 2 UART2 TX15 RX18 DIR40).
 *
 * Output ports run one task each that, every frame, asks the registered
 * frame source (normally the merge engine) for fresh data and transmits it,
 * so network data reaches the wire with at most one frame of latency.
 *
 * All bus traffic on a port (DMX frames, RDM requests, discovery) is
 * serialised by a per-port bus lock. RDM discovery is interleaved with DMX
 * output one transaction at a time, so fixtures keep receiving DMX while
 * discovery runs.
 */

#define DMX_PORT_1   1
#define DMX_PORT_2   2
#define DMX_PORT_MAX BOARD_DMX_PORTS

#define DMX_CHANNEL_COUNT 512
#define DMX_FRAME_SIZE    (DMX_CHANNEL_COUNT + 1)   /* start code + 512 slots */
/* Devices listed per port. A list only takes RAM for what was found, but a
 * discovery needs room for all of them (160 bytes each) while it runs.
 * At most 200: one ArtTodData block. */
#if CONFIG_SPIRAM
#define DMX_MAX_DEVICES   128
#else
#define DMX_MAX_DEVICES   64
#endif

/** Input signal is considered lost after this long without a valid frame */
#define DMX_INPUT_LOSS_MS 1000

typedef struct {
    rdm_uid_t uid;
    uint16_t device_model;
    uint32_t software_version;
    uint16_t dmx_footprint;
    uint16_t dmx_start_address;
    uint8_t personality_count;
    uint8_t current_personality;
    uint16_t sub_device_count;
    char manufacturer_label[33];
    char device_label[33];
    char device_model_desc[33];
    char software_label[33];
} rdm_device_t;

typedef struct {
    uint32_t frames_sent;
    uint32_t frames_received;
    uint32_t rdm_requests_sent;
    uint32_t rdm_responses_rx;
    uint32_t rdm_responder_requests;  /**< RDM requests answered in responder mode */
    uint32_t error_count;             /**< framing / overflow errors on input */
    uint32_t last_frame_time_ms;
    uint16_t last_slot_count;         /**< slots in the last input frame */
    bool signal_present;              /**< input: valid DMX seen recently */
} dmx_port_stats_t;

/** Output timing (output / RDM controller ports) */
typedef struct {
    uint8_t rate_hz;      /**< frames per second, 1-44 */
    uint16_t break_us;    /**< 92-1000 (RDM ports: 176-352) */
    uint16_t mab_us;      /**< 12-1000 (RDM ports: 12-88) */
    uint16_t slots;       /**< slots per frame, 24-512 */
} dmx_output_timing_t;

/**
 * Input analyser (DMX input / RDM responder ports). Counters run from the
 * port start or the last dmx_handler_reset_analysis().
 */
typedef struct {
    uint16_t refresh_hz;      /**< level frames (start code 0) in the last second */
    uint16_t packets_per_s;   /**< every packet in the last second */
    uint16_t slots_last;
    uint16_t slots_min;       /**< 0 = no frame yet */
    uint16_t slots_max;
    uint32_t frames;          /**< level frames */
    uint32_t err_framing;     /**< improperly framed slots (bad stop bits, noise) */
    uint32_t err_overflow;    /**< UART overflow */
    uint32_t err_short;       /**< frame cut short by a new break */
    uint32_t dropped_late;    /**< frames never forwarded: the input task was
                                   more than a frame late and a newer frame
                                   had replaced them */
    uint32_t alt_start_codes; /**< packets with a start code other than 0 / RDM */
    uint8_t last_alt_start_code;
    uint32_t rdm_packets;     /**< RDM packets seen on the line */
    uint32_t interval_min_ms; /**< shortest / longest time between level frames */
    uint32_t interval_max_ms;
    bool timing_on;           /**< break / MAB measurement running */
    uint32_t timing_left_s;   /**< seconds until it switches off again */
    uint32_t break_us;        /**< last measured break (0 = unknown) */
    uint32_t mab_us;          /**< last measured mark-after-break */
} dmx_input_analysis_t;

typedef struct {
    dmx_mode_t mode;
    bool is_active;
    uint16_t universe;
    dmx_port_stats_t stats;
    uint8_t rdm_device_count;
    bool rdm_list_full;               /**< DMX_MAX_DEVICES listed: there may be more */
    bool rdm_available;
    bool rdm_discovering;
    bool rdm_tod_valid;               /**< a discovery has completed since the port started */
} dmx_port_status_t;

typedef enum {
    DMX_DISC_FULL = 0,        /**< un-mute all, search everything; old list kept until done */
    DMX_DISC_INCREMENTAL,     /**< un-mute all, check known devices (two misses drop one), find
                                   new ones (full if no list yet) */
    DMX_DISC_FLUSH,           /**< drop the list now, then full discovery */
} dmx_disc_mode_t;

/** Input frame callback (called from the port's input task) */
typedef void (*dmx_rx_callback_t)(uint8_t port, const uint8_t *data, size_t size, void *user_data);
/** Input signal lost (called once when frames stop arriving) */
typedef void (*dmx_loss_callback_t)(uint8_t port, void *user_data);
/** Discovery finished (called from the discovery task, also when aborted) */
typedef void (*rdm_discovery_callback_t)(uint8_t port, uint8_t device_count, bool aborted,
                                         void *user_data);
/** Output frame source: fill @p data (512 slots) for the next frame */
typedef void (*dmx_frame_source_t)(uint8_t port, uint8_t *data, void *user_data);
/** RDM responder identify state changed */
typedef void (*dmx_identify_callback_t)(uint8_t port, bool identify, void *user_data);

esp_err_t dmx_handler_init(void);
esp_err_t dmx_handler_deinit(void);

/** Stop the port if running and apply @p config (call start afterwards) */
esp_err_t dmx_handler_configure_port(uint8_t port, const port_config_t *config);
esp_err_t dmx_handler_start_port(uint8_t port);
esp_err_t dmx_handler_stop_port(uint8_t port);

/**
 * Output timing. Applied live (from the next frame) and kept across
 * configure/start. Values are clamped to the limits above.
 */
esp_err_t dmx_handler_set_output_timing(uint8_t port, const dmx_output_timing_t *timing);
esp_err_t dmx_handler_get_output_timing(uint8_t port, dmx_output_timing_t *timing);

/** Input analyser snapshot; ESP_ERR_INVALID_STATE if the port is not an input */
esp_err_t dmx_handler_get_analysis(uint8_t port, dmx_input_analysis_t *out);
esp_err_t dmx_handler_reset_analysis(uint8_t port);
/**
 * Measure break / MAB lengths for @p seconds (0 = stop). The measurement
 * takes an interrupt on every edge of the line (about 10-20 % of one CPU at
 * full DMX rate), so it always switches itself off again (max 600 s).
 */
esp_err_t dmx_handler_measure_timing(uint8_t port, uint32_t seconds);

/** Last frame put on the wire (output ports) or received (input ports) */
esp_err_t dmx_handler_get_levels(uint8_t port, uint8_t data[DMX_CHANNEL_COUNT]);

/** Output data used when no frame source is registered */
esp_err_t dmx_handler_send_dmx(uint8_t port, const uint8_t *data);
esp_err_t dmx_handler_set_frame_source(uint8_t port, dmx_frame_source_t source, void *user_data);
/** Send the next output frame now instead of at the next frame slot (sync) */
esp_err_t dmx_handler_trigger_output(uint8_t port);

esp_err_t dmx_handler_read_dmx(uint8_t port, uint8_t *data, uint32_t timeout_ms);
esp_err_t dmx_handler_set_channel(uint8_t port, uint16_t channel, uint8_t value);
esp_err_t dmx_handler_set_channels(uint8_t port, uint16_t start_channel,
                                   const uint8_t *data, uint16_t length);
esp_err_t dmx_handler_blackout(uint8_t port);

/** true if RDM controller operations are allowed on the port */
bool dmx_handler_rdm_available(uint8_t port);
/** Start a full discovery in the background (returns immediately).
 *  ESP_ERR_INVALID_STATE if RDM is unavailable or discovery is already running. */
esp_err_t dmx_handler_rdm_discover(uint8_t port);
esp_err_t dmx_handler_rdm_discover_mode(uint8_t port, dmx_disc_mode_t mode);
/** Stop a running discovery; the previous device list is kept */
esp_err_t dmx_handler_rdm_abort_discovery(uint8_t port);
/** true once a discovery has completed since the port started (TOD known) */
bool dmx_handler_rdm_tod_valid(uint8_t port);
/** Copy up to @p max discovered UIDs; returns the number copied */
size_t dmx_handler_get_rdm_uids(uint8_t port, rdm_uid_t *uids, size_t max);
/**
 * Send a raw RDM request (start code 0xCC .. checksum, as built by an RDM
 * controller) and wait for the response. Discovery commands are refused.
 * @return ESP_OK with *resp_len > 0 (response incl. start code and checksum),
 *         ESP_OK with *resp_len == 0 for broadcasts, ESP_ERR_TIMEOUT if no
 *         device answered, ESP_ERR_INVALID_RESPONSE / _CRC for bad replies.
 */
esp_err_t dmx_handler_rdm_transaction(uint8_t port, const uint8_t *req, size_t req_len,
                                      uint8_t *resp, size_t resp_size, size_t *resp_len);
/** Block until discovery on @p port finishes; ESP_ERR_TIMEOUT if it does not */
esp_err_t dmx_handler_rdm_wait_discovery(uint8_t port, uint32_t timeout_ms);
bool dmx_handler_rdm_is_discovering(uint8_t port);
esp_err_t dmx_handler_get_rdm_devices(uint8_t port, rdm_device_t *devices, size_t *count);
esp_err_t dmx_handler_rdm_get(uint8_t port, const rdm_uid_t uid, uint16_t pid,
                              uint8_t *response_data, size_t *response_size);
esp_err_t dmx_handler_rdm_set(uint8_t port, const rdm_uid_t uid, uint16_t pid,
                              const uint8_t *data, size_t size);
/** Set the DMX start address of a discovered device (1-512) */
esp_err_t dmx_handler_rdm_set_address(uint8_t port, const rdm_uid_t uid, uint16_t address);
/** UID this port uses (controller source UID / responder UID), 6 bytes big-endian */
esp_err_t dmx_handler_get_uid(uint8_t port, uint8_t uid[6]);

esp_err_t dmx_handler_get_port_status(uint8_t port, dmx_port_status_t *status);
esp_err_t dmx_handler_register_rx_callback(uint8_t port, dmx_rx_callback_t callback,
                                           void *user_data);
esp_err_t dmx_handler_register_loss_callback(uint8_t port, dmx_loss_callback_t callback,
                                             void *user_data);
esp_err_t dmx_handler_register_discovery_callback(uint8_t port,
                                                  rdm_discovery_callback_t callback,
                                                  void *user_data);
esp_err_t dmx_handler_register_identify_callback(uint8_t port,
                                                 dmx_identify_callback_t callback,
                                                 void *user_data);

#ifdef __cplusplus
}
#endif

#endif // DMX_HANDLER_H
