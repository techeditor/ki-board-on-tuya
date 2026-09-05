/**
 * @file app_state.c
 * @brief Intent consumer: maps ak_event_t to scene + notification changes.
 */

#include "app_state.h"

#include <stdio.h>

#include "tal_api.h"
#include "lv_vendor.h"

#include "ak_scene.h"
#include "ak_notify_ui.h"
#include "app_notification.h"

/* Main Agent status cycle (k1 / nav left-right). */
static const mascot_anim_id_t s_status_cycle[] = {
    MASCOT_ANIM_THINKING,   MASCOT_ANIM_TYPING, MASCOT_ANIM_BUILDING, MASCOT_ANIM_SWEEPING, MASCOT_ANIM_CONFUSED,
    MASCOT_ANIM_CONDUCTING, MASCOT_ANIM_WIZARD, MASCOT_ANIM_BEACON,   MASCOT_ANIM_DEBUGGER,
};
#define STATUS_CYCLE_COUNT ((int)(sizeof(s_status_cycle) / sizeof(s_status_cycle[0])))

/* Demo notification messages, rotated on each add. */
static const char *const s_demo_msgs[] = {
    "Build finished", "Needs your approval", "Tests passed", "Waiting for input", "Sub-agent done",
};
#define DEMO_MSG_COUNT ((int)(sizeof(s_demo_msgs) / sizeof(s_demo_msgs[0])))

typedef struct {
    notification_store_t store;
    int                  status_idx; /* index into s_status_cycle          */
    int                  anim_idx;   /* index into all anims (nav up/down)  */
    bool                 sleeping;
    bool                 connected;
    uint32_t             notif_seq;   /* for unique notification ids         */
    int                  msg_idx;     /* rotates through s_demo_msgs         */
    bool                 oneshot_alt; /* alternate happy/alert               */
} app_state_t;

static app_state_t s_state = {0};

static void refresh_notif_ui(void)
{
    lv_vendor_disp_lock();
    ak_notify_ui_update(&s_state.store);
    lv_vendor_disp_unlock();
}

/* Newest notification id (highest seq), copied into out; false if empty. */
static bool newest_id(char *out, int out_size)
{
    const notification_t *best = NULL;

    for (int i = 0; i < NOTIF_MAX_COUNT; i++) {
        const notification_t *n = notif_store_get(&s_state.store, i);
        if (n == NULL) {
            continue;
        }
        if (best == NULL || n->seq > best->seq) {
            best = n;
        }
    }
    if (best == NULL) {
        return false;
    }
    snprintf(out, out_size, "%s", best->id);
    return true;
}

static void apply_status_cycle(int dir)
{
    s_state.sleeping   = false;
    s_state.status_idx = (s_state.status_idx + dir + STATUS_CYCLE_COUNT) % STATUS_CYCLE_COUNT;
    ak_scene_set_anim(s_status_cycle[s_state.status_idx]);
}

static void apply_anim_cycle(int dir)
{
    /* Cycle over the concrete anims, skipping the tiny MINI_MASCOT. */
    int count        = (int)MASCOT_ANIM_MINI_MASCOT; /* 0..MINI-1 */
    s_state.anim_idx = (s_state.anim_idx + dir + count) % count;
    ak_scene_set_anim((mascot_anim_id_t)s_state.anim_idx);
}

void app_state_apply(const ak_event_t *evt)
{
    if (evt == NULL) {
        return;
    }

    switch (evt->type) {
    case AK_EVT_STATUS_CYCLE:
        apply_status_cycle(evt->dir);
        break;

    case AK_EVT_ANIM_CYCLE:
        apply_anim_cycle(evt->dir);
        break;

    case AK_EVT_SLEEP_TOGGLE:
        s_state.sleeping = !s_state.sleeping;
        ak_scene_set_anim(s_state.sleeping ? MASCOT_ANIM_SLEEPING : MASCOT_ANIM_IDLE);
        break;

    case AK_EVT_ONESHOT_DEMO:
        s_state.oneshot_alt = !s_state.oneshot_alt;
        ak_scene_play_oneshot(s_state.oneshot_alt ? MASCOT_ANIM_HAPPY : MASCOT_ANIM_ALERT);
        break;

    case AK_EVT_NOTIF_ADD: {
        char id[NOTIF_MAX_ID_LEN];
        snprintf(id, sizeof(id), "n%u", (unsigned)(s_state.notif_seq++));
        notif_store_add(&s_state.store, id, "demo", s_demo_msgs[s_state.msg_idx]);
        s_state.msg_idx = (s_state.msg_idx + 1) % DEMO_MSG_COUNT;
        refresh_notif_ui();
        break;
    }

    case AK_EVT_NOTIF_DISMISS_LATEST: {
        char id[NOTIF_MAX_ID_LEN];
        if (newest_id(id, sizeof(id))) {
            notif_store_dismiss(&s_state.store, id);
            refresh_notif_ui();
        }
        break;
    }

    case AK_EVT_NOTIF_CLEAR:
        notif_store_clear(&s_state.store);
        refresh_notif_ui();
        break;

    case AK_EVT_CONNECT_TOGGLE:
        s_state.connected = !s_state.connected;
        if (!s_state.connected) {
            ak_scene_set_anim(MASCOT_ANIM_DISCONNECTED);
        } else {
            s_state.sleeping = false;
            ak_scene_set_anim(s_status_cycle[s_state.status_idx]);
        }
        break;

    case AK_EVT_NONE:
    default:
        break;
    }
}

OPERATE_RET app_state_init(void)
{
    notif_store_init(&s_state.store);
    s_state.status_idx  = 0;
    s_state.anim_idx    = (int)MASCOT_ANIM_IDLE;
    s_state.sleeping    = false;
    s_state.connected   = true;
    s_state.notif_seq   = 0;
    s_state.msg_idx     = 0;
    s_state.oneshot_alt = false;
    return OPRT_OK;
}
