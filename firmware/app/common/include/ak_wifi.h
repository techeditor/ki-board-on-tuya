/**
 * @file ak_wifi.h
 * @brief Wi-Fi provisioning for the product firmware.
 *
 * The companion sends the board Wi-Fi credentials over the physically trusted
 * USB/UART JSONL channel: {"type":"wifi","payload":{"ssid":"...","pass":"..."}}.
 * BLE-originated provisioning is rejected by the product firmware.
 * The board applies them via the Tuya netmgr (NETCONN_CMD_SSID_PSWD), which
 * connects to the AP and runs the cloud activation the board-side cloud ASR
 * needs.
 *
 * Gated by AK_ASR_BOARD_CLOUD (same as ak_asr), because only that profile pulls
 * in the Wi-Fi + netmgr stack:
 *   - AK_ASR_BOARD_CLOUD == 0 (default): a compiling STUB. ak_wifi_available()
 *     is false; ak_wifi_connect() is a no-op returning an error string via the
 *     status callback ("no wifi stack"). Keeps BLE/USB-only profiles lean.
 *   - AK_ASR_BOARD_CLOUD == 1: the real backend calling netmgr_conn_set.
 *
 * The status callback reports connecting/connected/failed so the app can emit a
 * wifi_status frame to the companion.
 */

#ifndef AK_WIFI_H
#define AK_WIFI_H

#include "tuya_cloud_types.h"

/** Wi-Fi provisioning state, mirrored to the companion as a wifi_status frame. */
typedef enum {
    AK_WIFI_CONNECTING = 0,
    AK_WIFI_CONNECTED,
    AK_WIFI_FAILED,
    AK_WIFI_DISCONNECTED,
} ak_wifi_state_t;

/** String form of a state, matching the protocol's wifi_status "state". */
const char *ak_wifi_state_str(ak_wifi_state_t s);

/** Status callback: invoked when provisioning state changes. `ssid` is the
 *  target AP (may be empty on disconnect). */
typedef void (*ak_wifi_status_cb)(ak_wifi_state_t state, const char *ssid);

/** Initialize the Wi-Fi provisioning layer and register the status callback.
 *  No-op OK in the stub. */
OPERATE_RET ak_wifi_init(ak_wifi_status_cb on_status);

/** True only when a real Wi-Fi stack is compiled in (cloud profile). */
bool ak_wifi_available(void);

/** Last known Wi-Fi target/current AP name and connection state. The SSID may
 *  be empty when no credentials have ever been stored. */
const char *ak_wifi_current_ssid(void);
ak_wifi_state_t ak_wifi_current_state(void);

/** Apply credentials and start connecting. Reports progress via the status
 *  callback. In the stub this immediately reports AK_WIFI_FAILED. */
void ak_wifi_connect(const char *ssid, const char *pass);

/* ── Device license (UUID/AuthKey) provisioning ────────────────────────────
 * Written to persistent storage via tuya_authorize_write; survives reboot. The
 * PID stays compile-time (CONFIG_TUYA_PRODUCT_ID) — it is product-wide, while
 * UUID/AuthKey are per-device. Gated by AK_ASR_BOARD_CLOUD like the rest of the
 * cloud stack; the stub returns an error string. */

/** Result of a license write, for the license_status frame. */
typedef enum {
    AK_LICENSE_WRITTEN = 0, /* newly written to storage */
    AK_LICENSE_FAILED,      /* write failed / not supported (stub) */
} ak_license_result_t;

#define AK_LICENSE_UUID_CAPACITY         40U
#define AK_LICENSE_AUTHKEY_TAIL_CAPACITY 5U

typedef struct {
    char        uuid[AK_LICENSE_UUID_CAPACITY];
    char        authkey_tail[AK_LICENSE_AUTHKEY_TAIL_CAPACITY];
    const char *source;
} ak_license_debug_info_t;

const char *ak_license_result_str(ak_license_result_t r);

/** Write UUID + AuthKey to persistent storage. Idempotent-ish: overwrites any
 *  existing license. Returns the result (also suitable for a status frame). */
ak_license_result_t ak_license_write(const char *uuid, const char *authkey);

/** True if the device already has a license stored (so the UI can show it is
 *  provisioned without echoing the secret). */
bool ak_license_present(void);

/** Read non-secret diagnostics for an explicit USB/UART0 request. Returns the
 *  full UUID, at most the last four AuthKey characters, and a source string:
 *  "flashed", "compile_time", "missing", "disabled", or "unavailable". */
bool ak_license_debug_read(ak_license_debug_info_t *info);

#endif /* AK_WIFI_H */
