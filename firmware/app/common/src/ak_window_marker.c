/**
 * @file ak_window_marker.c
 * @brief Pure encode/validate for the pairing-window boot marker.
 */

#include "ak_window_marker.h"

static const uint8_t s_magic[AK_WINDOW_MARKER_LEN] = {'A', 'K', 'W', '1'};

void ak_window_marker_encode(uint8_t out[AK_WINDOW_MARKER_LEN])
{
    for (size_t i = 0; i < AK_WINDOW_MARKER_LEN; i++) {
        out[i] = s_magic[i];
    }
}

bool ak_window_marker_valid(const uint8_t *buf, size_t len)
{
    if (buf == NULL || len != AK_WINDOW_MARKER_LEN) {
        return false;
    }
    for (size_t i = 0; i < AK_WINDOW_MARKER_LEN; i++) {
        if (buf[i] != s_magic[i]) {
            return false;
        }
    }
    return true;
}
