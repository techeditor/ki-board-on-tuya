/**
 * @file ak_font.c
 * @brief Subset bitmap font engine. See ak_font.h.
 */

#include "ak_font.h"

#include <stddef.h>

#define AK_FONT_MISSING_CP 0x7FU

uint32_t ak_utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t cp;
    unsigned need;

    if (p[0] == 0U) {
        return 0U;
    }
    if (p[0] < 0x80U) {
        *s += 1;
        return p[0];
    }
    if (p[0] >= 0xC2U && p[0] <= 0xDFU) {
        cp   = p[0] & 0x1FU;
        need = 1U;
    } else if (p[0] >= 0xE0U && p[0] <= 0xEFU) {
        cp   = p[0] & 0x0FU;
        need = 2U;
    } else if (p[0] >= 0xF0U && p[0] <= 0xF4U) {
        cp   = p[0] & 0x07U;
        need = 3U;
    } else {
        *s += 1; /* stray continuation byte or invalid lead */
        return 0xFFFDU;
    }
    for (unsigned i = 1U; i <= need; i++) {
        if ((p[i] & 0xC0U) != 0x80U) { /* also stops at NUL: never read past the terminator */
            *s += 1;
            return 0xFFFDU;
        }
        cp = (cp << 6) | (p[i] & 0x3FU);
    }
    *s += need + 1U;
    return cp;
}

static int ak_font_search(const ak_font_t *font, uint32_t cp)
{
    int lo = 0;
    int hi = (int)font->count - 1;

    if (cp > 0xFFFFU) {
        return -1;
    }
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (font->codes[mid] == cp) {
            return mid;
        }
        if (font->codes[mid] < cp) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return -1;
}

int ak_font_find(const ak_font_t *font, uint32_t cp)
{
    int idx;

    if (font == NULL) {
        return -1;
    }
    if (cp < 0x20U) {
        cp = 0x20U;
    }
    idx = ak_font_search(font, cp);
    return idx >= 0 ? idx : ak_font_search(font, AK_FONT_MISSING_CP);
}

uint8_t ak_font_level(const ak_font_t *font, int idx, uint8_t x, uint8_t y)
{
    if (font == NULL || idx < 0 || idx >= (int)font->count || x >= font->adv[idx] || y >= font->px) {
        return 0U;
    }
    uint32_t bit  = ((uint32_t)y * font->adv[idx] + x) * font->bpp;
    uint8_t  byte = font->data[font->offs[idx] + (bit >> 3)];
    uint8_t  sh   = (uint8_t)(8U - font->bpp - (bit & 7U));

    return (uint8_t)((byte >> sh) & ((1U << font->bpp) - 1U));
}

uint16_t ak_font_blend565(uint16_t fg, uint16_t bg, uint8_t level, uint8_t max_level)
{
    if (max_level == 0U || level >= max_level) {
        return level == 0U ? bg : fg;
    }
    if (level == 0U) {
        return bg;
    }
    uint32_t r = ((uint32_t)((fg >> 11) & 0x1FU) * level + (uint32_t)((bg >> 11) & 0x1FU) * (max_level - level) + max_level / 2U) / max_level;
    uint32_t g = ((uint32_t)((fg >> 5) & 0x3FU) * level + (uint32_t)((bg >> 5) & 0x3FU) * (max_level - level) + max_level / 2U) / max_level;
    uint32_t b = ((uint32_t)(fg & 0x1FU) * level + (uint32_t)(bg & 0x1FU) * (max_level - level) + max_level / 2U) / max_level;

    return (uint16_t)((r << 11) | (g << 5) | b);
}

uint16_t ak_font_text_width(const ak_font_t *font, const char *s)
{
    uint32_t w = 0U;
    uint32_t cp;

    if (font == NULL || s == NULL) {
        return 0U;
    }
    while ((cp = ak_utf8_next(&s)) != 0U) {
        int idx = ak_font_find(font, cp);
        if (idx >= 0) {
            w += font->adv[idx];
        }
    }
    return (uint16_t)(w > 0xFFFFU ? 0xFFFFU : w);
}

uint16_t ak_font_draw_text(const ak_font_t *font, uint16_t x, uint16_t y, const char *s,
                           uint16_t fg, uint16_t bg, uint16_t x_limit, ak_font_rect_fn rect)
{
    uint32_t cp;
    const uint8_t max_level = font != NULL ? (uint8_t)((1U << font->bpp) - 1U) : 0U;

    if (font == NULL || s == NULL || rect == NULL) {
        return x;
    }
    while ((cp = ak_utf8_next(&s)) != 0U) {
        int idx = ak_font_find(font, cp);
        if (idx < 0) {
            continue;
        }
        uint8_t w = font->adv[idx];
        if ((uint32_t)x + w > x_limit) {
            break;
        }
        rect(x, y, w, font->px, bg);
        for (uint8_t row = 0U; row < font->px; row++) {
            uint8_t col = 0U;
            while (col < w) {
                uint8_t level = ak_font_level(font, idx, col, row);
                uint8_t run   = 1U;
                if (level == 0U) {
                    col++;
                    continue;
                }
                while ((uint8_t)(col + run) < w && ak_font_level(font, idx, (uint8_t)(col + run), row) == level) {
                    run++;
                }
                rect((uint16_t)(x + col), (uint16_t)(y + row), run, 1U, ak_font_blend565(fg, bg, level, max_level));
                col = (uint8_t)(col + run);
            }
        }
        x = (uint16_t)(x + w);
    }
    return x;
}
