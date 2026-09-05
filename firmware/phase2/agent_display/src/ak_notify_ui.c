/**
 * @file ak_notify_ui.c
 * @brief Lightweight LVGL notification bar (P2-1 T4).
 */

#include "ak_notify_ui.h"

#include <stdio.h>

#include "tal_api.h"

#include "lvgl.h"

#define BAR_HEIGHT 40

static lv_obj_t *s_bar   = NULL;
static lv_obj_t *s_label = NULL;

/* Return the newest active notification (highest seq), or NULL if empty. */
static const notification_t *newest(const notification_store_t *store)
{
    const notification_t *best = NULL;

    for (int i = 0; i < NOTIF_MAX_COUNT; i++) {
        const notification_t *n = notif_store_get(store, i);
        if (n == NULL) {
            continue;
        }
        if (best == NULL || n->seq > best->seq) {
            best = n;
        }
    }
    return best;
}

OPERATE_RET ak_notify_ui_init(void)
{
    lv_obj_t *scr = lv_screen_active();

    s_bar = lv_obj_create(scr);
    lv_obj_remove_style_all(s_bar);
    lv_obj_set_size(s_bar, LV_PCT(100), BAR_HEIGHT);
    lv_obj_align(s_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x201830), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_bar, 4, LV_PART_MAIN);

    s_label = lv_label_create(s_bar);
    lv_label_set_long_mode(s_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_label, LV_PCT(100));
    lv_obj_align(s_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(s_label, lv_color_hex(0xE6E0FF), LV_PART_MAIN);

    lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN); /* hidden until a notification exists */
    return OPRT_OK;
}

void ak_notify_ui_update(const notification_store_t *store)
{
    const notification_t *n = newest(store);

    if (s_bar == NULL) {
        return;
    }
    if (n == NULL) {
        lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    char text[NOTIF_MAX_PROJ_LEN + NOTIF_MAX_MSG_LEN + 16];
    int  count = notif_store_count(store);
    if (count > 1) {
        snprintf(text, sizeof(text), "[%s] %s  (%d)", n->project, n->message, count);
    } else {
        snprintf(text, sizeof(text), "[%s] %s", n->project, n->message);
    }
    lv_label_set_text(s_label, text);
    lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
}
