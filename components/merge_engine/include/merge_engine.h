/**
 * @file merge_engine.h
 * @brief Multi-source DMX merge engine (Art-Net / sACN / local layer)
 *
 * Per output port the engine tracks up to MERGE_MAX_SOURCES network sources
 * and produces one 512-slot output frame.
 *
 *  - Sources are identified by CID (sACN) or IP address (Art-Net).
 *  - sACN priority (E1.31 packet priority and per-address priority 0xDD) is
 *    honoured: per slot, only sources at the highest priority take part.
 *    Art-Net sources are treated as priority 100 (the sACN default).
 *  - Merge modes:
 *      HTP     highest value wins (per slot)
 *      LTP     latest change wins (per slot)
 *      LAST    the source that sent the most recent packet owns the frame
 *      BACKUP  primary/backup failover (secondary universe, or oldest source)
 *      DISABLE no merge: the first source owns the port until it times out
 *  - The protocol filter (Art-Net only / sACN only / priority / both) is
 *    applied before merging.
 *  - Sequence numbers are checked per source (E1.31 6.7.2 window rule), so
 *    late/duplicate UDP packets are discarded instead of causing flicker.
 *  - A "local" layer (scene playback, web test) is HTP-merged on top of the
 *    network result and never times out.
 *  - When every source is lost the output either holds the last look or goes
 *    to zero, depending on the configured loss behaviour.
 *
 * Thread safety: all functions may be called from any task.
 */

#ifndef MERGE_ENGINE_H
#define MERGE_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "config_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum tracked network sources per port */
#define MERGE_MAX_SOURCES 6

/** Default source-loss timeout (E1.31 network data loss = 2.5 s) */
#define MERGE_DEFAULT_TIMEOUT_US 2500000

/** Priority assigned to Art-Net sources (same as the sACN default) */
#define MERGE_ARTNET_PRIORITY 100

typedef enum {
    SOURCE_PROTOCOL_ARTNET = 0,
    SOURCE_PROTOCOL_SACN   = 1,
    SOURCE_PROTOCOL_DMX_IN = 2,
} source_protocol_t;

/** One received network packet, as handed to merge_engine_push() */
typedef struct {
    source_protocol_t protocol;
    uint32_t source_ip;        /**< as received (network byte order) */
    const uint8_t *cid;        /**< 16-byte sACN CID, NULL for Art-Net */
    const char *name;          /**< source name, may be NULL */
    uint8_t priority;          /**< 0-200 (Art-Net: MERGE_ARTNET_PRIORITY) */
    uint8_t sequence;          /**< packet sequence number */
    bool check_sequence;       /**< false for Art-Net sequence 0 */
    uint8_t start_code;        /**< 0x00 = levels, 0xDD = per-address priority */
    const uint8_t *data;       /**< slot data (without the start code) */
    uint16_t length;           /**< 1..512 */
    bool backup;               /**< received on the port's secondary universe */
    bool terminated;           /**< sACN Stream_Terminated: drop the source now */
    uint16_t sync_address;     /**< sACN synchronization universe (0 = none) */
    bool force_sync;           /**< sACN Force_Synchronization option bit */
} merge_input_t;

/** Output synchronization state of a port */
typedef enum {
    MERGE_SYNC_NONE = 0,       /**< levels go out as they arrive */
    MERGE_SYNC_ARTNET,         /**< latched by ArtSync */
    MERGE_SYNC_SACN,           /**< latched by E1.31 synchronization packets */
} merge_sync_t;

/** Snapshot of one source, for status pages */
typedef struct {
    source_protocol_t protocol;
    uint32_t source_ip;
    char name[64];
    uint8_t priority;
    bool uses_pap;             /**< sending per-address priority (0xDD) */
    bool backup;
    bool contributing;         /**< currently part of the output */
    uint32_t age_ms;           /**< time since the last packet */
    uint32_t packets;
} merge_source_info_t;

typedef struct {
    uint32_t total_merges;     /**< output frames computed */
    uint32_t htp_merges;
    uint32_t ltp_merges;
    uint32_t last_merges;
    uint32_t backup_switches;  /**< backup / owner changes */
    uint32_t source_timeouts;
    uint32_t active_sources;
    uint32_t sequence_drops;   /**< late/duplicate packets discarded */
    uint32_t sources_rejected; /**< new sources refused (table full) */
    uint32_t packets_filtered; /**< dropped by the protocol filter */
    uint32_t sync_frames;      /**< frames latched by a sync packet */
} merge_stats_t;

esp_err_t merge_engine_init(void);
esp_err_t merge_engine_deinit(void);

/** Merge mode and source-loss timeout (ms) for a port (1 or 2) */
esp_err_t merge_engine_config(uint8_t port, merge_mode_t mode, uint32_t timeout_ms);

/** Which protocols the port accepts (see protocol_mode_t) */
esp_err_t merge_engine_set_protocol_mode(uint8_t port, protocol_mode_t mode);

/** true: hold the last look when all sources are lost; false: go to zero */
esp_err_t merge_engine_set_hold_last_look(uint8_t port, bool hold);

/**
 * What the network layer outputs once every source is lost (Art-Net 4
 * failsafe). Not applied before the first source has been seen, so a node
 * that never received data stays at zero (or its power-on scene).
 * @p scene 512 levels for LOSS_SCENE (copied; NULL = zero)
 */
esp_err_t merge_engine_set_loss_behavior(uint8_t port, loss_behavior_t loss, const uint8_t *scene);

/** true while the port shows its failsafe look (sources lost) */
bool merge_engine_in_failsafe(uint8_t port);

/** true while the port should send no DMX at all: every source is lost and
 *  its loss behaviour is LOSS_STOP_OUTPUT (and no local levels are up) */
bool merge_engine_output_stopped(uint8_t port);

/** Feed one received packet to a port */
esp_err_t merge_engine_push(uint8_t port, const merge_input_t *in);

/**
 * Local layer (scenes, web test). HTP-merged on top of the network output.
 * It never times out; clear it with merge_engine_clear_local().
 */
esp_err_t merge_engine_set_local(uint8_t port, const uint8_t *data);
esp_err_t merge_engine_set_local_channel(uint8_t port, uint16_t channel, uint8_t value);
esp_err_t merge_engine_get_local(uint8_t port, uint8_t *data, bool *active);
esp_err_t merge_engine_clear_local(uint8_t port);

/**
 * Compute the frame to put on the wire. Always fills @p data (512 bytes) and
 * returns ESP_OK; use merge_engine_is_output_active() to know whether any
 * source currently drives it.
 */
esp_err_t merge_engine_get_output(uint8_t port, uint8_t *data);

/** true while at least one network source or the local layer is active */
bool merge_engine_is_output_active(uint8_t port);

/**
 * ArtSync from @p source_ip (network byte order). Latches the current levels
 * of every port fed only by that controller (not merging) and puts it in
 * synchronous mode (until 4 s pass without ArtSync).
 * @return bit mask of latched ports (bit 0 = port 1); trigger their output
 */
uint8_t merge_engine_artnet_sync(uint32_t source_ip);

/**
 * E1.31 synchronization packet for @p sync_universe. Latches every port
 * whose sources all name that synchronization universe.
 * @return bit mask of latched ports
 */
uint8_t merge_engine_sacn_sync(uint16_t sync_universe);

merge_sync_t merge_engine_get_sync_state(uint8_t port);

/** Synchronization universes the port's sACN sources use (deduplicated) */
uint16_t merge_engine_sync_universes(uint8_t port, uint16_t *out, uint8_t max);

/**
 * Art-Net AcCancelMerge. If the port is merging Art-Net sources, the sender
 * of the next ArtDmx takes it over and other Art-Net senders are discarded.
 * sACN and local sources are untouched. The takeover ends when that sender
 * stops, on merge_engine_resume_merge(), a merge mode change or a blackout;
 * it isn't saved.
 */
esp_err_t merge_engine_cancel_merge(uint8_t port);

/** Undo merge_engine_cancel_merge() (AcMergeLtp / AcMergeHtp) */
esp_err_t merge_engine_resume_merge(uint8_t port);

/** Drop all sources, clear the local layer and zero the output */
esp_err_t merge_engine_blackout(uint8_t port);

/** Copy up to @p max source snapshots; returns the number copied */
uint8_t merge_engine_get_sources(uint8_t port, merge_source_info_t *out, uint8_t max);

esp_err_t merge_engine_get_stats(uint8_t port, merge_stats_t *stats);
esp_err_t merge_engine_reset_stats(uint8_t port);

#ifdef __cplusplus
}
#endif

#endif // MERGE_ENGINE_H
