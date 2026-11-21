#ifndef SACN_TRANSMITTER_H
#define SACN_TRANSMITTER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief E1.31 (sACN) source for DMX input ports.
 *
 * - CID is a version-4-format UUID derived from the chip MAC, so it is stable
 *   across reboots and unique per node (E1.31 requires a persistent CID).
 * - Stream termination (3 packets with the Stream_Terminated option) is sent
 *   when an input signal is lost, so receivers release the universe at once
 *   instead of waiting for the 2.5 s timeout.
 * - E1.31 universe discovery packets listing the active universes are sent
 *   every 10 s from sacn_transmitter_tick().
 */

#define SACN_TX_DEFAULT_PRIORITY 100

typedef struct {
    uint32_t packets_sent;
    uint32_t send_errors;
    uint32_t terminations_sent;
    uint32_t discovery_sent;
} sacn_tx_stats_t;

esp_err_t sacn_transmitter_init(const char *source_name);
esp_err_t sacn_transmitter_deinit(void);

/** Send one data packet (start code 0, @p length slots) */
esp_err_t sacn_transmitter_send(uint16_t universe, const uint8_t *data,
                                uint16_t length, uint8_t priority);

/** Send the 3 Stream_Terminated packets for @p universe and forget it */
esp_err_t sacn_transmitter_terminate(uint16_t universe);

/** Call about once a second: sends universe discovery every 10 s */
void sacn_transmitter_tick(void);

/** Update the source name carried in every packet */
void sacn_transmitter_set_source_name(const char *source_name);

/** Send multicast on the interface with this IPv4 address (network order), 0 = default */
void sacn_transmitter_set_interface(uint32_t ip_addr);

/** This node's CID (16 bytes) */
const uint8_t *sacn_transmitter_get_cid(void);

esp_err_t sacn_transmitter_get_stats(sacn_tx_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif // SACN_TRANSMITTER_H
