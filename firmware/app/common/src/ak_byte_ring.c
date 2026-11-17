/**
 * @file ak_byte_ring.c
 * @brief SPSC byte ring with whole-write drop and disconnect cut. See header.
 */

#include "ak_byte_ring.h"

#include <stddef.h>
#include <string.h>

#define RING_MASK (AK_BYTE_RING_SIZE - 1U)

/* Keep the compiler from reordering the data copy and the index publish.
 * The target is a single Cortex-M33 core, so a compiler barrier suffices. */
#define RING_BARRIER() __asm__ __volatile__("" ::: "memory")

void ak_byte_ring_init(ak_byte_ring_t *r)
{
    memset(r, 0, sizeof(*r));
}

bool ak_byte_ring_push(ak_byte_ring_t *r, const uint8_t *data, uint32_t n)
{
    if (data == NULL || n == 0U) {
        return true;
    }
    uint32_t head = r->head;
    uint32_t used = head - r->tail;
    if (n > AK_BYTE_RING_SIZE - used) {
        r->dropped++;
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        r->buf[(head + i) & RING_MASK] = data[i];
    }
    RING_BARRIER();
    r->head = head + n;
    return true;
}

void ak_byte_ring_cut(ak_byte_ring_t *r)
{
    r->cut_at = r->head;
    RING_BARRIER();
    r->cut_epoch++;
}

uint32_t ak_byte_ring_pop(ak_byte_ring_t *r, uint8_t *out, uint32_t cap)
{
    uint32_t epoch = r->cut_epoch;
    if (epoch != r->seen_epoch) {
        RING_BARRIER();
        uint32_t cut = r->cut_at;
        r->seen_epoch = epoch;
        if ((int32_t)(cut - r->tail) > 0) {
            r->tail = cut;
        }
        r->cut_pending = true;
    }
    uint32_t tail  = r->tail;
    uint32_t avail = r->head - tail;
    RING_BARRIER();
    uint32_t n = avail < cap ? avail : cap;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = r->buf[(tail + i) & RING_MASK];
    }
    RING_BARRIER();
    r->tail = tail + n;
    return n;
}

bool ak_byte_ring_take_cut(ak_byte_ring_t *r)
{
    bool cut      = r->cut_pending;
    r->cut_pending = false;
    return cut;
}
