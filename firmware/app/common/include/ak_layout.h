/**
 * @file ak_layout.h
 * @brief LCD style tokens and page geometry.
 *
 * One style at every font size: only the heights derived from the font cell
 * F (12 / 14 / 16 px, ak_text_line_h()) and the number of rows on a page
 * change; frames, radii, margins, highlight and scroll bar are the same.
 * Shared by src/tuya_app_main.c, ak_statuslcd.c and the host
 * tests, which check at every size that boxes, text and rules do not
 * overlap and that centred text is off by at most 1 px.
 *
 * Text is placed by box: ak_text_vtop(by, bh) (ak_text.h) gives the cell top
 * that centres the font's ink band in the box by..by+bh-1.
 */

#ifndef AK_LAYOUT_H_INCLUDED
#define AK_LAYOUT_H_INCLUDED

#include <stdint.h>

#include "ak_text.h"

#define AK_LAYOUT_W 320U /* the panel; ak_statuslcd.h takes AK_STATUSLCD_W/H from here */
#define AK_LAYOUT_H 240U

/* ---- tokens ---------------------------------------------------------------- */
#define LCD_F       ((uint16_t)ak_text_line_h()) /* font cell height */
#define LCD_L       ((uint16_t)(LCD_F + 2U))     /* line pitch of running text */
#define LCD_B       ((uint16_t)(LCD_F + 4U))     /* row / item / button box height */
#define LCD_R       ((uint16_t)(LCD_F + 6U))     /* row pitch: boxes 2 px apart */
#define LCD_M       8U                           /* page margin, left and right */
#define LCD_PAD     6U                           /* text inside a box, left / right */
#define LCD_ROW_PAD 8U                           /* text inside a list row */
#define LCD_R_TAG   3U                           /* corner radius: key tags, small chips */
#define LCD_R_ROW   4U                           /* list rows, menu items, buttons, bars */
#define LCD_R_PANEL 5U                           /* cards: sidebar slots, bubbles, key boxes */
#define LCD_GUTTER  8U                           /* right column kept for the scroll bar */
#define LCD_BAR_X   311U                         /* scroll bar: 3 px thumb on a 1 px track */
#define LCD_BAR_W   3U
#define LCD_BAR_MIN 8U
#define LCD_TITLE_H 22U

/* Status bar: a 1 px rule, then the text centred in the rest. */
#define LCD_STATUS_BAR_H ((uint16_t)(LCD_F + 4U))
#define LCD_STATUS_BAR_Y ((uint16_t)(AK_LAYOUT_H - LCD_STATUS_BAR_H))

/* Footer of full-screen pages: a 1 px rule, then two hint lines L apart. */
#define LCD_FOOTER_Y  ((uint16_t)(AK_LAYOUT_H - (2U * LCD_L + 3U)))
#define LCD_FOOTER_H  ((uint16_t)(AK_LAYOUT_H - LCD_FOOTER_Y))
#define LCD_HINT_Y(i) ((uint16_t)(LCD_FOOTER_Y + 2U + (uint16_t)(i)*LCD_L)) /* box top, height L */

/* A hint line right above the status bar (menus, ③). */
#define LCD_HINT_ABOVE_STATUS_Y ((uint16_t)(LCD_STATUS_BAR_Y - 2U - LCD_L))

/* List rows (settings, Saved Wi-Fi, credentials). */
#define LCD_LIST_X ((uint16_t)LCD_M)
#define LCD_LIST_W ((uint16_t)(AK_LAYOUT_W - 2U * LCD_M - LCD_GUTTER + 2U)) /* 298 */

/* Main page (sidebar + chat column). */
#define LCD_CHAT_X      70U
#define LCD_CHAT_TEXT_W 230U /* wrapped history; a bubble adds 4 px each side */
#define LCD_CHAT_ROW1_Y 24U  /* agent / title row box, height L */
#define LCD_CHAT_ROW2_Y ((uint16_t)(24U + LCD_L))
#define LCD_HISTORY_TOP ((uint16_t)(24U + 2U * LCD_L + 4U))
#define LCD_BUBBLE_PAD  4U
#define LCD_MSG_GAP     4U

/* Menu: 3 context lines, then the items, the hint above the status bar. */
#define LCD_MENU_CTX_Y  ((uint16_t)(LCD_TITLE_H + 4U))
#define LCD_MENU_ITEM_Y ((uint16_t)(LCD_MENU_CTX_Y + 3U * LCD_L + 4U))

/* ③ running text: from below the title to above the hint line. */
#define LCD_TEXT_TOP       ((uint16_t)(LCD_TITLE_H + 4U))
#define LCD_CONFIRM_TEXT_W ((uint16_t)(AK_LAYOUT_W - 2U * LCD_M - LCD_GUTTER)) /* 296: clear of the scroll bar */
#define LCD_TEXT_ROWS      ((uint16_t)((LCD_STATUS_BAR_Y - 4U - LCD_L - LCD_TEXT_TOP) / LCD_L))

/* Confirm dialogs: body from below the title, the key box above the time-out
 * line, the time-out line above the status bar. */
#define LCD_DLG_TOP       ((uint16_t)(LCD_TITLE_H + 6U))
#define LCD_DLG_KEYS_H    ((uint16_t)(LCD_B + 4U))
#define LCD_DLG_TIMEOUT_Y ((uint16_t)(LCD_STATUS_BAR_Y - 4U - LCD_L))
#define LCD_DLG_KEYS_Y    ((uint16_t)(LCD_DLG_TIMEOUT_Y - 4U - LCD_DLG_KEYS_H))

/* Pairing confirm: two buttons above the auto-cancel line, above the footer. */
#define LCD_BTN_H      ((uint16_t)(LCD_B + 8U))
#define LCD_BTN_W      ((uint16_t)((AK_LAYOUT_W - 2U * LCD_M - 8U) / 2U))
#define LCD_BTN_NOTE_Y ((uint16_t)(LCD_FOOTER_Y - 4U - LCD_L))
#define LCD_BTN_Y      ((uint16_t)(LCD_BTN_NOTE_Y - 6U - LCD_BTN_H))

/* Sidebar: three cards between the header and the status bar, 2 px apart. */
#define LCD_SIDEBAR_W     64U
#define LCD_CARD_X        1U
#define LCD_CARD_W        62U
#define LCD_CARD_TOP      24U
#define LCD_CARD_GHOST_DY 6U  /* ghost top below the card top, at rest */
#define LCD_CARD_LABEL_W  58U /* label box, 2 px inside the card */
#define LCD_CARD_DOT      8U  /* session state dot (FR-09 colour), top-right */
#define LCD_CARD_DOT_X    ((uint16_t)(LCD_CARD_X + LCD_CARD_W - 5U - LCD_CARD_DOT))
#define LCD_CARD_DOT_DY   5U

static inline uint16_t lcd_card_h(void)
{
    return (uint16_t)((LCD_STATUS_BAR_Y - 2U - LCD_CARD_TOP - 4U) / 3U);
}

static inline uint16_t lcd_card_y(unsigned i)
{
    return (uint16_t)(LCD_CARD_TOP + i * (lcd_card_h() + 2U));
}

static inline uint16_t lcd_card_hi(unsigned i) /* the last card takes the rest */
{
    return i < 2U ? lcd_card_h() : (uint16_t)(LCD_STATUS_BAR_Y - 2U - lcd_card_y(i));
}

/* List geometry: as many rows R apart as fit between the title and the
 * footer, the spare space split above and below. */
static inline uint16_t lcd_list_rows(void)
{
    uint16_t top = (uint16_t)(LCD_TITLE_H + 2U), bottom = (uint16_t)(LCD_FOOTER_Y - 2U);
    return (uint16_t)((bottom - top + 2U) / LCD_R);
}

static inline uint16_t lcd_list_y0(void)
{
    uint16_t top = (uint16_t)(LCD_TITLE_H + 2U), bottom = (uint16_t)(LCD_FOOTER_Y - 2U);
    return (uint16_t)(top + ((bottom - top) - (lcd_list_rows() * LCD_R - 2U)) / 2U);
}

/* Bind session: three cards between the title and the footer. */
#define LCD_BIND_TOP ((uint16_t)(LCD_TITLE_H + 4U))
static inline uint16_t lcd_bind_card_h(void)
{
    return (uint16_t)((LCD_FOOTER_Y - 4U - LCD_BIND_TOP - 8U) / 3U);
}

/**
 * @brief Row inset of a radius-r corner for corner row j (0..r-1): pixel (i, j)
 *        of the r x r corner square is inside when (2r-2i-1)^2 + (2r-2j-1)^2 <= 4r^2.
 *        r 3 -> 1,0,0; r 4 -> 2,1,0,0; r 5 -> 3,1,1,0,0.
 */
static inline uint16_t lcd_round_inset(uint16_t r, uint16_t j)
{
    uint32_t dy = (uint32_t)(2U * r - 2U * j - 1U);
    uint16_t i  = 0U;
    while (i < r && (uint32_t)(2U * r - 2U * i - 1U) * (2U * r - 2U * i - 1U) + dy * dy > 4U * (uint32_t)r * r) {
        i++;
    }
    return i;
}

#endif /* AK_LAYOUT_H_INCLUDED */
