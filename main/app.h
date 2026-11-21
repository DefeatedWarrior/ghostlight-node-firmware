/**
 * ESP-NODE-2RDM — Art-Net 4 / sACN (E1.31) to 2x DMX512/RDM node
 *
 * Data path (output ports):
 *   Art-Net rx / sACN rx  ->  merge engine (per port)  ->  DMX output task
 *   The DMX output task pulls a fresh merged frame for every DMX frame it
 *   sends (frame source callback), so there is no extra copy task or delay.
 *
 * Data path (input ports):
 *   DMX input task -> on change (plus a short repeat and a 1 s keepalive)
 *   -> ArtDmx and/or sACN; sACN streams are terminated when the input is lost.
 *
 * Synchronous output: ArtSync / E1.31 synchronization packets latch the
 * merged levels (merge engine) and trigger the DMX frame straight away, so
 * every universe of a synchronised controller changes on the same frame.
 *
 * RDM: the RDM bridge answers ArtTodRequest/ArtTodControl and passes ArtRdm
 * GET/SET to the fixtures (discovery runs on the node).
 *
 * Configuration changes from the web UI or Art-Net ArtAddress are applied
 * live by the main task (only network settings need a restart).
 */

/*
 * Shared state of the application (main/). The firmware is split as:
 *
 *   main.c           start-up order, main loop, mDNS, statistics
 *   app_routing.c    network <-> DMX (Art-Net / sACN receive, DMX input send, sync)
 *   app_artnet.c     ArtPollReply status, ArtAddress, ArtIpProg
 *   app_config.c     applies the settings to the running ports
 *   app_scenes.c     power-on scenes and the ArtAddress failsafe record
 *   app_indicator.c  status LED (locate / identify), port LEDs, reset button
 *
 * DMX tester mode (g_app.tester, CONFIG_NODE_TESTER): chosen at start-up
 * from config_t.tester. Art-Net, sACN, the RDM bridge and scenes are not
 * started; the ports run the tester jobs (config_tester_port_settings()),
 * send ports output the dmx_tester levels and receive ports feed the
 * dmx_tester statistics instead of the network.
 *
 * Callbacks from the network and DMX tasks only read g_app.route and set
 * event bits; everything that changes state runs on the main task.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "config_manager.h"
#include "dmx_handler.h"
#include "network_manager.h"
#include "artnet_receiver.h"
#include "sacn_receiver.h"
#include "dmx_tools.h"
#include "dmx_tester.h"

#define NPORTS      CONFIG_DMX_PORTS

/* Main task event bits (g_app.events) */
#define EV_APPLY    BIT0   /* apply config to the running system */
#define EV_SAVE     BIT1   /* save config (changed by ArtAddress) */
#define EV_NET      BIT2   /* an interface got an address */
#define EV_FAILREC  BIT3   /* ArtAddress FailRecord: store the output as failsafe scenes */
#define EV_TAKEOVER BIT4   /* network data arrived on a port playing its power-on scene */
#define EV_NETCFG   BIT5   /* network settings changed (ArtIpProg) */

/* Live routing for one port, read by the receive callbacks */
typedef struct {
    volatile dmx_mode_t mode;
    volatile protocol_mode_t proto;
    volatile merge_mode_t merge;
    volatile uint16_t artnet;          /* 15-bit Port-Address */
    volatile int16_t artnet_backup;    /* -1 = none */
    volatile uint16_t sacn;            /* 1..63999 */

    /* DMX input -> network */
    uint8_t last_tx[512];
    int64_t last_tx_us;
    uint8_t repeat;
    bool tx_started;
    uint16_t tx_sacn_universe;         /* universe the sACN stream runs on */
    uint32_t err_baseline;             /* ArtAddress ResetRxFlags */
} route_t;

typedef struct {
    route_t route[NPORTS];
    port_config_t applied[NPORTS];       /* settings the ports run with now */
    bool applied_valid;
    EventGroupHandle_t events;
    SemaphoreHandle_t apply_lock;
    volatile uint8_t indicator;          /* 0 normal, 1 locate, 2 mute */
    volatile bool identify[NPORTS];      /* RDM identify per port */
    volatile int64_t web_identify_until; /* web "Identify" button, 0 = off */
    volatile uint8_t startup_mask;       /* ports showing their power-on scene */
    bool tester;                         /* started in DMX tester mode (no Art-Net / sACN) */
} app_state_t;

extern app_state_t g_app;

static inline bool app_mode_is_output(dmx_mode_t m)
{
    return m == DMX_MODE_OUTPUT || m == DMX_MODE_RDM_MASTER;
}

static inline bool app_mode_is_input(dmx_mode_t m)
{
    return m == DMX_MODE_INPUT || m == DMX_MODE_RDM_RESPONDER;
}

/* true while the DMX self-test owns the port hardware (web DMX tools) */
static inline bool app_selftest_running(void)
{
#if CONFIG_NODE_DMX_TOOLS
    return dmx_selftest_running();
#else
    return false;
#endif
}

/* app_routing.c */
void app_on_network_up(void);
void app_on_artnet_dmx(uint16_t universe, const uint8_t *data,
                       uint16_t length, uint8_t sequence, uint32_t source_ip,
                       void *user_data);
void app_on_sacn_dmx(const sacn_packet_info_t *pkt, void *user_data);
void app_on_artnet_sync(uint32_t source_ip, void *user_data);
void app_on_sacn_sync(uint16_t sync_universe, const uint8_t *cid, uint32_t source_ip,
                      void *user_data);
void app_update_sacn_groups(void);
void app_dmx_frame_source(uint8_t port, uint8_t *data, void *user_data);
void app_on_dmx_input(uint8_t port, const uint8_t *data, size_t size, void *user_data);
void app_on_dmx_loss(uint8_t port, void *user_data);

/* app_artnet.c */
void app_artnet_status(artnet_node_status_t *st, void *user_data);
void app_artnet_address(const artnet_address_cmd_t *cmd, void *user_data);
void app_on_artnet_ipprog(const artnet_ipprog_cmd_t *cmd, artnet_ipprog_reply_t *reply,
                          void *user_data);

/* app_config.c */
void app_config_changed(void *user_data);
void app_apply_config(void);
void app_selftest_restore(void *user_data);

/* app_scenes.c */
void app_start_power_on_scenes(void);
void app_release_power_on_scenes(void);
void app_record_failsafe_scenes(void);

/* app_indicator.c */
void app_update_indicator(void);
void app_network_state_changed(network_state_t state, void *user_data);
void app_on_rdm_identify(uint8_t port, bool identify, void *user_data);
void app_web_identify(uint32_t seconds);
uint32_t app_web_identify_left(void);
/* Starts the reset button and port LED tasks (if the board has them) */
void app_indicator_start_tasks(void);
