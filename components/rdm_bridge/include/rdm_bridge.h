#ifndef RDM_BRIDGE_H
#define RDM_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief RDM over Art-Net (Art-Net 4 "Output Gateway")
 *
 * - Full RDM discovery when an RDM-enabled output port starts, then
 *   incremental discovery in the background (ArtTodControl can switch it).
 * - ArtTodRequest / ArtTodControl are answered with ArtTodData (unicast).
 *   Controllers that asked for a TOD are sent the new one when it changes.
 * - ArtRdm GET/SET requests are passed to the fixtures and the responses
 *   unicast back to the controller. Discovery commands are refused (the
 *   node runs discovery itself, as the spec requires).
 */

/** Create the bridge task and hook the Art-Net / DMX callbacks */
esp_err_t rdm_bridge_start(void);

/** Art-Net Port-Address of DMX port @p port (1-2); call when the config changes */
void rdm_bridge_set_port_address(uint8_t port, uint16_t port_address);

/** true if background (incremental) discovery is enabled on @p port */
bool rdm_bridge_background_enabled(uint8_t port);

/** Counters for the status page */
typedef struct {
    uint32_t tod_requests;
    uint32_t tod_data_sent;
    uint32_t rdm_requests;
    uint32_t rdm_responses;
    uint32_t rdm_timeouts;
    uint32_t rdm_rejected;
} rdm_bridge_stats_t;

void rdm_bridge_get_stats(rdm_bridge_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* RDM_BRIDGE_H */
