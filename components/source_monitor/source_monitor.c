/*
 * Art-Net / sACN sender table (see source_monitor.h)
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "source_monitor.h"

#define RATE_WINDOW_MS 1000
#define RATE_SILENT_MS 5000     /* rate reads 0 once silent this long */
#define LATE_WINDOW    20       /* E1.31 6.7.2: up to 20 back = out of order */
#define ADVERT_FORGET_MS 30000  /* discovery repeats every 10 s */

typedef struct {
    bool used;
    uint8_t proto;
    uint32_t ip;
    uint8_t cid[16];
    uint16_t universe;
    char name[SRCMON_NAME_LEN];
    uint8_t priority;
    uint16_t slots;
    bool preview;
    bool sequenced;
    bool have_seq;
    uint8_t last_seq;
    int64_t last_ms;
    int64_t pap_ms;             /* last 0xDD packet, -1 = never */
    uint32_t packets, lost, late;
    uint32_t lost_cur, lost_prev;
    uint32_t gap_cur, gap_prev;
    int64_t epoch_ms;
    int64_t win_ms;
    uint32_t win_packets;
    uint16_t rate_x10;
} row_t;

typedef struct {
    bool used;
    uint8_t cid[16];
    uint32_t ip;
    char name[SRCMON_NAME_LEN];
    uint16_t universes[SRCMON_ADVERT_UNIVERSES];
    uint16_t stored;
    uint16_t count;
    uint32_t pages_seen;        /* pages 0-31 counted since the last page 0 */
    int64_t last_ms;
} advert_t;

typedef struct {
    bool used;
    srcmon_node_t node;
    int64_t last_ms;
} node_row_t;

static row_t s_rows[SRCMON_MAX_SOURCES];
static advert_t s_adverts[SRCMON_MAX_ADVERTS];
static node_row_t s_nodes[SRCMON_MAX_NODES];
static SemaphoreHandle_t s_lock;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool lock(void)
{
    if (!s_lock) {
        /* srcmon_reset() from app start-up creates it before any receiver
         * runs; until then packets are not recorded */
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    return true;
}

static void copy_text(char *dst, const char *src, size_t size)
{
    size_t n = 0;
    if (src) {
        while (n < size - 1 && src[n]) {
            char c = src[n];
            dst[n] = (c >= 0x20 && c < 0x7F) ? c : '?';   /* JSON-safe ASCII */
            n++;
        }
    }
    dst[n] = '\0';
}

static void copy_name(char *dst, const char *src)
{
    copy_text(dst, src, SRCMON_NAME_LEN);
}

/* Move the 10 s "recent" epochs on */
static void roll(row_t *r, int64_t now)
{
    int64_t dt = now - r->epoch_ms;
    if (dt < SRCMON_EPOCH_MS) return;
    if (dt >= 2 * SRCMON_EPOCH_MS) {
        r->lost_prev = r->gap_prev = 0;
        r->epoch_ms = now;
    } else {
        r->lost_prev = r->lost_cur;
        r->gap_prev = r->gap_cur;
        r->epoch_ms += SRCMON_EPOCH_MS;
    }
    r->lost_cur = r->gap_cur = 0;
}

/* How far @p seq is ahead of @p last, in the sender's own counting */
static int seq_ahead(uint8_t proto, uint8_t last, uint8_t seq)
{
    if (proto == SRCMON_SACN) return (uint8_t)(seq - last);   /* 0-255 wraps */
    return ((int)seq - (int)last + 255) % 255;                /* Art-Net 1-255 */
}

static void sequence(row_t *r, const srcmon_packet_t *p, bool resync)
{
    bool uses = p->proto == SRCMON_SACN || p->sequence != 0;
    r->sequenced = uses;
    if (!uses) {
        r->have_seq = false;
        return;
    }
    if (!r->have_seq || resync) {
        r->last_seq = p->sequence;
        r->have_seq = true;
        return;
    }
    int modulo = p->proto == SRCMON_SACN ? 256 : 255;
    int d = seq_ahead(p->proto, r->last_seq, p->sequence);
    if (d == 0 || d >= modulo - LATE_WINDOW) {
        r->late++;                       /* repeat or out of order */
        return;
    }
    if (d < 128) {
        r->lost += (uint32_t)(d - 1);
        r->lost_cur += (uint32_t)(d - 1);
    }
    /* else a jump back too far to be late: the sender restarted */
    r->last_seq = p->sequence;
}

static bool same_cid(const uint8_t *a, const uint8_t *b)
{
    static const uint8_t zero[16];
    return memcmp(a, b ? b : zero, 16) == 0;
}

static row_t *find_row(const srcmon_packet_t *p, int64_t now)
{
    row_t *free_row = NULL, *oldest = NULL;
    for (int i = 0; i < SRCMON_MAX_SOURCES; i++) {
        row_t *r = &s_rows[i];
        if (r->used && now - r->last_ms > SRCMON_FORGET_MS) r->used = false;
        if (!r->used) {
            if (!free_row) free_row = r;
            continue;
        }
        if (r->proto == p->proto && r->ip == p->ip && r->universe == p->universe &&
            same_cid(r->cid, p->proto == SRCMON_SACN ? p->cid : NULL)) {
            return r;
        }
        if (!oldest || r->last_ms < oldest->last_ms) oldest = r;
    }
    row_t *r = free_row ? free_row : oldest;
    memset(r, 0, sizeof(*r));
    r->used = true;
    r->proto = p->proto;
    r->ip = p->ip;
    r->universe = p->universe;
    if (p->proto == SRCMON_SACN && p->cid) memcpy(r->cid, p->cid, 16);
    r->pap_ms = -1;
    r->epoch_ms = now;
    r->win_ms = now;
    r->last_ms = now;
    return r;
}

void srcmon_note(const srcmon_packet_t *p)
{
    if (!p || (p->proto != SRCMON_ARTNET && p->proto != SRCMON_SACN)) return;
    if (!lock()) return;
    int64_t now = now_ms();
    row_t *r = find_row(p, now);
    bool first = r->packets == 0;
    int64_t gap = now - r->last_ms;
    roll(r, now);

    sequence(r, p, !first && gap > SRCMON_RESYNC_MS);
    if (!first && gap > r->gap_cur) r->gap_cur = gap > UINT32_MAX ? UINT32_MAX : (uint32_t)gap;
    r->packets++;
    r->last_ms = now;

    r->win_packets += first ? 0 : 1;
    int64_t win = now - r->win_ms;
    if (win >= RATE_WINDOW_MS) {
        uint64_t x10 = (uint64_t)r->win_packets * 10000u / (uint64_t)win;
        r->rate_x10 = x10 > UINT16_MAX ? UINT16_MAX : (uint16_t)x10;
        r->win_ms = now;
        r->win_packets = 0;
    }

    if (p->proto == SRCMON_SACN) {
        copy_name(r->name, p->name);
        r->priority = p->priority;
    }
    if (p->proto == SRCMON_SACN && p->start_code == 0xDD) {
        r->pap_ms = now;
    } else {
        r->slots = p->slots;
        r->preview = p->preview;
    }
    xSemaphoreGive(s_lock);
}

void srcmon_note_discovery(const uint8_t *cid, uint32_t ip, const char *name,
                           uint8_t page, uint8_t last_page,
                           const uint8_t *universes_be, uint16_t count)
{
    if (!cid || (count && !universes_be)) return;
    if (!lock()) return;
    int64_t now = now_ms();
    advert_t *a = NULL, *free_a = NULL, *oldest = NULL;
    for (int i = 0; i < SRCMON_MAX_ADVERTS; i++) {
        advert_t *x = &s_adverts[i];
        if (x->used && now - x->last_ms > ADVERT_FORGET_MS) x->used = false;
        if (!x->used) {
            if (!free_a) free_a = x;
            continue;
        }
        if (x->ip == ip && memcmp(x->cid, cid, 16) == 0) {
            a = x;
            break;
        }
        if (!oldest || x->last_ms < oldest->last_ms) oldest = x;
    }
    if (!a) {
        a = free_a ? free_a : oldest;
        memset(a, 0, sizeof(*a));
        a->used = true;
        memcpy(a->cid, cid, 16);
        a->ip = ip;
    }
    (void)last_page;
    if (page == 0) {
        a->stored = 0;
        a->count = 0;
        a->pages_seen = 0;
    }
    bool counted = page < 32 && (a->pages_seen & (1u << page));
    if (!counted) {
        if (page < 32) a->pages_seen |= 1u << page;
        for (uint16_t i = 0; i < count && a->stored < SRCMON_ADVERT_UNIVERSES; i++) {
            a->universes[a->stored++] =
                (uint16_t)((universes_be[2 * i] << 8) | universes_be[2 * i + 1]);
        }
        uint32_t total = (uint32_t)a->count + count;
        a->count = total > UINT16_MAX ? UINT16_MAX : (uint16_t)total;
    }
    copy_name(a->name, name);
    a->last_ms = now;
    xSemaphoreGive(s_lock);
}

void srcmon_note_node(const artnet_poll_reply_t *reply, bool ip_conflict)
{
    if (!reply || !lock()) return;
    int64_t now = now_ms();
    node_row_t *row = NULL, *free_row = NULL, *oldest = NULL;
    for (int i = 0; i < SRCMON_MAX_NODES; i++) {
        node_row_t *x = &s_nodes[i];
        if (x->used && now - x->last_ms > SRCMON_NODE_FORGET_MS) x->used = false;
        if (!x->used) {
            if (!free_row) free_row = x;
            continue;
        }
        const artnet_poll_reply_t *r = &x->node.reply;
        if (r->ip == reply->ip && r->bind_index == reply->bind_index &&
            memcmp(r->mac, reply->mac, 6) == 0) {
            row = x;
            break;
        }
        if (!oldest || x->last_ms < oldest->last_ms) oldest = x;
    }
    if (!row) {
        row = free_row ? free_row : oldest;
        memset(row, 0, sizeof(*row));
        row->used = true;
    }
    uint32_t replies = row->node.replies;
    row->node.reply = *reply;
    artnet_poll_reply_t *r = &row->node.reply;
    copy_text(r->short_name, reply->short_name, sizeof(r->short_name));
    copy_text(r->long_name, reply->long_name, sizeof(r->long_name));
    row->node.ip_conflict = ip_conflict;
    row->node.replies = replies + 1;
    row->last_ms = now;
    xSemaphoreGive(s_lock);
}

static bool node_before(const srcmon_node_t *a, const srcmon_node_t *b)
{
    /* by address as a number (network byte order in memory) */
    const uint8_t *x = (const uint8_t *)&a->reply.ip, *y = (const uint8_t *)&b->reply.ip;
    int c = memcmp(x, y, 4);
    if (c) return c < 0;
    c = memcmp(a->reply.mac, b->reply.mac, 6);
    if (c) return c < 0;
    return a->reply.bind_index < b->reply.bind_index;
}

int srcmon_get_nodes(srcmon_node_t *out, int max)
{
    if (!out || max <= 0 || !lock()) return 0;
    int64_t now = now_ms();
    int n = 0;
    for (int i = 0; i < SRCMON_MAX_NODES && n < max; i++) {
        node_row_t *x = &s_nodes[i];
        if (!x->used) continue;
        int64_t age = now - x->last_ms;
        if (age > SRCMON_NODE_FORGET_MS) {
            x->used = false;
            continue;
        }
        srcmon_node_t v = x->node;
        v.age_ms = (uint32_t)age;
        int k = n++;
        while (k > 0 && node_before(&v, &out[k - 1])) {
            out[k] = out[k - 1];
            k--;
        }
        out[k] = v;
    }
    xSemaphoreGive(s_lock);
    return n;
}

int srcmon_output_nodes(uint16_t universe, uint32_t *ips, int max)
{
    if (!ips || max <= 0 || !lock()) return 0;
    int64_t now = now_ms();
    int n = 0;
    for (int i = 0; i < SRCMON_MAX_NODES && n < max; i++) {
        const node_row_t *x = &s_nodes[i];
        if (!x->used || x->node.ip_conflict || now - x->last_ms > SRCMON_NODE_FORGET_MS) continue;
        const artnet_poll_reply_t *r = &x->node.reply;
        bool outputs = false;
        for (int k = 0; k < r->num_ports && k < ARTNET_REPLY_PORTS; k++) {
            if ((r->ports[k].type & 0x80) && r->ports[k].out_address == universe) outputs = true;
        }
        uint32_t ip = r->ip ? r->ip : r->from_ip;
        if (!outputs || !ip) continue;
        bool seen = false;
        for (int k = 0; k < n; k++) seen |= ips[k] == ip;
        if (!seen) ips[n++] = ip;
    }
    xSemaphoreGive(s_lock);
    return n;
}

int srcmon_get_sources(srcmon_source_t *out, int max)
{
    if (!out || max <= 0 || !lock()) return 0;
    int64_t now = now_ms();
    int n = 0;
    for (int i = 0; i < SRCMON_MAX_SOURCES && n < max; i++) {
        row_t *r = &s_rows[i];
        if (!r->used) continue;
        int64_t age = now - r->last_ms;
        if (age > SRCMON_FORGET_MS) {
            r->used = false;
            continue;
        }
        roll(r, now);
        srcmon_source_t s = {
            .proto = r->proto,
            .ip = r->ip,
            .universe = r->universe,
            .priority = r->priority,
            .slots = r->slots,
            .pap = r->pap_ms >= 0 && now - r->pap_ms <= SRCMON_RESYNC_MS + 500,
            .preview = r->preview,
            .sequenced = r->sequenced,
            .packets = r->packets,
            .lost = r->lost,
            .lost_recent = r->lost_cur + r->lost_prev,
            .late = r->late,
            .rate_x10 = age > RATE_SILENT_MS ? 0 : r->rate_x10,
            .max_gap_ms = r->gap_cur > r->gap_prev ? r->gap_cur : r->gap_prev,
            .age_ms = (uint32_t)age,
        };
        memcpy(s.name, r->name, sizeof(s.name));
        /* newest first */
        int k = n++;
        while (k > 0 && out[k - 1].age_ms > s.age_ms) {
            out[k] = out[k - 1];
            k--;
        }
        out[k] = s;
    }
    xSemaphoreGive(s_lock);
    return n;
}

int srcmon_get_adverts(srcmon_advert_t *out, int max)
{
    if (!out || max <= 0 || !lock()) return 0;
    int64_t now = now_ms();
    int n = 0;
    for (int i = 0; i < SRCMON_MAX_ADVERTS && n < max; i++) {
        advert_t *a = &s_adverts[i];
        if (!a->used) continue;
        int64_t age = now - a->last_ms;
        if (age > ADVERT_FORGET_MS) {
            a->used = false;
            continue;
        }
        srcmon_advert_t v = { .ip = a->ip, .stored = a->stored, .count = a->count,
                              .age_ms = (uint32_t)age };
        memcpy(v.name, a->name, sizeof(v.name));
        memcpy(v.universes, a->universes, sizeof(v.universes));
        int k = n++;
        while (k > 0 && out[k - 1].age_ms > v.age_ms) {
            out[k] = out[k - 1];
            k--;
        }
        out[k] = v;
    }
    xSemaphoreGive(s_lock);
    return n;
}

void srcmon_reset(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_rows, 0, sizeof(s_rows));
    memset(s_adverts, 0, sizeof(s_adverts));
    memset(s_nodes, 0, sizeof(s_nodes));
    xSemaphoreGive(s_lock);
}
