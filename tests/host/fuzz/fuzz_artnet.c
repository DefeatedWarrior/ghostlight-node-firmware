/* libFuzzer target: one UDP datagram into the Art-Net receiver's packet
 * dispatcher, in a receive buffer like the firmware's (640 bytes, with the
 * bytes past the datagram poisoned for ASan). Every callback reads all the
 * data it is handed, so a length or pointer past the packet is reported. */
#include "../../../components/artnet_receiver/artnet_receiver.c"
#include "fakes.h"

static __typeof__(s_an) pristine;
static volatile uint32_t sink;

static void touch(const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) sink += b[i];
}

static void on_dmx(uint16_t universe, const uint8_t *data, uint16_t length,
                   uint8_t sequence, uint32_t source_ip, void *ctx)
{
    if (length == 0 || length > 512 || universe > 0x7FFF) __builtin_trap();
    touch(data, length);
}

static void on_address(const artnet_address_cmd_t *cmd, void *ctx)
{
    if (cmd->port < 1 || cmd->port > ARTNET_NODE_PORTS) __builtin_trap();
    if (cmd->short_name) touch(cmd->short_name, strlen(cmd->short_name) + 1);
    if (cmd->long_name) touch(cmd->long_name, strlen(cmd->long_name) + 1);
}

static void on_rdm(const artnet_rdm_event_t *ev, void *ctx)
{
    if (ev->address_count > 32) __builtin_trap();
    touch(ev->addresses, ev->address_count * sizeof(ev->addresses[0]));
    if (ev->rdm) touch(ev->rdm, ev->rdm_len);
}

static void on_sync(uint32_t ip, void *ctx) { sink += ip; }

static void on_ipprog(const artnet_ipprog_cmd_t *cmd, artnet_ipprog_reply_t *reply, void *ctx)
{
    touch(cmd, sizeof(*cmd));
    memcpy(reply->ip, cmd->ip, 4);
}

static void on_peer(const artnet_poll_reply_t *r, bool conflict, void *ctx)
{
    if (r->num_ports > ARTNET_REPLY_PORTS || r->ports[0].in_address > 0x7FFF ||
        strlen(r->short_name) >= sizeof(r->short_name) || strlen(r->long_name) >= sizeof(r->long_name)) {
        __builtin_trap();
    }
    touch(r, sizeof(*r));
}

static void on_status(artnet_node_status_t *st, void *ctx)
{
    snprintf(st->short_name, sizeof(st->short_name), "fuzz");
    memcpy(st->ip, (uint8_t[]){2, 0, 0, 10}, 4);
    memcpy(st->netmask, (uint8_t[]){255, 0, 0, 0}, 4);
    for (int p = 0; p < ARTNET_NODE_PORTS; p++) {
        st->ports[p].enabled = true;
        st->ports[p].is_input = p & 1;
        st->ports[p].port_address = (uint16_t)(p * 0x111);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static int once;
    if (!once) {
        pristine = s_an;
        once = 1;
    }
    /* fresh receiver per input, so every crash reproduces from its input */
    s_an = pristine;
    host_fakes_reset();
    artnet_receiver_init();
    artnet_receiver_set_callback(on_dmx, NULL);
    artnet_receiver_set_address_callback(on_address, NULL);
    artnet_receiver_set_status_callback(on_status, NULL);
    artnet_receiver_set_rdm_callback(on_rdm, NULL);
    artnet_receiver_set_sync_callback(on_sync, NULL);
    artnet_receiver_set_ipprog_callback(on_ipprog, NULL);
    artnet_receiver_set_poll_reply_callback(on_peer, NULL);
    s_an.sock = 42;

    size_t len = size < ARTNET_RX_BUF_SIZE ? size : ARTNET_RX_BUF_SIZE;   /* recvfrom() */
    uint8_t *buf = host_rx_buffer(data, len, ARTNET_RX_BUF_SIZE);
    struct sockaddr_in from = { .sin_family = AF_INET };
    from.sin_addr.s_addr = htonl(0x02000063);
    process_packet(buf, (int)len, &from);
    host_rx_free(buf, ARTNET_RX_BUF_SIZE);
    return 0;
}
