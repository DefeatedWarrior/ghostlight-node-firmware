/**
 * @file port_rules.h
 * @brief Per-port output processing: patch, limit and park (port_rule_t)
 *
 * The rules a port runs with are copied in by the main task when the
 * configuration is applied (port_rules_set) and read by the DMX output task
 * for every frame (port_rules_apply), so changing them never tears a frame.
 */
#ifndef PORT_RULES_H
#define PORT_RULES_H

#include <stdint.h>
#include <stdbool.h>
#include "config_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Apply @p count rules to 512 levels in place: patches first (each reads
 * the levels as they were before any patch), then limits, then parks.
 * Rules that fail config_port_rule_problem() are skipped.
 */
void port_rules_apply_list(const port_rule_t *rules, uint8_t count, uint8_t levels[512]);

/** Set the rules port @p port (1-based) runs with (copied) */
void port_rules_set(uint8_t port, const port_rule_t *rules, uint8_t count);

/** Apply port @p port's rules to its 512 output levels in place */
void port_rules_apply(uint8_t port, uint8_t levels[512]);

/** true if port @p port has any rules */
bool port_rules_active(uint8_t port);

#ifdef __cplusplus
}
#endif

#endif /* PORT_RULES_H */
