/**
 * @file ak_byte_ring.h
 * @brief Single-producer / single-consumer byte ring for BLE writes
 *        (firmware-review FW-08). Pure, host-testable.
 *
 * The BLE stack callback is the only producer and the app loop the only
 * consumer. Each index has exactly one writer: the producer moves `head`, the
 * consumer moves `tail`, so no read-modify-write is shared between contexts.
 * A write that does not fit is dropped whole (never half-queued), and a
 * disconnect marks a cut point: the consumer discards everything queued
 * before it, so bytes from one central can never reach the next one.
 */

#ifndef AK_BYTE_RING_H
#define AK_BYTE_RING_H

#include <stdbool.h>
#include <stdint.h>

#define AK_BYTE_RING_SIZE 512U /* power of two */

typedef struct {
    uint8_t           buf[AK_BYTE_RING_SIZE];
    volatile uint32_t head;      /* producer only; free-running */
    volatile uint32_t tail;      /* consumer only; free-running */
    volatile uint32_t cut_at;    /* producer: head at the last disconnect */
    volatile uint32_t cut_epoch; /* producer: bumps on every disconnect */
    uint32_t          seen_epoch;   /* consumer */
    bool              cut_pending;  /* consumer: a cut was applied, not yet taken */
    volatile uint32_t dropped;      /* producer: writes dropped for lack of room */
} ak_byte_ring_t;

/** Only while no producer can run (init / before callbacks are registered). */
void ak_byte_ring_init(ak_byte_ring_t *r);

/** Producer: queue a whole write, or drop it and return false. */
bool ak_byte_ring_push(ak_byte_ring_t *r, const uint8_t *data, uint32_t n);

/** Producer: the link went down; nothing queued so far may be delivered. */
void ak_byte_ring_cut(ak_byte_ring_t *r);

/** Consumer: copy up to `cap` bytes out. Applies a pending cut first. */
uint32_t ak_byte_ring_pop(ak_byte_ring_t *r, uint8_t *out, uint32_t cap);

/**
 * Consumer: true once after a pop applied a cut. The caller must then drop
 * any partial line it assembled from earlier bytes.
 */
bool ak_byte_ring_take_cut(ak_byte_ring_t *r);

#endif /* AK_BYTE_RING_H */
