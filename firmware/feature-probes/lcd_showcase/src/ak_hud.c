/**
 * @file ak_hud.c
 * @brief Info-dense LVGL HUD for FP-lcd-showcase (see ak_hud.h).
 *
 * LVGL v9 objects (labels + styled containers). Layout is built once in
 * ak_hud_init(); ak_hud_render() only updates text and colors. The corner
 * mascot and the notification bar are drawn by their own modules on top of the
 * free region this HUD leaves (context bottom-right + very bottom strip).
 */

#include "ak_hud.h"

#include <stdio.h>
#include <string.h>

#include "tal_api.h"
#include "lvgl.h"
#include "lv_vendor.h"

/* ---- palette (RGB888 for LVGL; semantics match ki-board / FP-1) ---- */
#define COL_BG     0x101018
#define COL_PANEL  0x1B1B27
#define COL_KIRO   0xC8B4FF /* Kiro violet     */
#define COL_CYAN   0x00E5FF
#define COL_GREEN  0x33D17A
#define COL_RED    0xF2504B
#define COL_AMBER  0xFFC24B
#define COL_TEXT   0xFFFFFF
#define COL_MUTED  0x8A8A99
#define COL_BORDER 0x2E2E3E

#define SCREEN_W 320
#define SCREEN_H 240

/* zone geometry */
#define BAR_H    22
#define SLOTS_Y  24
#define SLOTS_H  62
#define SLOT_GAP 4
#define CTX_Y    88
#define CTX_H    112
#define NAV_Y    202
#define NAV_H    18

typedef struct {
    lv_obj_t *bar;
    lv_obj_t *bar_left;  /* platform / mode / effort */
    lv_obj_t *bar_right; /* conn + fw                */

    lv_obj_t *slot_card[SHOWCASE_SLOT_COUNT];
    lv_obj_t *slot_name[SHOWCASE_SLOT_COUNT];
    lv_obj_t *slot_state[SHOWCASE_SLOT_COUNT];
    lv_obj_t *slot_detail[SHOWCASE_SLOT_COUNT];

    lv_obj_t *ctx_panel;
    lv_obj_t *ctx_title; /* "Context"  */
    lv_obj_t *ctx_line;  /* > building firmware... */
    lv_obj_t *ctx_last;  /* last: "..." */
    lv_obj_t *ctx_wave;  /* [ listening ... ] shown when recording */

    lv_obj_t *nav; /* bottom hints */
} hud_t;

static hud_t s_hud = {0};

static lv_obj_t *mk_label(lv_obj_t *parent, uint32_t color, lv_align_t align, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
    lv_label_set_text(l, "");
    lv_obj_align(l, align, x, y);
    return l;
}

static lv_obj_t *mk_panel(lv_obj_t *parent, int x, int y, int w, int h, uint32_t border)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, w, h);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_style_bg_color(p, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(p, lv_color_hex(border), LV_PART_MAIN);
    lv_obj_set_style_border_width(p, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(p, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(p, 3, LV_PART_MAIN);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

OPERATE_RET ak_hud_init(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* ---- top status bar ---- */
    s_hud.bar = mk_panel(scr, 0, 0, SCREEN_W, BAR_H, COL_BORDER);
    lv_obj_set_style_radius(s_hud.bar, 0, LV_PART_MAIN);
    s_hud.bar_left  = mk_label(s_hud.bar, COL_KIRO, LV_ALIGN_LEFT_MID, 0, 0);
    s_hud.bar_right = mk_label(s_hud.bar, COL_GREEN, LV_ALIGN_RIGHT_MID, 0, 0);

    /* ---- three slot cards ---- */
    int card_w = (SCREEN_W - 2 * SLOT_GAP - 4) / SHOWCASE_SLOT_COUNT;
    for (int i = 0; i < SHOWCASE_SLOT_COUNT; i++) {
        int x                = 2 + i * (card_w + SLOT_GAP);
        s_hud.slot_card[i]   = mk_panel(scr, x, SLOTS_Y, card_w, SLOTS_H, COL_BORDER);
        s_hud.slot_name[i]   = mk_label(s_hud.slot_card[i], COL_TEXT, LV_ALIGN_TOP_LEFT, 0, 0);
        s_hud.slot_state[i]  = mk_label(s_hud.slot_card[i], COL_MUTED, LV_ALIGN_LEFT_MID, 0, 2);
        s_hud.slot_detail[i] = mk_label(s_hud.slot_card[i], COL_MUTED, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }

    /* ---- context panel (mascot sits in its bottom-right, drawn by ak_scene) ---- */
    s_hud.ctx_panel = mk_panel(scr, 2, CTX_Y, SCREEN_W - 4, CTX_H, COL_BORDER);
    s_hud.ctx_title = mk_label(s_hud.ctx_panel, COL_CYAN, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(s_hud.ctx_title, "Context");
    s_hud.ctx_line = mk_label(s_hud.ctx_panel, COL_TEXT, LV_ALIGN_TOP_LEFT, 0, 20);
    s_hud.ctx_last = mk_label(s_hud.ctx_panel, COL_MUTED, LV_ALIGN_TOP_LEFT, 0, 40);
    s_hud.ctx_wave = mk_label(s_hud.ctx_panel, COL_AMBER, LV_ALIGN_TOP_LEFT, 0, 62);

    /* ---- bottom nav hints ---- */
    s_hud.nav = mk_label(scr, COL_MUTED, LV_ALIGN_BOTTOM_LEFT, 4, -2);
    lv_label_set_text(s_hud.nav,
                      LV_SYMBOL_LEFT " Back  " LV_SYMBOL_RIGHT " Approve  " LV_SYMBOL_OK " Details   K4:PTT  K5:Send");

    PR_NOTICE("HUD skeleton built (320x240, 4 zones)");
    return OPRT_OK;
}

static const char *platform_name(platform_t p)
{
    switch (p) {
    case PLAT_KIROCREW:
        return "KiroCrew";
    case PLAT_KIROCLI:
        return "Kiro CLI";
    case PLAT_CODEX:
        return "Codex";
    default:
        return "?";
    }
}

static const char *slot_state_str(const showcase_slot_t *s)
{
    if (!s->occupied) {
        return "Empty";
    }
    switch (s->status) {
    case SLOT_IDLE:
        return "Idle";
    case SLOT_RUN:
        return "Run";
    case SLOT_CONFIRM:
        return "Confirm?";
    case SLOT_ERROR:
        return "Error";
    default:
        return "-";
    }
}

static uint32_t slot_state_color(const showcase_slot_t *s)
{
    if (!s->occupied) {
        return COL_MUTED;
    }
    switch (s->status) {
    case SLOT_RUN:
        return COL_GREEN;
    case SLOT_CONFIRM:
        return COL_AMBER;
    case SLOT_ERROR:
        return COL_RED;
    case SLOT_IDLE:
    default:
        return COL_TEXT;
    }
}

void ak_hud_render(const showcase_view_t *view)
{
    char buf[64];

    /* status bar */
    snprintf(buf, sizeof(buf), "%s  " LV_SYMBOL_BULLET "  %s  " LV_SYMBOL_BULLET "  eff:%s",
             platform_name(view->platform), view->mode, view->effort);
    lv_label_set_text(s_hud.bar_left, buf);

    snprintf(buf, sizeof(buf), "%s  v%s", view->connected ? "ON" : "OFF", view->fw);
    lv_label_set_text(s_hud.bar_right, buf);
    lv_obj_set_style_text_color(s_hud.bar_right, lv_color_hex(view->connected ? COL_GREEN : COL_MUTED), LV_PART_MAIN);

    /* slot cards */
    for (int i = 0; i < SHOWCASE_SLOT_COUNT; i++) {
        const showcase_slot_t *s       = &view->slots[i];
        bool                   focused = (view->focus == i);
        uint32_t               border  = focused ? COL_CYAN : COL_BORDER;
        lv_obj_set_style_border_color(s_hud.slot_card[i], lv_color_hex(border), LV_PART_MAIN);
        lv_obj_set_style_border_width(s_hud.slot_card[i], focused ? 2 : 1, LV_PART_MAIN);

        snprintf(buf, sizeof(buf), "%s%s K%d", focused ? ">" : " ", "", i + 1);
        /* name line: "K1  Kilo" */
        snprintf(buf, sizeof(buf), "%sK%d %s", focused ? ">" : " ", i + 1, s->occupied ? s->name : "-");
        lv_label_set_text(s_hud.slot_name[i], buf);
        lv_obj_set_style_text_color(s_hud.slot_name[i], lv_color_hex(focused ? COL_CYAN : COL_TEXT), LV_PART_MAIN);

        lv_label_set_text(s_hud.slot_state[i], slot_state_str(s));
        lv_obj_set_style_text_color(s_hud.slot_state[i], lv_color_hex(slot_state_color(s)), LV_PART_MAIN);

        lv_label_set_text(s_hud.slot_detail[i], s->occupied && s->detail[0] ? s->detail : "");
    }

    /* context */
    snprintf(buf, sizeof(buf), "> %s", view->context[0] ? view->context : "-");
    lv_label_set_text(s_hud.ctx_line, buf);

    if (view->last_msg[0]) {
        snprintf(buf, sizeof(buf), "last: \"%s\"", view->last_msg);
        lv_label_set_text(s_hud.ctx_last, buf);
    } else {
        lv_label_set_text(s_hud.ctx_last, "");
    }

    if (view->recording) {
        lv_label_set_text(s_hud.ctx_wave, "[ listening " LV_SYMBOL_AUDIO " " LV_SYMBOL_AUDIO " ]");
    } else if (view->awaiting_confirm) {
        lv_label_set_text(s_hud.ctx_wave, LV_SYMBOL_WARNING " press " LV_SYMBOL_RIGHT " to approve");
    } else {
        lv_label_set_text(s_hud.ctx_wave, "");
    }
}
