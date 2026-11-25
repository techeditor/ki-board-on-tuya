/**
 * @file ak_inputbar.c
 * @brief Voice input strip text and geometry (pure). See ak_inputbar.h.
 */

#include "ak_inputbar.h"

#include <string.h>

#include "ak_layout.h"
#include "ak_text.h"
#include "ak_ui.h"

size_t ak_input_escaped_len(const char *s)
{
    size_t n = 0U;
    for (; s != NULL && *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        n += (c == '"' || c == '\\') ? 2U : (c < 0x20U ? 6U : 1U);
    }
    return n;
}

/* Control characters (line breaks, tabs) become spaces, runs of spaces
 * collapse to one; no leading or trailing space. Writes at most cap-1 bytes;
 * `*trunc` tells whether text was left behind for lack of room. */
static size_t input_clean(const char *in, char *out, size_t cap, bool *trunc)
{
    size_t n         = 0U;
    bool   had_space = false;
    *trunc           = false;
    for (; in != NULL && *in != '\0'; in++) {
        unsigned char c = (unsigned char)*in;
        if (c < 0x20U || c == 0x7FU || c == ' ') {
            had_space = n > 0U;
            continue;
        }
        if (n + 1U + (had_space ? 1U : 0U) >= cap) {
            *trunc = true;
            break;
        }
        if (had_space) {
            out[n++]  = ' ';
            had_space = false;
        }
        out[n++] = (char)c;
    }
    out[n] = '\0';
    ak_text_utf8_trim(out);
    return strlen(out);
}

size_t ak_input_append(char *buf, const char *add, bool *clipped)
{
    char   clean[AK_INPUT_TEXT_MAX + 1U];
    size_t have = strlen(buf);
    bool   cut  = false;
    size_t len  = input_clean(add, clean, sizeof(clean), &cut);
    size_t join = (have > 0U && len > 0U && (unsigned char)buf[have - 1U] < 0x80U && buf[have - 1U] != ' ' &&
                   (unsigned char)clean[0] < 0x80U)
                      ? 1U
                      : 0U;

    /* Take whole characters while the escaped size fits. */
    size_t used = ak_input_escaped_len(buf) + join;
    size_t take = 0U;
    while (take < len) {
        size_t step = 1U;
        while (take + step < len && ((unsigned char)clean[take + step] & 0xC0U) == 0x80U) {
            step++;
        }
        size_t cost = 0U;
        for (size_t i = 0U; i < step; i++) {
            unsigned char c = (unsigned char)clean[take + i];
            cost += (c == '"' || c == '\\') ? 2U : 1U;
        }
        if (used + cost > AK_INPUT_ESCAPED_MAX || have + join + take + step > AK_INPUT_TEXT_MAX) {
            cut = true;
            break;
        }
        used += cost;
        take += step;
    }
    if (clipped != NULL) {
        *clipped = cut;
    }
    if (take == 0U) {
        return 0U;
    }
    if (join != 0U) {
        buf[have++] = ' ';
    }
    memcpy(buf + have, clean, take);
    buf[have + take] = '\0';
    /* A cut text must not end in a space. */
    size_t end = strlen(buf);
    while (end > 0U && buf[end - 1U] == ' ') {
        buf[--end] = '\0';
    }
    return take + join;
}

size_t ak_input_set(char *buf, const char *text, bool *clipped)
{
    buf[0] = '\0';
    return ak_input_append(buf, text, clipped);
}

bool ak_input_pop(char *buf)
{
    return ak_utf8_pop_last(buf);
}

/* ---- geometry ---- */

uint16_t ak_inputbar_rows(uint16_t lines)
{
    return lines <= 2U ? 2U : 3U;
}

/* The strip is the chat column's width: x from LCD_SIDEBAR_W + 2, a bubble
 * wide (LCD_CHAT_TEXT_W + 2 * LCD_BUBBLE_PAD), 1 px frame and LCD_PAD inside,
 * the right LCD_BAR_W px kept for the scroll bar. */
uint16_t ak_inputbar_text_w(void)
{
    return (uint16_t)(LCD_CHAT_TEXT_W + 2U * LCD_BUBBLE_PAD - 2U - 2U * LCD_PAD - LCD_BAR_W);
}

uint16_t ak_inputbar_lines(const char *text)
{
    return text != NULL && text[0] != '\0' ? ak_text_wrap_count(text, ak_inputbar_text_w()) : 0U;
}

uint16_t ak_inputbar_max_first(const char *text)
{
    uint16_t lines = ak_inputbar_lines(text);
    uint16_t rows  = ak_inputbar_rows(lines);
    return lines > rows ? (uint16_t)(lines - rows) : 0U;
}

uint16_t ak_inputbar_height(const char *text)
{
    return (uint16_t)(ak_inputbar_rows(ak_inputbar_lines(text)) * LCD_L + 4U);
}

uint16_t ak_inputbar_top(const char *text)
{
    return (uint16_t)(LCD_STATUS_BAR_Y - 2U - ak_inputbar_height(text));
}
