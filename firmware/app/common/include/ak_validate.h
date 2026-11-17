/**
 * @file ak_validate.h
 * @brief Pure format checks for provisioning fields (host-testable).
 *
 * Inputs that fail these checks are rejected before anything is written to
 * persistent storage (firmware-review FW-02 / FW-12).
 */

#ifndef AK_VALIDATE_H
#define AK_VALIDATE_H

#include <stdbool.h>

#define AK_WIFI_SSID_MAX    32U /* bytes (802.11) */
#define AK_WIFI_PASS_MIN    8U  /* WPA2 passphrase */
#define AK_WIFI_PASS_MAX    63U
#define AK_LICENSE_UUID_LEN       20U
#define AK_LICENSE_UUID_LEN_SHORT 16U /* legacy length, also accepted by the SDK */
#define AK_LICENSE_KEY_LEN        32U

/** SSID: 1..32 bytes, no control characters. */
bool ak_wifi_ssid_valid(const char *ssid);

/** Password: empty (open network) or 8..63 printable ASCII characters. */
bool ak_wifi_pass_valid(const char *pass);

/** Tuya UUID: exactly 20 (or legacy 16) characters of [A-Za-z0-9]. */
bool ak_license_uuid_valid(const char *uuid);

/** Tuya AuthKey: exactly 32 characters of [A-Za-z0-9]. */
bool ak_license_authkey_valid(const char *authkey);

#endif /* AK_VALIDATE_H */
