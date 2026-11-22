/*
 * Captive portal DNS (private to network_manager)
 *
 * Phones and laptops that join the node's own Wi-Fi ask the DHCP-offered DNS
 * server (the node) for a test page such as captive.apple.com. Every name
 * resolves to the node, the web server redirects the unknown page to the
 * setup page, and the device opens it by itself. Off in the settings
 * (network.ap_captive = false), every name gets "no such name".
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Start the DNS task (once; later calls do nothing). Answers only clients
 *  in the access point's own subnet, while the access point is on. */
void captive_dns_start(void);

/**
 * The reply to one DNS query (dns_reply.c, no ESP-IDF dependencies).
 * With @p captive, an A query gets @p ip (network byte order, TTL 10 s) and
 * other types an empty answer, so devices fall back to IPv4; without it,
 * every name is NXDOMAIN. Returns the reply length, or 0 to send nothing
 * (a response, or shorter than a DNS header).
 */
size_t dns_captive_reply(const uint8_t *query, size_t len, uint8_t *out, size_t out_size,
                         uint32_t ip, bool captive);

#define DNS_REPLY_MAX 300   /* header + longest name + type/class + one A record */
