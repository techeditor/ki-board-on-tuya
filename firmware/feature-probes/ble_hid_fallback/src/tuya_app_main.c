/**
 * @file tuya_app_main.c
 * @brief FP-6 ble-hid-fallback (T1 spike) — standard BLE HID keyboard bring-up.
 *
 * GOAL (compile+link spike): prove the TuyaOpen T5AI toolchain can build and
 * link a COMPLIANT BLE HID (HOGP / HID-over-GATT) keyboard using the vendored
 * native NimBLE host — the thing Tuya's TAL command-service layer (fixed
 * 0x1910, no descriptor field) cannot express. The spike success bar is
 * "tos.py build => BUILD SUCCESS"; real pairing/typing is verified on hardware
 * by the user later (spec tasks T3/T5).
 *
 * What this file exercises so the linker must resolve every HOGP symbol:
 *   - a custom HID Service 0x1812 registered via native
 *     ble_gatts_count_cfg() + ble_gatts_add_svcs() with:
 *       * Report Map     0x2A4B  (static keyboard boot-report descriptor)
 *       * HID Information 0x2A4A
 *       * HID Ctrl Point  0x2A4C
 *       * Protocol Mode   0x2A4E
 *       * Input Report    0x2A4D  READ|NOTIFY|READ_ENC, carrying a
 *                                 Report Reference descriptor 0x2908 (+ auto CCCD)
 *   - Device Information Service 0x180A (PnP ID 0x2A50) + Battery 0x180F
 *   - Security Manager pairing/bonding turned on via ble_hs_cfg.sm_* +
 *     ble_store_* bond callbacks (Just Works, LE Secure Connections)
 *   - advertising as a HID keyboard: appearance 0x03C1 + service UUID 0x1812,
 *     driven with native ble_gap_adv_set_fields()/ble_gap_adv_start()
 *   - sending a keyboard Input Report with ble_gatts_notify_custom()
 *
 * SPIKE SCOPING (deliberate, documented in the spec's design D8-R4): host-stack
 * bring-up is delegated to tal_ble_bt_init() — the SAME proven path FP-2 uses —
 * and the HID service + SM config + native advertising are ADDED on top inside
 * the TAL_BLE_STACK_INIT event. Replicating tkl_bluetooth.c's private init
 * sequence (tuya_ble_pre_init/ble_hs_sched_start, host/-private symbols not on
 * the public include path) is left to spec task T2, and would risk the
 * double-init hazard R4 warns about. This spike proves the RISKY part — that
 * the native HID GATT + descriptor + SM symbols are present and linkable in the
 * T5AI build — which is exactly what a GO/NO-GO gate needs.
 *
 * A tiny status LCD shows advertise/connect/encrypted state so the board is
 * readable without a serial cable, per project convention.
 */

#include <stdio.h>
#include <string.h>

#include "ak_gfx.h"
#include "ak_input.h"
#include "ak_statuslcd.h"

#include "tal_api.h"
#include "tal_bluetooth.h"
#include "tkl_output.h"

/* Native NimBLE host headers — exported on T5AI because boards/T5AI selects
 * ENABLE_NIMBLE, which puts src/tal_bluetooth/nimble/include on the public
 * include path (src/tal_bluetooth/CMakeLists.txt). These declare the HOGP
 * primitives TAL's tal_bluetooth.h does not export. */
#include "ble_gap.h"
#include "ble_gatt.h"
#include "ble_hs.h"
#include "ble_hs_adv.h"
#include "ble_hs_mbuf.h"
#include "ble_svc_gap.h"
#include "ble_uuid.h"
#include "tuya_ble_mbuf.h"

#define APP_TASK_STACK_SIZE     (6U * 1024U)
#define POLL_INTERVAL_MS        5U
#define BLE_HID_NAME            "AK-KB-HID"
#define BLE_APPEARANCE_KEYBOARD 0x03C1 /* HID subtype: Keyboard */

/* ------------------------------------------------------------ HID constants */

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
 * [modifiers][reserved][keycode x6]. This is the canonical HOGP keyboard
 * descriptor. */
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

static THREAD_HANDLE s_app_thread     = NULL;
static uint16_t      s_conn_handle    = 0xFFFF;
static uint16_t      s_input_report_h = 0; /* Input Report value handle (out) */
static bool          s_connected      = false;
static bool          s_encrypted      = false;

/* ------------------------------------------------------------ GATT access */

static int gatt_access_static(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const uint8_t *data = NULL;
    uint16_t       len  = 0;

    (void)conn_handle;
    (void)attr_handle;

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
            len = 0;
            break; /* empty report on read */
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
    ble_hs_cfg.sm_io_cap         = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding        = 1;
    ble_hs_cfg.sm_sc             = 1;
    ble_hs_cfg.sm_mitm           = 0;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_hs_cfg.store_read_cb   = bond_store_read;
    ble_hs_cfg.store_write_cb  = bond_store_write;
    ble_hs_cfg.store_delete_cb = bond_store_delete;
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

/* ------------------------------------------------------------ status LCD */

static void draw_status(void)
{
    const char *state;
    uint16_t    bar;

    if (!ak_statuslcd_ready()) {
        return;
    }
    if (s_connected && s_encrypted) {
        state = "PAIRED";
        bar   = AK_COL_GREEN;
    } else if (s_connected) {
        state = "CONNECTED";
        bar   = AK_COL_DKBLUE;
    } else {
        state = "ADVERTISING";
        bar   = AK_COL_ORANGE;
    }
    ak_statuslcd_title("FP-6 ble-hid-fallback", bar);
    ak_gfx_text(6, AK_STATUSLCD_CONTENT_Y + 4, "HID " BLE_HID_NAME, AK_COL_GRAY, AK_COL_BLACK, 1);
    ak_gfx_text(6, AK_STATUSLCD_CONTENT_Y + 18, state, AK_COL_YELLOW, AK_COL_BLACK, 2);
    ak_gfx_text(6, AK_STATUSLCD_CONTENT_Y + 44, "svc 0x1812  appr 0x03C1", AK_COL_CYAN, AK_COL_BLACK, 1);
    ak_gfx_text(6, AK_STATUSLCD_H - 12, "voice=type  send=Enter", AK_COL_GRAY, AK_COL_BLACK, 1);
    ak_statuslcd_flush();
}

/* ------------------------------------------------------------ send a key */

/* Send one keyboard Input Report: press (8 bytes) then release (all zero). */
static void hid_send_key(uint8_t modifier, uint8_t keycode)
{
    uint8_t         report[8] = {modifier, 0, keycode, 0, 0, 0, 0, 0};
    struct os_mbuf *om;

    if (!s_connected || !s_encrypted || s_input_report_h == 0) {
        return;
    }
    om = ble_hs_mbuf_from_flat(report, sizeof(report));
    if (om) {
        ble_gatts_notify_custom(s_conn_handle, s_input_report_h, om);
    }
    memset(report, 0, sizeof(report));
    om = ble_hs_mbuf_from_flat(report, sizeof(report));
    if (om) {
        ble_gatts_notify_custom(s_conn_handle, s_input_report_h, om);
    }
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
            PR_NOTICE("HID central connected, conn=%u", s_conn_handle);
        } else {
            start_advertising();
        }
        draw_status();
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        s_connected   = false;
        s_encrypted   = false;
        s_conn_handle = 0xFFFF;
        PR_NOTICE("HID central disconnected, re-advertising");
        start_advertising();
        draw_status();
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        s_encrypted = (event->enc_change.status == 0);
        PR_NOTICE("encryption change: status=%d encrypted=%d", event->enc_change.status, s_encrypted);
        draw_status();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        PR_NOTICE("subscribe: attr=%u notify=%d", event->subscribe.attr_handle, event->subscribe.cur_notify);
        break;

    default:
        break;
    }
    return 0;
}

/* ------------------------------------------------------------ HID bring-up */

/* Register the HID GATT table + SM config. Called once the host stack is up. */
static void hid_gatt_register(void)
{
    int rc;

    configure_security();
    ble_svc_gap_device_name_set(BLE_HID_NAME);
    ble_svc_gap_device_appearance_set(BLE_APPEARANCE_KEYBOARD);

    rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        PR_ERR("ble_gatts_count_cfg failed: %d", rc);
        return;
    }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        PR_ERR("ble_gatts_add_svcs failed: %d", rc);
        return;
    }
    PR_NOTICE("HID GATT registered (0x1812 + DIS + BAS), input report handle pending");

    start_advertising();
}

/* ------------------------------------------------------------ BLE events (TAL) */

static void ble_event_cb(TAL_BLE_EVT_PARAMS_T *event)
{
    if (event == NULL) {
        return;
    }
    switch (event->type) {
    case TAL_BLE_STACK_INIT:
        if (event->ble_event.init == 0) {
            PR_NOTICE("BLE host up; registering HID service");
            hid_gatt_register();
            draw_status();
        } else {
            PR_ERR("BLE init failed: %d", event->ble_event.init);
        }
        break;
    default:
        /* HID connection/security handled by the native GAP event cb registered
         * in start_advertising(); TAL's peripheral events are unused here. */
        break;
    }
}

/* ------------------------------------------------------------ input -> keys */

/* Minimal ASCII->HID keycode for the spike demo string; unsupported chars skip. */
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

static void send_text(const char *s)
{
    for (; *s; s++) {
        uint8_t mod;
        uint8_t kc = ascii_to_keycode(*s, &mod);
        if (kc) {
            hid_send_key(mod, kc);
        }
    }
}

static void on_input(ak_input_kind_t kind, const char *id, ak_input_event_t event, uint32_t held_ms)
{
    const char *action = ak_input_event_str(event);
    (void)held_ms;

    if (action[0] == '\0') {
        return;
    }
    /* Spike demo mapping (design D6): a key -> send text, another -> Enter. */
    if (kind == AK_INPUT_KIND_KEY) {
        if (strcmp(id, "k1") == 0) {
            send_text("hello kiro");
        } else if (strcmp(id, "k2") == 0) {
            hid_send_key(0, 0x28); /* Enter */
        } else if (strcmp(id, "k3") == 0) {
            hid_send_key(0, 0x2A); /* Backspace */
        }
    } else if (kind == AK_INPUT_KIND_NAV) {
        if (strcmp(id, "left") == 0) {
            hid_send_key(0x01, 0x2B); /* LeftCtrl+Tab */
        } else if (strcmp(id, "right") == 0) {
            hid_send_key(0x01, 0x2B);
        }
    }
}

/* ------------------------------------------------------------ main */

static void user_main(void)
{
    OPERATE_RET rt;

    tal_log_init(TAL_LOG_LEVEL_DEBUG, 2048, (TAL_LOG_OUTPUT_CB)tkl_log_output);
    PR_NOTICE("======== Agentic Keyboard FP-6: ble-hid-fallback (T1 spike) ========");
    PR_NOTICE("Built: %s %s", __DATE__, __TIME__);

    rt = ak_input_init();
    if (rt != OPRT_OK) {
        PR_ERR("Input init failed: %d", rt);
    }
    rt = ak_statuslcd_init();
    if (rt != OPRT_OK) {
        PR_ERR("Status LCD init failed: %d (continuing headless)", rt);
    }
    draw_status();

    tal_kv_init(&(tal_kv_cfg_t){.seed = "agentic-fp6", .key = "ble-hid-fallback"});
    tal_sw_timer_init();
    tal_workq_init();

    /* Host-stack bring-up delegated to TAL (spike scoping). The HID service and
     * SM config are added in ble_event_cb on TAL_BLE_STACK_INIT. */
    rt = tal_ble_bt_init(TAL_BLE_ROLE_PERIPERAL, ble_event_cb);
    if (rt != OPRT_OK) {
        PR_ERR("BLE init failed: %d", rt);
        return;
    }

    while (1) {
        ak_input_poll(tal_system_get_millisecond(), on_input);
        tal_system_sleep(POLL_INTERVAL_MS);
    }
}

static void app_thread(void *arg)
{
    (void)arg;
    user_main();
    tal_thread_delete(s_app_thread);
    s_app_thread = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T thread_cfg = {
        .stackDepth = APP_TASK_STACK_SIZE,
        .priority   = THREAD_PRIO_1,
        .thrdname   = "fp6_ble_hid",
    };
    tal_thread_create_and_start(&s_app_thread, NULL, NULL, app_thread, NULL, &thread_cfg);
}
