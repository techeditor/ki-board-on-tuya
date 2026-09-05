/**
 * @file ak_gfx.c
 * @brief Minimal software text/box renderer on a TDL RGB565 frame buffer.
 */

#include "ak_gfx.h"

#include "ak_font8x8.h"
#include "tdl_display_draw.h"

static TDL_DISP_FRAME_BUFF_T *s_fb      = NULL;
static bool                   s_is_swap = false;

void ak_gfx_bind(TDL_DISP_FRAME_BUFF_T *fb, bool is_swap)
{
    s_fb      = fb;
    s_is_swap = is_swap;
}

void ak_gfx_clear(uint16_t color)
{
    if (s_fb == NULL) {
        return;
    }
    tdl_disp_draw_fill_full(s_fb, color, s_is_swap);
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
    tdl_disp_draw_fill(s_fb, &rect, color, s_is_swap);
}

void ak_gfx_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale)
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
        uint8_t bits = ak_font8x8[code][row];
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
