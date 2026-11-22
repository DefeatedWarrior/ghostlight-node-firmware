/**
 * @file merge_engine.c
 * @brief Multi-source DMX merge engine — see merge_engine.h for the model.
 */

#include "merge_engine.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <inttypes.h>

static const char *TAG = "merge_engine";

#define MERGE_PORTS CONFIG_DMX_PORTS

/* Per-address priority (0xDD) is considered stale after this long without a
 * 0xDD packet; the source then reverts to its packet priority. */
#define PAP_TIMEOUT_US   2500000
/* A new sACN source joining a port that already has sources is held back for
 * up to this long, waiting for a possible 0xDD packet, so it cannot flash
 * levels into slots it does not actually source (E1.31 recommendation). */
#define PAP_WAIT_US      1500000

#define NO_SOURCE 0xFF

/* Art-Net 4: back to non-synchronous output after 4 s without ArtSync */
#define ARTNET_SYNC_TIMEOUT_US  4000000
/* E1.31 11.2.2: synchronization is lost after E131_NETWORK_DATA_LOSS_TIMEOUT */
#define SACN_SYNC_TIMEOUT_US    2500000

typedef struct {
    bool in_use;
    source_protocol_t protocol;
    uint32_t ip;
    uint8_t cid[16];
    char name[64];
    uint8_t priority;          /* packet (universe) priority */
    bool backup;
    bool seq_valid;
    uint8_t seq;
    bool has_levels;
    bool contributing;
    uint8_t levels[512];
    uint8_t pap[512];
    int64_t first_seen_us;
    int64_t last_rx_us;        /* last 0x00 (levels) packet */
    int64_t pap_rx_us;         /* last 0xDD packet, 0 = never */
    int64_t pending_until_us;
    uint32_t packets;
    uint16_t sync_address;     /* sACN synchronization universe, 0 = none */
    bool force_sync;           /* sACN Force_Synchronization option */
} source_t;

typedef struct {
    SemaphoreHandle_t mutex;
    merge_mode_t mode;
    protocol_mode_t protocol_mode;
    loss_behavior_t loss;      /* failsafe look once every source is lost */
    uint8_t loss_scene[512];   /* LOSS_SCENE levels */
    bool had_net;              /* a source has been seen since power-on */
    bool failsafe;             /* currently showing the failsafe look */
    int64_t timeout_us;

    source_t sources[MERGE_MAX_SOURCES];
    uint8_t ltp_src[512];      /* LTP: source index that last changed each slot */
    uint8_t owner;             /* owner-based modes: current owner index */

    uint8_t last_net[512];     /* last frame produced from network sources */
    bool net_active;

    uint8_t local[512];
    bool local_active;

    /* Synchronous output: the network layer only changes when a sync
     * packet latches it into held[] */
    merge_sync_t sync;
    uint32_t sync_ip;          /* Art-Net: controller sending ArtSync */
    uint16_t sync_addr;        /* sACN: synchronization universe */
    int64_t last_sync_us;
    uint8_t held[512];

    /* Art-Net AcCancelMerge */
    int64_t cancel_until_us;   /* the next ArtDmx takes over, 0 = not asked */
    bool sole;                 /* only sole_ip's ArtDmx is taken */
    uint32_t sole_ip;

    merge_stats_t stats;
} port_ctx_t;

static struct {
    bool initialized;
    port_ctx_t port[MERGE_PORTS];
} s_merge;

static port_ctx_t *ctx_for(uint8_t port)
{
    if (!s_merge.initialized || port < 1 || port > MERGE_PORTS) {
        return NULL;
    }
    return &s_merge.port[port - 1];
}

static const char *proto_name(source_protocol_t p)
{
    switch (p) {
        case SOURCE_PROTOCOL_ARTNET: return "Art-Net";
        case SOURCE_PROTOCOL_SACN:   return "sACN";
        case SOURCE_PROTOCOL_DMX_IN: return "DMX-in";
        default:                     return "?";
    }
}

static void log_source(const char *what, uint8_t port, const source_t *s)
{
    uint32_t ip = s->ip;
    if (s->protocol == SOURCE_PROTOCOL_DMX_IN) {
        /* source_ip is the input port number */
        ESP_LOGI(TAG, "Port %u: %s DMX-in source port %u", port, what, (unsigned)ip);
        return;
    }
    ESP_LOGI(TAG, "Port %u: %s %s source %u.%u.%u.%u \"%s\" prio %u%s",
             port, what, proto_name(s->protocol),
             (unsigned)(ip & 0xFF), (unsigned)((ip >> 8) & 0xFF),
             (unsigned)((ip >> 16) & 0xFF), (unsigned)((ip >> 24) & 0xFF),
             s->name, s->priority, s->backup ? " (backup)" : "");
}

static void remove_source(port_ctx_t *ctx, uint8_t idx)
{
    for (int c = 0; c < 512; c++) {
        if (ctx->ltp_src[c] == idx) {
            ctx->ltp_src[c] = NO_SOURCE;
        }
    }
    if (ctx->owner == idx) {
        ctx->owner = NO_SOURCE;
    }
    const source_t *s = &ctx->sources[idx];
    if (ctx->sole && s->protocol == SOURCE_PROTOCOL_ARTNET && s->ip == ctx->sole_ip) {
        ctx->sole = false;     /* the console that took over has gone */
    }
    memset(&ctx->sources[idx], 0, sizeof(source_t));
}

static void cancel_clear(port_ctx_t *ctx)
{
    ctx->cancel_until_us = 0;
    ctx->sole = false;
}

/* AcCancelMerge: false if this ArtDmx is to be discarded */
static bool cancel_filter(port_ctx_t *ctx, uint8_t port, const merge_input_t *in, int64_t now)
{
    if (ctx->cancel_until_us) {
        bool due = now <= ctx->cancel_until_us;
        ctx->cancel_until_us = 0;
        if (due) {
            /* The sources before the lock, so removing them can't clear it */
            ctx->sole = false;
            for (uint8_t i = 0; i < MERGE_MAX_SOURCES; i++) {
                const source_t *s = &ctx->sources[i];
                if (s->in_use && s->protocol == SOURCE_PROTOCOL_ARTNET && s->ip != in->source_ip) {
                    remove_source(ctx, i);
                }
            }
            ctx->sole = true;
            ctx->sole_ip = in->source_ip;
            uint32_t ip = in->source_ip;
            ESP_LOGI(TAG, "Port %u: merge cancelled, only %u.%u.%u.%u's ArtDmx is taken", port,
                     (unsigned)(ip & 0xFF), (unsigned)((ip >> 8) & 0xFF),
                     (unsigned)((ip >> 16) & 0xFF), (unsigned)((ip >> 24) & 0xFF));
            return true;
        }
    }
    if (!ctx->sole || in->source_ip == ctx->sole_ip) return true;
    for (int i = 0; i < MERGE_MAX_SOURCES; i++) {
        const source_t *s = &ctx->sources[i];
        if (s->in_use && s->protocol == SOURCE_PROTOCOL_ARTNET && s->ip == ctx->sole_ip &&
            now - s->last_rx_us <= ctx->timeout_us) {
            return false;
        }
    }
    ctx->sole = false;         /* it stopped */
    return true;
}

static void expire_sources(port_ctx_t *ctx, uint8_t port, int64_t now)
{
    for (uint8_t i = 0; i < MERGE_MAX_SOURCES; i++) {
        source_t *s = &ctx->sources[i];
        if (s->in_use && (now - s->last_rx_us) > ctx->timeout_us) {
            log_source("lost (timeout)", port, s);
            ctx->stats.source_timeouts++;
            remove_source(ctx, i);
        }
    }
}

static bool protocol_allowed(protocol_mode_t pm, source_protocol_t p)
{
    if (p == SOURCE_PROTOCOL_DMX_IN) return true;
    switch (pm) {
        case PROTOCOL_ARTNET_ONLY: return p == SOURCE_PROTOCOL_ARTNET;
        case PROTOCOL_SACN_ONLY:   return p == SOURCE_PROTOCOL_SACN;
        default:                   return true;
    }
}

static int find_source(port_ctx_t *ctx, const merge_input_t *in)
{
    for (int i = 0; i < MERGE_MAX_SOURCES; i++) {
        const source_t *s = &ctx->sources[i];
        if (!s->in_use || s->protocol != in->protocol) continue;
        if (in->protocol == SOURCE_PROTOCOL_SACN && in->cid) {
            if (memcmp(s->cid, in->cid, 16) == 0) return i;
        } else if (s->ip == in->source_ip) {
            return i;
        }
    }
    return -1;
}

static int alloc_source(port_ctx_t *ctx)
{
    for (int i = 0; i < MERGE_MAX_SOURCES; i++) {
        if (!ctx->sources[i].in_use) return i;
    }
    return -1;
}

static inline bool pap_current(const source_t *s, int64_t now)
{
    return s->pap_rx_us != 0 && (now - s->pap_rx_us) < PAP_TIMEOUT_US;
}

// ============================================================================
// Init / config
// ============================================================================

esp_err_t merge_engine_init(void)
{
    if (s_merge.initialized) {
        return ESP_OK;
    }
    memset(&s_merge, 0, sizeof(s_merge));
    for (int p = 0; p < MERGE_PORTS; p++) {
        port_ctx_t *ctx = &s_merge.port[p];
        ctx->mutex = xSemaphoreCreateMutex();
        if (!ctx->mutex) {
            ESP_LOGE(TAG, "mutex alloc failed");
            return ESP_ERR_NO_MEM;
        }
        ctx->mode = MERGE_MODE_HTP;
        ctx->protocol_mode = PROTOCOL_MERGE_BOTH;
        ctx->loss = LOSS_HOLD_LAST_LOOK;
        ctx->timeout_us = MERGE_DEFAULT_TIMEOUT_US;
        ctx->owner = NO_SOURCE;
        memset(ctx->ltp_src, NO_SOURCE, sizeof(ctx->ltp_src));
    }
    s_merge.initialized = true;
    ESP_LOGI(TAG, "Merge engine initialised (%d sources/port)", MERGE_MAX_SOURCES);
    return ESP_OK;
}

esp_err_t merge_engine_deinit(void)
{
    if (!s_merge.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    for (int p = 0; p < MERGE_PORTS; p++) {
        if (s_merge.port[p].mutex) {
            vSemaphoreDelete(s_merge.port[p].mutex);
        }
    }
    memset(&s_merge, 0, sizeof(s_merge));
    return ESP_OK;
}

esp_err_t merge_engine_config(uint8_t port, merge_mode_t mode, uint32_t timeout_ms)
{
    port_ctx_t *ctx = ctx_for(port);
    if (!ctx) return ESP_ERR_INVALID_ARG;
    if (mode > MERGE_MODE_DISABLE) return ESP_ERR_INVALID_ARG;
    if (timeout_ms < 500) timeout_ms = 500;
    if (timeout_ms > 60000) timeout_ms = 60000;

    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    if (ctx->mode != mode) {
        memset(ctx->ltp_src, NO_SOURCE, sizeof(ctx->ltp_src));
        ctx->owner = NO_SOURCE;
        cancel_clear(ctx);
    }
    ctx->mode = mode;
    ctx->timeout_us = (int64_t)timeout_ms * 1000;
    xSemaphoreGive(ctx->mutex);

    ESP_LOGI(TAG, "Port %u: merge mode %d, timeout %" PRIu32 " ms", port, mode, timeout_ms);
    return ESP_OK;
}

esp_err_t merge_engine_set_protocol_mode(uint8_t port, protocol_mode_t mode)
{
    port_ctx_t *ctx = ctx_for(port);
    if (!ctx || mode > PROTOCOL_MERGE_BOTH) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    ctx->protocol_mode = mode;
    /* Drop sources the new filter no longer accepts */
    for (uint8_t i = 0; i < MERGE_MAX_SOURCES; i++) {
        if (ctx->sources[i].in_use &&
            !protocol_allowed(mode, ctx->sources[i].protocol)) {
            remove_source(ctx, i);
        }
    }
    xSemaphoreGive(ctx->mutex);
    return ESP_OK;
}

esp_err_t merge_engine_set_hold_last_look(uint8_t port, bool hold)
{
    return merge_engine_set_loss_behavior(port, hold ? LOSS_HOLD_LAST_LOOK : LOSS_BLACKOUT, NULL);
}

esp_err_t merge_engine_set_loss_behavior(uint8_t port, loss_behavior_t loss, const uint8_t *scene)
{
    port_ctx_t *ctx = ctx_for(port);
    if (!ctx || loss > LOSS_LAST) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    ctx->loss = loss;
    if (scene) memcpy(ctx->loss_scene, scene, 512);
    else memset(ctx->loss_scene, 0, 512);
    xSemaphoreGive(ctx->mutex);
    return ESP_OK;
}

bool merge_engine_in_failsafe(uint8_t port)
{
    port_ctx_t *ctx = ctx_for(port);
    return ctx && ctx->failsafe;
}

// ============================================================================
// Input
// ============================================================================

esp_err_t merge_engine_push(uint8_t port, const merge_input_t *in)
{
    port_ctx_t *ctx = ctx_for(port);
    /* A Stream_Terminated packet's slots are ignored (E1.31 6.2.6), so it
     * may carry none */
    if (!ctx || !in || !in->data || in->length > 512 ||
        (in->length == 0 && !in->terminated)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (in->start_code != 0x00 && in->start_code != 0xDD) {
        return ESP_OK;  /* other alternate start codes are not merged */
    }

    int64_t now = esp_timer_get_time();
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);

    if (!protocol_allowed(ctx->protocol_mode, in->protocol)) {
        ctx->stats.packets_filtered++;
        xSemaphoreGive(ctx->mutex);
        return ESP_OK;
    }

    if (in->protocol == SOURCE_PROTOCOL_ARTNET && !in->terminated &&
        !cancel_filter(ctx, port, in, now)) {
        ctx->stats.packets_filtered++;
        xSemaphoreGive(ctx->mutex);
        return ESP_OK;
    }

    int idx = find_source(ctx, in);

    if (in->terminated) {
        if (idx >= 0) {
            log_source("terminated", port, &ctx->sources[idx]);
            remove_source(ctx, (uint8_t)idx);
        }
        xSemaphoreGive(ctx->mutex);
        return ESP_OK;
    }

    bool is_new = false;
    if (idx < 0) {
        idx = alloc_source(ctx);
        if (idx < 0) {
            expire_sources(ctx, port, now);
            idx = alloc_source(ctx);
        }
        if (idx < 0) {
            ctx->stats.sources_rejected++;
            xSemaphoreGive(ctx->mutex);
            return ESP_ERR_NO_MEM;
        }
        source_t *s = &ctx->sources[idx];
        memset(s, 0, sizeof(*s));
        s->in_use = true;
        s->protocol = in->protocol;
        s->ip = in->source_ip;
        if (in->cid) memcpy(s->cid, in->cid, 16);
        s->first_seen_us = now;
        s->last_rx_us = now;
        s->backup = in->backup;

        /* Hold a new sACN source back briefly if it would merge with others */
        if (in->protocol == SOURCE_PROTOCOL_SACN) {
            for (int i = 0; i < MERGE_MAX_SOURCES; i++) {
                if (i != idx && ctx->sources[i].in_use && ctx->sources[i].has_levels) {
                    s->pending_until_us = now + PAP_WAIT_US;
                    break;
                }
            }
        }
        is_new = true;
    }

    source_t *s = &ctx->sources[idx];

    /* E1.31 6.7.2: discard if -20 < (new - last) <= 0 */
    if (in->check_sequence) {
        if (s->seq_valid) {
            int8_t diff = (int8_t)(in->sequence - s->seq);
            if (diff <= 0 && diff > -20) {
                ctx->stats.sequence_drops++;
                xSemaphoreGive(ctx->mutex);
                return ESP_OK;
            }
        }
        s->seq = in->sequence;
        s->seq_valid = true;
    }

    if (in->name) {
        strncpy(s->name, in->name, sizeof(s->name) - 1);
        s->name[sizeof(s->name) - 1] = '\0';
    }
    s->packets++;

    if (in->start_code == 0xDD) {
        memset(s->pap, 0, sizeof(s->pap));
        memcpy(s->pap, in->data, in->length);
        s->pap_rx_us = now;
    } else {
        bool first_levels = !s->has_levels;
        s->priority = in->priority;
        s->backup = in->backup;
        s->last_rx_us = now;
        s->sync_address = in->protocol == SOURCE_PROTOCOL_SACN ? in->sync_address : 0;
        s->force_sync = in->force_sync;

        if (ctx->mode == MERGE_MODE_LTP) {
            for (int c = 0; c < 512; c++) {
                uint8_t v = (c < in->length) ? in->data[c] : 0;
                if (first_levels || v != s->levels[c]) {
                    ctx->ltp_src[c] = (uint8_t)idx;
                }
            }
        }
        memcpy(s->levels, in->data, in->length);
        if (in->length < 512) {
            memset(s->levels + in->length, 0, 512 - in->length);
        }
        s->has_levels = true;
        if (first_levels || is_new) {
            log_source("new", port, s);
        }
    }

    xSemaphoreGive(ctx->mutex);
    return ESP_OK;
}

// ============================================================================
// Local layer
// ============================================================================

esp_err_t merge_engine_set_local(uint8_t port, const uint8_t *data)
{
    port_ctx_t *ctx = ctx_for(port);
    if (!ctx || !data) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    memcpy(ctx->local, data, 512);
    ctx->local_active = true;
    xSemaphoreGive(ctx->mutex);
    return ESP_OK;
}

esp_err_t merge_engine_set_local_channel(uint8_t port, uint16_t channel, uint8_t value)
{
    port_ctx_t *ctx = ctx_for(port);
    if (!ctx || channel < 1 || channel > 512) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    ctx->local[channel - 1] = value;
    ctx->local_active = true;
    xSemaphoreGive(ctx->mutex);
    return ESP_OK;
}

esp_err_t merge_engine_get_local(uint8_t port, uint8_t *data, bool *active)
{
    port_ctx_t *ctx = ctx_for(port);
    if (!ctx) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    if (data) memcpy(data, ctx->local, 512);
    if (active) *active = ctx->local_active;
    xSemaphoreGive(ctx->mutex);
    return ESP_OK;
}

esp_err_t merge_engine_clear_local(uint8_t port)
{
    port_ctx_t *ctx = ctx_for(port);
    if (!ctx) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    memset(ctx->local, 0, 512);
    ctx->local_active = false;
    xSemaphoreGive(ctx->mutex);
    return ESP_OK;
}

// ============================================================================
// Output
// ============================================================================

/*
 * Merge the network sources into @p out. Sets each source's contributing
 * flag; returns false if no source contributes. Caller holds the mutex.
 */
static bool compute_net(port_ctx_t *ctx, uint8_t port, int64_t now, uint8_t *out)
{
    expire_sources(ctx, port, now);

    /* ---- candidate selection ---- */
    uint8_t cand[MERGE_MAX_SOURCES];
    uint8_t n = 0;
    bool have_artnet = false, have_sacn = false, have_primary = false;

    for (uint8_t i = 0; i < MERGE_MAX_SOURCES; i++) {
        source_t *s = &ctx->sources[i];
        s->contributing = false;
        if (!s->in_use || !s->has_levels) continue;
        if (!protocol_allowed(ctx->protocol_mode, s->protocol)) continue;
        if (s->pending_until_us && now < s->pending_until_us && !pap_current(s, now)) continue;
        s->pending_until_us = 0;
        cand[n++] = i;
        if (s->protocol == SOURCE_PROTOCOL_ARTNET) have_artnet = true;
        if (s->protocol == SOURCE_PROTOCOL_SACN)   have_sacn = true;
        if (!s->backup) have_primary = true;
    }

    /* Protocol priority modes: the preferred protocol hides the other one */
    if ((ctx->protocol_mode == PROTOCOL_ARTNET_PRIORITY && have_artnet) ||
        (ctx->protocol_mode == PROTOCOL_SACN_PRIORITY && have_sacn)) {
        source_protocol_t drop = (ctx->protocol_mode == PROTOCOL_ARTNET_PRIORITY)
                                 ? SOURCE_PROTOCOL_SACN : SOURCE_PROTOCOL_ARTNET;
        uint8_t m = 0;
        for (uint8_t k = 0; k < n; k++) {
            if (ctx->sources[cand[k]].protocol != drop) cand[m++] = cand[k];
        }
        n = m;
    }

    /* Backup mode: primaries hide backups */
    if (ctx->mode == MERGE_MODE_BACKUP && have_primary) {
        uint8_t m = 0;
        for (uint8_t k = 0; k < n; k++) {
            if (!ctx->sources[cand[k]].backup) cand[m++] = cand[k];
        }
        n = m;
    }

    ctx->stats.active_sources = n;
    if (n == 0) {
        return false;
    }
    ctx->stats.total_merges++;

    if (ctx->mode == MERGE_MODE_HTP || ctx->mode == MERGE_MODE_LTP) {
        bool use_pap[MERGE_MAX_SOURCES];
        uint8_t uprio[MERGE_MAX_SOURCES];
        for (uint8_t k = 0; k < n; k++) {
            const source_t *s = &ctx->sources[cand[k]];
            use_pap[k] = pap_current(s, now);
            uprio[k] = s->priority ? s->priority : 1;
            ctx->sources[cand[k]].contributing = true;
        }
        bool ltp = (ctx->mode == MERGE_MODE_LTP);
        for (int c = 0; c < 512; c++) {
            uint8_t best_p = 0, best_v = 0;
            bool ltp_hit = false;
            for (uint8_t k = 0; k < n; k++) {
                const source_t *s = &ctx->sources[cand[k]];
                uint8_t p = use_pap[k] ? s->pap[c] : uprio[k];
                if (p == 0) continue;             /* slot not sourced */
                uint8_t v = s->levels[c];
                if (p > best_p) {
                    best_p = p;
                    best_v = v;
                    ltp_hit = ltp && ctx->ltp_src[c] == cand[k];
                } else if (p == best_p) {
                    if (ltp && ctx->ltp_src[c] == cand[k]) {
                        best_v = v;
                        ltp_hit = true;
                    } else if (!ltp_hit && v > best_v) {
                        best_v = v;
                    }
                }
            }
            out[c] = best_v;
        }
        if (ltp) ctx->stats.ltp_merges++; else ctx->stats.htp_merges++;
    } else {
        /* Owner modes: keep only the highest universe priority */
        uint8_t top = 0;
        for (uint8_t k = 0; k < n; k++) {
            if (ctx->sources[cand[k]].priority > top) top = ctx->sources[cand[k]].priority;
        }
        uint8_t owner = NO_SOURCE;
        for (uint8_t k = 0; k < n; k++) {
            const source_t *s = &ctx->sources[cand[k]];
            if (s->priority != top) continue;
            if (owner == NO_SOURCE) { owner = cand[k]; continue; }
            const source_t *o = &ctx->sources[owner];
            if (ctx->mode == MERGE_MODE_LAST) {
                if (s->last_rx_us > o->last_rx_us) owner = cand[k];
            } else {
                /* DISABLE / BACKUP: the longest-standing source keeps control */
                if (s->first_seen_us < o->first_seen_us) owner = cand[k];
            }
        }
        if (ctx->mode != MERGE_MODE_LAST && owner != ctx->owner &&
            ctx->owner != NO_SOURCE) {
            ctx->stats.backup_switches++;
            log_source("now controlling", port, &ctx->sources[owner]);
        }
        ctx->owner = owner;
        ctx->sources[owner].contributing = true;
        memcpy(out, ctx->sources[owner].levels, 512);
        if (ctx->mode == MERGE_MODE_LAST) ctx->stats.last_merges++;
    }
    return true;
}

/*
 * Can the port's current output follow a sync packet of this kind?
 * Art-Net: exactly one contributing source, Art-Net from @p ip (ArtSync is
 * ignored while merging, and must come from the ArtDmx sender).
 * sACN: every contributing source is sACN and names @p addr as its
 * synchronization universe. *force is set if any of them asks to revert to
 * unsynchronized output when sync is lost.
 */
static bool sync_eligible(const port_ctx_t *ctx, merge_sync_t kind, uint32_t ip,
                          uint16_t addr, bool *force)
{
    int n = 0;
    bool f = false;
    for (int i = 0; i < MERGE_MAX_SOURCES; i++) {
        const source_t *s = &ctx->sources[i];
        if (!s->in_use || !s->contributing) continue;
        n++;
        if (kind == MERGE_SYNC_ARTNET) {
            if (s->protocol != SOURCE_PROTOCOL_ARTNET || s->ip != ip) return false;
        } else {
            if (s->protocol != SOURCE_PROTOCOL_SACN || s->sync_address != addr) return false;
            f |= s->force_sync;
        }
    }
    if (force) *force = f;
    if (n == 0) return false;
    return kind == MERGE_SYNC_SACN || n == 1;
}

static void sync_leave(port_ctx_t *ctx, uint8_t port, const char *why)
{
    if (ctx->sync != MERGE_SYNC_NONE) {
        ESP_LOGI(TAG, "Port %u: %s sync off (%s)", port,
                 ctx->sync == MERGE_SYNC_ARTNET ? "Art-Net" : "sACN", why);
    }
    ctx->sync = MERGE_SYNC_NONE;
}

esp_err_t merge_engine_get_output(uint8_t port, uint8_t *data)
{
    port_ctx_t *ctx = ctx_for(port);
    if (!ctx || !data) return ESP_ERR_INVALID_ARG;

    int64_t now = esp_timer_get_time();
    uint8_t out[512];
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);

    bool net_active = compute_net(ctx, port, now, out);

    if (ctx->sync != MERGE_SYNC_NONE) {
        bool force = false;
        if (!net_active ||
            !sync_eligible(ctx, ctx->sync, ctx->sync_ip, ctx->sync_addr, &force)) {
            sync_leave(ctx, port, net_active ? "sources changed" : "no sources");
        } else if (ctx->sync == MERGE_SYNC_ARTNET &&
                   now - ctx->last_sync_us > ARTNET_SYNC_TIMEOUT_US) {
            sync_leave(ctx, port, "no ArtSync for 4 s");
        } else if (ctx->sync == MERGE_SYNC_SACN && force &&
                   now - ctx->last_sync_us > SACN_SYNC_TIMEOUT_US) {
            sync_leave(ctx, port, "sync packets lost");
        }
        /* sACN without Force_Synchronization keeps holding until sync resumes */
    }

    if (ctx->sync != MERGE_SYNC_NONE) {
        memcpy(out, ctx->held, 512);
        memcpy(ctx->last_net, out, 512);
    } else if (net_active) {
        memcpy(ctx->last_net, out, 512);
    } else {
        ctx->owner = NO_SOURCE;
        /* Before any source was seen there is nothing to fail over from */
        loss_behavior_t loss = ctx->had_net ? ctx->loss : LOSS_HOLD_LAST_LOOK;
        switch (loss) {
            case LOSS_HOLD_LAST_LOOK: memcpy(out, ctx->last_net, 512); break;
            case LOSS_FULL:           memset(out, 255, 512); break;
            case LOSS_SCENE:          memcpy(out, ctx->loss_scene, 512); break;
            case LOSS_BLACKOUT:
            default:
                memset(out, 0, 512);
                memset(ctx->last_net, 0, 512);
                break;
        }
    }
    if (net_active) ctx->had_net = true;
    bool failsafe = ctx->had_net && !net_active && ctx->sync == MERGE_SYNC_NONE;
    if (failsafe != ctx->failsafe) {
        ctx->failsafe = failsafe;
        if (failsafe) {
            static const char *names[] = {"hold last look", "zero", "full", "scene"};
            ESP_LOGW(TAG, "Port %u: all sources lost, failsafe: %s", port,
                     ctx->loss <= LOSS_LAST ? names[ctx->loss] : "?");
        }
    }

    if (ctx->local_active) {
        for (int c = 0; c < 512; c++) {
            if (ctx->local[c] > out[c]) out[c] = ctx->local[c];
        }
    }

    ctx->net_active = net_active;
    xSemaphoreGive(ctx->mutex);

    memcpy(data, out, 512);
    return ESP_OK;
}

/* Latch every port that can follow this sync packet; returns the port mask */
static uint8_t sync_latch(merge_sync_t kind, uint32_t ip, uint16_t addr)
{
    uint8_t mask = 0;
    int64_t now = esp_timer_get_time();
    for (uint8_t p = 1; p <= MERGE_PORTS; p++) {
        port_ctx_t *ctx = ctx_for(p);
        if (!ctx) continue;
        uint8_t out[512];
        xSemaphoreTake(ctx->mutex, portMAX_DELAY);
        if (compute_net(ctx, p, now, out) && sync_eligible(ctx, kind, ip, addr, NULL)) {
            if (ctx->sync != kind || ctx->sync_ip != ip || ctx->sync_addr != addr) {
                ESP_LOGI(TAG, "Port %u: %s sync on", p,
                         kind == MERGE_SYNC_ARTNET ? "Art-Net" : "sACN");
            }
            memcpy(ctx->held, out, 512);
            ctx->sync = kind;
            ctx->sync_ip = ip;
            ctx->sync_addr = addr;
            ctx->last_sync_us = now;
            ctx->stats.sync_frames++;
            mask |= (uint8_t)(1u << (p - 1));
        }
        xSemaphoreGive(ctx->mutex);
    }
    return mask;
}

uint8_t merge_engine_artnet_sync(uint32_t source_ip)
{
    if (!s_merge.initialized) return 0;
    return sync_latch(MERGE_SYNC_ARTNET, source_ip, 0);
}

uint8_t merge_engine_sacn_sync(uint16_t sync_universe)
{
    if (!s_merge.initialized || sync_universe == 0) return 0;
    return sync_latch(MERGE_SYNC_SACN, 0, sync_universe);
}

merge_sync_t merge_engine_get_sync_state(uint8_t port)
{
    port_ctx_t *ctx = ctx_for(port);
    if (!ctx) return MERGE_SYNC_NONE;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    merge_sync_t st = ctx->sync;
    xSemaphoreGive(ctx->mutex);
    return st;
}

uint16_t merge_engine_sync_universes(uint8_t port, uint16_t *out, uint8_t max)
{
    port_ctx_t *ctx = ctx_for(port);
    if (!ctx || !out || max == 0) return 0;
    uint16_t n = 0;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    for (int i = 0; i < MERGE_MAX_SOURCES && n < max; i++) {
        const source_t *s = &ctx->sources[i];
        if (!s->in_use || s->protocol != SOURCE_PROTOCOL_SACN || s->sync_address == 0) continue;
        bool dup = false;
        for (uint16_t k = 0; k < n; k++) dup |= out[k] == s->sync_address;
        if (!dup) out[n++] = s->sync_address;
    }
    xSemaphoreGive(ctx->mutex);
    return n;
}

bool merge_engine_is_output_active(uint8_t port)
{
    port_ctx_t *ctx = ctx_for(port);
    if (!ctx) return false;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    bool active = ctx->net_active || ctx->local_active;
    xSemaphoreGive(ctx->mutex);
    return active;
}

esp_err_t merge_engine_cancel_merge(uint8_t port)
{
    port_ctx_t *ctx = ctx_for(port);
    if (!ctx) return ESP_ERR_INVALID_ARG;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    expire_sources(ctx, port, now);
    int artnet = 0;
    for (int i = 0; i < MERGE_MAX_SOURCES; i++) {
        const source_t *s = &ctx->sources[i];
        if (s->in_use && s->protocol == SOURCE_PROTOCOL_ARTNET) artnet++;
    }
    /* "If Node is currently in merge mode, cancel merge mode upon receipt of
     * next ArtDmx": when that is later than a source may be silent for, the
     * merge has ended by itself */
    if (artnet > 1) ctx->cancel_until_us = now + ctx->timeout_us;
    xSemaphoreGive(ctx->mutex);
    return ESP_OK;
}

esp_err_t merge_engine_resume_merge(uint8_t port)
{
    port_ctx_t *ctx = ctx_for(port);
    if (!ctx) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    cancel_clear(ctx);
    xSemaphoreGive(ctx->mutex);
    return ESP_OK;
}

esp_err_t merge_engine_blackout(uint8_t port)
{
    port_ctx_t *ctx = ctx_for(port);
    if (!ctx) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    for (uint8_t i = 0; i < MERGE_MAX_SOURCES; i++) {
        if (ctx->sources[i].in_use) remove_source(ctx, i);
    }
    memset(ctx->ltp_src, NO_SOURCE, sizeof(ctx->ltp_src));
    memset(ctx->last_net, 0, 512);
    memset(ctx->local, 0, 512);
    ctx->local_active = false;
    ctx->net_active = false;
    ctx->sync = MERGE_SYNC_NONE;
    cancel_clear(ctx);
    xSemaphoreGive(ctx->mutex);
    ESP_LOGI(TAG, "Port %u blackout", port);
    return ESP_OK;
}

uint8_t merge_engine_get_sources(uint8_t port, merge_source_info_t *out, uint8_t max)
{
    port_ctx_t *ctx = ctx_for(port);
    if (!ctx || !out || max == 0) return 0;
    int64_t now = esp_timer_get_time();
    uint8_t n = 0;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    for (uint8_t i = 0; i < MERGE_MAX_SOURCES && n < max; i++) {
        const source_t *s = &ctx->sources[i];
        if (!s->in_use) continue;
        merge_source_info_t *o = &out[n++];
        o->protocol = s->protocol;
        o->source_ip = s->ip;
        memcpy(o->name, s->name, sizeof(o->name));
        o->priority = s->priority;
        o->uses_pap = pap_current(s, now);
        o->backup = s->backup;
        o->contributing = s->contributing;
        o->age_ms = (uint32_t)((now - s->last_rx_us) / 1000);
        o->packets = s->packets;
    }
    xSemaphoreGive(ctx->mutex);
    return n;
}

esp_err_t merge_engine_get_stats(uint8_t port, merge_stats_t *stats)
{
    port_ctx_t *ctx = ctx_for(port);
    if (!ctx || !stats) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    *stats = ctx->stats;
    xSemaphoreGive(ctx->mutex);
    return ESP_OK;
}

esp_err_t merge_engine_reset_stats(uint8_t port)
{
    port_ctx_t *ctx = ctx_for(port);
    if (!ctx) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    uint32_t active = ctx->stats.active_sources;
    memset(&ctx->stats, 0, sizeof(ctx->stats));
    ctx->stats.active_sources = active;
    xSemaphoreGive(ctx->mutex);
    return ESP_OK;
}
