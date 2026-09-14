/**
 * @file ak_ble_gatt.c
 * @brief BLE GATT transport backend for the Agentic Keyboard product firmware.
 *
 * See ak_ble_gatt.h. Ported from the real-hardware-verified FP-2
 * ble_gatt_link probe (firmware/feature-probes/ble_gatt_link):
 *
 *   - advertise local name "AK-KB" + 16-bit service UUID 0x1910,
 *   - use Tuya's built-in BLE common service (chars 0x2B10 notify / 0x2B11 write),
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

#include <string.h>

#include "tal_api.h"
#include "tal_bluetooth.h"
#include "tkl_bluetooth.h"

#define BLE_NAME                 "AK-KB"
#define SERVER_MTU               247U
#define ADV_MAINTAIN_INTERVAL_MS 1000U
#define INVALID_BLE_HANDLE       0xFFFFU

/* RX ring buffer: companion writes (0x2B11) land here for ak_ble_gatt_read.
 * Power of two so the mask wraps cheaply. 512 comfortably holds several MTU
 * writes of JSONL between app-loop drains (loop polls every ~5 ms). */
#define RX_RING_SIZE 512U
#define RX_RING_MASK (RX_RING_SIZE - 1U)

static uint8_t           s_rx_ring[RX_RING_SIZE];
static volatile uint32_t s_rx_head; /* producer (BLE event cb) writes here */
static volatile uint32_t s_rx_tail; /* consumer (app loop) reads here */

static volatile bool       s_connected;
static volatile bool       s_subscribed;
static bool                s_tkl_takeover;
static volatile uint16_t   s_conn_handle;
static volatile uint16_t   s_notify_handle;
static uint32_t            s_last_adv_maintain_ms;
static volatile bool       s_greet_pending; /* set on subscribe, cleared by take_greet */
static TAL_BLE_PEER_INFO_T s_peer;

/* Advertising: flags + complete list of 16-bit service UUIDs (0x1910). */
static uint8_t s_adv_data[] = {
    0x02, 0x01, 0x06,       /* flags: LE General Discoverable, BR/EDR not supported */
    0x03, 0x03, 0x10, 0x19, /* complete list of 16-bit service UUIDs: 0x1910 */
};

/* Scan response: complete local name "AK-KB". */
static uint8_t s_scan_rsp[] = {
    0x06, 0x09, 'A', 'K', '-', 'K', 'B',
};

/* ------------------------------------------------------------ RX ring */

static void rx_ring_push(const uint8_t *data, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint32_t next = (s_rx_head + 1U) & RX_RING_MASK;
        if (next == (s_rx_tail & RX_RING_MASK)) {
            /* Full: drop the oldest byte to keep accepting the newest data. */
            s_rx_tail = (s_rx_tail + 1U) & RX_RING_MASK;
        }
        s_rx_ring[s_rx_head & RX_RING_MASK] = data[i];
        s_rx_head                           = next;
    }
}

/* ------------------------------------------------------------ advertising */

static void start_advertising(void)
{
    TAL_BLE_DATA_T adv = {.len = sizeof(s_adv_data), .p_data = s_adv_data};
    TAL_BLE_DATA_T rsp = {.len = sizeof(s_scan_rsp), .p_data = s_scan_rsp};
    /* update works both while another advertiser is active and while stopped;
     * data_set alone may fail to replace Tuya's already-running advertisement. */
    (void)tal_ble_advertising_data_update(&adv, &rsp);
    (void)tal_ble_advertising_start(TUYAOS_BLE_DEFAULT_ADV_PARAM);
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
            PR_NOTICE("BLE ready, advertising as %s", BLE_NAME);
        } else {
            PR_ERR("BLE init failed: %d", event->ble_event.init);
        }
        break;

    case TAL_BLE_EVT_PERIPHERAL_CONNECT:
        if (event->ble_event.connect.result == OPRT_OK) {
            s_peer       = event->ble_event.connect.peer;
            s_connected  = true;
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
        tal_ble_server_exchange_mtu_reply(s_peer, SERVER_MTU);
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
        if (rx->p_data != NULL && rx->len > 0) {
            rx_ring_push(rx->p_data, rx->len);
        }
        break;
    }

    case TAL_BLE_EVT_DISCONNECT:
        s_connected  = false;
        s_subscribed = false;
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
        (void)tkl_ble_gatts_exchange_mtu_reply(event->conn_handle, SERVER_MTU);
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
        TKL_BLE_DATA_T *rx = &event->gatt_event.write_report.report;
        if (rx->p_data != NULL && rx->length > 0) {
            rx_ring_push(rx->p_data, rx->length);
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
    s_rx_head              = 0;
    s_rx_tail              = 0;
    s_connected            = false;
    s_subscribed           = false;
    s_conn_handle          = INVALID_BLE_HANDLE;
    s_notify_handle        = INVALID_BLE_HANDLE;
    s_last_adv_maintain_ms = 0;
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
    PR_NOTICE("BLE callbacks taken over without GATT reinitialization");
    return OPRT_OK;
}

void ak_ble_gatt_maintain(uint32_t now_ms)
{
    if (s_connected || (uint32_t)(now_ms - s_last_adv_maintain_ms) < ADV_MAINTAIN_INTERVAL_MS) {
        return;
    }
    s_last_adv_maintain_ms = now_ms;

    /* netmgr's Tuya BLE monitor stops advertising once MQTT connects. Reapply
     * our data and start here; start is intentionally best-effort because an
     * already-running advertiser is the desired state on some BLE ports. */
    start_advertising();
}

int ak_ble_gatt_read(uint8_t *data, uint32_t length)
{
    if (data == NULL || length == 0) {
        return 0;
    }
    uint32_t n = 0;
    while (n < length && (s_rx_tail & RX_RING_MASK) != (s_rx_head & RX_RING_MASK)) {
        data[n++] = s_rx_ring[s_rx_tail & RX_RING_MASK];
        s_rx_tail = (s_rx_tail + 1U) & RX_RING_MASK;
    }
    return (int)n;
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

    OPERATE_RET rt;
    if (s_tkl_takeover) {
        if (s_conn_handle == INVALID_BLE_HANDLE || s_notify_handle == INVALID_BLE_HANDLE) {
            return 0;
        }
        rt = tkl_ble_gatts_value_notify(s_conn_handle, s_notify_handle,
                                        (uint8_t *)data, (uint16_t)length);
    } else {
        TAL_BLE_DATA_T d = {.len = (uint16_t)length, .p_data = (uint8_t *)data};
        rt = tal_ble_server_common_send(&d);
    }
    if (rt == OPRT_OK) {
        return (int)length;
    }
    PR_WARN("BLE notify failed: %d (conn=0x%04X char=0x%04X)", rt,
            (unsigned)s_conn_handle, (unsigned)s_notify_handle);
    return 0;
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
