/**
 * @file ak_scene.h
 * @brief Single-mascot scene engine (P2-1 T3) — the single-session slice of
 *        CodeTank's app_scene.c, adapted to our 320x240 landscape panel.
 *
 * Owns one LVGL image, the full mascot animation registry, and a frame-advance
 * timer that honours each animation's own frame_ms and looping flag. Supports:
 *   - ak_scene_set_anim():   set the persistent (looping) state
 *   - ak_scene_play_oneshot(): play a one-shot (alert/happy/going_away/...),
 *                              then automatically fall back to the persistent state
 *
 * The mascot_anim_id_t enum is copied verbatim from CodeTank app_scene.h so the
 * ported sprite registry (sprite_mascot_registry.h) compiles unchanged.
 *
 * Threading: the internal frame timer runs on the LVGL task (already inside the
 * lv_vendor lock). set_anim / play_oneshot are called from other tasks and take
 * the lock themselves.
 */

#ifndef AK_SCENE_H
#define AK_SCENE_H

#include "tuya_cloud_types.h"

/* Copied verbatim from CodeTank source/embedded/include/app_scene.h. */
typedef enum {
    MASCOT_ANIM_IDLE,
    MASCOT_ANIM_ALERT,
    MASCOT_ANIM_HAPPY,
    MASCOT_ANIM_SLEEPING,
    MASCOT_ANIM_DISCONNECTED,
    MASCOT_ANIM_THINKING,
    MASCOT_ANIM_TYPING,
    MASCOT_ANIM_JUGGLING,
    MASCOT_ANIM_BUILDING,
    MASCOT_ANIM_CONFUSED,
    MASCOT_ANIM_DIZZY,
    MASCOT_ANIM_SWEEPING,
    MASCOT_ANIM_WALKING,
    MASCOT_ANIM_GOING_AWAY,
    MASCOT_ANIM_DEBUGGER,
    MASCOT_ANIM_WIZARD,
    MASCOT_ANIM_CONDUCTING,
    MASCOT_ANIM_BEACON,
    MASCOT_ANIM_MINI_MASCOT,
    MASCOT_ANIM_COUNT,
} mascot_anim_id_t;

/** Create the mascot image (idle) centered on the active screen; start the timer. */
OPERATE_RET ak_scene_init(void);

/** Set the persistent looping animation (resets to its frame 0). */
void ak_scene_set_anim(mascot_anim_id_t anim);

/** Play a one-shot animation once, then fall back to the persistent animation. */
void ak_scene_play_oneshot(mascot_anim_id_t anim);

/** Current persistent animation id (ignores a one-shot in flight). */
mascot_anim_id_t ak_scene_persistent(void);

/** Human-readable animation name (for logs). */
const char *ak_scene_anim_name(mascot_anim_id_t anim);

#endif /* AK_SCENE_H */
