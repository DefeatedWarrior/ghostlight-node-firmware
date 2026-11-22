/*
 * Per-port output processing (see port_rules.h)
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "port_rules.h"

static struct {
    port_rule_t rules[CONFIG_PORT_RULES_MAX];
    uint8_t count;
} s_ports[CONFIG_DMX_PORTS];
static SemaphoreHandle_t s_lock;

static void range(const port_rule_t *r, uint16_t *first, uint16_t *n)
{
    *first = r->channel - 1;
    *n = r->count;
}

void port_rules_apply_list(const port_rule_t *rules, uint8_t count, uint8_t levels[512])
{
    if (!rules || !levels || count == 0) return;
    if (count > CONFIG_PORT_RULES_MAX) count = CONFIG_PORT_RULES_MAX;

    bool any_patch = false;
    for (uint8_t i = 0; i < count; i++) any_patch |= rules[i].type == PORT_RULE_PATCH;
    if (any_patch) {
        uint8_t in[512];
        memcpy(in, levels, sizeof(in));
        for (uint8_t i = 0; i < count; i++) {
            const port_rule_t *r = &rules[i];
            if (r->type != PORT_RULE_PATCH || config_port_rule_problem(r)) continue;
            memcpy(&levels[r->channel - 1], &in[r->from - 1], r->count);
        }
    }
    for (uint8_t i = 0; i < count; i++) {
        const port_rule_t *r = &rules[i];
        if (r->type != PORT_RULE_LIMIT || config_port_rule_problem(r)) continue;
        uint16_t first, n;
        range(r, &first, &n);
        for (uint16_t k = first; k < first + n; k++) {
            if (levels[k] > r->level) levels[k] = r->level;
        }
    }
    for (uint8_t i = 0; i < count; i++) {
        const port_rule_t *r = &rules[i];
        if (r->type != PORT_RULE_PARK || config_port_rule_problem(r)) continue;
        memset(&levels[r->channel - 1], r->level, r->count);
    }
}

static bool lock(void)
{
    if (!s_lock) {
        /* first use is the main task applying the config, before the DMX
         * output starts, so creating it here does not race */
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    return true;
}

void port_rules_set(uint8_t port, const port_rule_t *rules, uint8_t count)
{
    if (port < 1 || port > CONFIG_DMX_PORTS) return;
    if (count > CONFIG_PORT_RULES_MAX) count = CONFIG_PORT_RULES_MAX;
    if (!lock()) return;
    memset(s_ports[port - 1].rules, 0, sizeof(s_ports[port - 1].rules));
    if (rules && count) memcpy(s_ports[port - 1].rules, rules, count * sizeof(port_rule_t));
    s_ports[port - 1].count = rules ? count : 0;
    xSemaphoreGive(s_lock);
}

void port_rules_apply(uint8_t port, uint8_t levels[512])
{
    if (port < 1 || port > CONFIG_DMX_PORTS || !s_ports[port - 1].count) return;
    if (!lock()) return;
    port_rule_t rules[CONFIG_PORT_RULES_MAX];
    uint8_t count = s_ports[port - 1].count;
    memcpy(rules, s_ports[port - 1].rules, sizeof(rules));
    xSemaphoreGive(s_lock);
    port_rules_apply_list(rules, count, levels);
}

bool port_rules_active(uint8_t port)
{
    return port >= 1 && port <= CONFIG_DMX_PORTS && s_ports[port - 1].count > 0;
}
