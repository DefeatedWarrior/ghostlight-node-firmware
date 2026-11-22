/**
 * @file source_monitor.h
 * @brief Every Art-Net / sACN sender the node hears, for "why is nothing
 *        lighting up?" checks.
 *
 * The receivers report each DMX packet (srcmon_note) and each sACN universe
 * discovery page (srcmon_note_discovery), routed to a port or not. The table
 * keeps one row per protocol, sender and universe: packet rate, lost and
 * out-of-order packets from the sequence numbers, and the longest gap
 * between packets. A row nobody has sent for a minute is dropped, a discovery
 * list after 30 s (senders repeat it every 10 s).
 *
 * It also keeps the other Art-Net nodes' ArtPollReply (srcmon_note_node):
 * one row per node and BindIndex, dropped after SRCMON_NODE_FORGET_MS.
 *
 * Thread safe: the receive tasks write, the web server reads.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "artnet_receiver.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SRCMON_MAX_SOURCES      24
#define SRCMON_MAX_ADVERTS      8
#define SRCMON_ADVERT_UNIVERSES 16      /**< universes kept per discovery sender */
#define SRCMON_NAME_LEN         32
#define SRCMON_FORGET_MS        60000   /**< rows silent this long are dropped */
#define SRCMON_RESYNC_MS        2000    /**< after a pause this long, sequence restarts */
#define SRCMON_EPOCH_MS         10000   /**< "recent" = this and the previous epoch */
#define SRCMON_MAX_NODES        32      /**< ArtPollReply rows (a node may send several) */
#define SRCMON_NODE_FORGET_MS   30000   /**< consoles poll every 2.5-3 s */

typedef enum {
    SRCMON_ARTNET = 1,
    SRCMON_SACN   = 2,
} srcmon_proto_t;

/** One DMX packet as the receiver saw it */
typedef struct {
    uint8_t proto;              /**< srcmon_proto_t */
    uint32_t ip;                /**< sender, network byte order */
    const uint8_t *cid;         /**< sACN component ID (16 bytes), NULL for Art-Net */
    uint16_t universe;          /**< as on the wire (Art-Net 15-bit, sACN 1-63999) */
    const char *name;           /**< sACN source name, NULL for Art-Net */
    uint8_t priority;           /**< sACN priority (Art-Net: 0) */
    uint8_t start_code;         /**< sACN start code (Art-Net: 0) */
    bool preview;               /**< sACN preview data (not for live output) */
    uint8_t sequence;           /**< Art-Net 0 = sequence not used */
    uint16_t slots;             /**< DMX slots in the packet */
} srcmon_packet_t;

typedef struct {
    uint8_t proto;
    uint32_t ip;                /**< network byte order */
    uint16_t universe;
    char name[SRCMON_NAME_LEN]; /**< sACN source name ("" for Art-Net) */
    uint8_t priority;
    uint16_t slots;             /**< slots in the last level packet (start code 0) */
    bool pap;                   /**< sends per-address priority (0xDD) */
    bool preview;               /**< the last level packet was preview data */
    bool sequenced;             /**< sends sequence numbers */
    uint32_t packets;
    uint32_t lost;              /**< missing sequence numbers, all time */
    uint32_t lost_recent;       /**< ... in the last 10-20 s */
    uint32_t late;              /**< repeated or out-of-order packets */
    uint16_t rate_x10;          /**< packets per second x 10 (0 once silent for 5 s) */
    uint32_t max_gap_ms;        /**< longest gap between packets in the last 10-20 s */
    uint32_t age_ms;            /**< since the last packet */
} srcmon_source_t;

/** A sACN sender's universe discovery list */
typedef struct {
    uint32_t ip;
    char name[SRCMON_NAME_LEN];
    uint16_t universes[SRCMON_ADVERT_UNIVERSES];
    uint16_t stored;            /**< entries in universes[] */
    uint16_t count;             /**< universes advertised (may be more than stored) */
    uint32_t age_ms;
} srcmon_advert_t;

/** Another node's ArtPollReply, as artnet_receiver reports it */
typedef struct {
    artnet_poll_reply_t reply;  /**< names made ASCII-safe */
    bool ip_conflict;           /**< uses this node's IP address */
    uint32_t replies;
    uint32_t age_ms;
} srcmon_node_t;

void srcmon_note(const srcmon_packet_t *p);

void srcmon_note_node(const artnet_poll_reply_t *reply, bool ip_conflict);

/** One discovery page (universes_be: @p count big-endian 16-bit values) */
void srcmon_note_discovery(const uint8_t *cid, uint32_t ip, const char *name,
                           uint8_t page, uint8_t last_page,
                           const uint8_t *universes_be, uint16_t count);

/** Copy up to @p max rows, newest first. Returns how many. */
int srcmon_get_sources(srcmon_source_t *out, int max);
int srcmon_get_adverts(srcmon_advert_t *out, int max);
/** Nodes sorted by IP address, then BindIndex */
int srcmon_get_nodes(srcmon_node_t *out, int max);

/**
 * Addresses (network byte order, each once) of the other Art-Net nodes whose
 * last ArtPollReply has an output port on Port-Address @p universe: where an
 * input port sends ArtDmx unicast (send_to "nodes"). Nodes that clash with
 * this node's address are left out. Returns how many were written to @p ips.
 */
int srcmon_output_nodes(uint16_t universe, uint32_t *ips, int max);

/** Forget everything. Call once at start-up, before the receivers run:
 *  until then nothing is recorded. */
void srcmon_reset(void);

#ifdef __cplusplus
}
#endif
