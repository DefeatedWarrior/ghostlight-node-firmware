#ifndef ARTNET_TRANSMITTER_H
#define ARTNET_TRANSMITTER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief ArtDmx sender for DMX input ports.
 *
 * Packets go to the subnet's directed broadcast address (set with
 * artnet_transmitter_set_destination(); limited broadcast until then) and,
 * when the node's own 6454 socket is provided, use 6454 as the source port
 * as the Art-Net specification requires. Sequence numbers are kept per
 * universe and never 0 (0 means "sequencing disabled").
 */

typedef struct {
    uint32_t packets_sent;
    uint32_t send_errors;
} artnet_tx_stats_t;

esp_err_t artnet_transmitter_init(void);
esp_err_t artnet_transmitter_deinit(void);

/** Send ArtDmx for a 15-bit Port-Address. @p physical is the input port (0-based). */
esp_err_t artnet_transmitter_send_port(uint16_t universe, uint8_t physical,
                                       const uint8_t *data, uint16_t length);

/**
 * Send ArtDmx unicast to each of @p ips (@p count addresses, network byte
 * order), with one sequence number for all of them. @p count 0 sends to the
 * broadcast destination like artnet_transmitter_send_port().
 */
esp_err_t artnet_transmitter_send_port_to(uint16_t universe, uint8_t physical,
                                          const uint8_t *data, uint16_t length,
                                          const uint32_t *ips, int count);

/** Compatibility wrapper (physical port 0) */
esp_err_t artnet_transmitter_send(uint16_t universe, const uint8_t *data, uint16_t length);

/** Destination IPv4 (network byte order), normally the directed broadcast; 0 = 255.255.255.255 */
void artnet_transmitter_set_destination(uint32_t ip_addr);

/** Send through this already-bound socket (the node's UDP 6454 socket); -1 = own socket */
void artnet_transmitter_use_socket(int sock);

esp_err_t artnet_transmitter_get_stats(artnet_tx_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif // ARTNET_TRANSMITTER_H
