/* Host stub: the timer is simulated in test_dmx_rdm.c */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef void *gptimer_handle_t;
typedef struct { uint64_t count_value, alarm_value; } gptimer_alarm_event_data_t;
typedef struct {
    uint64_t alarm_count, reload_count;
    struct { uint32_t auto_reload_on_alarm : 1; } flags;
} gptimer_alarm_config_t;
typedef struct { int clk_src, direction; uint32_t resolution_hz; } gptimer_config_t;
typedef struct {
    bool (*on_alarm)(gptimer_handle_t, const gptimer_alarm_event_data_t *, void *);
} gptimer_event_callbacks_t;
#define GPTIMER_CLK_SRC_DEFAULT 0
#define GPTIMER_COUNT_UP 0
int gptimer_new_timer(const gptimer_config_t *c, gptimer_handle_t *h);
int gptimer_register_event_callbacks(gptimer_handle_t h, const gptimer_event_callbacks_t *c, void *arg);
int gptimer_enable(gptimer_handle_t h);
int gptimer_disable(gptimer_handle_t h);
int gptimer_del_timer(gptimer_handle_t h);
int gptimer_start(gptimer_handle_t h);
int gptimer_stop(gptimer_handle_t h);
int gptimer_set_raw_count(gptimer_handle_t h, uint64_t v);
int gptimer_set_alarm_action(gptimer_handle_t h, const gptimer_alarm_config_t *c);
