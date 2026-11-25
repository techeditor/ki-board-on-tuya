/**
 * @file ak_inputbar_draw.c
 * @brief Voice input strip drawing. See ak_inputbar_draw.h.
 */

#include "ak_inputbar_draw.h"

#include <string.h>

#include "ak_gfx.h"
#include "ak_inputbar.h"
#include "ak_layout.h"
#include "ak_palette.h"
#include "ak_text.h"

#define INPUT_LINE_BYTES 96U /* one wrapped line: 226 px is at most ~37 ASCII or 18 CJK characters at 12 px */

void ak_inputbar_draw(const char *text, ak_scroll_t *scroll)
{
    uint16_t lines = ak_inputbar_lines(text);
    uint16_t rows  = ak_inputbar_rows(lines);
    uint16_t top   = ak_inputbar_top(text);
    uint16_t x     = (uint16_t)(LCD_SIDEBAR_W + 2U);
    uint16_t w     = (uint16_t)(LCD_CHAT_TEXT_W + 2U * LCD_BUBBLE_PAD);
    uint16_t h     = ak_inputbar_height(text);
    uint16_t tw    = ak_inputbar_text_w();

    /* Frame in the accent colour: it is the one thing on the page the user types into. */
    ak_gfx_round_frame(x, top, w, h, LCD_R_ROW, 1U, AK_UI_TEXT_ACCENT, AK_UI_BG);

    uint16_t max_first = lines > rows ? (uint16_t)(lines - rows) : 0U;
    ak_scroll_update(scroll, max_first);

    const char *p = text != NULL ? text : "";
    for (uint16_t i = 0U; i < scroll->first && *p != '\0'; i++) {
        while (*p == ' ') {
            p++;
        }
        size_t len = ak_text_wrap(p, tw);
        p += len > 0U ? len : 1U;
    }
    uint16_t shown = 0U;
    while (*p != '\0' && shown < rows) {
        char line[INPUT_LINE_BYTES];
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        size_t len  = ak_text_wrap(p, tw);
        size_t take = len < sizeof(line) - 1U ? len : sizeof(line) - 1U;
        while (take > 0U && ((unsigned char)p[take] & 0xC0U) == 0x80U) {
            take--;
        }
        memcpy(line, p, take);
        line[take] = '\0';
        while (take > 0U && line[take - 1U] == ' ') {
            line[--take] = '\0';
        }
        (void)ak_gfx_text_box((uint16_t)(x + 1U + LCD_PAD), (uint16_t)(top + 2U + shown * LCD_L), LCD_L, line,
                              AK_UI_TEXT, AK_UI_BG);
        p += len > 0U ? len : 1U;
        shown++;
    }

    /* Scroll bar (FR-17 style), only when the text is longer than the strip. */
    uint16_t bar_top = 0U;
    uint16_t bar_h   = 0U;
    uint16_t track   = (uint16_t)(h - 4U);
    if (ak_scroll_bar(lines, shown, scroll->first, track, LCD_BAR_MIN, &bar_top, &bar_h)) {
        ak_gfx_fill_rect((uint16_t)(x + w - 5U), (uint16_t)(top + 2U), 1U, track, AK_UI_BAR_TRACK);
        ak_gfx_round_rect((uint16_t)(x + w - 6U), (uint16_t)(top + 2U + bar_top), LCD_BAR_W, bar_h, 2U,
                          AK_UI_BAR_THUMB);
    }
}
