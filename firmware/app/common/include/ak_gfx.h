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

/* Common RGB565 colors (native, pre-swap). */
#define AK_RGB565(r, g, b) ((uint16_t)((((r)&0xF8U) << 8) | (((g)&0xFCU) << 3) | (((b)&0xF8U) >> 3)))

#define AK_COL_BLACK   0x0000U
#define AK_COL_WHITE   0xFFFFU
#define AK_COL_RED     0xF800U
#define AK_COL_GREEN   0x07E0U
#define AK_COL_BLUE    0x001FU
#define AK_COL_YELLOW  0xFFE0U
#define AK_COL_CYAN    0x07FFU
#define AK_COL_MAGENTA 0xF81FU
#define AK_COL_GRAY    0x8410U
#define AK_COL_DKBLUE  0x0010U
#define AK_COL_ORANGE  0xFD20U

/**
 * @brief Bind the frame buffer that subsequent draw calls target.
 *
 * @param fb      Frame buffer created via tdl_disp_create_frame_buff().
 * @param is_swap Byte-swap flag reported by the display device info.
 */
void ak_gfx_bind(TDL_DISP_FRAME_BUFF_T *fb, bool is_swap);

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

/** @brief Draw a NUL-terminated string left-to-right on one line. */
void ak_gfx_text(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale);

/** @brief Draw a NUL-terminated string using the thin body-text style. */
void ak_gfx_text_thin(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale);

/**
 * @brief Draw an opaque 1-bpp bitmap, row-major and MSB-first.
 *
 * Each row occupies ceil(w / 8) bytes. Set bits use @p fg; clear bits use
 * @p bg. The function clips through ak_gfx_fill_rect().
 */
void ak_gfx_icon(uint16_t x, uint16_t y, const uint8_t *bitmap, uint16_t w, uint16_t h,
                 uint16_t fg, uint16_t bg);

#endif /* AK_GFX_H */
