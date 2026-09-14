/**
 * @file ak_gfx.c
 * @brief Minimal software text/box renderer on a TDL RGB565 frame buffer.
 */

#include "ak_gfx.h"

#include "ak_font8x8.h"
#include "tdl_display_draw.h"

static TDL_DISP_FRAME_BUFF_T *s_fb         = NULL;
static bool                   s_is_swap    = false;
static uint8_t                s_brightness = 100U;

void ak_gfx_bind(TDL_DISP_FRAME_BUFF_T *fb, bool is_swap)
{
    s_fb      = fb;
    s_is_swap = is_swap;
}

void ak_gfx_set_brightness(uint8_t brightness)
{
    if (brightness < 10U) {
        brightness = 10U;
    } else if (brightness > 100U) {
        brightness = 100U;
    }
    s_brightness = brightness;
}

static uint16_t ak_gfx_scale_color(uint16_t color)
{
    uint32_t red   = (uint32_t)((color >> 11) & 0x1FU);
    uint32_t green = (uint32_t)((color >> 5) & 0x3FU);
    uint32_t blue  = (uint32_t)(color & 0x1FU);

    red   = red * s_brightness / 100U;
    green = green * s_brightness / 100U;
    blue  = blue * s_brightness / 100U;
    return (uint16_t)((red << 11) | (green << 5) | blue);
}

void ak_gfx_clear(uint16_t color)
{
    if (s_fb == NULL) {
        return;
    }
    tdl_disp_draw_fill_full(s_fb, ak_gfx_scale_color(color), s_is_swap);
}

void ak_gfx_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (s_fb == NULL || w == 0 || h == 0) {
        return;
    }
    if (x >= s_fb->width || y >= s_fb->height) {
        return;
    }
    if ((uint32_t)x + w > s_fb->width) {
        w = (uint16_t)(s_fb->width - x);
    }
    if ((uint32_t)y + h > s_fb->height) {
        h = (uint16_t)(s_fb->height - y);
    }

    TDL_DISP_RECT_T rect = {
        .x0 = x,
        .y0 = y,
        .x1 = (uint16_t)(x + w - 1),
        .y1 = (uint16_t)(y + h - 1),
    };
    tdl_disp_draw_fill(s_fb, &rect, ak_gfx_scale_color(color), s_is_swap);
}

static uint8_t ak_gfx_thin_row(uint8_t code, uint8_t row)
{
    uint8_t bits = ak_font8x8[code][row];
    uint8_t above = (row > 0U) ? ak_font8x8[code][row - 1U] : 0U;
    uint8_t below = (row + 1U < AK_FONT_H) ? ak_font8x8[code][row + 1U] : 0U;
    uint8_t col = 0U;

    /* Collapse short horizontal runs only when the same run continues in an
     * adjacent row. This turns 2-3 px VGA stems into 1 px strokes while
     * retaining one-row horizontal bars and the overall 8x8 glyph shape. */
    while (col < AK_FONT_W) {
        if ((bits & (uint8_t)(1U << col)) == 0U) {
            col++;
            continue;
        }

        uint8_t start = col;
        while (col < AK_FONT_W && (bits & (uint8_t)(1U << col)) != 0U) {
            col++;
        }
        uint8_t len = (uint8_t)(col - start);
        if (len >= 2U && len <= 3U) {
            uint8_t mask = (uint8_t)(((1U << len) - 1U) << start);
            if ((above & mask) == mask || (below & mask) == mask) {
                uint8_t keep = (uint8_t)((start + len <= AK_FONT_W / 2U)
                                             ? start
                                             : (uint8_t)(start + len - 1U));
                bits = (uint8_t)((bits & (uint8_t)~mask) | (uint8_t)(1U << keep));
            }
        }
    }
    return bits;
}

static void ak_gfx_char_impl(uint16_t x, uint16_t y, char c, uint16_t fg,
                             uint16_t bg, uint8_t scale, bool thin)
{
    uint8_t code = (uint8_t)c;

    if (s_fb == NULL) {
        return;
    }
    if (scale == 0) {
        scale = 1;
    }
    if (code >= 128) {
        code = '?';
    }

    /* Opaque background cell first, then the foreground pixels on top. */
    ak_gfx_fill_rect(x, y, (uint16_t)(AK_FONT_W * scale), (uint16_t)(AK_FONT_H * scale), bg);

    for (uint8_t row = 0; row < AK_FONT_H; row++) {
        uint8_t bits = thin ? ak_gfx_thin_row(code, row) : ak_font8x8[code][row];
        if (bits == 0) {
            continue;
        }
        for (uint8_t col = 0; col < AK_FONT_W; col++) {
            if (bits & (uint8_t)(1U << col)) {
                ak_gfx_fill_rect((uint16_t)(x + col * scale), (uint16_t)(y + row * scale), scale, scale, fg);
            }
        }
    }
}

void ak_gfx_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale)
{
    ak_gfx_char_impl(x, y, c, fg, bg, scale, false);
}

void ak_gfx_char_thin(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale)
{
    ak_gfx_char_impl(x, y, c, fg, bg, scale, true);
}

void ak_gfx_text(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale)
{
    if (s == NULL) {
        return;
    }
    if (scale == 0) {
        scale = 1;
    }

    uint16_t cursor = x;
    for (; *s != '\0'; s++) {
        ak_gfx_char(cursor, y, *s, fg, bg, scale);
        cursor = (uint16_t)(cursor + AK_FONT_W * scale);
    }
}

void ak_gfx_text_thin(uint16_t x, uint16_t y, const char *s, uint16_t fg,
                      uint16_t bg, uint8_t scale)
{
    if (s == NULL) {
        return;
    }
    if (scale == 0) {
        scale = 1;
    }

    uint16_t cursor = x;
    for (; *s != '\0'; s++) {
        ak_gfx_char_thin(cursor, y, *s, fg, bg, scale);
        cursor = (uint16_t)(cursor + AK_FONT_W * scale);
    }
}

void ak_gfx_icon(uint16_t x, uint16_t y, const uint8_t *bitmap, uint16_t w, uint16_t h,
                 uint16_t fg, uint16_t bg)
{
    if (s_fb == NULL || bitmap == NULL || w == 0 || h == 0) {
        return;
    }

    const uint16_t stride = (uint16_t)((w + 7U) / 8U);

    /* Paint the clear bits in one operation, then draw foreground row runs. */
    ak_gfx_fill_rect(x, y, w, h, bg);
    for (uint16_t row = 0; row < h; row++) {
        uint16_t run_start = w;
        for (uint16_t col = 0; col <= w; col++) {
            bool on = false;
            if (col < w) {
                uint8_t bits = bitmap[(uint32_t)row * stride + col / 8U];
                on = (bits & (uint8_t)(0x80U >> (col % 8U))) != 0;
            }
            if (on && run_start == w) {
                run_start = col;
            } else if (!on && run_start != w) {
                ak_gfx_fill_rect((uint16_t)(x + run_start), (uint16_t)(y + row),
                                 (uint16_t)(col - run_start), 1U, fg);
                run_start = w;
            }
        }
    }
}
