/**
 * @file ak_window_marker.h
 * @brief Content of the one-shot KV marker that asks the next boot to open the
 *        pairing window and enable the Tuya BLE/SoftAP netcfg (FR-12 B).
 *
 * Pure: no KV or SDK calls here, so the host tests can cover it. The KV
 * read/write/delete lives in ak_reset_netcfg.c. The marker is deleted as soon
 * as the next boot reads it, so an abnormal reboot cannot leave the window
 * open for good.
 */

#ifndef AK_WINDOW_MARKER_H
#define AK_WINDOW_MARKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AK_WINDOW_MARKER_LEN 4U

/** Writes the marker bytes ("AKW1") into out[0..AK_WINDOW_MARKER_LEN). */
void ak_window_marker_encode(uint8_t out[AK_WINDOW_MARKER_LEN]);

/** True only for exactly the marker bytes; a legacy 1-byte value is rejected. */
bool ak_window_marker_valid(const uint8_t *buf, size_t len);

#endif /* AK_WINDOW_MARKER_H */
