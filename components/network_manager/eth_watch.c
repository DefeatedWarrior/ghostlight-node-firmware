/* When to set a deaf W5500 up again: see eth_watch.h */
#include "eth_watch.h"

#include <string.h>

void eth_watch_init(eth_watch_t *w, uint32_t frames, uint32_t now_ms)
{
    memset(w, 0, sizeof(*w));
    w->frames = frames;
    w->heard_ms = now_ms;
    w->limit_s = ETH_QUIET_S;
}

bool eth_watch_step(eth_watch_t *w, bool link, uint32_t frames, uint32_t now_ms)
{
    bool heard = frames != w->frames;
    w->frames = frames;
    if (heard || !link) {
        if (heard) w->limit_s = ETH_QUIET_S;   /* it works: the short wait again */
        w->heard_ms = now_ms;                  /* no link: nothing to hear yet */
        return false;
    }
    if (now_ms - w->heard_ms < w->limit_s * 1000u) {
        return false;
    }
    w->heard_ms = now_ms;
    w->limit_s = w->limit_s * 2 > ETH_QUIET_MAX_S ? ETH_QUIET_MAX_S : w->limit_s * 2;
    w->tries++;
    return true;
}
