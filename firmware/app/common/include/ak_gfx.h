/**
 * @file ak_gfx.h
 * @brief Minimal software text/box renderer on a TDL RGB565 frame buffer.
 *
 * The bring-up LCD driver only exposes solid fills; this adds an 8x8 bitmap
 * font blitter so the combined self-test can print prompts and live status.
 * All coordinates are in the bound frame buffer's pixel space.
 */

#ifndef AK_GFX_H
#define AK_GFX_H

#include <stdint.h>

#include "tdl_display_manage.h"
#include "tuya_cloud_types.h"

/* Colours: ak_palette.h (by use). Values are native RGB565, pre-swap. */

/**
 * @brief Bind the frame buffer that subsequent draw calls target.
 *
 * @param fb      Frame buffer created via tdl_disp_create_frame_buff().
 * @param is_swap Byte-swap flag reported by the display device info.
 */
void ak_gfx_bind(TDL_DISP_FRAME_BUFF_T *fb, bool is_swap);

/** @brief Scale subsequently drawn RGB565 colors to 10..100 percent. */
void ak_gfx_set_brightness(uint8_t brightness);

/** @brief Fill the whole bound frame buffer with a solid RGB565 color. */
void ak_gfx_clear(uint16_t color);

/** @brief Fill a rectangle (clipped to the frame buffer). */
void ak_gfx_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

/**
 * @brief Draw one ASCII glyph with an opaque background cell.
 *
 * @param x,y   Top-left of the glyph cell.
 * @param c     Character (0x20..0x7E rendered; others blank).
 * @param fg    Foreground (pixel) color.
 * @param bg    Background (cell) color.
 * @param scale Integer pixel scale (1..4). Cell size is 8*scale square.
 */
void ak_gfx_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale);

/**
 * @brief Draw one glyph with single-pixel stems for light-weight body text.
 *
 * Keeps the same 8x8 cell and API as ak_gfx_char(), so layouts can mix a
 * regular title with thin content without adding a second font table.
 */
void ak_gfx_char_thin(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale);

/**
 * @brief Draw a NUL-terminated string left-to-right on one line.
 *
 * UTF-8, in the UI font of the current size (ak_text.h), x scale. y is the
 * top of the 8 px row; the font cell starts ak_text_dy() x scale above it.
 * New pages place text by box (ak_gfx_text_box / _center / _right). Characters
 * outside the font draw as a box. Measure with ak_text_width() (ak_text.h).
 */
void ak_gfx_text(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale);

/** @brief Thin body-text style of ak_gfx_text() (differs only for 8x8 text; the UI fonts have one weight). */
void ak_gfx_text_thin(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale);

/**
 * @brief Draw an opaque 1-bpp bitmap, row-major and MSB-first.
 *
 * Each row occupies ceil(w / 8) bytes. Set bits use @p fg; clear bits use
 * @p bg. The function clips through ak_gfx_fill_rect().
 */
void ak_gfx_icon(uint16_t x, uint16_t y, const uint8_t *bitmap, uint16_t w, uint16_t h,
                 uint16_t fg, uint16_t bg);

/**
 * @brief Draw a 1-bpp bitmap with its holes: pixels set in @p bitmap use @p fg,
 *        pixels set only in @p shape (the bitmap with its enclosed holes
 *        filled, e.g. ghost eyes) use @p hole, the rest @p bg.
 */
void ak_gfx_icon_shape(uint16_t x, uint16_t y, const uint8_t *bitmap, const uint8_t *shape, uint16_t w, uint16_t h,
                       uint16_t fg, uint16_t hole, uint16_t bg);

/**
 * @brief Draw an opaque 4-bpp palette image (16 RGB565 entries).
 *
 * Each row is ceil(w / 2) bytes; the high nibble is the left pixel. Runs of
 * one color are drawn with one fill per run.
 * shortcut: one fill per color run, so a noisy photo-like image is slow at
 * boot; upgrade to a direct framebuffer copy if the splash delay matters.
 */
void ak_gfx_pal4(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t palette[16],
                 const uint8_t *data);

/* ---- LCD style primitives (ak_layout.h) ----------------------------------- */

/** @brief Filled rectangle with radius-r corners (lcd_round_inset; r is clamped to w/2, h/2). */
void ak_gfx_round_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t r, uint16_t color);

/** @brief A t px frame in `color` with radius-r corners, filled with `fill` inside (radius r - t). */
void ak_gfx_round_frame(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t r, uint16_t t, uint16_t color,
                        uint16_t fill);

/** @brief Text left-aligned at x, centred vertically in by..by+bh-1 (ak_text_vtop). Returns the x after it. */
uint16_t ak_gfx_text_box(uint16_t x, uint16_t by, uint16_t bh, const char *s, uint16_t fg, uint16_t bg);

/** @brief Text right-aligned to end at xr, centred vertically. Returns its left x. */
uint16_t ak_gfx_text_right(uint16_t xr, uint16_t by, uint16_t bh, const char *s, uint16_t fg, uint16_t bg);

/** @brief Text centred both ways in the box: x = bx + (bw - width) / 2, top = ak_text_vtop(by, bh). */
uint16_t ak_gfx_text_center(uint16_t bx, uint16_t by, uint16_t bw, uint16_t bh, const char *s, uint16_t fg,
                            uint16_t bg);

#endif /* AK_GFX_H */
