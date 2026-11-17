/**
 * @file ak_text.h
 * @brief Font size, width and line wrapping for LCD strings (pure C, host-tested).
 *
 * Rendering rule shared with ak_gfx_text(), by font size (Settings > Font
 * size, kept in KV and synced with the Companion as device_settings
 * font_size):
 *   12 px (default): a pure-ASCII string is drawn with the 8x8 font (8 px per
 *     character); a string with any non-ASCII byte is drawn whole with
 *     ak_font_ui12 (ASCII 6 px, CJK 12 px, 12 px tall).
 *   16 px: every string is drawn with ak_font_ui16 (ASCII 8 px, CJK 16 px,
 *     16 px tall).
 * The y a caller passes is the top of the 8 px text row; a font cell starts
 * ak_text_dy() above it, so the glyphs centre on that row.
 */

#ifndef AK_TEXT_H
#define AK_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ak_font.h"

#define AK_TEXT_ASCII_W 8U /* ASCII advance of the 8x8 font and of Unifont 16 */

typedef enum {
    AK_TEXT_SIZE_12 = 0, /* default: 8x8 for pure ASCII, Fusion Pixel 12 otherwise */
    AK_TEXT_SIZE_16,     /* GNU Unifont 16 for all text */
    /* shortcut: a 14 px size is the next value here (font table, layout, and
     * ak_text_sizes_offered()); it is not offered yet. */
    AK_TEXT_SIZE_COUNT,
} ak_text_size_t;

#define AK_TEXT_SIZE_DEFAULT AK_TEXT_SIZE_12

/** @brief Select the font size for every later width / draw call. Out-of-range values are ignored. */
void           ak_text_size_set(ak_text_size_t size);
ak_text_size_t ak_text_size_get(void);
/** @brief Nominal px of a size (12 / 16): the device_settings wire value and the KV byte. */
uint8_t ak_text_size_px(ak_text_size_t size);
/** @brief Size for a wire / KV px value; false for a size the firmware does not offer. */
bool ak_text_size_from_px(uint32_t px, ak_text_size_t *out);
/** @brief The size after `size` in the Settings list (wraps; same for both directions while two exist). */
ak_text_size_t ak_text_size_next(ak_text_size_t size, bool forward);

/** @brief UI font of the current size. */
const ak_font_t *ak_text_font(void);
/** @brief How far the current font cell starts above y (12 px: 2, 16 px: 4). */
uint16_t ak_text_dy(void);
/** @brief Current font cell height (12 / 16). */
uint16_t ak_text_line_h(void);
/** @brief A layout value for the current size: at12 at 12 px, at16 at 16 px. */
uint16_t ak_text_pick(uint16_t at12, uint16_t at16);

/** @brief True when s[0..n) has a non-ASCII byte. */
bool ak_text_is_wide(const char *s, size_t n);
/** @brief True when s[0..n) is drawn with ak_text_font() (always at 16 px), false for the 8x8 font. */
bool ak_text_uses_font(const char *s, size_t n);

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

/** @brief Cut a NUL-terminated buffer back to the last whole UTF-8 character. */
void ak_text_utf8_trim(char *s);

#endif /* AK_TEXT_H */
