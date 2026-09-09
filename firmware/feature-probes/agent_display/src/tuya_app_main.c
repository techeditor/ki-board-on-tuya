/**
 * @file tuya_app_main.c
 * @brief FP-1 agent_display — key-driven Agent status + notification display.
 *
 * Pipeline (see .kiro/specs/feature-probes-agent-display/):
 *   5 keys + 5-way nav --(ak_input)--> input_map (pure) --> ak_event_t intent
 *      --> app_state --> ak_scene (mascot) / ak_notify_ui (notifications)
 *      --> LVGL --> ST7789 (320x240 landscape).
 *
 * No Wi-Fi/TCP/cloud: the keys stand in for a real Agent event source, which
 * FP-3 will replace with serial JSONL from the companion while reusing the same
 * app_state. LVGL runs on our own panel (CL28CK230-18A, INVOFF init) via
 * TDD->TDL->LVGL, independent of CodeTank's official-board LCD config.
 */

#include "ak_panel.h"
#include "ak_scene.h"
#include "ak_notify_ui.h"
#include "app_state.h"
#include "ak_input.h"
#include "input_map.h"

#include "tal_api.h"
#include "tkl_output.h"

#include "lvgl.h"
#include "lv_vendor.h"

#define APP_TASK_STACK_SIZE   (6U * 1024U)
#define INPUT_TASK_STACK_SIZE (4U * 1024U)
#define INPUT_POLL_MS         10U

static THREAD_HANDLE s_app_thread   = NULL;
static THREAD_HANDLE s_input_thread = NULL;

static void draw_chrome(void)
{
    /* All LVGL object calls must hold the vendor display mutex. */
    lv_vendor_disp_lock();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101018), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Kiro Agent Display");
    lv_obj_set_style_text_color(title, lv_color_hex(0xC8B4FF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    lv_vendor_disp_unlock();
}

/* ak_input callback: classify -> log -> apply. Runs on the input poll thread. */
static void on_input(ak_input_kind_t kind, const char *id, ak_input_event_t event, uint32_t held_ms)
{
    ak_event_t evt;

    if (!input_map_translate(kind == AK_INPUT_KIND_NAV, id, event, held_ms, &evt)) {
        return;
    }
    PR_NOTICE("[evt] %s '%s' %s held=%ums -> intent=%d dir=%d", (kind == AK_INPUT_KIND_NAV) ? "nav" : "key", id,
              ak_input_event_str(event), held_ms, (int)evt.type, evt.dir);
    app_state_apply(&evt);
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
    PR_NOTICE("======== agent_display FP-1 (key-driven states) ========");

    rt = ak_panel_register();
    if (rt != OPRT_OK) {
        PR_ERR("panel register failed: %d", rt);
        return;
    }
    PR_NOTICE("panel '%s' registered (ST7789, rotation 90)", AK_PANEL_NAME);

    /* Finds the panel by name, opens it, allocates draw buffers, applies rotation. */
    lv_vendor_init(AK_PANEL_NAME);
    lv_vendor_start(5, 1024 * 8);
    PR_NOTICE("LVGL started");

    draw_chrome();

    rt = ak_scene_init();
    if (rt != OPRT_OK) {
        PR_ERR("scene init failed: %d", rt);
        return;
    }

    lv_vendor_disp_lock();
    ak_notify_ui_init();
    lv_vendor_disp_unlock();

    app_state_init();

    THREAD_CFG_T input_cfg = {
        .stackDepth = INPUT_TASK_STACK_SIZE,
        .priority   = THREAD_PRIO_2,
        .thrdname   = "ak_input",
    };
    tal_thread_create_and_start(&s_input_thread, NULL, NULL, input_thread, NULL, &input_cfg);

    PR_NOTICE("ready: press keys to drive Agent status / notifications");
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
        .thrdname   = "agent_disp",
    };

    tal_thread_create_and_start(&s_app_thread, NULL, NULL, app_thread, NULL, &thread_cfg);
}
