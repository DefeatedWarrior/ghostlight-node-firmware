#ifndef ARTNET_RECEIVER_H
#define ARTNET_RECEIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "board.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Art-Net 4 node: ArtDmx receive, ArtPoll/ArtPollReply discovery and
 *        ArtAddress remote programming.
 *
 * The node reports one ArtPollReply per enabled DMX port (Art-Net 4 style,
 * BindIndex = port number, NumPorts = 1), so each port can sit on any
 * Net/Sub-Net/Universe. Node/port status is pulled from the application via
 * the status callback each time a reply is built, and ArtAddress commands are
 * handed to the application via the address callback.
 *
 * All sends happen on the receiver task; other tasks request a reply with
 * artnet_receiver_notify_change().
 */

#define ARTNET_PORT              6454
#define ARTNET_HEADER            "Art-Net\0"
#define ARTNET_PROTOCOL_VERSION  14
#define ARTNET_NODE_PORTS        BOARD_DMX_PORTS

#define ARTNET_OP_POLL           0x2000
#define ARTNET_OP_POLL_REPLY     0x2100
#define ARTNET_OP_DMX            0x5000
#define ARTNET_OP_SYNC           0x5200
#define ARTNET_OP_ADDRESS        0x6000
#define ARTNET_OP_TOD_REQUEST    0x8000
#define ARTNET_OP_TOD_DATA       0x8100
#define ARTNET_OP_TOD_CONTROL    0x8200
#define ARTNET_OP_RDM            0x8300
#define ARTNET_OP_IP_PROG        0xF800
#define ARTNET_OP_IP_PROG_REPLY  0xF900

/* Failsafe state reported in ArtPollReply Status3 (bits 7-6) */
typedef enum {
    ARTNET_FAILSAFE_HOLD = 0,
    ARTNET_FAILSAFE_ZERO = 1,
    ARTNET_FAILSAFE_FULL = 2,
    ARTNET_FAILSAFE_SCENE = 3,
} artnet_failsafe_t;

/* ArtTodControl commands */
#define ARTNET_ATC_NONE          0x00
#define ARTNET_ATC_FLUSH         0x01   /* flush the TOD and run full discovery */
#define ARTNET_ATC_END           0x02   /* stop discovery, keep the TOD */
#define ARTNET_ATC_INC_ON        0x03   /* enable incremental (background) discovery */
#define ARTNET_ATC_INC_OFF       0x04   /* disable incremental discovery */

/* ArtTodData CommandResponse */
#define ARTNET_TOD_FULL          0x00
#define ARTNET_TOD_NAK           0xFF   /* TOD not available / discovery incomplete */

/* UIDs per ArtTodData packet */
#define ARTNET_TOD_BLOCK_UIDS    200

/* ArtAddress command codes */
#define ARTNET_AC_NONE           0x00
#define ARTNET_AC_CANCEL_MERGE   0x01
#define ARTNET_AC_LED_NORMAL     0x02
#define ARTNET_AC_LED_MUTE       0x03
#define ARTNET_AC_LED_LOCATE     0x04
#define ARTNET_AC_RESET_RX_FLAGS 0x05
#define ARTNET_AC_FAIL_HOLD      0x08
#define ARTNET_AC_FAIL_ZERO      0x09
#define ARTNET_AC_FAIL_FULL      0x0A
#define ARTNET_AC_FAIL_SCENE     0x0B
#define ARTNET_AC_FAIL_RECORD    0x0C
#define ARTNET_AC_MERGE_LTP0     0x10
#define ARTNET_AC_MERGE_HTP0     0x50
#define ARTNET_AC_ARTNET_SEL0    0x60
#define ARTNET_AC_ACN_SEL0       0x70
#define ARTNET_AC_CLEAR_OP0      0x90
#define ARTNET_AC_RDM_ENABLE0    0xC0
#define ARTNET_AC_RDM_DISABLE0   0xD0

/** NodeReport codes */
typedef enum {
    ARTNET_RC_DEBUG       = 0x0000,
    ARTNET_RC_POWER_OK    = 0x0001,
    ARTNET_RC_POWER_FAIL  = 0x0002,
    ARTNET_RC_PARSE_FAIL  = 0x0004,
    ARTNET_RC_UDP_FAIL    = 0x0005,
    ARTNET_RC_SH_NAME_OK  = 0x0006,
    ARTNET_RC_LO_NAME_OK  = 0x0007,
    ARTNET_RC_DMX_ERROR   = 0x0008,
    ARTNET_RC_CONFIG_ERR  = 0x000C,
    ARTNET_RC_FACTORY_RES = 0x0010,
} artnet_report_code_t;

/** Per-port status reported in ArtPollReply */
typedef struct {
    bool enabled;           /**< port reported at all */
    bool is_input;          /**< DMX input (DMX -> network) instead of output */
    uint16_t port_address;  /**< 15-bit Art-Net Port-Address */
    bool data_active;       /**< output: data being sent / input: data received */
    bool merging;           /**< output is merging more than one source */
    bool ltp;               /**< merge mode is LTP */
    bool sacn;              /**< port uses sACN rather than Art-Net */
    bool rdm_capable;
    bool rdm_enabled;
    bool rdm_discovering;
    bool rdm_background;    /**< incremental background discovery enabled */
    uint8_t responder_uid[6]; /**< RDM responder UID (zero if not a responder) */
    bool input_errors;      /**< input: receive errors detected */
} artnet_port_status_t;

/** Node status, filled by the application on demand */
typedef struct {
    char short_name[18];
    char long_name[64];
    uint8_t ip[4];
    uint8_t netmask[4];
    uint8_t mac[6];
    bool dhcp;
    uint16_t firmware_version;
    uint8_t failsafe;       /**< artnet_failsafe_t: output on data loss */
    uint8_t indicator;      /**< 0 normal, 1 locate, 2 mute */
    artnet_port_status_t ports[ARTNET_NODE_PORTS];
} artnet_node_status_t;

/** Parsed ArtAddress packet (fields set to -1 / NULL mean "no change") */
typedef struct {
    uint8_t port;           /**< target port 1..ARTNET_NODE_PORTS (from BindIndex) */
    const char *short_name; /**< NULL = no change */
    const char *long_name;  /**< NULL = no change */
    int16_t net;            /**< -1 = no change, else 0..127 */
    int16_t sub;            /**< -1 = no change, else 0..15 */
    int16_t universe;       /**< -1 = no change, else 0..15 (SwOut/SwIn low nibble) */
    bool reset_address;     /**< a field asked to reset to the default */
    uint8_t command;        /**< ARTNET_AC_* */
    uint8_t acn_priority;   /**< 0..200, 255 = no change */
    uint32_t source_ip;
} artnet_address_cmd_t;

typedef struct {
    uint32_t packets_received;
    uint32_t dmx_packets;
    uint32_t poll_packets;
    uint32_t poll_replies_sent;
    uint32_t address_packets;
    uint32_t sync_packets;
    uint32_t unsupported_packets;
    uint32_t invalid_packets;
    uint32_t own_packets;
    uint32_t peer_replies;      /**< ArtPollReply from other nodes */
    uint32_t sequence_errors;   /**< kept for API compatibility (merge engine counts drops) */
} artnet_stats_t;

/** RDM related packets from controllers (ArtTodRequest / ArtTodControl / ArtRdm) */
typedef enum {
    ARTNET_RDM_EV_TOD_REQUEST = 0,
    ARTNET_RDM_EV_TOD_CONTROL,
    ARTNET_RDM_EV_RDM,
} artnet_rdm_event_type_t;

typedef struct {
    artnet_rdm_event_type_t type;
    uint32_t source_ip;          /**< controller, network byte order */
    uint8_t net;                 /**< Net (Port-Address bits 14-8) */
    uint8_t command;             /**< TodControl command / ArtRdm command (0 = ArProcess) */
    /** TOD_REQUEST: requested Port-Addresses (Net:Address) */
    uint8_t address_count;
    uint16_t addresses[32];
    /** TOD_CONTROL / RDM: target Port-Address */
    uint16_t port_address;
    /** RDM: RDM packet without the start code (sub-start code first),
     *  checksum included if the controller sent one */
    const uint8_t *rdm;
    uint16_t rdm_len;
} artnet_rdm_event_t;

typedef void (*artnet_rdm_callback_t)(const artnet_rdm_event_t *ev, void *user_data);
/** ArtSync received from @p source_ip (network byte order) */
typedef void (*artnet_sync_callback_t)(uint32_t source_ip, void *user_data);

typedef void (*artnet_dmx_callback_t)(uint16_t universe, const uint8_t *data,
                                      uint16_t length, uint8_t sequence,
                                      uint32_t source_ip, void *user_data);
typedef void (*artnet_status_callback_t)(artnet_node_status_t *status, void *user_data);
typedef void (*artnet_address_callback_t)(const artnet_address_cmd_t *cmd, void *user_data);

/** Parsed ArtIpProg (addresses in wire order, i.e. network byte order) */
typedef struct {
    bool program;           /**< Command bit 7: change something (else query only) */
    bool dhcp;              /**< bit 6: switch to DHCP (other bits ignored) */
    bool set_gateway;       /**< bit 4 */
    bool reset_default;     /**< bit 3: back to the default (DHCP) */
    bool set_ip;            /**< bit 2 */
    bool set_mask;          /**< bit 1 */
    uint8_t ip[4];
    uint8_t mask[4];
    uint8_t gateway[4];
    uint32_t source_ip;
} artnet_ipprog_cmd_t;

/** What ArtIpProgReply reports (the settings now in force or about to be) */
typedef struct {
    uint8_t ip[4];
    uint8_t mask[4];
    uint8_t gateway[4];
    bool dhcp;
} artnet_ipprog_reply_t;

/**
 * ArtIpProg received. Fill @p reply (it is sent right after the callback
 * returns, so apply network changes later, not inside the callback).
 */
typedef void (*artnet_ipprog_callback_t)(const artnet_ipprog_cmd_t *cmd,
                                         artnet_ipprog_reply_t *reply, void *user_data);

/** Another node's ArtPollReply. Nodes with many ports send one per group of
 *  ports (BindIndex 1, 2, ...). Addresses in network byte order. */
#define ARTNET_REPLY_PORTS 4
typedef struct {
    uint32_t ip;                /**< the node's address (IpAddress field) */
    uint32_t from_ip;           /**< where the packet came from */
    uint8_t mac[6];             /**< all zero if the node does not say */
    uint8_t bind_index;         /**< 0 = not given */
    char short_name[18];        /**< NUL-terminated */
    char long_name[64];
    uint16_t firmware;          /**< VersInfo */
    uint16_t oem;
    uint16_t esta;              /**< ESTA manufacturer code */
    uint8_t style;              /**< StNode 0, StController 1, StMedia 2, StRoute 3, ... */
    uint8_t status2;            /**< bit 1 DHCP in use, bit 3 15-bit addresses */
    uint8_t num_ports;          /**< 0-4 */
    struct {
        uint8_t type;           /**< PortTypes: bit 7 output, bit 6 input, bits 5-0 protocol (0 = DMX512) */
        uint8_t good_input;     /**< bit 7: data received */
        uint8_t good_output;    /**< bit 7: data being output */
        uint16_t in_address;    /**< 15-bit Port-Address (Net:Sub-Net:SwIn) */
        uint16_t out_address;   /**< ... (Net:Sub-Net:SwOut) */
    } ports[ARTNET_REPLY_PORTS];
} artnet_poll_reply_t;

/** Parse an ArtPollReply (the whole UDP payload). False if it is not one. */
bool artnet_parse_poll_reply(const uint8_t *b, int len, artnet_poll_reply_t *out);

/**
 * ArtPollReply from another node. @p ip_conflict: it uses this node's IP
 * address (a different device, going by its MAC address).
 */
typedef void (*artnet_poll_reply_callback_t)(const artnet_poll_reply_t *reply, bool ip_conflict,
                                             void *user_data);

esp_err_t artnet_receiver_init(void);
esp_err_t artnet_receiver_deinit(void);
esp_err_t artnet_receiver_start(void);
esp_err_t artnet_receiver_stop(void);

esp_err_t artnet_receiver_set_callback(artnet_dmx_callback_t callback, void *user_data);
esp_err_t artnet_receiver_set_status_callback(artnet_status_callback_t callback, void *user_data);
esp_err_t artnet_receiver_set_address_callback(artnet_address_callback_t callback, void *user_data);
/** ArtIpProg handler; without one ArtIpProg is ignored */
esp_err_t artnet_receiver_set_ipprog_callback(artnet_ipprog_callback_t callback, void *user_data);

/** Set the NodeReport code and text shown in ArtPollReply */
esp_err_t artnet_receiver_set_rdm_callback(artnet_rdm_callback_t callback, void *user_data);
esp_err_t artnet_receiver_set_sync_callback(artnet_sync_callback_t callback, void *user_data);
esp_err_t artnet_receiver_set_poll_reply_callback(artnet_poll_reply_callback_t callback, void *user_data);

/**
 * Ask the other nodes to announce themselves: broadcast an ArtPoll (from the
 * receive task, shortly after). Polls at most every ARTNET_SELF_POLL_MS; true
 * if this call queued one. Art-Net 4 nodes answer the poller directly, so
 * without this the node only hears the ones that broadcast their replies.
 */
#define ARTNET_SELF_POLL_MS 5000
bool artnet_receiver_poll_network(void);

/**
 * Unicast ArtTodData for one port to @p dest_ip (network byte order).
 * Sends ceil(total/200) packets (at least one). @p nak sends TodNak with
 * no UIDs. @p uids are 6-byte big-endian UIDs.
 */
esp_err_t artnet_receiver_send_tod_data(uint32_t dest_ip, uint8_t bind_index,
                                        uint16_t port_address, bool nak,
                                        const uint8_t (*uids)[6], uint16_t total);
/** Unicast an ArtRdm carrying @p rdm (no start code, checksum included) */
esp_err_t artnet_receiver_send_rdm(uint32_t dest_ip, uint8_t bind_index, uint16_t port_address,
                                   const uint8_t *rdm, uint16_t len);

void artnet_receiver_set_node_report(artnet_report_code_t code, const char *text);

/**
 * Status changed (config, IP, port state). Sends an unsolicited ArtPollReply
 * if a controller asked for them, or always when @p force is true (used once
 * after the network comes up so controllers see the node immediately).
 */
void artnet_receiver_notify_change(bool force);

esp_err_t artnet_receiver_get_stats(artnet_stats_t *stats);

/** The node's UDP 6454 socket (-1 if not running), so transmitted ArtDmx can
 *  use 6454 as the source port as the Art-Net spec requires */
int artnet_receiver_get_socket(void);
esp_err_t artnet_receiver_enable_poll_reply(bool enable);
bool artnet_receiver_is_running(void);

#ifdef __cplusplus
}
#endif

#endif // ARTNET_RECEIVER_H
