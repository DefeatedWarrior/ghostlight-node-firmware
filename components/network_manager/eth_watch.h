/**
 * @file eth_watch.h
 * @brief When to set a W5500 that has gone deaf up again
 *
 * A W5500 can stop receiving while its link stays up: a glitch on its
 * supply or its SPI lines resets it, its socket and MAC address are gone,
 * and the interface looks fine while nothing arrives. Ghostlight runs the
 * W5500 with its MAC filter off (network_manager.c), so every frame on the
 * segment counts. On a show network something always talks (ArtPoll every
 * few seconds, ARP, mDNS), so a minute of silence with the link up means a
 * stuck chip. Each try that brings nothing back doubles the wait, so a
 * network that really is silent costs a short break now and then, no more.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ETH_QUIET_S       60    /* nothing received this long with the link up */
#define ETH_QUIET_MAX_S   960   /* the longest wait, after tries that brought nothing */

typedef struct {
    uint32_t frames;      /* frame count at the last look */
    uint32_t heard_ms;    /* when a frame last arrived (or the link came up) */
    uint32_t limit_s;     /* the silence allowed now */
    uint32_t tries;       /* set-ups so far */
} eth_watch_t;

/** Start watching; @p frames is the frame count now (see eth_watch_step) */
void eth_watch_init(eth_watch_t *w, uint32_t frames, uint32_t now_ms);

/**
 * Look again. @p frames counts the frames received so far (any number that
 * changes when one arrives). @return true: set the chip up again now.
 */
bool eth_watch_step(eth_watch_t *w, bool link, uint32_t frames, uint32_t now_ms);
