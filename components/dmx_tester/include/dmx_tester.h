/**
 * @file dmx_tester.h
 * @brief DMX tester mode: what the ports send and what they receive
 *
 * In tester mode (CONFIG_NODE_TESTER, switched on in the web UI and kept
 * across restarts) the node is not an Art-Net / sACN node. Each port has
 * one job (config_t.tester): send, receive or RDM controller.
 *
 * - Send / RDM ports output the levels held here (set from the web UI),
 *   unless a test pattern (dmx_tools) runs on the port.
 * - Receive ports feed every frame in, and this keeps per-channel
 *   statistics (lowest, highest, number of changes, changed in the last
 *   second: the flicker watch), the time between frames (average, jitter,
 *   histogram) and how often the signal was lost.
 *
 * The rest of the analysis (refresh rate, slot count, start codes, break /
 * MAB, framing errors) comes from dmx_handler_get_analysis().
 *
 * Memory (about 4.5 KB per port) is only allocated when tester mode is on.
 */
#ifndef DMX_TESTER_H
#define DMX_TESTER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Frame-interval histogram: bucket k counts intervals below
 *  DMX_TESTER_HIST_EDGES_MS[k] (and at least the previous edge); the last
 *  bucket counts everything from the last edge up. */
#define DMX_TESTER_HIST_BUCKETS 9
extern const uint16_t DMX_TESTER_HIST_EDGES_MS[DMX_TESTER_HIST_BUCKETS - 1];

typedef struct {
    uint32_t frames;               /**< level frames since the last reset */
    uint32_t seconds;              /**< since the last reset */
    uint32_t losses;               /**< times the signal was lost */
    int32_t last_loss_s;           /**< seconds since the last loss, -1 = none */
    uint32_t interval_avg_us;      /**< time between frames (gaps over 1 s left out) */
    uint32_t interval_jitter_us;   /**< standard deviation of that */
    uint32_t interval_min_us;      /**< 0 = fewer than two frames yet */
    uint32_t interval_max_us;      /**< includes gaps */
    uint32_t hist[DMX_TESTER_HIST_BUCKETS];
    uint16_t changing;             /**< channels that changed in the last second */
    uint16_t changed;              /**< channels that changed at least once */
} dmx_tester_stats_t;

/** Allocate the buffers (only when @p enabled) and remember the mode */
esp_err_t dmx_tester_init(bool enabled);
/** true when the node started in tester mode */
bool dmx_tester_enabled(void);

// ---- Send ------------------------------------------------------------------

/** Set @p count levels starting at channel @p start (1-based) */
esp_err_t dmx_tester_set_levels(uint8_t port, uint16_t start, const uint8_t *values, uint16_t count);
/** Set channels @p first..@p last (1-based, inclusive) to @p value */
esp_err_t dmx_tester_fill(uint8_t port, uint16_t first, uint16_t last, uint8_t value);
/** The levels a send port outputs (512 slots, zero if tester mode is off) */
void dmx_tester_get_levels(uint8_t port, uint8_t data[512]);

// ---- Receive ---------------------------------------------------------------

/** A level frame arrived (512 slots; dmx_handler fills in what a short frame leaves out) */
void dmx_tester_on_frame(uint8_t port, const uint8_t data[512]);
/** The input signal was lost */
void dmx_tester_on_loss(uint8_t port);
/** Start the statistics of @p port again */
esp_err_t dmx_tester_reset(uint8_t port);
esp_err_t dmx_tester_get_stats(uint8_t port, dmx_tester_stats_t *out);
/**
 * Per-channel statistics (any pointer may be NULL): lowest and highest level
 * seen, number of changes, and a bit mask (bit c%8 of byte c/8, channel c+1)
 * of the channels that changed in the last second.
 */
esp_err_t dmx_tester_get_channels(uint8_t port, uint8_t min[512], uint8_t max[512],
                                  uint32_t changes[512], uint8_t changing[64]);

#ifdef __cplusplus
}
#endif

#endif /* DMX_TESTER_H */
