/**
 * @file ak_text.h
 * @brief Font size, width and line wrapping for LCD strings (pure C, host-tested).
 *
 * Rendering rule shared with ak_gfx_text(), by font size (Settings > Font
 * size, kept in KV and synced with the Companion as device_settings
 * font_size): every string is drawn whole with the UI font of the size,
 *   12 px (default): ak_font_ui12 (ASCII 6 px, CJK 12 px, 12 px tall),
 *   14 px: ak_font_ui14 (ASCII 7 px, CJK 14 px, 14 px tall),
 *   16 px: ak_font_ui16 (ASCII 8 px, CJK 16 px, 16 px tall).
 * The 8x8 font is only drawn glyph by glyph (ak_gfx_char). Pages place text
 * by box: ak_text_vtop() centres the font's ink band (ak_layout.h). The older
 * ak_gfx_text() y is the top of an 8 px row; the cell starts ak_text_dy() above it.
 */

#ifndef AK_TEXT_H
#define AK_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ak_font.h"

#define AK_TEXT_ASCII_W 8U /* ASCII advance of the 8x8 font and of Unifont 16 */

/* Enum order is the Settings order (12 -> 14 -> 16). It is only kept in RAM:
 * KV and the wire carry the px value (ak_text_size_px). */
typedef enum {
    AK_TEXT_SIZE_12 = 0, /* default: Fusion Pixel 12 for all text */
    AK_TEXT_SIZE_14,     /* Ki-board Pixel 14 for all text */
    AK_TEXT_SIZE_16,     /* GNU Unifont 16 for all text */
    AK_TEXT_SIZE_COUNT,
} ak_text_size_t;

#define AK_TEXT_SIZE_DEFAULT AK_TEXT_SIZE_12

/** @brief Select the font size for every later width / draw call. Out-of-range values are ignored. */
void           ak_text_size_set(ak_text_size_t size);
ak_text_size_t ak_text_size_get(void);
/** @brief Nominal px of a size (12 / 14 / 16): the device_settings wire value and the KV byte. */
uint8_t ak_text_size_px(ak_text_size_t size);
/** @brief Size for a wire / KV px value; false for a size the firmware does not offer. */
bool ak_text_size_from_px(uint32_t px, ak_text_size_t *out);
/** @brief The size after `size` in the Settings list (wraps in both directions). */
ak_text_size_t ak_text_size_next(ak_text_size_t size, bool forward);

/** @brief UI font of the current size. */
const ak_font_t *ak_text_font(void);
/** @brief How far the current font cell starts above y (12 px: 2, 14 px: 3, 16 px: 4). */
uint16_t ak_text_dy(void);
/** @brief Current font cell height (12 / 14 / 16). */
uint16_t ak_text_line_h(void);
/** @brief A layout value for the current size: at12 at 12 px, at14 at 14 px, at16 at 16 px. */
uint16_t ak_text_pick(uint16_t at12, uint16_t at14, uint16_t at16);

/** @brief True when s[0..n) has a non-ASCII byte. */
bool ak_text_is_wide(const char *s, size_t n);
/** @brief True when s[0..n) is drawn with ak_text_font(): always (one style at every size). */
bool ak_text_uses_font(const char *s, size_t n);

/** @brief First / last ink row of CJK and capitals in the current font (12 px 1..11, 14 px 1..13, 16 px 0..15). */
void ak_text_ink(uint8_t *top, uint8_t *bot);
/** @brief Cell top that centres the current font's ink band in the box by..by+bh-1 (floor of the half). */
uint16_t ak_text_vtop(uint16_t by, uint16_t bh);
/** @brief Byte length of the longest whole-character prefix of s at most max_px wide. */
size_t ak_text_clip(const char *s, uint16_t max_px);

/** @brief Pixel width of s[0..n) under the rule above. */
uint16_t ak_text_width_n(const char *s, size_t n);
uint16_t ak_text_width(const char *s);

/**
 * @brief Byte length of the next line of s that is at most max_px wide.
 *
 * Breaks after a space, or before / after a CJK character; a run with no break
 * opportunity is cut at a character boundary. Never splits a UTF-8 sequence.
 * Returns at least one character while s is not empty, 0 at the end.
 * Leading / trailing spaces are left to the caller.
 */
size_t ak_text_wrap(const char *s, uint16_t max_px);

/** @brief Number of lines ak_text_wrap() splits s into, leading spaces of each line skipped (capped at 0xFFFF). */
uint16_t ak_text_wrap_count(const char *s, uint16_t max_px);

/** @brief Cut a NUL-terminated buffer back to the last whole UTF-8 character. */
void ak_text_utf8_trim(char *s);

#endif /* AK_TEXT_H */
