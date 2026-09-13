/**
 * @file ak_wifi.h
 * @brief Wi-Fi provisioning for the product firmware.
 *
 * The companion sends the board Wi-Fi credentials over the existing JSONL
 * channel (BLE/USB): {"type":"wifi","payload":{"ssid":"...","pass":"..."}}.
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

/** Apply credentials and start connecting. Reports progress via the status
 *  callback. In the stub this immediately reports AK_WIFI_FAILED. */
void ak_wifi_connect(const char *ssid, const char *pass);

#endif /* AK_WIFI_H */
