/**
 * @file ak_text.c
 * @brief LCD string width and wrapping. See ak_text.h.
 */

#include "ak_text.h"

#include <string.h>

#include "ak_font.h"
#include "ak_font_ui.h"

static ak_text_size_t s_size = AK_TEXT_SIZE_DEFAULT;

void ak_text_size_set(ak_text_size_t size)
{
    if ((unsigned)size < (unsigned)AK_TEXT_SIZE_COUNT) {
        s_size = size;
    }
}

ak_text_size_t ak_text_size_get(void)
{
    return s_size;
}

uint8_t ak_text_size_px(ak_text_size_t size)
{
    return size == AK_TEXT_SIZE_16 ? 16U : 12U;
}

bool ak_text_size_from_px(uint32_t px, ak_text_size_t *out)
{
    if (out == NULL || (px != 12U && px != 16U)) {
        return false;
    }
    *out = px == 16U ? AK_TEXT_SIZE_16 : AK_TEXT_SIZE_12;
    return true;
}

ak_text_size_t ak_text_size_next(ak_text_size_t size, bool forward)
{
    unsigned n = (unsigned)AK_TEXT_SIZE_COUNT;
    unsigned i = (unsigned)size < n ? (unsigned)size : 0U;
    return (ak_text_size_t)((i + (forward ? 1U : n - 1U)) % n);
}

const ak_font_t *ak_text_font(void)
{
    return s_size == AK_TEXT_SIZE_16 ? &ak_font_ui16 : &ak_font_ui12;
}

uint16_t ak_text_dy(void)
{
    return s_size == AK_TEXT_SIZE_16 ? 4U : 2U;
}

uint16_t ak_text_line_h(void)
{
    return ak_text_font()->px;
}

uint16_t ak_text_pick(uint16_t at12, uint16_t at16)
{
    return s_size == AK_TEXT_SIZE_16 ? at16 : at12;
}

bool ak_text_uses_font(const char *s, size_t n)
{
    return s_size == AK_TEXT_SIZE_16 || ak_text_is_wide(s, n);
}

bool ak_text_is_wide(const char *s, size_t n)
{
    for (size_t i = 0U; s != NULL && i < n && s[i] != '\0'; i++) {
        if ((unsigned char)s[i] >= 0x80U) {
            return true;
        }
    }
    return false;
}

uint16_t ak_text_width_n(const char *s, size_t n)
{
    if (s == NULL) {
        return 0U;
    }
    size_t len = 0U;
    while (len < n && s[len] != '\0') {
        len++;
    }
    if (!ak_text_uses_font(s, len)) {
        return (uint16_t)(len * AK_TEXT_ASCII_W);
    }
    const ak_font_t *font = ak_text_font();
    uint32_t    w   = 0U;
    const char *p   = s;
    const char *end = s + len;
    while (p < end) {
        uint32_t cp = ak_utf8_next(&p);
        if (cp == 0U || p > end) { /* a sequence running past n is not counted */
            break;
        }
        int idx = ak_font_find(font, cp);
        if (idx >= 0) {
            w += font->adv[idx];
        }
    }
    return (uint16_t)(w > 0xFFFFU ? 0xFFFFU : w);
}

uint16_t ak_text_width(const char *s)
{
    return s == NULL ? 0U : ak_text_width_n(s, strlen(s));
}

/* CJK ideographs, kana, Hangul and full-width forms: a line may break on
 * either side of one. */
static bool is_cjk(uint32_t cp)
{
    return (cp >= 0x2E80U && cp <= 0x9FFFU) || (cp >= 0xAC00U && cp <= 0xD7A3U) ||
           (cp >= 0xF900U && cp <= 0xFAFFU) || (cp >= 0xFF00U && cp <= 0xFFEFU);
}

/* Closing punctuation that should not start a line (kept with the text before). */
static bool no_line_start(uint32_t cp)
{
    return cp == 0x3001U || cp == 0x3002U || cp == 0xFF0CU || cp == 0xFF01U || cp == 0xFF1FU ||
           cp == 0xFF1BU || cp == 0xFF1AU || cp == 0xFF09U || cp == 0x300BU || cp == 0x201DU;
}

size_t ak_text_wrap(const char *s, uint16_t max_px)
{
    if (s == NULL || s[0] == '\0') {
        return 0U;
    }
    const char *p    = s;
    size_t      brk  = 0U; /* last break opportunity (byte length) */
    size_t      prev = 0U; /* the one before it */
    while (*p != '\0') {
        const char *q  = p;
        uint32_t    cp = ak_utf8_next(&q);
        if (cp == 0U) {
            break;
        }
        size_t here = (size_t)(p - s);
        size_t end  = (size_t)(q - s);
        if (ak_text_width_n(s, end) > max_px) {
            if (here == 0U) {
                return end; /* one character wider than the line: still progress */
            }
            if (no_line_start(cp) && brk == here && prev > 0U) {
                return prev; /* keep "，" with the character before it */
            }
            return brk > 0U ? brk : here;
        }
        if (cp == ' ' || is_cjk(cp)) {
            if (is_cjk(cp) && !no_line_start(cp) && here > 0U && brk != here) {
                prev = brk;
                brk  = here; /* before a CJK character */
            }
            prev = brk;
            brk  = end; /* after a space or a CJK character */
        }
        p = q;
    }
    return (size_t)(p - s);
}

void ak_text_utf8_trim(char *s)
{
    if (s == NULL) {
        return;
    }
    size_t len  = strlen(s);
    size_t lead = len;
    while (lead > 0U && ((unsigned char)s[lead - 1U] & 0xC0U) == 0x80U) {
        lead--;
    }
    if (lead == 0U) {
        s[0] = '\0';
        return;
    }
    unsigned char c    = (unsigned char)s[lead - 1U];
    size_t        need = c < 0x80U ? 1U : (c & 0xE0U) == 0xC0U ? 2U : (c & 0xF0U) == 0xE0U ? 3U : 4U;
    if (lead - 1U + need > len) {
        s[lead - 1U] = '\0';
    }
}
