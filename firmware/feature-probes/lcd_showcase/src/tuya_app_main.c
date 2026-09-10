/**
 * @file tuya_app_main.c
 * @brief FP-lcd-showcase — info-dense LCD 界面示例.
 *
 * Renders the product's info-dense status UI (top bar / K1-K3 slot cards /
 * context area / bottom nav hints) with the mascot demoted to a corner accent,
 * per docs/agentic-keyboard-interaction-design.md. Keys only SIMULATE a data
 * source to step through example screen states — no Wi-Fi / BLE / cloud / mic.
 *
 * Pipeline:
 *   5 keys + 5-way --(ak_input, reused)--> showcase_map(pure) --> showcase_intent_t
 *      --> showcase_state --> showcase_view_t
 *          --> ak_hud (HUD) + ak_scene (corner mascot) + ak_notify_ui (notif bar)
 *          --> LVGL --> ST7789 (320x240 landscape, rotation 270).
 *
 * showcase_view_t is the stable "state -> draw" boundary: FP-4 can fill the same
 * snapshot from real Kiro events (serial JSONL) and reuse this HUD unchanged.
 *
 * Display layer ported from CodeTank (ref_projects/CodeTank; scene/RLE/notif
 * adapted from clawd-tank, MIT). HUD layout references ki-board ui_render.cpp
 * look-and-feel, rewritten for our 320x240 panel.
 */

#include "ak_panel.h"
#include "ak_scene.h"
#include "ak_hud.h"
#include "ak_notify_ui.h"
#include "app_notification.h"
#include "showcase_state.h"
#include "showcase_map.h"
#include "ak_input.h"

#include "tal_api.h"
#include "tkl_output.h"

#include "lvgl.h"
#include "lv_vendor.h"

#define APP_TASK_STACK_SIZE   (6U * 1024U)
#define INPUT_TASK_STACK_SIZE (4U * 1024U)
#define INPUT_POLL_MS         10U

static THREAD_HANDLE s_app_thread   = NULL;
static THREAD_HANDLE s_input_thread = NULL;

/* One demo notification store, driven by the notify hook. */
static notification_store_t s_notif_store;

/* ---- hooks: showcase_state side effects -> real display modules ---- */

static void hook_set_anim(sc_anim_t anim)
{
    ak_scene_set_anim((mascot_anim_id_t)anim);
}

static void hook_play_oneshot(sc_anim_t anim)
{
    ak_scene_play_oneshot((mascot_anim_id_t)anim);
}

static void hook_render(const showcase_view_t *view)
{
    lv_vendor_disp_lock();
    ak_hud_render(view);
    lv_vendor_disp_unlock();
}

static void hook_notify(bool show, const char *msg)
{
    if (show) {
        notif_store_add(&s_notif_store, "demo", "showcase", msg);
    } else {
        notif_store_clear(&s_notif_store);
    }
    lv_vendor_disp_lock();
    ak_notify_ui_update(&s_notif_store);
    lv_vendor_disp_unlock();
}

static const showcase_hooks_t s_hooks = {
    .set_anim     = hook_set_anim,
    .play_oneshot = hook_play_oneshot,
    .render       = hook_render,
    .notify       = hook_notify,
};

/* ak_input callback: classify -> log -> apply. Runs on the input poll thread. */
static void on_input(ak_input_kind_t kind, const char *id, ak_input_event_t event, uint32_t held_ms)
{
    showcase_intent_t intent;

    if (!showcase_map_translate(kind == AK_INPUT_KIND_NAV, id, event, held_ms, &intent)) {
        return;
    }
    showcase_state_apply(&intent);
    const showcase_view_t *v = showcase_state_view();
    PR_NOTICE("[showcase] %s '%s' -> intent=%s scene=%u focus=%u rec=%d conn=%d",
              (kind == AK_INPUT_KIND_NAV) ? "nav" : "key", id, showcase_intent_name(intent.type), v->scene, v->focus,
              v->recording, v->connected);
}

static void input_thread(void *arg)
{
    (void)arg;

    if (ak_input_init() != OPRT_OK) {
        PR_ERR("input init failed");
        s_input_thread = NULL;
        return;
    }
    PR_NOTICE("input scan started (5 keys + 5-way nav)");

    while (1) {
        ak_input_poll(tal_system_get_millisecond(), on_input);
        tal_system_sleep(INPUT_POLL_MS);
    }
}

static void user_main(void)
{
    OPERATE_RET rt;

    tal_log_init(TAL_LOG_LEVEL_DEBUG, 2048, (TAL_LOG_OUTPUT_CB)tkl_log_output);
    PR_NOTICE("======== lcd_showcase FP (info-dense LCD demo) ========");

    rt = ak_panel_register();
    if (rt != OPRT_OK) {
        PR_ERR("panel register failed: %d", rt);
        return;
    }
    PR_NOTICE("panel '%s' registered (ST7789, rotation 270)", AK_PANEL_NAME);

    lv_vendor_init(AK_PANEL_NAME);
    lv_vendor_start(5, 1024 * 8);
    PR_NOTICE("LVGL started");

    /* Build the static HUD skeleton first (sets screen bg + zones). */
    lv_vendor_disp_lock();
    ak_hud_init();
    lv_vendor_disp_unlock();

    /* Corner mascot: bottom-right of the context area, small, not centered. */
    ak_scene_set_align(LV_ALIGN_BOTTOM_RIGHT, -6, -22);
    rt = ak_scene_init();
    if (rt != OPRT_OK) {
        PR_ERR("scene init failed: %d", rt);
        return;
    }

    lv_vendor_disp_lock();
    ak_notify_ui_init();
    lv_vendor_disp_unlock();

    notif_store_init(&s_notif_store);

    /* Seed the view (scene 0) and paint the first frame. */
    showcase_state_init(&s_hooks);

    THREAD_CFG_T input_cfg = {
        .stackDepth = INPUT_TASK_STACK_SIZE,
        .priority   = THREAD_PRIO_2,
        .thrdname   = "sc_input",
    };
    tal_thread_create_and_start(&s_input_thread, NULL, NULL, input_thread, NULL, &input_cfg);

    PR_NOTICE("ready: K1-3 focus slot / K4 PTT / K5 slot-cycle / nav L-R scene / center notif / up-down conn");
}

static void app_thread(void *arg)
{
    (void)arg;
    user_main();
    tal_thread_delete(s_app_thread);
    s_app_thread = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T thread_cfg = {
        .stackDepth = APP_TASK_STACK_SIZE,
        .priority   = THREAD_PRIO_1,
        .thrdname   = "lcd_show",
    };

    tal_thread_create_and_start(&s_app_thread, NULL, NULL, app_thread, NULL, &thread_cfg);
}
