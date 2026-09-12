/**
 * @file ak_ble_hid.c
 * @brief FP-6 ble-hid-fallback — native NimBLE HOGP keyboard implementation.
 *
 * ISOLATION: this translation unit includes ONLY nimble headers (nimble/...)s. It NEVER
 * includes tal_api.h, tuya_list.h, or any TAL header. Reason (D10 spike
 * blocker #1): tuya_ble_linkqueue.h defines a two-parameter LIST_HEAD while
 * TuyaOS's tuya_list.h defines a one-parameter LIST_HEAD, both without include
 * guards; they cannot coexist in one TU. By keeping every NimBLE header out of
 * tuya_app_main.c and every TAL header out of this file, the two LIST_HEAD
 * definitions never meet. Blocker #2 (private header tuya_ble_mempool.h, pulled
 * transitively by ble_hs.h -> hci_common.h -> tuya_ble.h) is solved at the
 * probe CMakeLists level by adding nimble/host to THIS file's include path.
 *
 * It builds a compliant HID-over-GATT (HOGP) keyboard using the native GATT
 * server that TAL's fixed command service cannot express:
 *   - HID Service 0x1812: Report Map 0x2A4B, HID Info 0x2A4A, HID Ctrl Point
 *     0x2A4C, Protocol Mode 0x2A4E, Input Report 0x2A4D (READ|NOTIFY|READ_ENC)
 *     carrying a Report Reference descriptor 0x2908 (+ auto CCCD).
 *   - Device Information Service 0x180A (PnP ID 0x2A50).
 *   - Battery Service 0x180F (Battery Level 0x2A19).
 *   - Security Manager: Just Works (NO_INPUT_OUTPUT), bonding, LE Secure
 *     Connections, via ble_hs_cfg.sm_* + ble_store_* callbacks.
 *   - Advertising: appearance 0x03C1 (Keyboard) + service UUID 0x1812 via
 *     native ble_gap_adv_set_fields()/ble_gap_adv_start().
 *   - Sending an 8-byte boot-keyboard Input Report with ble_gattc_notify_custom().
 *
 * The plain-C bridge (ak_ble_hid.h) is the only surface the TAL side touches.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Native NimBLE host headers ONLY — no TAL headers in this TU (see file
 * banner). nimble/host is on this file's private include path (probe
 * CMakeLists) so the transitive tuya_ble_mempool.h resolves. */
#include "ble_gap.h"
#include "ble_gatt.h"
#include "ble_hs.h"
#include "ble_hs_adv.h"
#include "ble_hs_mbuf.h"
#include "ble_sm.h"
#include "ble_store.h"
#include "ble_svc_gap.h"
#include "ble_uuid.h"
#include "tuya_ble_mbuf.h"

#include "ak_ble_hid.h"

/* ------------------------------------------------------------ HID constants */

#define BLE_HID_NAME            "AK-KB-HID"
#define BLE_APPEARANCE_KEYBOARD 0x03C1 /* HID subtype: Keyboard */

/* GATT 16-bit UUIDs (HOGP). */
#define UUID_SVC_HID         0x1812
#define UUID_SVC_DIS         0x180A
#define UUID_SVC_BAS         0x180F
#define UUID_CHR_HID_INFO    0x2A4A
#define UUID_CHR_REPORT_MAP  0x2A4B
#define UUID_CHR_HID_CTRL_PT 0x2A4C
#define UUID_CHR_REPORT      0x2A4D
#define UUID_CHR_PROTO_MODE  0x2A4E
#define UUID_CHR_PNP_ID      0x2A50
#define UUID_CHR_BATT_LEVEL  0x2A19
#define UUID_DSC_REPORT_REF  0x2908 /* HOGP Report Reference descriptor */

/* Standard boot keyboard Report Map: 8-byte input report
 * [modifiers][reserved][keycode x6] — the canonical HOGP keyboard descriptor. */
static const uint8_t s_report_map[] = {
    0x05, 0x01, /* Usage Page (Generic Desktop) */
    0x09, 0x06, /* Usage (Keyboard) */
    0xA1, 0x01, /* Collection (Application) */
    0x05, 0x07, /*   Usage Page (Key Codes) */
    0x19, 0xE0, /*   Usage Minimum (224) */
    0x29, 0xE7, /*   Usage Maximum (231) */
    0x15, 0x00, /*   Logical Minimum (0) */
    0x25, 0x01, /*   Logical Maximum (1) */
    0x75, 0x01, /*   Report Size (1) */
    0x95, 0x08, /*   Report Count (8) */
    0x81, 0x02, /*   Input (Data, Variable, Absolute) ; modifier byte */
    0x95, 0x01, /*   Report Count (1) */
    0x75, 0x08, /*   Report Size (8) */
    0x81, 0x01, /*   Input (Constant) ; reserved byte */
    0x95, 0x06, /*   Report Count (6) */
    0x75, 0x08, /*   Report Size (8) */
    0x15, 0x00, /*   Logical Minimum (0) */
    0x25, 0x65, /*   Logical Maximum (101) */
    0x05, 0x07, /*   Usage Page (Key Codes) */
    0x19, 0x00, /*   Usage Minimum (0) */
    0x29, 0x65, /*   Usage Maximum (101) */
    0x81, 0x00, /*   Input (Data, Array) ; keycodes */
    0xC0,       /* End Collection */
};

/* HID Information: bcdHID=0x0111, country=0, flags=RemoteWake|NormallyConnectable. */
static const uint8_t s_hid_info[] = {0x11, 0x01, 0x00, 0x03};

/* PnP ID: vendor source=USB-IF(0x02), VID 0x1915, PID 0xEEEE, version 0x0001. */
static const uint8_t s_pnp_id[] = {0x02, 0x15, 0x19, 0xEE, 0xEE, 0x01, 0x00};

/* Report Reference descriptor value: Report ID 0, type=Input(0x01). */
static const uint8_t s_report_ref[] = {0x00, 0x01};

static uint8_t s_protocol_mode = 0x01; /* Report Protocol */
static uint8_t s_batt_level    = 100;

/* ------------------------------------------------------------ state */

static uint16_t s_conn_handle    = 0xFFFF;
static uint16_t s_input_report_h = 0; /* Input Report value handle (out) */
static bool     s_connected      = false;
static bool     s_encrypted      = false;

/* ------------------------------------------------------------ GATT access */

static int gatt_access_static(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const uint8_t *data = NULL;
    uint16_t       len  = 0;

    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
    case BLE_GATT_ACCESS_OP_READ_DSC: {
        uint16_t uuid16 = ble_uuid_u16(ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR ? ctxt->chr->uuid : ctxt->dsc->uuid);
        switch (uuid16) {
        case UUID_CHR_REPORT_MAP:
            data = s_report_map;
            len  = sizeof(s_report_map);
            break;
        case UUID_CHR_HID_INFO:
            data = s_hid_info;
            len  = sizeof(s_hid_info);
            break;
        case UUID_CHR_PNP_ID:
            data = s_pnp_id;
            len  = sizeof(s_pnp_id);
            break;
        case UUID_CHR_PROTO_MODE:
            data = &s_protocol_mode;
            len  = 1;
            break;
        case UUID_CHR_BATT_LEVEL:
            data = &s_batt_level;
            len  = 1;
            break;
        case UUID_CHR_REPORT:
            len = 0; /* empty report on read */
            break;
        case UUID_DSC_REPORT_REF:
            data = s_report_ref;
            len  = sizeof(s_report_ref);
            break;
        default:
            return BLE_ATT_ERR_UNLIKELY;
        }
        if (len && data) {
            int rc = os_mbuf_append(ctxt->om, data, len);
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return 0;
    }
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        /* HID Control Point / Protocol Mode writes: accept and ignore for spike. */
        return 0;
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

/* Report Reference descriptor for the Input Report characteristic. */
static struct ble_gatt_dsc_def s_report_dscs[] = {
    {
        .uuid      = BLE_UUID16_DECLARE(UUID_DSC_REPORT_REF),
        .att_flags = BLE_ATT_F_READ,
        .access_cb = gatt_access_static,
    },
    {0},
};

/* HID Service characteristics. */
static struct ble_gatt_chr_def s_hid_chrs[] = {
    {
        .uuid      = BLE_UUID16_DECLARE(UUID_CHR_REPORT_MAP),
        .access_cb = gatt_access_static,
        .flags     = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
    },
    {
        .uuid      = BLE_UUID16_DECLARE(UUID_CHR_HID_INFO),
        .access_cb = gatt_access_static,
        .flags     = BLE_GATT_CHR_F_READ,
    },
    {
        .uuid      = BLE_UUID16_DECLARE(UUID_CHR_HID_CTRL_PT),
        .access_cb = gatt_access_static,
        .flags     = BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid      = BLE_UUID16_DECLARE(UUID_CHR_PROTO_MODE),
        .access_cb = gatt_access_static,
        .flags     = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        /* Input Report: encrypted read + notify, with Report Reference desc. */
        .uuid        = BLE_UUID16_DECLARE(UUID_CHR_REPORT),
        .access_cb   = gatt_access_static,
        .descriptors = s_report_dscs,
        .flags       = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
        .val_handle  = &s_input_report_h,
    },
    {0},
};

/* Device Information Service characteristics. */
static struct ble_gatt_chr_def s_dis_chrs[] = {
    {
        .uuid      = BLE_UUID16_DECLARE(UUID_CHR_PNP_ID),
        .access_cb = gatt_access_static,
        .flags     = BLE_GATT_CHR_F_READ,
    },
    {0},
};

/* Battery Service characteristics. */
static struct ble_gatt_chr_def s_bas_chrs[] = {
    {
        .uuid      = BLE_UUID16_DECLARE(UUID_CHR_BATT_LEVEL),
        .access_cb = gatt_access_static,
        .flags     = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
    },
    {0},
};

/* Full GATT service table. */
static struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = BLE_UUID16_DECLARE(UUID_SVC_HID),
        .characteristics = s_hid_chrs,
    },
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = BLE_UUID16_DECLARE(UUID_SVC_DIS),
        .characteristics = s_dis_chrs,
    },
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = BLE_UUID16_DECLARE(UUID_SVC_BAS),
        .characteristics = s_bas_chrs,
    },
    {0},
};

/* ------------------------------------------------------------ SM bond store
 * Minimal bond persistence stubs so the SM key-distribution path links and the
 * store callbacks are wired. A real implementation (spec T3) backs these with
 * tal_kv; for the spike they satisfy the linker and return "not found". */

static int bond_store_read(int obj_type, const union ble_store_key *key, union ble_store_value *value)
{
    (void)obj_type;
    (void)key;
    (void)value;
    return BLE_HS_ENOENT;
}

static int bond_store_write(int obj_type, const union ble_store_value *val)
{
    (void)obj_type;
    (void)val;
    return 0;
}

static int bond_store_delete(int obj_type, const union ble_store_key *key)
{
    (void)obj_type;
    (void)key;
    return 0;
}

/* ------------------------------------------------------------ SM config */

static void configure_security(void)
{
    /* Turn on pairing/bonding with LE Secure Connections, Just Works
     * (NO_INPUT_OUTPUT). HOGP requires encryption before Report reads; the
     * Input Report's _READ_ENC flag triggers the central to pair. */
    /* This SDK names the global config struct tuya_ble_hs_cfg (no ble_hs_cfg
     * alias); see ble_hs.h:286. */
    tuya_ble_hs_cfg.sm_io_cap         = BLE_HS_IO_NO_INPUT_OUTPUT;
    tuya_ble_hs_cfg.sm_bonding        = 1;
    tuya_ble_hs_cfg.sm_sc             = 1;
    tuya_ble_hs_cfg.sm_mitm           = 0;
    tuya_ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    tuya_ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    tuya_ble_hs_cfg.store_read_cb   = bond_store_read;
    tuya_ble_hs_cfg.store_write_cb  = bond_store_write;
    tuya_ble_hs_cfg.store_delete_cb = bond_store_delete;
}

/* ------------------------------------------------------------ advertising */

static int hid_gap_event(struct ble_gap_event *event, void *arg);

static void start_advertising(void)
{
    struct ble_hs_adv_fields  fields   = {0};
    struct ble_gap_adv_params adv      = {0};
    ble_uuid16_t              hid_uuid = BLE_UUID16_INIT(UUID_SVC_HID);

    fields.flags                 = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.appearance            = BLE_APPEARANCE_KEYBOARD;
    fields.appearance_is_present = 1;
    fields.uuids16               = &hid_uuid;
    fields.num_uuids16           = 1;
    fields.uuids16_is_complete   = 1;
    fields.name                  = (const uint8_t *)BLE_HID_NAME;
    fields.name_len              = (uint8_t)strlen(BLE_HID_NAME);
    fields.name_is_complete      = 1;
    ble_gap_adv_set_fields(&fields);

    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv, hid_gap_event, NULL);
}

/* ------------------------------------------------------------ GAP events */

static int hid_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_connected   = true;
        } else {
            start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        s_connected   = false;
        s_encrypted   = false;
        s_conn_handle = 0xFFFF;
        start_advertising();
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        s_encrypted = (event->enc_change.status == 0);
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
    default:
        break;
    }
    return 0;
}

/* ------------------------------------------------------------ send a key */

/* Send one keyboard Input Report: press (8 bytes) then release (all zero). */
static bool hid_send_report(uint8_t modifier, uint8_t keycode)
{
    uint8_t         report[8] = {modifier, 0, keycode, 0, 0, 0, 0, 0};
    struct os_mbuf *om;

    if (!s_connected || !s_encrypted || s_input_report_h == 0) {
        return false;
    }
    om = ble_hs_mbuf_from_flat(report, sizeof(report));
    if (om) {
        ble_gattc_notify_custom(s_conn_handle, s_input_report_h, om);
    }
    memset(report, 0, sizeof(report));
    om = ble_hs_mbuf_from_flat(report, sizeof(report));
    if (om) {
        ble_gattc_notify_custom(s_conn_handle, s_input_report_h, om);
    }
    return true;
}

/* Minimal ASCII->HID keycode for the demo string; unsupported chars skip. */
static uint8_t ascii_to_keycode(char c, uint8_t *modifier)
{
    *modifier = 0;
    if (c >= 'a' && c <= 'z') {
        return (uint8_t)(0x04 + (c - 'a'));
    }
    if (c >= 'A' && c <= 'Z') {
        *modifier = 0x02; /* LeftShift */
        return (uint8_t)(0x04 + (c - 'A'));
    }
    if (c >= '1' && c <= '9') {
        return (uint8_t)(0x1E + (c - '1'));
    }
    if (c == '0') {
        return 0x27;
    }
    if (c == ' ') {
        return 0x2C;
    }
    return 0;
}

/* ------------------------------------------------------------ bridge API */

void ak_ble_hid_init(void)
{
    configure_security();
    ble_svc_gap_device_name_set(BLE_HID_NAME);
    ble_svc_gap_device_appearance_set(BLE_APPEARANCE_KEYBOARD);

    if (ble_gatts_count_cfg(s_gatt_svcs) != 0) {
        return;
    }
    if (ble_gatts_add_svcs(s_gatt_svcs) != 0) {
        return;
    }
    start_advertising();
}

bool ak_ble_hid_ready(void)
{
    return s_connected && s_encrypted;
}

bool ak_ble_hid_connected(void)
{
    return s_connected;
}

bool ak_ble_hid_send_char(char c)
{
    uint8_t mod;
    uint8_t kc = ascii_to_keycode(c, &mod);

    if (kc == 0) {
        return false;
    }
    return hid_send_report(mod, kc);
}

bool ak_ble_hid_send_key(uint8_t mod, uint8_t keycode)
{
    return hid_send_report(mod, keycode);
}

void ak_ble_hid_send_text(const char *s)
{
    if (s == NULL) {
        return;
    }
    for (; *s; s++) {
        (void)ak_ble_hid_send_char(*s);
    }
}
