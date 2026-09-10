/**
 * @file showcase_state.h
 * @brief Showcase state machine + the "state -> draw" snapshot boundary.
 *
 * Consumes showcase_intent_t (from showcase_map or, later, FP-4's JSONL parser),
 * maintains a showcase_view_t, and drives two SIDE EFFECTS through injected
 * callbacks so the pure state logic stays host-testable:
 *   - a "scene" hook  -> corner mascot animation (ak_scene on device)
 *   - a "render" hook -> HUD repaint (ak_hud on device)
 *   - a "notify" hook -> notification bar (ak_notify_ui on device)
 *
 * On the host, tests install no-op / recording hooks and assert on the view.
 */

#ifndef SHOWCASE_STATE_H
#define SHOWCASE_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "showcase_map.h"

/* ---- the "state -> draw" snapshot ---- */

typedef enum {
    SLOT_EMPTY = 0,
    SLOT_IDLE,
    SLOT_RUN,
    SLOT_CONFIRM,
    SLOT_ERROR,
} slot_status_t;

typedef struct {
    bool          occupied;
    slot_status_t status;
    char          name[16];   /* slot / agent name         */
    char          detail[24]; /* cwd / session name        */
} showcase_slot_t;

typedef enum {
    PLAT_KIROCREW = 0,
    PLAT_KIROCLI,
    PLAT_CODEX,
} platform_t;

#define SHOWCASE_SLOT_COUNT  3
#define SHOWCASE_SCENE_COUNT 6

typedef struct {
    platform_t      platform;
    char            mode[8];   /* "PTT" / "IDLE"            */
    char            effort[8]; /* "High" / "Med" / "Low"    */
    bool            connected;
    char            fw[8]; /* "0.1.0"                   */
    showcase_slot_t slots[SHOWCASE_SLOT_COUNT];
    uint8_t         focus;     /* 0..2 focused slot         */
    bool            recording; /* PTT recording             */
    char            context[40];
    char            last_msg[40];
    bool            awaiting_confirm;
    uint8_t         scene;       /* 0..SHOWCASE_SCENE_COUNT-1 */
    bool            notif_shown; /* demo notification visible */
} showcase_view_t;

/* Mascot animation ids, mirrored from ak_scene.h's mascot_anim_id_t so the
 * state module does not need to include the SDK-touching ak_scene.h on host.
 * Values MUST match ak_scene.h. */
typedef enum {
    SC_ANIM_IDLE = 0,
    SC_ANIM_ALERT,
    SC_ANIM_HAPPY,
    SC_ANIM_SLEEPING,
    SC_ANIM_DISCONNECTED,
    SC_ANIM_THINKING,
    SC_ANIM_TYPING,
    SC_ANIM_CONFUSED = 9, /* matches MASCOT_ANIM_CONFUSED */
} sc_anim_t;

/* Side-effect hooks (installed by tuya_app_main on device; no-op on host). */
typedef struct {
    void (*set_anim)(sc_anim_t anim);     /* persistent looping anim   */
    void (*play_oneshot)(sc_anim_t anim); /* one-shot, then fall back  */
    void (*render)(const showcase_view_t *view);
    void (*notify)(bool show, const char *msg); /* demo notification bar */
} showcase_hooks_t;

/** Install hooks + seed the initial view (scene 0). Renders once. */
void showcase_state_init(const showcase_hooks_t *hooks);

/** Apply one intent: update the view, drive scene/render/notify hooks. */
void showcase_state_apply(const showcase_intent_t *intent);

/** Read-only snapshot of the current view. */
const showcase_view_t *showcase_state_view(void);

/** Which mascot anim the current view implies (pure; used by tests + apply). */
sc_anim_t showcase_state_anim_for_view(const showcase_view_t *view);

#endif /* SHOWCASE_STATE_H */
