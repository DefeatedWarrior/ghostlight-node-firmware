/* Host stub: a UART with both FIFOs and an interrupt mask, on the simulated
 * clock of test_dmx_rdm.c */
#pragma once
#include "freertos/FreeRTOS.h"

/* bit values from ESP-IDF's esp32s3 hal/uart_ll.h */
#define UART_INTR_RXFIFO_FULL  (1 << 0)
#define UART_INTR_TXFIFO_EMPTY (1 << 1)
#define UART_INTR_PARITY_ERR   (1 << 2)
#define UART_INTR_FRAM_ERR     (1 << 3)
#define UART_INTR_RXFIFO_OVF   (1 << 4)
#define UART_INTR_BRK_DET      (1 << 7)
#define UART_INTR_TX_DONE      (1 << 14)
#define UART_LL_INTR_MASK      0x7FFFFFFF

typedef struct {
    uint8_t fifo[600];
    int fifo_len;
    uint32_t intr, ena;
    struct { uint32_t txd_inv, sw_rts; } conf0;
} uart_dev_t;
extern uart_dev_t host_uart[SOC_UART_NUM];
int host_tx_room(uart_dev_t *d);
void host_tx_write(uart_dev_t *d, const uint8_t *b, int n);

#define UART_LL_GET_HW(n) (&host_uart[n])
#define uart_ll_get_intsts_mask(d) ((d)->intr & (d)->ena)
#define uart_ll_clr_intsts_mask(d, m) ((d)->intr &= ~(uint32_t)(m))
#define uart_ll_ena_intr_mask(d, m) ((d)->ena |= (uint32_t)(m))
#define uart_ll_disable_intr_mask(d, m) ((d)->ena &= ~(uint32_t)(m))
#define uart_ll_get_rxfifo_len(d) ((d)->fifo_len)
#define uart_ll_rxfifo_rst(d) ((d)->fifo_len = 0)
#define uart_ll_set_rts_active_level(d, l) ((d)->conf0.sw_rts = (l))
static inline void uart_ll_read_rxfifo(uart_dev_t *d, uint8_t *buf, int n)
{
    memcpy(buf, d->fifo, (size_t)n);
    memmove(d->fifo, d->fifo + n, (size_t)(d->fifo_len - n));
    d->fifo_len -= n;
}
#define uart_ll_get_txfifo_len(d) host_tx_room(d)
#define uart_ll_write_txfifo(d, b, n) host_tx_write(d, b, (int)(n))
#define uart_ll_txfifo_rst(d) ((void)(d))
#define uart_ll_set_sclk(d, s) ((void)(d))
#define uart_ll_set_baudrate(d, b, f) ((void)(d), (void)(f))
#define uart_ll_get_baudrate(d, f) ((void)(d), (void)(f), 250000u)
#define uart_ll_set_mode(d, v) ((void)(d))
#define uart_ll_set_parity(d, v) ((void)(d))
#define uart_ll_set_data_bit_num(d, v) ((void)(d))
#define uart_ll_set_stop_bits(d, v) ((void)(d))
#define uart_ll_tx_break(d, v) ((void)(d))
#define uart_ll_set_tx_idle_num(d, v) ((void)(d))
#define uart_ll_set_hw_flow_ctrl(d, v, t) ((void)(d))
#define uart_ll_set_txfifo_empty_thr(d, v) ((void)(d))
#define uart_ll_set_rxfifo_full_thr(d, v) ((void)(d))
