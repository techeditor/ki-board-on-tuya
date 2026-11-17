/**
 * @file ak_fonttest.c
 * @brief Hardware-test "Font" view. Draws the shared sample text with one of the
 *        candidate subset fonts so they can be compared on the real panel.
 */

#include <stdio.h>
#include <string.h>

#include "ak_fonttest.h"

#include "ak_font.h"
#include "ak_font_data.h"
#include "ak_gfx.h"
#include "ak_i18n.h"
#include "ak_statuslcd.h"

#define FT_TITLE_H 22U
#define FT_BODY_Y  26U
#define FT_FOOT_Y  207U
#define FT_MARGIN  4U

unsigned ak_fonttest_count(void)
{
    return AK_FONT_TEST_COUNT;
}

static unsigned ak_fonttest_norm(int idx)
{
    int n = (int)AK_FONT_TEST_COUNT;

    idx %= n;
    return (unsigned)(idx < 0 ? idx + n : idx);
}

static const ak_font_t *ak_fonttest_font(int idx)
{
    return ak_font_test_set[ak_fonttest_norm(idx)];
}

const char *ak_fonttest_name(int idx)
{
    return ak_fonttest_font(idx)->name;
}

/* Copy the longest prefix of s that fits max_w into out (at least one glyph, so
 * the caller always makes progress). Returns the unconsumed rest. */
static const char *ak_fonttest_take(const ak_font_t *font, const char *s, uint16_t max_w,
                                    char *out, size_t cap)
{
    const char *cur = s;
    uint32_t    width = 0U;
    size_t      used = 0U;

    for (;;) {
        const char *next = cur;
        uint32_t    cp = ak_utf8_next(&next);
        if (cp == 0U) {
            break;
        }
        int idx = ak_font_find(font, cp);
        uint32_t adv = idx >= 0 ? font->adv[idx] : 0U;
        size_t   len = (size_t)(next - cur);
        if ((width + adv > max_w && used > 0U) || used + len + 1U > cap) {
            break;
        }
        memcpy(out + used, cur, len);
        used += len;
        width += adv;
        cur = next;
    }
    out[used] = '\0';
    return cur;
}

void ak_fonttest_draw(int idx)
{
    const ak_font_t *font = ak_fonttest_font(idx);
    char     line[96];
    char     foot[48];
    uint16_t y = FT_BODY_Y;

    if (ak_statuslcd_flush_busy()) {
        (void)ak_statuslcd_flush_wait();
    }
    if (!ak_statuslcd_ready()) {
        return;
    }

    ak_gfx_clear(AK_COL_BLACK);
    ak_gfx_fill_rect(0U, 0U, AK_STATUSLCD_W, FT_TITLE_H, AK_COL_PURPLE);
    ak_gfx_text(6U, 7U, ak_tr(AK_STR_FONT_TITLE), AK_COL_BLACK, AK_COL_PURPLE, 1);

    for (unsigned l = 0U; l < AK_FONT_SAMPLE_LINES; l++) {
        const char *rest = ak_font_sample[l];
        while (*rest != '\0') {
            if ((uint32_t)y + font->px > FT_FOOT_Y) {
                break;
            }
            rest = ak_fonttest_take(font, rest, (uint16_t)(AK_STATUSLCD_W - 2U * FT_MARGIN), line, sizeof(line));
            (void)ak_font_draw_text(font, FT_MARGIN, y, line, AK_COL_WHITE, AK_COL_BLACK, AK_STATUSLCD_W, ak_gfx_fill_rect);
            y = (uint16_t)(y + font->line);
        }
    }

    ak_gfx_fill_rect(0U, FT_FOOT_Y, AK_STATUSLCD_W, (uint16_t)(AK_STATUSLCD_H - FT_FOOT_Y), AK_COL_BLACK);
    ak_gfx_fill_rect(0U, FT_FOOT_Y, AK_STATUSLCD_W, 1U, AK_COL_PURPLE);
    (void)snprintf(foot, sizeof(foot), "%u/%u %s", ak_fonttest_norm(idx) + 1U, (unsigned)AK_FONT_TEST_COUNT, font->name);
    ak_gfx_text_thin(4U, 211U, foot, AK_COL_CYAN, AK_COL_BLACK, 1);
    ak_gfx_text_thin(4U, 225U, ak_tr(AK_STR_FONT_HINT), AK_COL_GRAY, AK_COL_BLACK, 1);
    ak_statuslcd_flush();
}
