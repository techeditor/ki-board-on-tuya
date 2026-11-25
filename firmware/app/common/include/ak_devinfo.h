/**
 * @file ak_devinfo.h
 * @brief Device info: one read-only snapshot of the keyboard's connection,
 *        Wi-Fi, Bluetooth and identity state, and the page lines built from it
 *        (Settings > Device info). Pure and host-testable.
 *
 * Secrets are never part of the snapshot: AuthKey, License and the Bluetooth
 * pairing key are only "set" or "not set". The same snapshot feeds the
 * `device_info` frame (ak_proto_device_info), so the page and the Companion
 * show the same facts.
 */

#ifndef AK_DEVINFO_H
#define AK_DEVINFO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AK_DEVINFO_SSID_MAX 33U /* 32 bytes + NUL (802.11) */
#define AK_DEVINFO_IP_MAX   16U /* "255.255.255.255" */
#define AK_DEVINFO_MAC_STR  18U /* "AA:BB:CC:DD:EE:FF" */
#define AK_DEVINFO_NAME_MAX 32U
#define AK_DEVINFO_FW_MAX   16U

typedef enum {
    AK_DI_LINK_NONE = 0, /* no Companion connected */
    AK_DI_LINK_USB,
    AK_DI_LINK_BLE,
} ak_di_link_t;

typedef enum {
    AK_DI_CLOUD_INACTIVE = 0, /* not activated (no License / not provisioned) */
    AK_DI_CLOUD_ACTIVE,       /* activated and the cloud answers */
    AK_DI_CLOUD_OFFLINE,      /* activated, the cloud connection is down */
} ak_di_cloud_t;

typedef struct {
    /* Connection */
    ak_di_link_t  companion;
    bool          handshake; /* the Companion's hello arrived on that link */
    ak_di_cloud_t cloud;
    /* Wi-Fi (empty / false when the stack is not built or not linked) */
    char    ssid[AK_DEVINFO_SSID_MAX];
    char    ip[AK_DEVINFO_IP_MAX];
    bool    rssi_valid;
    int8_t  rssi; /* dBm */
    bool    wifi_mac_valid;
    uint8_t wifi_mac[6];
    /* Bluetooth */
    bool    ble_mac_valid;
    uint8_t ble_mac[6];
    char    ble_name[AK_DEVINFO_NAME_MAX];
    bool    ble_available; /* a BLE transport is built into this firmware */
    bool    ble_paired;    /* a pairing key is stored (its value is never shown) */
    /* Device */
    char fw[AK_DEVINFO_FW_MAX];
    bool pid;
    bool uuid;
    bool authkey;
} ak_devinfo_t;

/** Copy a network name into the snapshot; control characters become '?', so a
 *  hostile SSID costs at most 2 bytes per character in the frame and none of
 *  it can disturb the page. */
void ak_devinfo_set_ssid(ak_devinfo_t *d, const char *ssid);

/** "AA:BB:CC:DD:EE:FF" (upper case) into out[AK_DEVINFO_MAC_STR]. */
void ak_devinfo_mac_str(const uint8_t mac[6], char out[AK_DEVINFO_MAC_STR]);

/** Wire names used by the `device_info` frame. */
const char *ak_devinfo_link_str(ak_di_link_t link);
const char *ak_devinfo_cloud_str(ak_di_cloud_t cloud);

/* ── Page lines ── */
#define AK_DEVINFO_MAX_LINES 24U
#define AK_DEVINFO_VALUE_MAX 40U

typedef enum {
    AK_DI_TONE_NORMAL = 0,
    AK_DI_TONE_DIM,    /* not available: "--" */
    AK_DI_TONE_ATTENTION, /* not set / not activated / offline */
} ak_di_tone_t;

typedef struct {
    const char  *label; /* translated; a group name when `header` */
    char         value[AK_DEVINFO_VALUE_MAX];
    ak_di_tone_t tone;
    bool         header;
} ak_devinfo_line_t;

/**
 * Build the page: four groups (Connection, Wi-Fi, Bluetooth, Device), each a
 * header line followed by its rows, in the current interface language. Returns
 * the number of lines (always the same for the same build: 17).
 */
size_t ak_devinfo_build(const ak_devinfo_t *in, ak_devinfo_line_t out[AK_DEVINFO_MAX_LINES]);

#endif /* AK_DEVINFO_H */
