/**
 * @file ak_rx.c
 * @brief Line assembler + tolerant JSON field extractors (host-testable).
 */

#include "ak_rx.h"

#include <stdio.h>
#include <string.h>

void ak_rx_reader_init(ak_rx_reader_t *reader)
{
    reader->len      = 0;
    reader->overflow = false;
}

void ak_rx_reader_feed(ak_rx_reader_t *reader, const uint8_t *data, size_t n, ak_rx_line_cb cb)
{
    for (size_t i = 0; i < n; i++) {
        char c = (char)data[i];

        if (c == '\n') {
            if (!reader->overflow && reader->len > 0) {
                reader->buf[reader->len] = '\0';
                if (cb != NULL) {
                    cb(reader->buf);
                }
            }
            reader->len      = 0;
            reader->overflow = false;
            continue;
        }
        if (c == '\r') {
            continue;
        }
        if (reader->len < AK_RX_LINE_MAX - 1U) {
            reader->buf[reader->len++] = c;
        } else {
            reader->overflow = true; /* drop the rest of this oversized line */
        }
    }
}

/* Locate the value position just past `"key"` and its ':'. Returns NULL if the
 * key is not present as an object member. */
static const char *find_value(const char *json, const char *key)
{
    char needle[64];
    int  len = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (len <= 0 || (size_t)len >= sizeof(needle)) {
        return NULL;
    }

    const char *p = strstr(json, needle);
    if (p == NULL) {
        return NULL;
    }
    p += (size_t)len; /* past "key" */
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != ':') {
        return NULL;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

bool ak_json_str(const char *json, const char *key, char *out, size_t out_size)
{
    const char *p = find_value(json, key);
    if (p == NULL || *p != '"' || out_size == 0) {
        return false;
    }
    p++; /* opening quote */

    size_t i = 0;
    while (*p != '\0' && *p != '"') {
        if (*p == '\\' && p[1] != '\0') {
            p++; /* keep the escaped char verbatim (sufficient for our payloads) */
        }
        if (i < out_size - 1) {
            out[i++] = *p;
        }
        p++;
    }
    if (*p != '"') {
        return false; /* unterminated */
    }
    out[i] = '\0';
    return true;
}

bool ak_json_uint(const char *json, const char *key, uint32_t *out)
{
    const char *p = find_value(json, key);
    if (p == NULL || *p < '0' || *p > '9') {
        return false;
    }
    uint32_t value = 0;
    while (*p >= '0' && *p <= '9') {
        value = value * 10U + (uint32_t)(*p - '0');
        p++;
    }
    *out = value;
    return true;
}
