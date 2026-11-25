/**
 * @file ak_inputbar.h
 * @brief The voice input strip on the home page (keyboard mode): the text
 *        buffer K4 / K5 / K6 edit and send, and where the strip sits.
 *
 * The recognised text is appended to the strip; edits happen at the end only
 * (no caret). The text that K6 sends travels in one `key` frame, which
 * UART and BLE cap at 511 bytes, so the limit is the JSON-escaped size of the
 * text, not its raw size. Pure C, host-tested; drawing is ak_inputbar_draw.c.
 */

#ifndef AK_INPUTBAR_H
#define AK_INPUTBAR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Largest JSON-escaped text (bytes) one `confirm` key frame carries: the
 * 511-byte frame limit less the envelope (`{"v":1,"seq":4294967295,"type":
 * "key","payload":{"id":"k6","action":"single","bind":"confirm","slot":3,
 * "text":"..."}}`, 125 bytes). The limit leaves 6 spare. A host test builds the
 * worst-case frame against this. About 126 CJK characters or 380 ASCII ones. */
#define AK_INPUT_ESCAPED_MAX 380U
/* The raw buffer: ASCII text escapes 1:1, so raw bytes <= escaped bytes. */
#define AK_INPUT_TEXT_MAX    AK_INPUT_ESCAPED_MAX

/** JSON-escaped length of `s` (quote and backslash take 2 bytes, a control
 *  character 6; everything else is copied as UTF-8). */
size_t ak_input_escaped_len(const char *s);

/** Append `add` to `buf` (capacity AK_INPUT_TEXT_MAX + 1). Control characters
 *  and line breaks become spaces; leading / trailing spaces of `add` go. Two
 *  ASCII neighbours are joined by one space (CJK joins as is). Text that does
 *  not fit is cut at a character boundary. Returns the bytes added;
 *  `*clipped` tells whether anything was cut off. */
size_t ak_input_append(char *buf, const char *add, bool *clipped);

/** Replace `buf` with `text` under the same rules. */
size_t ak_input_set(char *buf, const char *text, bool *clipped);

/** Delete the last UTF-8 character. False when `buf` is empty. */
bool ak_input_pop(char *buf);

/* ---- geometry (ak_layout.h tokens, any font size) ---- */

/** Rows the strip shows for `lines` wrapped lines: 2 up to two lines, else 3. */
uint16_t ak_inputbar_rows(uint16_t lines);
/** Text width inside the strip, px. */
uint16_t ak_inputbar_text_w(void);
/** Wrapped lines of `text` in the strip. */
uint16_t ak_inputbar_lines(const char *text);
/** First line of the bottom-most full view (the scroll maximum). */
uint16_t ak_inputbar_max_first(const char *text);
/** Strip height / top y for `text`; the chat column ends 2 px above the top. */
uint16_t ak_inputbar_height(const char *text);
uint16_t ak_inputbar_top(const char *text);

#endif /* AK_INPUTBAR_H */
