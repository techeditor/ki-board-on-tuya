/**
 * @file ak_ghost.c
 * @brief FR-14 chat ghost animation. See ak_ghost.h.
 *
 * A host-side preview tool reads the two tables below to draw the GIF
 * preview, so keep them as plain {...} integer lists.
 */

#include "ak_ghost.h"

#include <stddef.h>

/* Float: one step per frame, 16 steps = 1.28 s per up-and-down cycle. */
static const int8_t k_float_dy[16] = {0, -1, -1, -2, -2, -2, -1, -1, 0, 1, 1, 2, 2, 2, 1, 1};
/* Hop on a status change: 12 steps = AK_GHOST_STATUS_RUN_MS; eyes close at steps 5..6. */
static const int8_t k_hop_dy[12] = {-1, -2, -2, -2, -1, 0, 1, 0, -1, 0, 0, 0};

#define FLOAT_STEPS (sizeof(k_float_dy) / sizeof(k_float_dy[0]))
#define HOP_STEPS   (sizeof(k_hop_dy) / sizeof(k_hop_dy[0]))

static const ak_ghost_pose_t k_rest = {0, false};

static bool pose_eq(ak_ghost_pose_t a, ak_ghost_pose_t b)
{
    return a.dy == b.dy && a.blink == b.blink;
}

void ak_ghost_init(ak_ghost_t *g, bool enabled)
{
    if (g == NULL) {
        return;
    }
    for (uint8_t i = 0U; i < AK_GHOST_SLOTS; i++) {
        g->kind[i]     = AK_GHOST_NONE;
        g->start_ms[i] = 0U;
        g->pose[i]     = k_rest;
    }
    g->enabled = enabled;
    g->last_ms = 0U;
}

void ak_ghost_set_enabled(ak_ghost_t *g, bool enabled)
{
    if (g == NULL) {
        return;
    }
    g->enabled = enabled;
    if (!enabled) {
        for (uint8_t i = 0U; i < AK_GHOST_SLOTS; i++) {
            g->kind[i] = AK_GHOST_NONE; /* the next tick rests and reports the pose */
        }
    }
}

static void start(ak_ghost_t *g, uint8_t slot, ak_ghost_kind_t kind, uint32_t now_ms)
{
    if (g == NULL || slot >= AK_GHOST_SLOTS || !g->enabled) {
        return;
    }
    g->kind[slot]     = (uint8_t)kind;
    g->start_ms[slot] = now_ms;
}

void ak_ghost_select(ak_ghost_t *g, uint8_t slot, uint32_t now_ms)
{
    if (g == NULL || slot >= AK_GHOST_SLOTS || !g->enabled) {
        return;
    }
    for (uint8_t i = 0U; i < AK_GHOST_SLOTS; i++) {
        if (i != slot && g->kind[i] == AK_GHOST_SELECT) {
            g->kind[i] = AK_GHOST_NONE;
        }
    }
    start(g, slot, AK_GHOST_SELECT, now_ms);
}

void ak_ghost_status(ak_ghost_t *g, uint8_t slot, uint32_t now_ms)
{
    start(g, slot, AK_GHOST_STATUS, now_ms);
}

bool ak_ghost_active(const ak_ghost_t *g)
{
    if (g == NULL) {
        return false;
    }
    for (uint8_t i = 0U; i < AK_GHOST_SLOTS; i++) {
        if (g->kind[i] != AK_GHOST_NONE || !pose_eq(g->pose[i], k_rest)) {
            return true;
        }
    }
    return false;
}

ak_ghost_pose_t ak_ghost_pose_at(ak_ghost_kind_t kind, uint32_t elapsed_ms)
{
    ak_ghost_pose_t p    = k_rest;
    uint32_t        step = elapsed_ms / AK_GHOST_FRAME_MS;

    if (kind == AK_GHOST_SELECT && elapsed_ms < AK_GHOST_SELECT_RUN_MS) {
        p.dy    = k_float_dy[step % FLOAT_STEPS];
        p.blink = elapsed_ms % AK_GHOST_BLINK_EVERY_MS >= AK_GHOST_BLINK_EVERY_MS - AK_GHOST_BLINK_MS;
    } else if (kind == AK_GHOST_STATUS && step < HOP_STEPS) {
        p.dy    = k_hop_dy[step];
        p.blink = step == 5U || step == 6U;
    }
    return p;
}

uint8_t ak_ghost_tick(ak_ghost_t *g, uint32_t now_ms, bool allowed)
{
    uint8_t changed = 0U;

    if (g == NULL) {
        return 0U;
    }
    if (!allowed) {
        for (uint8_t i = 0U; i < AK_GHOST_SLOTS; i++) {
            g->kind[i] = AK_GHOST_NONE;
            g->pose[i] = k_rest;
        }
        return 0U;
    }
    if ((uint32_t)(now_ms - g->last_ms) < AK_GHOST_FRAME_MS) {
        return 0U;
    }
    g->last_ms = now_ms;
    for (uint8_t i = 0U; i < AK_GHOST_SLOTS; i++) {
        ak_ghost_pose_t p = k_rest;
        if (g->kind[i] != AK_GHOST_NONE) {
            uint32_t elapsed = now_ms - g->start_ms[i];
            uint32_t run = g->kind[i] == AK_GHOST_SELECT ? AK_GHOST_SELECT_RUN_MS : AK_GHOST_STATUS_RUN_MS;
            if (elapsed >= run) {
                g->kind[i] = AK_GHOST_NONE; /* time limit reached: rest */
            } else {
                p = ak_ghost_pose_at((ak_ghost_kind_t)g->kind[i], elapsed);
            }
        }
        if (!pose_eq(p, g->pose[i])) {
            g->pose[i] = p;
            changed |= (uint8_t)(1U << i);
        }
    }
    return changed;
}

ak_ghost_pose_t ak_ghost_pose(const ak_ghost_t *g, uint8_t slot)
{
    return (g == NULL || slot >= AK_GHOST_SLOTS) ? k_rest : g->pose[slot];
}
