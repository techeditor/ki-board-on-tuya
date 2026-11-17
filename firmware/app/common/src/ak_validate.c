/**
 * @file ak_validate.c
 * @brief Pure format checks for provisioning fields.
 */

#include "ak_validate.h"

#include <string.h>

static bool alnum_exact(const char *s, size_t want)
{
    if (s == NULL) {
        return false;
    }
    size_t n = 0;
    for (; s[n] != '\0'; n++) {
        char c = s[n];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
            return false;
        }
        if (n >= want) {
            return false;
        }
    }
    return n == want;
}

bool ak_wifi_ssid_valid(const char *ssid)
{
    if (ssid == NULL) {
        return false;
    }
    size_t n = strlen(ssid);
    if (n == 0 || n > AK_WIFI_SSID_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)ssid[i];
        if (c < 0x20U || c == 0x7FU) {
            return false;
        }
    }
    return true;
}

bool ak_wifi_pass_valid(const char *pass)
{
    if (pass == NULL) {
        return false;
    }
    size_t n = strlen(pass);
    if (n == 0) {
        return true;
    }
    if (n < AK_WIFI_PASS_MIN || n > AK_WIFI_PASS_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)pass[i];
        if (c < 0x20U || c > 0x7EU) {
            return false;
        }
    }
    return true;
}

bool ak_license_uuid_valid(const char *uuid)
{
    return alnum_exact(uuid, AK_LICENSE_UUID_LEN) || alnum_exact(uuid, AK_LICENSE_UUID_LEN_SHORT);
}

bool ak_license_authkey_valid(const char *authkey)
{
    return alnum_exact(authkey, AK_LICENSE_KEY_LEN);
}
