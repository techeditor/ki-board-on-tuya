/**
 * @file ak_ble_hid.h
 * @brief FP-6 ble-hid-fallback — clean bridge API for the native NimBLE HID
 *        keyboard implementation.
 *
 * ISOLATION CONTRACT (the crux of this probe):
 *   This header is DELIBERATELY plain C — it exposes NO NimBLE types and
 *   NO TAL types. It is the ONLY interface between:
 *     - ak_ble_hid.c   : includes ONLY nimble headers (nimble/...)s (never tal_api.h /
 *                        tuya_list.h / any TAL header), so the two-parameter
 *                        LIST_HEAD from tuya_ble_linkqueue.h never shares a
 *                        translation unit with the one-parameter LIST_HEAD
 *                        from tuya_list.h (the D10 spike blocker #1).
 *     - tuya_app_main.c: includes ONLY tal_api.h + this header (never a nimble
 *                        header), so it is unaffected by nimble's headers.
 *
 * Because neither side sees the other's headers, the macro/private-header
 * collisions that blocked the single-TU D10 spike cannot occur. See
 * .kiro/specs/feature-probes-ble-hid-fallback/design.md D11.
 */

#ifndef AK_BLE_HID_H
#define AK_BLE_HID_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the HOGP GATT database (HID 0x1812 + DIS 0x180A + BAS 0x180F),
 *        turn on the Security Manager (Just Works, bonding, LE Secure
 *        Connections), set the keyboard appearance (0x03C1), and start
 *        advertising as a BLE HID keyboard.
 *
 * Call once after the NimBLE host stack is up (i.e. from the TAL
 * TAL_BLE_STACK_INIT event in tuya_app_main.c). Safe to call from the app
 * thread; all NimBLE state lives inside ak_ble_hid.c.
 */
void ak_ble_hid_init(void);

/**
 * @brief True once a central is connected AND the link is encrypted — i.e. the
 *        board may send Input Reports. Drives the status LCD from the TAL side
 *        without exposing any NimBLE state.
 */
bool ak_ble_hid_ready(void);

/**
 * @brief True once a central is connected (regardless of encryption).
 */
bool ak_ble_hid_connected(void);

/**
 * @brief Send a single printable ASCII character as a keyboard Input Report
 *        (press then release). Uppercase letters auto-apply LeftShift.
 * @return true if a report was queued; false if unsupported char or not ready.
 */
bool ak_ble_hid_send_char(char c);

/**
 * @brief Send one raw keyboard usage with a modifier bitmask (press then
 *        release). E.g. Enter = ak_ble_hid_send_key(0x00, 0x28).
 * @param mod      HID modifier bitmask (bit0 LeftCtrl, bit1 LeftShift, ...).
 * @param keycode  HID Usage ID (Keyboard/Keypad page 0x07).
 * @return true if a report was queued; false if not ready.
 */
bool ak_ble_hid_send_key(uint8_t mod, uint8_t keycode);

/**
 * @brief Send a whole ASCII string as a sequence of key reports.
 */
void ak_ble_hid_send_text(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* AK_BLE_HID_H */
