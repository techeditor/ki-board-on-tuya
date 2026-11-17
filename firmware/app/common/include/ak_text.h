/**
 * @file ak_text.h
 * @brief Width and line wrapping for LCD strings (pure C, host-tested).
 *
 * Rendering rule shared with ak_gfx_text(): a string that is pure ASCII is
 * drawn with the 8x8 font (8 px per character, unchanged look); a string that
 * contains any non-ASCII byte is drawn whole with ak_font_ui (ASCII 6 px,
 * CJK 12 px, 12 px tall, 2 px above the 8x8 cell), so Chinese and English mix
 * in one font with one baseline.
 */

#ifndef AK_TEXT_H
#define AK_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AK_TEXT_ASCII_W 8U /* 8x8 font advance */
#define AK_TEXT_WIDE_DY 2U /* ak_font_ui cell starts this far above the 8x8 cell */

/** @brief True when s[0..n) has a non-ASCII byte (drawn with ak_font_ui). */
bool ak_text_is_wide(const char *s, size_t n);

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
