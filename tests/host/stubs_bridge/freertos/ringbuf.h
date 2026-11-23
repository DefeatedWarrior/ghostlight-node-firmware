/* Host stub for test_rdm_bridge: the ring buffer is faked in the test, with
 * ESP-IDF's byte budget for RINGBUF_TYPE_NOSPLIT */
#pragma once
#include "freertos/FreeRTOS.h"
typedef void *RingbufHandle_t;
typedef enum { RINGBUF_TYPE_NOSPLIT = 0, RINGBUF_TYPE_ALLOWSPLIT, RINGBUF_TYPE_BYTEBUF } RingbufferType_t;
RingbufHandle_t xRingbufferCreate(size_t size, RingbufferType_t type);
RingbufHandle_t xRingbufferCreateWithCaps(size_t size, RingbufferType_t type, unsigned caps);
BaseType_t xRingbufferSend(RingbufHandle_t rb, const void *item, size_t size, TickType_t wait);
void *xRingbufferReceive(RingbufHandle_t rb, size_t *size, TickType_t wait);
void vRingbufferReturnItem(RingbufHandle_t rb, void *item);
void vRingbufferDelete(RingbufHandle_t rb);
void vRingbufferDeleteWithCaps(RingbufHandle_t rb);
