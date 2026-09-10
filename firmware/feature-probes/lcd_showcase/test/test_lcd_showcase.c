/**
 * @file test_lcd_showcase.c
 * @brief Host unit tests for the pure FP-lcd-showcase modules:
 *        showcase_map (input->intent) and showcase_state (scene machine).
 *        No SDK / LVGL / hardware required — display side effects are captured
 *        through recording hooks.
 */

#include <stdio.h>
#include <string.h>

#include "showcase_map.h"
#include "showcase_state.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                                                   \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

/* ---------------- recording hooks ---------------- */

static sc_anim_t g_last_anim;
static sc_anim_t g_last_oneshot;
static int       g_render_count;
static bool      g_notif_shown;

static void rec_set_anim(sc_anim_t a)
{
    g_last_anim = a;
}
static void rec_oneshot(sc_anim_t a)
{
    g_last_oneshot = a;
}
static void rec_render(const showcase_view_t *v)
{
    (void)v;
    g_render_count++;
}
static void rec_notify(bool show, const char *msg)
{
    (void)msg;
    g_notif_shown = show;
}

static const showcase_hooks_t k_hooks = {
    .set_anim     = rec_set_anim,
    .play_oneshot = rec_oneshot,
    .render       = rec_render,
    .notify       = rec_notify,
};

static void reset_hooks(void)
{
    g_last_anim    = SC_ANIM_IDLE;
    g_last_oneshot = (sc_anim_t)-1;
    g_render_count = 0;
    g_notif_shown  = false;
}

/* ---------------- showcase_map ---------------- */

static showcase_intent_t map_key(const char *id, ak_input_event_t ev, uint32_t held, int *ok)
{
    showcase_intent_t out;
    *ok = showcase_map_translate(false, id, ev, held, &out) ? 1 : 0;
    return out;
}

static showcase_intent_t map_nav(const char *id, ak_input_event_t ev, uint32_t held, int *ok)
{
    showcase_intent_t out;
    *ok = showcase_map_translate(true, id, ev, held, &out) ? 1 : 0;
    return out;
}

static void test_map(void)
{
    printf("test_map\n");
    int               ok;
    showcase_intent_t e;

    e = map_key("k1", AK_INPUT_EVENT_UP, 100, &ok);
    CHECK(ok && e.type == SC_INTENT_FOCUS_SLOT && e.arg == 0, "k1 -> focus 0");
    e = map_key("k2", AK_INPUT_EVENT_UP, 100, &ok);
    CHECK(ok && e.type == SC_INTENT_FOCUS_SLOT && e.arg == 1, "k2 -> focus 1");
    e = map_key("k3", AK_INPUT_EVENT_UP, 100, &ok);
    CHECK(ok && e.type == SC_INTENT_FOCUS_SLOT && e.arg == 2, "k3 -> focus 2");
    e = map_key("k4", AK_INPUT_EVENT_UP, 100, &ok);
    CHECK(ok && e.type == SC_INTENT_PTT_TOGGLE, "k4 -> ptt toggle");
    e = map_key("k5", AK_INPUT_EVENT_UP, 100, &ok);
    CHECK(ok && e.type == SC_INTENT_SLOT_CYCLE, "k5 -> slot cycle");

    e = map_nav("left", AK_INPUT_EVENT_UP, 100, &ok);
    CHECK(ok && e.type == SC_INTENT_SCENE_STEP && e.dir == -1, "nav left -> scene -1");
    e = map_nav("right", AK_INPUT_EVENT_UP, 100, &ok);
    CHECK(ok && e.type == SC_INTENT_SCENE_STEP && e.dir == +1, "nav right -> scene +1");
    e = map_nav("center", AK_INPUT_EVENT_UP, 100, &ok);
    CHECK(ok && e.type == SC_INTENT_NOTIF_TOGGLE, "nav center -> notif toggle");
    e = map_nav("up", AK_INPUT_EVENT_UP, 100, &ok);
    CHECK(ok && e.type == SC_INTENT_CONN_TOGGLE, "nav up -> conn toggle");
    e = map_nav("down", AK_INPUT_EVENT_UP, 100, &ok);
    CHECK(ok && e.type == SC_INTENT_CONN_TOGGLE, "nav down -> conn toggle");

    /* DOWN / LONG produce nothing (short-press only probe). */
    (void)map_key("k1", AK_INPUT_EVENT_DOWN, 0, &ok);
    CHECK(!ok, "DOWN -> none");
    (void)map_key("k1", AK_INPUT_EVENT_UP, 900, &ok);
    CHECK(!ok, "long-hold UP -> none");
}

/* ---------------- showcase_state ---------------- */

static void apply(showcase_intent_type_t t, int arg, int dir)
{
    showcase_intent_t i = {t, arg, dir};
    showcase_state_apply(&i);
}

static void test_state_init(void)
{
    printf("test_state_init\n");
    reset_hooks();
    showcase_state_init(&k_hooks);
    const showcase_view_t *v = showcase_state_view();
    CHECK(v->scene == 0, "init scene 0");
    CHECK(v->platform == PLAT_KIROCREW, "init platform kirocrew");
    CHECK(v->slots[0].status == SLOT_RUN, "slot0 run");
    CHECK(v->slots[2].occupied == false, "slot2 empty");
    CHECK(g_render_count >= 1, "init renders once");
}

static void test_scene_step_wraps(void)
{
    printf("test_scene_step_wraps\n");
    showcase_state_init(&k_hooks);
    for (int expect = 1; expect < SHOWCASE_SCENE_COUNT; expect++) {
        apply(SC_INTENT_SCENE_STEP, 0, +1);
        CHECK(showcase_state_view()->scene == (uint8_t)expect, "scene steps +1");
    }
    apply(SC_INTENT_SCENE_STEP, 0, +1); /* 5 -> wrap to 0 */
    CHECK(showcase_state_view()->scene == 0, "scene wraps to 0");
    apply(SC_INTENT_SCENE_STEP, 0, -1); /* 0 -> wrap to 5 */
    CHECK(showcase_state_view()->scene == SHOWCASE_SCENE_COUNT - 1, "scene wraps to last");
}

static void test_focus_ptt_conn(void)
{
    printf("test_focus_ptt_conn\n");
    showcase_state_init(&k_hooks);

    apply(SC_INTENT_FOCUS_SLOT, 1, 0);
    CHECK(showcase_state_view()->focus == 1, "focus -> 1");
    apply(SC_INTENT_FOCUS_SLOT, 2, 0);
    CHECK(showcase_state_view()->focus == 2, "focus -> 2");

    bool rec0 = showcase_state_view()->recording;
    apply(SC_INTENT_PTT_TOGGLE, 0, 0);
    CHECK(showcase_state_view()->recording != rec0, "ptt toggles recording");
    CHECK(strcmp(showcase_state_view()->mode, "PTT") == 0, "mode PTT when recording");

    bool conn0 = showcase_state_view()->connected;
    apply(SC_INTENT_CONN_TOGGLE, 0, 0);
    CHECK(showcase_state_view()->connected != conn0, "conn toggles");
    CHECK(g_last_anim == SC_ANIM_DISCONNECTED, "disconnected -> disconnected anim");
}

static void test_slot_cycle_and_confirm(void)
{
    printf("test_slot_cycle_and_confirm\n");
    showcase_state_init(&k_hooks);
    apply(SC_INTENT_FOCUS_SLOT, 1, 0); /* slot1 starts Idle */
    reset_hooks();

    apply(SC_INTENT_SLOT_CYCLE, 0, 0); /* Idle -> Run */
    CHECK(showcase_state_view()->slots[1].status == SLOT_RUN, "idle -> run");

    apply(SC_INTENT_SLOT_CYCLE, 0, 0); /* Run -> Confirm (one-shot alert edge) */
    CHECK(showcase_state_view()->slots[1].status == SLOT_CONFIRM, "run -> confirm");
    CHECK(showcase_state_view()->awaiting_confirm == true, "awaiting confirm set");
    CHECK(g_last_oneshot == SC_ANIM_ALERT, "confirm plays alert one-shot");

    apply(SC_INTENT_SLOT_CYCLE, 0, 0); /* Confirm -> Idle */
    CHECK(showcase_state_view()->slots[1].status == SLOT_IDLE, "confirm -> idle");
    CHECK(showcase_state_view()->awaiting_confirm == false, "awaiting cleared");
}

static void test_notif_toggle(void)
{
    printf("test_notif_toggle\n");
    showcase_state_init(&k_hooks);
    bool n0 = showcase_state_view()->notif_shown;
    apply(SC_INTENT_NOTIF_TOGGLE, 0, 0);
    CHECK(showcase_state_view()->notif_shown != n0, "notif toggles");
    CHECK(g_notif_shown == showcase_state_view()->notif_shown, "notify hook matches view");
}

static void test_anim_for_view(void)
{
    printf("test_anim_for_view\n");
    showcase_view_t v;
    memset(&v, 0, sizeof(v));
    v.connected         = true;
    v.focus             = 0;
    v.slots[0].occupied = true;

    v.slots[0].status = SLOT_RUN;
    CHECK(showcase_state_anim_for_view(&v) == SC_ANIM_TYPING, "run -> typing");
    v.recording = true;
    CHECK(showcase_state_anim_for_view(&v) == SC_ANIM_THINKING, "recording -> thinking");
    v.recording        = false;
    v.awaiting_confirm = true;
    CHECK(showcase_state_anim_for_view(&v) == SC_ANIM_ALERT, "confirm -> alert");
    v.awaiting_confirm = false;
    v.connected        = false;
    CHECK(showcase_state_anim_for_view(&v) == SC_ANIM_DISCONNECTED, "disconnected -> disconnected");
}

int main(void)
{
    printf("== lcd_showcase (FP) host tests ==\n");
    test_map();
    test_state_init();
    test_scene_step_wraps();
    test_focus_ptt_conn();
    test_slot_cycle_and_confirm();
    test_notif_toggle();
    test_anim_for_view();

    if (g_failures == 0) {
        printf("ALL PASSED\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
