/**
 * @file showcase_state.c
 * @brief Showcase state machine (see showcase_state.h). SDK-free / host-testable:
 *        all display side effects go through injected hooks.
 */

#include "showcase_state.h"

#include <string.h>

static showcase_view_t  s_view;
static showcase_hooks_t s_hooks;

/* ---------- scene table: 6 canonical showcase scenes ---------- */

static void load_scene(uint8_t scene)
{
    /* Reset the per-scene fields; platform/effort/fw are constant chrome. */
    s_view.scene            = scene;
    s_view.recording        = false;
    s_view.awaiting_confirm = false;
    s_view.notif_shown      = false;
    s_view.connected        = true;
    s_view.focus            = 0;
    strncpy(s_view.mode, "IDLE", sizeof(s_view.mode));
    s_view.context[0]  = '\0';
    s_view.last_msg[0] = '\0';

    /* Default 3 slots. */
    memset(s_view.slots, 0, sizeof(s_view.slots));
    s_view.slots[0] = (showcase_slot_t){true, SLOT_RUN, "Kilo", "kiro/crew"};
    s_view.slots[1] = (showcase_slot_t){true, SLOT_IDLE, "Sonnet", "keyboard"};
    s_view.slots[2] = (showcase_slot_t){false, SLOT_EMPTY, "", ""};
    strncpy(s_view.context, "building firmware...", sizeof(s_view.context) - 1);
    strncpy(s_view.last_msg, "add rate limiting to upload", sizeof(s_view.last_msg) - 1);

    switch (scene) {
    case 0: /* three-slot normal */
        s_view.focus = 0;
        break;
    case 1: /* focus K2 */
        s_view.focus = 1;
        break;
    case 2: /* PTT recording */
        s_view.recording = true;
        s_view.focus     = 0;
        strncpy(s_view.mode, "PTT", sizeof(s_view.mode));
        strncpy(s_view.context, "listening...", sizeof(s_view.context) - 1);
        break;
    case 3: /* awaiting confirm */
        s_view.awaiting_confirm = true;
        s_view.focus            = 0;
        s_view.slots[0].status  = SLOT_CONFIRM;
        strncpy(s_view.context, "Approve tool call?", sizeof(s_view.context) - 1);
        break;
    case 4: /* notification */
        s_view.notif_shown = true;
        s_view.focus       = 0;
        break;
    case 5: /* disconnected */
        s_view.connected = false;
        strncpy(s_view.context, "companion offline", sizeof(s_view.context) - 1);
        break;
    default:
        break;
    }
}

/* ---------- pure: which mascot anim the view implies ---------- */

sc_anim_t showcase_state_anim_for_view(const showcase_view_t *view)
{
    if (!view->connected) {
        return SC_ANIM_DISCONNECTED;
    }
    if (view->awaiting_confirm) {
        return SC_ANIM_ALERT; /* caller may choose to play as one-shot */
    }
    if (view->recording) {
        return SC_ANIM_THINKING;
    }
    switch (view->slots[view->focus].status) {
    case SLOT_RUN:
        return SC_ANIM_TYPING;
    case SLOT_CONFIRM:
        return SC_ANIM_ALERT;
    case SLOT_ERROR:
        return SC_ANIM_CONFUSED;
    case SLOT_IDLE:
    case SLOT_EMPTY:
    default:
        return SC_ANIM_IDLE;
    }
}

/* ---------- side-effect drivers (guarded; no-op if hook absent) ---------- */

static void drive_scene(bool confirm_edge)
{
    sc_anim_t anim = showcase_state_anim_for_view(&s_view);
    if (confirm_edge && anim == SC_ANIM_ALERT) {
        if (s_hooks.play_oneshot) {
            s_hooks.play_oneshot(SC_ANIM_ALERT);
        }
        return;
    }
    if (s_hooks.set_anim) {
        s_hooks.set_anim(anim);
    }
}

static void drive_render(void)
{
    if (s_hooks.render) {
        s_hooks.render(&s_view);
    }
}

static void drive_notify(void)
{
    if (s_hooks.notify) {
        s_hooks.notify(s_view.notif_shown, s_view.notif_shown ? "Build finished" : "");
    }
}

static void refresh_all(bool confirm_edge)
{
    drive_scene(confirm_edge);
    drive_notify();
    drive_render();
}

/* ---------- public API ---------- */

void showcase_state_init(const showcase_hooks_t *hooks)
{
    if (hooks) {
        s_hooks = *hooks;
    } else {
        memset(&s_hooks, 0, sizeof(s_hooks));
    }

    memset(&s_view, 0, sizeof(s_view));
    s_view.platform = PLAT_KIROCREW;
    strncpy(s_view.effort, "High", sizeof(s_view.effort));
    strncpy(s_view.fw, "0.1.0", sizeof(s_view.fw));
    load_scene(0);
    refresh_all(false);
}

void showcase_state_apply(const showcase_intent_t *intent)
{
    if (intent == NULL || intent->type == SC_INTENT_NONE) {
        return;
    }

    bool confirm_edge = false;

    switch (intent->type) {
    case SC_INTENT_FOCUS_SLOT:
        if (intent->arg >= 0 && intent->arg < SHOWCASE_SLOT_COUNT) {
            s_view.focus = (uint8_t)intent->arg;
        }
        break;

    case SC_INTENT_PTT_TOGGLE:
        s_view.recording = !s_view.recording;
        if (s_view.recording) {
            strncpy(s_view.mode, "PTT", sizeof(s_view.mode));
            strncpy(s_view.context, "listening...", sizeof(s_view.context) - 1);
            s_view.context[sizeof(s_view.context) - 1] = '\0';
        } else {
            strncpy(s_view.mode, "IDLE", sizeof(s_view.mode));
        }
        break;

    case SC_INTENT_SLOT_CYCLE: {
        /* focused slot status: Idle -> Run -> Confirm -> Idle (skip Empty/Error) */
        showcase_slot_t *sl = &s_view.slots[s_view.focus];
        sl->occupied        = true;
        switch (sl->status) {
        case SLOT_IDLE:
            sl->status = SLOT_RUN;
            break;
        case SLOT_RUN:
            sl->status              = SLOT_CONFIRM;
            s_view.awaiting_confirm = true;
            confirm_edge            = true;
            break;
        case SLOT_CONFIRM:
        default:
            sl->status              = SLOT_IDLE;
            s_view.awaiting_confirm = false;
            break;
        }
        break;
    }

    case SC_INTENT_SCENE_STEP: {
        int next = ((int)s_view.scene + intent->dir + SHOWCASE_SCENE_COUNT) % SHOWCASE_SCENE_COUNT;
        load_scene((uint8_t)next);
        confirm_edge = (next == 3); /* scene 3 = awaiting-confirm: alert one-shot */
        break;
    }

    case SC_INTENT_NOTIF_TOGGLE:
        s_view.notif_shown = !s_view.notif_shown;
        break;

    case SC_INTENT_CONN_TOGGLE:
        s_view.connected = !s_view.connected;
        break;

    case SC_INTENT_NONE:
    default:
        return;
    }

    refresh_all(confirm_edge);
}

const showcase_view_t *showcase_state_view(void)
{
    return &s_view;
}
