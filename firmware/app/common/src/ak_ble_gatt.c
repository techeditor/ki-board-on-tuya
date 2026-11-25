/**
 * @file ak_ble_gatt.c
 * @brief BLE GATT transport backend for the Agentic Keyboard product firmware.
 *
 * See ak_ble_gatt.h. Ported from an earlier internal prototype that was
 * verified on real hardware (FP-2 ble_gatt_link):
 *
 *   - advertise local name "Ki-board-XXXXXX" + service UUID,
 *   - use Tuya's built-in BLE common service selected by TAL,
 *   - NOTIFY JSONL frames via tal_ble_server_common_send() only while subscribed,
 *   - receive companion writes in TAL_BLE_EVT_WRITE_REQ -> write_report.report
 *     and push them into an internal RX ring the app drains via ak_ble_gatt_read.
 *
 * The event callback runs in the BLE stack context; the app main loop calls
 * ak_ble_gatt_read()/ak_ble_gatt_write() from its own thread, so the RX ring
 * head/tail are the only shared state. On this single-core target the ring is
 * a lock-free SPSC buffer (callback = producer, app loop = consumer) with
 * volatile indices; a byte lost under a simultaneous wrap is tolerable for a
 * best-effort link and never corrupts the buffer structure.
 */

#include "ak_ble_gatt.h"
#include "ak_byte_ring.h"

#include <stdio.h>
#include <string.h>

#include "tal_api.h"
#include "tal_bluetooth.h"
#include "tal_wifi.h"
#include "tkl_bluetooth.h"

/* NimBLE exposes this public host-state query, but including ble_gap.h in the
 * same translation unit as Tuya TAL/TKL headers redefines LIST_HEAD and pulls
 * an unexported tuya_ble_mempool.h. Keep the exact public signature here so the
 * BLE-specific module can query state without crossing that SDK packaging bug. */
extern int ble_gap_adv_active(void);

#define BLE_NAME_PREFIX          "Ki-board-" /* Ki-board-D3E4F5; 15 chars, scan response 17 of 31 bytes */
#define BLE_NAME_SUFFIX_HEX_LEN  6U
#define BLE_NAME_MAX_LEN         ((sizeof(BLE_NAME_PREFIX) - 1U) + BLE_NAME_SUFFIX_HEX_LEN)
#define SERVER_MTU                 247U
#define ATT_DEFAULT_MTU             23U
#define ATT_NOTIFICATION_OVERHEAD   3U
#define ATT_DEFAULT_PAYLOAD         (ATT_DEFAULT_MTU - ATT_NOTIFICATION_OVERHEAD)
#define ADV_MAINTAIN_INTERVAL_MS   1000U
/* shortcut: Tuya's ble_mgr monitor runs every 3 s and rewrites the advertisement
 * on IoT transitions; re-apply 4 s after such an event. Upgrade: read the
 * controller's advertising data back and compare, if the SDK exposes it. */
#define ADV_SETTLE_MS              4000U
#define INVALID_BLE_HANDLE         0xFFFFU

/* RX ring: companion writes (0x2B11) land here for ak_ble_gatt_read. SPSC
 * (BLE callback produces, app loop consumes); 512 B holds several MTU writes
 * between app-loop drains (~5 ms). See ak_byte_ring.h (FW-08). */
static ak_byte_ring_t s_rx_ring;

static volatile bool       s_connected;
static volatile bool       s_subscribed;
static bool                s_tkl_takeover;
static volatile uint16_t   s_conn_handle;
static volatile uint16_t   s_notify_handle;
static volatile uint16_t   s_notify_payload_max;
static uint32_t            s_last_adv_maintain_ms;
static volatile bool       s_adv_settle_pending;
static volatile uint32_t   s_adv_settle_due_ms;
static bool                s_usb_suspended;
static volatile bool       s_greet_pending; /* set on subscribe, cleared by take_greet */
static TAL_BLE_PEER_INFO_T s_peer;

/* Advertising: flags + complete list of the 16-bit TAL command service. */
static uint8_t s_adv_data[] = {
    0x02, 0x01, 0x06, /* flags: LE General Discoverable, BR/EDR not supported */
    0x03, 0x03,
    (uint8_t)(AK_BLE_GATT_SERVICE_UUID & 0xFFU),
    (uint8_t)(AK_BLE_GATT_SERVICE_UUID >> 8),
};

/* Scan response is generated at runtime as Ki-board-XXXXXX,
 * where XXXXXX normally comes from the device Wi-Fi MAC suffix. */
static char    s_ble_name[BLE_NAME_MAX_LEN + 1U];
static uint8_t s_scan_rsp[BLE_NAME_MAX_LEN + 2U];
static uint8_t s_scan_rsp_len;

/* ------------------------------------------------------------ RX ring */

/* Queue one GATT write, only from the connection that is live for AK. */
static void rx_ring_push(uint16_t conn_handle, const uint8_t *data, uint32_t n)
{
    if (!s_connected || conn_handle != s_conn_handle || data == NULL || n == 0U) {
        return; /* stale or foreign connection */
    }
    (void)ak_byte_ring_push(&s_rx_ring, data, n); /* too big for the room left: dropped whole */
}

/* ------------------------------------------------------------ advertising */

static void update_notify_payload(uint16_t peer_mtu)
{
    uint16_t effective_mtu = peer_mtu < SERVER_MTU ? peer_mtu : SERVER_MTU;
    if (effective_mtu < ATT_DEFAULT_MTU) {
        effective_mtu = ATT_DEFAULT_MTU;
    }
    s_notify_payload_max = (uint16_t)(effective_mtu - ATT_NOTIFICATION_OVERHEAD);
}

static bool device_address_valid(const uint8_t address[6])
{
    bool any_nonzero = false;
    bool any_not_ff  = false;
    for (uint8_t i = 0U; i < 6U; i++) {
        any_nonzero = any_nonzero || address[i] != 0U;
        any_not_ff  = any_not_ff || address[i] != 0xFFU;
    }
    return any_nonzero && any_not_ff;
}

static void prepare_device_name(void)
{
    if (s_scan_rsp_len != 0U) {
        return;
    }

    uint8_t  suffix[3];
    NW_MAC_S wifi_mac = {0};
    if (tal_wifi_get_mac(WF_STATION, &wifi_mac) == OPRT_OK &&
        device_address_valid(wifi_mac.mac)) {
        memcpy(suffix, &wifi_mac.mac[3], sizeof(suffix));
    } else {
        TKL_BLE_GAP_ADDR_T ble_address = {0};
        if (tkl_ble_gap_address_get(&ble_address) == OPRT_OK &&
            device_address_valid(ble_address.addr)) {
            memcpy(suffix, &ble_address.addr[3], sizeof(suffix));
        } else {
            uint32_t random_suffix = (uint32_t)tal_system_get_random(0xFFFFFFU);
            suffix[0] = (uint8_t)(random_suffix >> 16);
            suffix[1] = (uint8_t)(random_suffix >> 8);
            suffix[2] = (uint8_t)random_suffix;
            PR_WARN("device address unavailable; BLE name suffix is temporary");
        }
    }

    int name_len = snprintf(s_ble_name, sizeof(s_ble_name),
                            BLE_NAME_PREFIX "%02X%02X%02X",
                            suffix[0], suffix[1], suffix[2]);
    if (name_len < 0 || (size_t)name_len > BLE_NAME_MAX_LEN) {
        /* Compile-time sizes make this unreachable; keep the AD structure safe
         * if the prefix or suffix format changes later. */
        name_len = 0;
        s_ble_name[0] = '\0';
    }
    s_scan_rsp[0] = (uint8_t)(name_len + 1);
    s_scan_rsp[1] = 0x09; /* Complete Local Name. */
    memcpy(&s_scan_rsp[2], s_ble_name, (size_t)name_len);
    s_scan_rsp_len = (uint8_t)(name_len + 2);
}

static OPERATE_RET start_advertising(void)
{
    if (s_usb_suspended) {
        return OPRT_OK;
    }

    prepare_device_name();
    TAL_BLE_DATA_T adv = {.len = sizeof(s_adv_data), .p_data = s_adv_data};
    TAL_BLE_DATA_T rsp = {.len = s_scan_rsp_len, .p_data = s_scan_rsp};
    /* update works both while another advertiser is active and while stopped;
     * data_set alone may fail to replace Tuya's already-running advertisement. */
    OPERATE_RET rt = tal_ble_advertising_data_update(&adv, &rsp);
    if (rt != OPRT_OK) {
        return rt;
    }

    /* The old watchdog called start once per second even while advertising,
     * making NimBLE return rc=2 (already active) and flooding UART1. Query the
     * real host state instead of relying on a local flag: Tuya's BLE manager
     * can stop advertising behind our callback during the ownership handoff. */
    if (ble_gap_adv_active()) {
        return OPRT_OK;
    }
    return tal_ble_advertising_start(TUYAOS_BLE_DEFAULT_ADV_PARAM);
}

/* ------------------------------------------------------------ BLE events */

static void ble_event_cb(TAL_BLE_EVT_PARAMS_T *event)
{
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case TAL_BLE_STACK_INIT:
        if (event->ble_event.init == 0) {
            start_advertising();
            PR_NOTICE("BLE ready, advertising as %s", s_ble_name);
        } else {
            PR_ERR("BLE init failed: %d", event->ble_event.init);
        }
        break;

    case TAL_BLE_EVT_PERIPHERAL_CONNECT:
        if (event->ble_event.connect.result == OPRT_OK) {
            s_peer        = event->ble_event.connect.peer;
            s_conn_handle = s_peer.conn_handle;
            s_connected   = true;
            s_subscribed = false;
            PR_NOTICE("BLE central connected");
        } else {
            s_connected  = false;
            s_subscribed = false;
            PR_WARN("BLE central connect failed: %d; continuing advertising",
                    event->ble_event.connect.result);
            start_advertising();
        }
        break;

    case TAL_BLE_EVT_MTU_REQUEST:
        update_notify_payload(event->ble_event.exchange_mtu.mtu);
        (void)tal_ble_server_exchange_mtu_reply(s_peer, SERVER_MTU);
        PR_NOTICE("BLE MTU=%u, notify payload=%u", (unsigned)event->ble_event.exchange_mtu.mtu,
                  (unsigned)s_notify_payload_max);
        break;

    case TAL_BLE_EVT_SUBSCRIBE:
        s_subscribed = event->ble_event.subscribe.cur_notify ? true : false;
        if (s_subscribed) {
            /* Greet on subscribe, not at init: latch for the app to send hello_ack. */
            s_greet_pending = true;
            PR_NOTICE("BLE notify subscribed; hello_ack pending");
        }
        break;

    case TAL_BLE_EVT_WRITE_REQ: {
        TAL_BLE_DATA_T *rx = &event->ble_event.write_report.report;
        /* Only the common write characteristic of the live connection. */
        if (event->ble_event.write_report.peer.char_handle[0] ==
            s_peer.char_handle[TAL_COMMON_WRITE_CHAR_INDEX]) {
            rx_ring_push(event->ble_event.write_report.peer.conn_handle, rx->p_data, rx->len);
        }
        break;
    }

    case TAL_BLE_EVT_DISCONNECT:
        s_connected   = false;
        s_subscribed  = false;
        s_conn_handle = INVALID_BLE_HANDLE;
        ak_byte_ring_cut(&s_rx_ring); /* nothing from this central reaches the next */
        PR_NOTICE("BLE disconnected (reason 0x%02X); re-advertising", (unsigned)event->ble_event.disconnect.reason);
        start_advertising();
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------ TKL takeover events */

/* Cloud DUAL mode reaches here after netmgr has already initialized TAL and
 * registered the common service. Re-register only the platform callbacks: a
 * second tal_ble_bt_init() rewrites TAL's service table with INVALID handles,
 * while T5's one-shot service guard skips the handle repopulation. */
static void tkl_gap_event_cb(TKL_BLE_GAP_PARAMS_EVT_T *event)
{
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case TKL_BLE_GAP_EVT_CONNECT:
        if (event->result == OPRT_OK && event->gap_event.connect.role == TKL_BLE_ROLE_SERVER) {
            s_conn_handle  = event->conn_handle;
            s_notify_handle = INVALID_BLE_HANDLE;
            s_connected    = true;
            s_subscribed   = false;
            PR_NOTICE("BLE central connected (TKL takeover)");
        }
        break;

    case TKL_BLE_GAP_EVT_DISCONNECT:
        if (event->conn_handle == s_conn_handle) {
            s_conn_handle   = INVALID_BLE_HANDLE;
            s_notify_handle = INVALID_BLE_HANDLE;
            s_connected     = false;
            s_subscribed    = false;
            ak_byte_ring_cut(&s_rx_ring);
            PR_NOTICE("BLE disconnected (reason 0x%02X); re-advertising",
                      (unsigned)event->gap_event.disconnect.reason);
            start_advertising();
        }
        break;

    case TKL_BLE_EVT_STACK_INIT:
        if (event->result == OPRT_OK) {
            start_advertising();
        }
        break;

    default:
        break;
    }
}

static void tkl_gatt_event_cb(TKL_BLE_GATT_PARAMS_EVT_T *event)
{
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case TKL_BLE_GATT_EVT_MTU_REQUEST:
        update_notify_payload(event->gatt_event.exchange_mtu);
        (void)tkl_ble_gatts_exchange_mtu_reply(event->conn_handle, SERVER_MTU);
        PR_NOTICE("BLE MTU=%u, notify payload=%u", (unsigned)event->gatt_event.exchange_mtu,
                  (unsigned)s_notify_payload_max);
        break;

    case TKL_BLE_GATT_EVT_SUBSCRIBE:
        if (event->result == OPRT_OK && event->gatt_event.subscribe.cur_notify) {
            s_conn_handle   = event->conn_handle;
            s_notify_handle = event->gatt_event.subscribe.char_handle;
            s_connected     = true;
            s_subscribed    = true;
            s_greet_pending = true;
            PR_NOTICE("BLE notify subscribed; handle=0x%04X; hello_ack pending",
                      (unsigned)s_notify_handle);
        } else if (event->conn_handle == s_conn_handle) {
            s_subscribed    = false;
            s_notify_handle = INVALID_BLE_HANDLE;
        }
        break;

    case TKL_BLE_GATT_EVT_WRITE_REQ: {
        /* Only the subscribed AK connection, and never the notify handle. */
        TKL_BLE_DATA_T *rx = &event->gatt_event.write_report.report;
        if (s_subscribed && event->gatt_event.write_report.char_handle != s_notify_handle) {
            rx_ring_push(event->conn_handle, rx->p_data, rx->length);
        }
        break;
    }

    default:
        break;
    }
}

/* ------------------------------------------------------------ interface */

static void reset_link_state(void)
{
    ak_byte_ring_init(&s_rx_ring);
    s_connected            = false;
    s_subscribed           = false;
    s_conn_handle          = INVALID_BLE_HANDLE;
    s_notify_handle        = INVALID_BLE_HANDLE;
    s_notify_payload_max   = ATT_DEFAULT_PAYLOAD;
    s_last_adv_maintain_ms = 0;
    s_usb_suspended       = false;
    s_greet_pending        = false;
}

OPERATE_RET ak_ble_gatt_init(void)
{
    reset_link_state();
    s_tkl_takeover = false;
    return tal_ble_bt_init(TAL_BLE_ROLE_PERIPERAL, ble_event_cb);
}

OPERATE_RET ak_ble_gatt_takeover(void)
{
    OPERATE_RET rt;

    reset_link_state();
    s_tkl_takeover = true;

    rt = tkl_ble_gap_callback_register(tkl_gap_event_cb);
    if (rt != OPRT_OK) {
        return rt;
    }
    rt = tkl_ble_gatt_callback_register(tkl_gatt_event_cb);
    if (rt != OPRT_OK) {
        return rt;
    }

    /* The stack-init event normally happened inside netmgr_init() before this
     * takeover, so advertise immediately instead of waiting for another one. */
    start_advertising();
    ak_ble_gatt_adv_settle(); /* Tuya's netcfg start may still rewrite the advertisement */
    PR_NOTICE("BLE callbacks taken over without GATT reinitialization");
    return OPRT_OK;
}

void ak_ble_gatt_adv_settle(void)
{
    s_adv_settle_due_ms = tal_system_get_millisecond() + ADV_SETTLE_MS;
    s_adv_settle_pending = true;
}

void ak_ble_gatt_stop_adv(void)
{
    if (ble_gap_adv_active()) {
        OPERATE_RET rt = tal_ble_advertising_stop();
        if (rt != OPRT_OK) {
            PR_WARN("BLE advertisement stop (pairing window closed) failed: %d", rt);
        }
    }
}

void ak_ble_gatt_suspend_for_usb(void)
{
    s_usb_suspended = true;
    s_greet_pending = false;

    /* Only stop an AK-owned advertisement. The app never calls this while
     * provisioning (s_ak_ble_active is false then), so Tuya phone-pairing BLE
     * remains available. A BLE connection cannot coexist with Companion USB;
     * Companion itself enforces disconnect-before-switch. */
    if (!s_connected && ble_gap_adv_active()) {
        OPERATE_RET rt = tal_ble_advertising_stop();
        if (rt != OPRT_OK) {
            PR_WARN("BLE advertisement stop for USB failed: %d", rt);
        } else {
            PR_NOTICE("AK BLE advertisement stopped; Companion selected USB");
        }
    }
}

void ak_ble_gatt_maintain(uint32_t now_ms)
{
    if (s_usb_suspended) {
        return;
    }
    /* One-shot re-apply after a settle event: rewrites our data over Tuya's
     * (start_advertising only starts when idle). Skipped while connected; the
     * disconnect path re-applies the data anyway. */
    if (s_adv_settle_pending && (int32_t)(now_ms - s_adv_settle_due_ms) >= 0) {
        s_adv_settle_pending = false;
        if (!s_connected) {
            OPERATE_RET rt = start_advertising();
            if (rt != OPRT_OK) {
                PR_WARN("AK BLE advertisement re-apply failed: %d", rt);
            }
        }
    }
    if (s_connected ||
        (uint32_t)(now_ms - s_last_adv_maintain_ms) < ADV_MAINTAIN_INTERVAL_MS) {
        return;
    }
    s_last_adv_maintain_ms = now_ms;

    /* Tuya's BLE manager can stop advertising during its ownership handoff.
     * Restart only when the host is actually idle; never call start every
     * second on a live advertiser (which produced the repeated rc=2 logs). */
    if (!ble_gap_adv_active()) {
        OPERATE_RET rt = start_advertising();
        if (rt != OPRT_OK) {
            PR_WARN("AK BLE advertisement restart failed: %d", rt);
        }
    }
}

int ak_ble_gatt_read(uint8_t *data, uint32_t length)
{
    if (data == NULL || length == 0) {
        return 0;
    }
    return (int)ak_byte_ring_pop(&s_rx_ring, data, length);
}

bool ak_ble_gatt_take_link_reset(void)
{
    return ak_byte_ring_take_cut(&s_rx_ring);
}

int ak_ble_gatt_write(const void *data, uint32_t length)
{
    if (data == NULL || length == 0) {
        return 0;
    }
    if (!s_connected || !s_subscribed) {
        /* No subscribed central: drop the frame (mirrors FP-2 emit_frame). */
        return 0;
    }

    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t sent = 0U;
    while (sent < length) {
        uint16_t payload_max = s_notify_payload_max;
        if (payload_max < ATT_DEFAULT_PAYLOAD) {
            payload_max = ATT_DEFAULT_PAYLOAD;
        }
        uint32_t remaining = length - sent;
        uint16_t chunk = (uint16_t)(remaining < payload_max ? remaining : payload_max);

        OPERATE_RET rt;
        if (s_tkl_takeover) {
            if (s_conn_handle == INVALID_BLE_HANDLE || s_notify_handle == INVALID_BLE_HANDLE) {
                return (int)sent;
            }
            rt = tkl_ble_gatts_value_notify(s_conn_handle, s_notify_handle,
                                            (uint8_t *)&bytes[sent], chunk);
        } else {
            TAL_BLE_DATA_T d = {.len = chunk, .p_data = (uint8_t *)&bytes[sent]};
            rt = tal_ble_server_common_send(&d);
        }
        if (rt != OPRT_OK) {
            PR_WARN("BLE notify failed: %d after %u/%u bytes (conn=0x%04X char=0x%04X)",
                    rt, (unsigned)sent, (unsigned)length,
                    (unsigned)s_conn_handle, (unsigned)s_notify_handle);
            return (int)sent;
        }
        sent += chunk;
    }
    return (int)sent;
}

bool ak_ble_gatt_take_greet(void)
{
    if (s_greet_pending) {
        s_greet_pending = false;
        return true;
    }
    return false;
}

ak_ble_link_state_t ak_ble_gatt_link_state(void)
{
    if (s_connected && s_subscribed) {
        return AK_BLE_LINK_LIVE;
    }
    if (s_connected) {
        return AK_BLE_LINK_CONNECTED;
    }
    return AK_BLE_LINK_ADVERTISING;
}

const char *ak_ble_gatt_device_name(void)
{
    prepare_device_name();
    return s_ble_name;
}

bool ak_ble_gatt_address(uint8_t out[6])
{
    TKL_BLE_GAP_ADDR_T address = {0};
    if (tkl_ble_gap_address_get(&address) != OPRT_OK || !device_address_valid(address.addr)) {
        return false;
    }
    memcpy(out, address.addr, 6U);
    return true;
}

const char *ak_ble_gatt_link_state_str(ak_ble_link_state_t state)
{
    switch (state) {
    case AK_BLE_LINK_LIVE:
        return "live";
    case AK_BLE_LINK_CONNECTED:
        return "connected";
    case AK_BLE_LINK_ADVERTISING:
    default:
        return "advertising";
    }
}
