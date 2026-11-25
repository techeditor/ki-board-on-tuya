/**
 * @file ak_devinfo.c
 * @brief Device info page lines (see ak_devinfo.h). Pure.
 */

#include "ak_devinfo.h"

#include <stdio.h>
#include <string.h>

#include "ak_i18n.h"

#define DASH "--"

void ak_devinfo_mac_str(const uint8_t mac[6], char out[AK_DEVINFO_MAC_STR])
{
    (void)snprintf(out, AK_DEVINFO_MAC_STR, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
                   mac[5]);
}

void ak_devinfo_set_ssid(ak_devinfo_t *d, const char *ssid)
{
    size_t i = 0U;
    if (ssid != NULL) {
        for (; i + 1U < sizeof(d->ssid) && ssid[i] != '\0'; i++) {
            unsigned char c = (unsigned char)ssid[i];
            d->ssid[i]      = (c < 0x20U || c == 0x7FU) ? '?' : (char)c;
        }
    }
    d->ssid[i] = '\0';
}

const char *ak_devinfo_link_str(ak_di_link_t link)
{
    switch (link) {
    case AK_DI_LINK_USB:
        return "usb";
    case AK_DI_LINK_BLE:
        return "ble";
    case AK_DI_LINK_NONE:
    default:
        return "none";
    }
}

const char *ak_devinfo_cloud_str(ak_di_cloud_t cloud)
{
    switch (cloud) {
    case AK_DI_CLOUD_ACTIVE:
        return "active";
    case AK_DI_CLOUD_OFFLINE:
        return "offline";
    case AK_DI_CLOUD_INACTIVE:
    default:
        return "inactive";
    }
}

static size_t put(ak_devinfo_line_t out[], size_t n, const char *label, const char *value, ak_di_tone_t tone,
                  bool header)
{
    if (n >= AK_DEVINFO_MAX_LINES) {
        return n;
    }
    out[n].label  = label;
    out[n].tone   = tone;
    out[n].header = header;
    (void)snprintf(out[n].value, sizeof(out[n].value), "%s", value == NULL ? "" : value);
    return n + 1U;
}

static size_t put_set(ak_devinfo_line_t out[], size_t n, const char *label, bool set)
{
    return put(out, n, label, ak_tr(set ? AK_STR_CRED_SET : AK_STR_CRED_NOT_SET),
               set ? AK_DI_TONE_NORMAL : AK_DI_TONE_ATTENTION, false);
}

size_t ak_devinfo_build(const ak_devinfo_t *in, ak_devinfo_line_t out[AK_DEVINFO_MAX_LINES])
{
    size_t n = 0U;
    char   text[AK_DEVINFO_VALUE_MAX];

    /* Connection */
    n = put(out, n, ak_tr(AK_STR_DI_G_CONN), "", AK_DI_TONE_NORMAL, true);
    ak_str_id_t link_text = AK_STR_DI_NOT_CONNECTED;
    if (in->companion == AK_DI_LINK_USB) {
        link_text = in->handshake ? AK_STR_DI_USB_HELLO : AK_STR_DI_USB_NO_HELLO;
    } else if (in->companion == AK_DI_LINK_BLE) {
        link_text = in->handshake ? AK_STR_DI_BLE_HELLO : AK_STR_DI_BLE_NO_HELLO;
    }
    n = put(out, n, ak_tr(AK_STR_DI_COMPANION), ak_tr(link_text),
            in->companion != AK_DI_LINK_NONE && in->handshake ? AK_DI_TONE_NORMAL : AK_DI_TONE_ATTENTION, false);
    ak_str_id_t cloud_text = in->cloud == AK_DI_CLOUD_ACTIVE    ? AK_STR_DI_CLOUD_ACTIVE
                             : in->cloud == AK_DI_CLOUD_OFFLINE ? AK_STR_DI_CLOUD_OFFLINE
                                                                : AK_STR_DI_CLOUD_INACTIVE;
    n = put(out, n, ak_tr(AK_STR_DI_CLOUD), ak_tr(cloud_text),
            in->cloud == AK_DI_CLOUD_ACTIVE ? AK_DI_TONE_NORMAL : AK_DI_TONE_ATTENTION, false);

    /* Wi-Fi: the MAC shows even while the link is down, the rest needs a link. */
    n = put(out, n, ak_tr(AK_STR_DI_G_WIFI), "", AK_DI_TONE_NORMAL, true);
    n = put(out, n, ak_tr(AK_STR_DI_SSID), in->ssid[0] != '\0' ? in->ssid : DASH,
            in->ssid[0] != '\0' ? AK_DI_TONE_NORMAL : AK_DI_TONE_DIM, false);
    n = put(out, n, ak_tr(AK_STR_DI_IP), in->ip[0] != '\0' ? in->ip : DASH,
            in->ip[0] != '\0' ? AK_DI_TONE_NORMAL : AK_DI_TONE_DIM, false);
    if (in->rssi_valid) {
        (void)snprintf(text, sizeof(text), "%d dBm", (int)in->rssi);
    } else {
        (void)snprintf(text, sizeof(text), "%s", DASH);
    }
    n = put(out, n, ak_tr(AK_STR_DI_SIGNAL), text, in->rssi_valid ? AK_DI_TONE_NORMAL : AK_DI_TONE_DIM, false);
    if (in->wifi_mac_valid) {
        ak_devinfo_mac_str(in->wifi_mac, text);
    } else {
        (void)snprintf(text, sizeof(text), "%s", DASH);
    }
    n = put(out, n, ak_tr(AK_STR_DI_WIFI_MAC), text, in->wifi_mac_valid ? AK_DI_TONE_NORMAL : AK_DI_TONE_DIM, false);

    /* Bluetooth */
    n = put(out, n, ak_tr(AK_STR_DI_G_BLE), "", AK_DI_TONE_NORMAL, true);
    if (in->ble_mac_valid) {
        ak_devinfo_mac_str(in->ble_mac, text);
    } else {
        (void)snprintf(text, sizeof(text), "%s", DASH);
    }
    n = put(out, n, ak_tr(AK_STR_DI_BLE_MAC), text, in->ble_mac_valid ? AK_DI_TONE_NORMAL : AK_DI_TONE_DIM, false);
    n = put(out, n, ak_tr(AK_STR_DI_BLE_NAME), in->ble_name[0] != '\0' ? in->ble_name : DASH,
            in->ble_name[0] != '\0' ? AK_DI_TONE_NORMAL : AK_DI_TONE_DIM, false);
    if (in->ble_available) {
        n = put(out, n, ak_tr(AK_STR_DI_PAIRING), ak_tr(in->ble_paired ? AK_STR_DI_PAIRED : AK_STR_DI_NOT_PAIRED),
                in->ble_paired ? AK_DI_TONE_NORMAL : AK_DI_TONE_ATTENTION, false);
    } else {
        n = put(out, n, ak_tr(AK_STR_DI_PAIRING), DASH, AK_DI_TONE_DIM, false);
    }

    /* Device: only "set" / "not set" for anything secret. */
    n = put(out, n, ak_tr(AK_STR_DI_G_DEVICE), "", AK_DI_TONE_NORMAL, true);
    n = put(out, n, ak_tr(AK_STR_DI_FIRMWARE), in->fw, AK_DI_TONE_NORMAL, false);
    n = put_set(out, n, ak_tr(AK_STR_CRED_PID), in->pid);
    n = put_set(out, n, ak_tr(AK_STR_DI_UUID), in->uuid);
    n = put_set(out, n, ak_tr(AK_STR_DI_AUTHKEY), in->authkey);
    return n;
}
