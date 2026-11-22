/* libFuzzer target: one UDP datagram into the sACN receiver's parser, in a
 * receive buffer like the firmware's (SACN_RX_BUF_SIZE bytes, with the bytes
 * past the datagram poisoned for ASan). The callbacks read everything they
 * are handed and check the decoded fields are in range. */
#include "../../../components/sacn_receiver/sacn_receiver.c"
#include "fakes.h"

static __typeof__(s_sacn) pristine;
static volatile uint32_t sink;

static void touch(const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) sink += b[i];
}

static void on_data(const sacn_packet_info_t *pkt, void *ctx)
{
    if (pkt->length > 512 || pkt->priority > 200) __builtin_trap();
    if (pkt->universe < 1 || pkt->universe > 63999) __builtin_trap();
    if (pkt->start_code != 0x00 && pkt->start_code != 0xDD) __builtin_trap();
    touch(pkt->cid, 16);
    touch(pkt->source_name, strlen(pkt->source_name) + 1);
    touch(pkt->data, pkt->length);
}

static void on_sync(uint16_t universe, const uint8_t *cid, uint32_t ip, void *ctx)
{
    if (universe < 1 || universe > 63999) __builtin_trap();
    touch(cid, 16);
}

static void on_discovery(const uint8_t *cid, const char *name, uint32_t ip, uint8_t page,
                         uint8_t last_page, const uint8_t *universes_be, uint16_t count,
                         void *ctx)
{
    if (count > 512) __builtin_trap();
    touch(cid, 16);
    touch(name, strlen(name) + 1);
    touch(universes_be, (size_t)count * 2);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static int once;
    if (!once) {
        pristine = s_sacn;
        once = 1;
    }
    s_sacn = pristine;
    host_fakes_reset();
    sacn_receiver_init();
    sacn_receiver_set_callback(on_data, NULL);
    sacn_receiver_set_sync_callback(on_sync, NULL);
    sacn_receiver_set_discovery_callback(on_discovery, NULL);

    size_t len = size < SACN_RX_BUF_SIZE ? size : SACN_RX_BUF_SIZE;   /* recvfrom() */
    uint8_t *buf = host_rx_buffer(data, len, SACN_RX_BUF_SIZE);
    struct sockaddr_in from = { .sin_family = AF_INET };
    from.sin_addr.s_addr = htonl(0x02000063);
    process_packet(buf, (int)len, &from);
    /* the same packet again: exercises the sync sequence check */
    process_packet(buf, (int)len, &from);
    host_rx_free(buf, SACN_RX_BUF_SIZE);
    return 0;
}
