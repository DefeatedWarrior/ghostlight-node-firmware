#ifndef SACN_RECEIVER_H
#define SACN_RECEIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief sACN (ANSI E1.31) receiver
 *
 * Receives E1.31 data packets on UDP 5568 (multicast 239.255.hi.lo and
 * unicast). Each valid data packet (start code 0x00 levels or 0xDD
 * per-address priority) is handed to the registered callback together with
 * the source CID, priority, sequence and option flags; sequence checking and
 * merging are done per source by the merge engine.
 *
 * Multicast membership is re-joined with sacn_receiver_refresh_subscriptions()
 * whenever a network interface comes up, because lwIP only joins groups on
 * interfaces that exist at join time.
 */

#define SACN_PORT            5568
#define SACN_MAX_UNIVERSES   8

#define SACN_PACKET_IDENTIFIER "ASC-E1.17\0\0\0"

#define SACN_ROOT_VECTOR_DATA      0x00000004
#define SACN_ROOT_VECTOR_EXTENDED  0x00000008
#define SACN_FRAME_VECTOR_DATA     0x00000002
#define SACN_EXT_VECTOR_SYNC       0x00000001   /* VECTOR_E131_EXTENDED_SYNCHRONIZATION */
#define SACN_EXT_VECTOR_DISCOVERY  0x00000002   /* VECTOR_E131_EXTENDED_DISCOVERY */
#define SACN_SYNC_PACKET_LEN       49
#define SACN_DMP_VECTOR            0x02

#define SACN_OPT_PREVIEW      0x80
#define SACN_OPT_STREAM_TERM  0x40
#define SACN_OPT_FORCE_SYNC   0x20

#define SACN_DISCOVERY_UNIVERSE 64214

typedef struct __attribute__((packed)) {
    uint16_t preamble_size;
    uint16_t postamble_size;
    uint8_t  acn_pid[12];
    uint16_t flags_length;
    uint32_t vector;
    uint8_t  cid[16];
} sacn_root_layer_t;

typedef struct __attribute__((packed)) {
    uint16_t flags_length;
    uint32_t vector;
    char     source_name[64];
    uint8_t  priority;
    uint16_t sync_address;
    uint8_t  sequence_number;
    uint8_t  options;
    uint16_t universe;
} sacn_framing_layer_t;

typedef struct __attribute__((packed)) {
    uint16_t flags_length;
    uint8_t  vector;
    uint8_t  address_type;
    uint16_t first_address;
    uint16_t address_increment;
    uint16_t property_count;
    uint8_t  start_code;
    uint8_t  data[512];
} sacn_dmp_layer_t;

typedef struct __attribute__((packed)) {
    sacn_root_layer_t    root;
    sacn_framing_layer_t framing;
    sacn_dmp_layer_t     dmp;
} sacn_packet_t;

/** Decoded data packet handed to the callback (pointers valid during the call) */
typedef struct {
    uint16_t universe;
    const uint8_t *cid;        /**< 16 bytes */
    const char *source_name;   /**< NUL-terminated */
    uint8_t priority;          /**< 0-200 (clamped) */
    uint8_t sequence;
    uint8_t options;
    bool preview;
    bool terminated;
    uint16_t sync_address;
    uint8_t start_code;        /**< 0x00 or 0xDD */
    const uint8_t *data;       /**< slots, without the start code */
    uint16_t length;           /**< number of slots (0-512) */
    uint32_t source_ip;        /**< network byte order */
} sacn_packet_info_t;

typedef struct {
    uint32_t packets_received;
    uint32_t data_packets;       /**< valid 0x00 packets */
    uint32_t pap_packets;        /**< valid 0xDD packets */
    uint32_t preview_packets;
    uint32_t terminated_packets;
    uint32_t extended_packets;   /**< other extended packets (ignored) */
    uint32_t discovery_packets;  /**< valid universe discovery packets */
    uint32_t sync_packets;       /**< valid E1.31 synchronization packets */
    uint32_t own_packets;        /**< our own transmissions, ignored */
    uint32_t invalid_packets;
    uint32_t sequence_errors;    /**< kept for API compatibility (merge engine counts drops) */
} sacn_stats_t;

typedef void (*sacn_dmx_callback_t)(const sacn_packet_info_t *pkt, void *user_data);
/**
 * E1.31 universe discovery page (receive task): the universes a source says
 * it sends, @p count big-endian 16-bit values at @p universes_be. Sources
 * repeat it every 10 s, one page per 512 universes.
 */
typedef void (*sacn_discovery_callback_t)(const uint8_t *cid, const char *source_name,
                                          uint32_t source_ip, uint8_t page, uint8_t last_page,
                                          const uint8_t *universes_be, uint16_t count,
                                          void *user_data);
/** E1.31 synchronization packet for @p sync_universe (receive task) */
typedef void (*sacn_sync_callback_t)(uint16_t sync_universe, const uint8_t *cid,
                                     uint32_t source_ip, void *user_data);

esp_err_t sacn_receiver_init(void);
esp_err_t sacn_receiver_deinit(void);
esp_err_t sacn_receiver_start(void);
esp_err_t sacn_receiver_stop(void);

/** Add a universe (1-63999) to the wanted set and join its multicast group */
esp_err_t sacn_receiver_subscribe_universe(uint16_t universe);
esp_err_t sacn_receiver_unsubscribe_universe(uint16_t universe);
/** Replace the wanted set with @p universes (0 entries are skipped) */
esp_err_t sacn_receiver_set_universes(const uint16_t *universes, uint8_t count);
/** Re-join every wanted group (call after an interface gets an IP) */
esp_err_t sacn_receiver_refresh_subscriptions(void);

/** Packets carrying this CID (our own transmitter) are ignored */
void sacn_receiver_set_local_cid(const uint8_t cid[16]);

esp_err_t sacn_receiver_set_callback(sacn_dmx_callback_t callback, void *user_data);
esp_err_t sacn_receiver_set_sync_callback(sacn_sync_callback_t callback, void *user_data);
/** Universe discovery pages (the receiver always listens to 239.255.250.214) */
esp_err_t sacn_receiver_set_discovery_callback(sacn_discovery_callback_t callback, void *user_data);
esp_err_t sacn_receiver_get_stats(sacn_stats_t *stats);
bool sacn_receiver_is_running(void);
/** Number of groups currently joined */
uint8_t sacn_receiver_get_subscription_count(void);

#ifdef __cplusplus
}
#endif

#endif // SACN_RECEIVER_H
