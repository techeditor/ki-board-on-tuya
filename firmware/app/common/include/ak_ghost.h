/**
 * @file ak_ghost.h
 * @brief FR-14: animation of the three chat ghosts in the LCD sidebar (pure C).
 *
 * Selecting a chat makes its ghost float up and down and blink for
 * AK_GHOST_SELECT_RUN_MS; a status change on any chat plays a short hop.
 * Then the ghost rests again, so a resting keyboard sends nothing to the LCD.
 *
 * The app calls ak_ghost_tick() from its loop. A tick at most every
 * AK_GHOST_FRAME_MS returns which slots changed pose; only those 32 x 36 px
 * areas are redrawn and sent to the panel. When the main page is not on screen
 * (②③④⑦, settings, OTA ...), on low battery or with the setting off, the app
 * passes allowed = false and every animation stops at once.
 */

#ifndef AK_GHOST_H
#define AK_GHOST_H

#include <stdbool.h>
#include <stdint.h>

#define AK_GHOST_SLOTS 3U

#define AK_GHOST_FRAME_MS        80U   /* one pose step (12.5 fps) */
#define AK_GHOST_SELECT_RUN_MS   6000U /* float + blink after a chat is selected */
#define AK_GHOST_STATUS_RUN_MS   960U  /* hop after a status change (12 steps) */
#define AK_GHOST_BLINK_EVERY_MS  2000U /* while floating: one blink per period ... */
#define AK_GHOST_BLINK_MS        160U  /* ... closed for this long, at its end */
#define AK_GHOST_AMPLITUDE_PX    2     /* |dy| never exceeds this */
#define AK_GHOST_LOW_BATTERY_PCT 15U   /* app: no animation at or below, unless charging */

typedef struct {
    int8_t dy;    /* vertical offset in px, negative = up */
    bool   blink; /* eyes closed */
} ak_ghost_pose_t;

typedef enum {
    AK_GHOST_NONE = 0,
    AK_GHOST_SELECT,
    AK_GHOST_STATUS,
} ak_ghost_kind_t;

typedef struct {
    bool            enabled;
    uint8_t         kind[AK_GHOST_SLOTS];
    uint32_t        start_ms[AK_GHOST_SLOTS];
    ak_ghost_pose_t pose[AK_GHOST_SLOTS];
    uint32_t        last_ms;
} ak_ghost_t;

void ak_ghost_init(ak_ghost_t *g, bool enabled);

/** Setting on / off. Off stops every animation; the next tick rests the poses. */
void ak_ghost_set_enabled(ak_ghost_t *g, bool enabled);

/** Chat `slot` was selected: float + blink there, stop any other float. */
void ak_ghost_select(ak_ghost_t *g, uint8_t slot, uint32_t now_ms);

/** Chat `slot` changed status: a short hop (replaces its float). */
void ak_ghost_status(ak_ghost_t *g, uint8_t slot, uint32_t now_ms);

/** Something still runs or a pose is not at rest (the app may skip ticks otherwise). */
bool ak_ghost_active(const ak_ghost_t *g);

/**
 * Advance at most once per AK_GHOST_FRAME_MS. With allowed = false every
 * animation stops and the poses rest without reporting a change (the sidebar
 * is not on screen and is redrawn at rest later).
 * @return bit i set when slot i needs a redraw with ak_ghost_pose(g, i).
 */
uint8_t ak_ghost_tick(ak_ghost_t *g, uint32_t now_ms, bool allowed);

ak_ghost_pose_t ak_ghost_pose(const ak_ghost_t *g, uint8_t slot);

/** Pose of an animation `elapsed_ms` after it started (also used by the GIF preview). */
ak_ghost_pose_t ak_ghost_pose_at(ak_ghost_kind_t kind, uint32_t elapsed_ms);

#endif /* AK_GHOST_H */
