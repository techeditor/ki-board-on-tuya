/**
 * @file ak_splash.c
 * @brief Boot splash format checks (pure C, no device dependencies).
 */

#include "ak_splash.h"

#include <string.h>

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* shortcut: bit-by-bit CRC, about 0.5 M steps for a full 60 KB area; upgrade to
 * a 256-entry table if the boot read time shows up on the board. */
uint32_t ak_splash_crc32(const uint8_t *buf, size_t len)
{
    uint32_t crc = 0xFFFFFFFFU;

    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc & 1U) ? (crc >> 1) ^ 0xEDB88320U : crc >> 1;
        }
    }
    return ~crc;
}

bool ak_splash_is_erased(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != 0xFFU) {
            return false;
        }
    }
    return true;
}

bool ak_splash_parse_header(const uint8_t *buf, size_t len, ak_splash_t *out, size_t *total_len)
{
    if (buf == NULL || out == NULL || total_len == NULL || len < AK_SPLASH_HDR_LEN) {
        return false;
    }
    if (memcmp(buf, "AKSP", 4) != 0 || rd16(buf + 4) != 1U || rd16(buf + 6) != 1U) {
        return false;
    }

    uint16_t width  = rd16(buf + 8);
    uint16_t height = rd16(buf + 10);
    if (width == 0U || width > AK_SPLASH_MAX_W || height == 0U || height > AK_SPLASH_MAX_H) {
        return false;
    }

    uint32_t data_len = rd32(buf + 12);
    if (data_len != (uint32_t)((width + 1U) / 2U) * height) {
        return false;
    }

    size_t total = AK_SPLASH_HDR_LEN + (size_t)data_len + AK_SPLASH_CRC_LEN;
    if (total > AK_SPLASH_MAX_BYTES) {
        return false;
    }

    out->width  = width;
    out->height = height;
    for (unsigned i = 0; i < 16U; i++) {
        out->palette[i] = rd16(buf + 16U + 2U * i);
    }
    out->data  = buf + AK_SPLASH_HDR_LEN;
    *total_len = total;
    return true;
}

bool ak_splash_parse(const uint8_t *buf, size_t len, ak_splash_t *out)
{
    size_t total = 0;

    if (!ak_splash_parse_header(buf, len, out, &total) || len < total) {
        return false;
    }
    size_t body = total - AK_SPLASH_CRC_LEN;
    return ak_splash_crc32(buf, body) == rd32(buf + body);
}

/* Event values of ak_input_event_t (ak_input_fsm.h); kept as ints here so
 * this file stays free of the input headers. */
enum { SPLASH_EV_DOWN = 1, SPLASH_EV_UP = 2, SPLASH_EV_LONG = 3 };

uint32_t ak_splash_hold_ms(bool custom)
{
    return custom ? AK_SPLASH_HOLD_CUSTOM_MS : AK_SPLASH_HOLD_DEFAULT_MS;
}

void ak_splash_hold_begin(ak_splash_hold_t *h, bool custom, uint32_t now_ms)
{
    h->start_ms = now_ms;
    h->hold_ms  = ak_splash_hold_ms(custom);
    h->held     = 0U;
    h->skipped  = false;
}

void ak_splash_hold_key(ak_splash_hold_t *h, int event)
{
    if (event == SPLASH_EV_DOWN) {
        h->skipped = true;
        if (h->held < 0xFFU) {
            h->held++;
        }
    } else if (event == SPLASH_EV_LONG) {
        h->skipped = true;
    } else if (event == SPLASH_EV_UP && h->held > 0U) {
        /* held == 0: a key pressed before the splash; nothing to wait for. */
        h->held--;
    }
}

void ak_splash_hold_encoder(ak_splash_hold_t *h, int32_t detents)
{
    if (detents != 0) {
        h->skipped = true;
    }
}

bool ak_splash_hold_done(const ak_splash_hold_t *h, uint32_t now_ms)
{
    uint32_t elapsed = now_ms - h->start_ms;

    if (elapsed >= h->hold_ms + AK_SPLASH_RELEASE_MAX_MS) {
        return true;
    }
    return (h->skipped || elapsed >= h->hold_ms) && h->held == 0U;
}
