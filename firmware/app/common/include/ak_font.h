/**
 * @file ak_font.h
 * @brief Subset bitmap font engine for the status LCD (UTF-8, 1/2/4 bpp).
 *
 * Pure C with no SDK dependency, so it is unit-tested on the host. Pixels leave
 * through a rectangle callback (ak_gfx_fill_rect on the board, a mock in tests).
 *
 * Font layout (all tables are const, so they stay in XIP flash, not RAM):
 *   codes[count]  sorted ascending BMP code points
 *   adv[count]    advance == bitmap width in px (6/12 for fixed fonts, per-glyph
 *                 for proportional ones)
 *   offs[count]   byte offset of the glyph bitmap in data[]
 *   data[]        row-major, MSB first, bpp bits per pixel, each glyph padded to
 *                 a whole byte; glyph height is font->px
 * Code point 0x7F is the "missing glyph" box; unknown code points use it.
 */

#ifndef AK_FONT_ENGINE_H
#define AK_FONT_ENGINE_H

#include <stdint.h>

typedef struct {
    const char     *name;
    uint8_t         px;    /**< cell height in px */
    uint8_t         bpp;   /**< 1, 2 or 4 */
    uint8_t         line;  /**< suggested line advance in px */
    uint16_t        count;
    const uint16_t *codes;
    const uint8_t  *adv;
    const uint32_t *offs;
    const uint8_t  *data;
} ak_font_t;

/** Paint a w x h solid rectangle; the draw path never needs anything else. */
typedef void (*ak_font_rect_fn)(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

/**
 * @brief Decode one UTF-8 code point and advance *s. Returns 0 at NUL (without
 *        advancing). Malformed or truncated input yields 0xFFFD and consumes
 *        one byte, so a bad string can never stall or overrun the caller.
 */
uint32_t ak_utf8_next(const char **s);

/** @brief Glyph index for a code point, or the missing-glyph box; -1 if the font has neither. */
int ak_font_find(const ak_font_t *font, uint32_t cp);

/** @brief Pixel level 0..(2^bpp - 1) at (x, y) of glyph idx; 0 outside the bitmap. */
uint8_t ak_font_level(const ak_font_t *font, int idx, uint8_t x, uint8_t y);

/** @brief RGB565 colour between bg (level 0) and fg (level == max). */
uint16_t ak_font_blend565(uint16_t fg, uint16_t bg, uint8_t level, uint8_t max_level);

/** @brief Pixel width of a UTF-8 string. */
uint16_t ak_font_text_width(const ak_font_t *font, const char *s);

/**
 * @brief Draw a UTF-8 string, one opaque cell (bg) per glyph, clipped at x_limit.
 * @return x position after the last glyph drawn.
 */
uint16_t ak_font_draw_text(const ak_font_t *font, uint16_t x, uint16_t y, const char *s,
                           uint16_t fg, uint16_t bg, uint16_t x_limit, ak_font_rect_fn rect);

#endif /* AK_FONT_ENGINE_H */
