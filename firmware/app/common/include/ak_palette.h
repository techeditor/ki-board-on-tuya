/**
 * @file ak_palette.h
 * @brief LCD colours by use.
 *
 * Drawing code uses only the AK_UI_* / AK_STATE_* names below, never a raw
 * RGB565 value or AK_COL_* (the host tests check the sources). The values
 * come from two sets:
 *   - the Kiro brand palette (Brand Guidelines v2.0, Color): six colours;
 *   - functional colours with a fixed meaning (FR-09 session / key state,
 *     warnings, errors), allowed only on small areas: dots, key tags, a 2 px
 *     rule, a frame, a status bar notice, the low battery fill.
 * Text is drawn only in the brand's small-text pairs (White on Purple; White,
 * Light Purple, Light Grey on Black; Black, Dark Grey on White) or in the
 * functional pairs listed in AK_PALETTE_PAIRS; test_palette checks every pair.
 */

#ifndef AK_PALETTE_H
#define AK_PALETTE_H

#include <stdint.h>

#define AK_PAL_RGB(r, g, b) ((uint16_t)((((r)&0xF8U) << 8) | (((g)&0xFCU) << 3) | (((b)&0xF8U) >> 3)))

/* ---- Kiro brand palette ---------------------------------------------------- */
#define AK_KIRO_PURPLE  AK_PAL_RGB(0x8EU, 0x48U, 0xFFU) /* 0x8A5F */
#define AK_KIRO_LPURPLE AK_PAL_RGB(0xC6U, 0xA0U, 0xFFU) /* 0xC51F */
#define AK_KIRO_BLACK   ((uint16_t)0x0000U)
#define AK_KIRO_WHITE   ((uint16_t)0xFFFFU)
#define AK_KIRO_DGREY   AK_PAL_RGB(0x64U, 0x64U, 0x64U) /* 0x632C */
#define AK_KIRO_LGREY   AK_PAL_RGB(0x95U, 0x95U, 0x95U) /* 0x94B2 */

/* ---- functional colours (small areas only) ---------------------------------- */
#define AK_FN_RED    ((uint16_t)0xF800U)
#define AK_FN_ORANGE ((uint16_t)0xFD20U)
#define AK_FN_GREEN  ((uint16_t)0x07E0U)
#define AK_FN_YELLOW ((uint16_t)0xFFE0U)

/* ---- page ------------------------------------------------------------------- */
#define AK_UI_BG          AK_KIRO_BLACK
#define AK_UI_TEXT        AK_KIRO_WHITE   /* body text */
#define AK_UI_TEXT_DIM    AK_KIRO_LGREY   /* secondary: idle rows, 2nd hint, time-outs, states */
#define AK_UI_TEXT_ACCENT AK_KIRO_LPURPLE /* values, 1st hint, page headings, bound names, notes */
#define AK_UI_FRAME       AK_KIRO_DGREY   /* idle row / card / button frames */
#define AK_UI_RULE        AK_KIRO_PURPLE  /* status bar and footer rule */

/* Title bar of a page; a warning dialog uses a black bar with a 2 px rule in
 * its functional colour (AK_UI_WARN_*) and the same rule colour on its key box. */
#define AK_UI_TITLE_BG    AK_KIRO_PURPLE
#define AK_UI_TITLE_FG    AK_KIRO_WHITE
#define AK_UI_NO_WARN     AK_KIRO_PURPLE /* ak_title_bar / ak_draw_dialog: not a warning */
#define AK_UI_WARN_DANGER AK_FN_RED      /* reset network, clear BLE pairing */
#define AK_UI_WARN_DOUBLE AK_FN_YELLOW   /* unbind & re-pair (double confirm, K6 flashes yellow) */
#define AK_UI_WARN_BG     AK_KIRO_BLACK
#define AK_UI_KEYS_FRAME  AK_KIRO_PURPLE /* dialog key box when not a warning */

/* Header: Purple while the Companion is connected, black (grey text, dark
 * grey rule) while it is not. */
#define AK_UI_HEADER_BG       AK_KIRO_PURPLE
#define AK_UI_HEADER_FG       AK_KIRO_WHITE
#define AK_UI_HEADER_OFF_BG   AK_KIRO_BLACK
#define AK_UI_HEADER_OFF_FG   AK_KIRO_LGREY
#define AK_UI_HEADER_OFF_RULE AK_KIRO_DGREY

/* Battery gauge: white fill, red below 20 % (functional). */
#define AK_UI_BAT_SHELL   AK_KIRO_WHITE
#define AK_UI_BAT_EMPTY   AK_KIRO_BLACK
#define AK_UI_BAT_FILL    AK_KIRO_WHITE
#define AK_UI_BAT_FILL_FG AK_KIRO_BLACK
#define AK_UI_BAT_LOW     AK_FN_RED
#define AK_UI_BAT_LOW_FG  AK_KIRO_WHITE
#define AK_UI_BAT_DIGIT   AK_KIRO_WHITE /* digits over the empty part */

/* Selection and editing (list rows, menu items, buttons, bind cards). */
#define AK_UI_SEL_BG  AK_KIRO_PURPLE
#define AK_UI_SEL_FG  AK_KIRO_WHITE
#define AK_UI_EDIT_BG AK_KIRO_WHITE /* a row being edited is inverted */
#define AK_UI_EDIT_FG AK_KIRO_BLACK

/* Chat: agent bubbles are a Purple outline on black, the "new" pill Purple. */
#define AK_UI_BUBBLE_FRAME AK_KIRO_PURPLE
#define AK_UI_PILL_BG      AK_KIRO_PURPLE
#define AK_UI_PILL_FG      AK_KIRO_WHITE

/* Scroll bar, OTA progress. */
#define AK_UI_BAR_TRACK AK_KIRO_DGREY
#define AK_UI_BAR_THUMB AK_KIRO_LGREY
#define AK_UI_PROGRESS  AK_KIRO_PURPLE

/* Key tags (menus, bind page): White with Black text; functional roles keep
 * their key-LED colour; the voice item is Purple. */
#define AK_UI_TAG_BG        AK_KIRO_WHITE
#define AK_UI_TAG_FG        AK_KIRO_BLACK
#define AK_UI_TAG_CONFIRM   AK_FN_GREEN
#define AK_UI_TAG_CANCEL    AK_FN_RED
#define AK_UI_TAG_ATTENTION AK_FN_YELLOW
#define AK_UI_TAG_FN_FG     AK_KIRO_BLACK
#define AK_UI_TAG_VOICE     AK_KIRO_PURPLE
#define AK_UI_TAG_VOICE_FG  AK_KIRO_WHITE

/* Hardware test key cells: held Purple, idle a dark grey outline. */
#define AK_UI_CELL_HELD    AK_KIRO_PURPLE
#define AK_UI_CELL_HELD_FG AK_KIRO_WHITE
#define AK_UI_CELL_FG      AK_KIRO_LGREY

/* Sidebar cards: black with a dark grey frame, the selected one Purple; the
 * ghost is always White with Black eyes (brand rule); the session state is
 * an 8 px dot in its FR-09 colour. */
#define AK_UI_CARD_BG     AK_KIRO_BLACK
#define AK_UI_CARD_SEL_BG AK_KIRO_PURPLE
#define AK_UI_CARD_FG     AK_KIRO_LGREY /* label of an idle card */
#define AK_UI_CARD_SEL_FG AK_KIRO_WHITE
#define AK_UI_GHOST_BODY  AK_KIRO_WHITE
#define AK_UI_GHOST_EYES  AK_KIRO_BLACK

/* Session state (FR-09), used for the card dot only. */
#define AK_STATE_UNBOUND AK_KIRO_DGREY
#define AK_STATE_IDLE    AK_FN_GREEN
#define AK_STATE_RUNNING AK_FN_RED
#define AK_STATE_WAITING AK_FN_ORANGE
#define AK_STATE_INFO    AK_KIRO_LPURPLE

/* Status bar messages: all White except warnings and errors (functional).
 * The kinds are kept apart so one line here can restyle a kind. */
#define AK_UI_ST_INFO   AK_KIRO_WHITE /* progress, notes, hints */
#define AK_UI_ST_OK     AK_KIRO_WHITE /* done, connected, ready */
#define AK_UI_ST_NOTE   AK_KIRO_WHITE /* cancelled, disconnected, no speech */
#define AK_UI_ST_ACTIVE AK_KIRO_WHITE /* listening, recognising, dictating */
#define AK_UI_ST_WARN   AK_FN_YELLOW  /* needs cloud / Companion, request waiting */
#define AK_UI_ST_ERROR  AK_FN_RED     /* failed */

/* Boot splash (no custom image): the lockup bitmap White on Purple, its
 * holes (ghost eyes) Black, and the product name below it. */
#define AK_UI_SPLASH_BG   AK_KIRO_PURPLE
#define AK_UI_SPLASH_INK  AK_KIRO_WHITE
#define AK_UI_SPLASH_EYES AK_KIRO_BLACK

#ifdef AK_PALETTE_PAIRS
/* Every text colour on every background it is drawn on (host test). */
typedef struct {
    uint16_t    fg;
    uint16_t    bg;
    const char *what;
} ak_palette_pair_t;

static const ak_palette_pair_t k_ak_palette_pairs[] = {
    {AK_UI_TEXT, AK_UI_BG, "body text"},
    {AK_UI_TEXT_DIM, AK_UI_BG, "secondary text"},
    {AK_UI_TEXT_ACCENT, AK_UI_BG, "accent text"},
    {AK_UI_TITLE_FG, AK_UI_TITLE_BG, "title bar"},
    {AK_UI_TITLE_FG, AK_UI_WARN_BG, "warning title bar"},
    {AK_UI_HEADER_FG, AK_UI_HEADER_BG, "header"},
    {AK_UI_HEADER_OFF_FG, AK_UI_HEADER_OFF_BG, "header, Companion not connected"},
    {AK_UI_BAT_FILL_FG, AK_UI_BAT_FILL, "battery digit on the fill"},
    {AK_UI_BAT_LOW_FG, AK_UI_BAT_LOW, "battery digit on the low fill"},
    {AK_UI_BAT_DIGIT, AK_UI_BAT_EMPTY, "battery digit on the empty part"},
    {AK_UI_SEL_FG, AK_UI_SEL_BG, "selected row"},
    {AK_UI_EDIT_FG, AK_UI_EDIT_BG, "row being edited"},
    {AK_UI_PILL_FG, AK_UI_PILL_BG, "new-content pill"},
    {AK_UI_TAG_FG, AK_UI_TAG_BG, "key tag"},
    {AK_UI_TAG_FN_FG, AK_UI_TAG_CONFIRM, "confirm key tag"},
    {AK_UI_TAG_FN_FG, AK_UI_TAG_CANCEL, "cancel key tag"},
    {AK_UI_TAG_FN_FG, AK_UI_TAG_ATTENTION, "attention key tag"},
    {AK_UI_TAG_VOICE_FG, AK_UI_TAG_VOICE, "voice key tag"},
    {AK_UI_CELL_HELD_FG, AK_UI_CELL_HELD, "held test key"},
    {AK_UI_CELL_FG, AK_UI_BG, "idle test key"},
    {AK_UI_CARD_FG, AK_UI_CARD_BG, "idle card label"},
    {AK_UI_CARD_SEL_FG, AK_UI_CARD_SEL_BG, "selected card label"},
    {AK_UI_ST_INFO, AK_UI_BG, "status: info"},
    {AK_UI_ST_OK, AK_UI_BG, "status: ok"},
    {AK_UI_ST_NOTE, AK_UI_BG, "status: note"},
    {AK_UI_ST_ACTIVE, AK_UI_BG, "status: active"},
    {AK_UI_ST_WARN, AK_UI_BG, "status: warning"},
    {AK_UI_ST_ERROR, AK_UI_BG, "status: error"},
    {AK_UI_SPLASH_INK, AK_UI_SPLASH_BG, "splash name"},
};

/* Non-text colours (fills, frames, rules, dots): each must be a brand or a
 * functional colour. */
static const uint16_t k_ak_palette_fills[] = {
    AK_UI_FRAME,      AK_UI_RULE,       AK_UI_NO_WARN,         AK_UI_WARN_DANGER,  AK_UI_WARN_DOUBLE,
    AK_UI_KEYS_FRAME, AK_UI_BAT_SHELL,  AK_UI_HEADER_OFF_RULE, AK_UI_BUBBLE_FRAME, AK_UI_BAR_TRACK,
    AK_UI_BAR_THUMB,  AK_UI_PROGRESS,   AK_UI_GHOST_BODY,      AK_UI_GHOST_EYES,   AK_STATE_UNBOUND,
    AK_STATE_IDLE,    AK_STATE_RUNNING, AK_STATE_WAITING,      AK_STATE_INFO,      AK_UI_SPLASH_EYES,
};
#endif /* AK_PALETTE_PAIRS */

#endif /* AK_PALETTE_H */
