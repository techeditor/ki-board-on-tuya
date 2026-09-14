/**
 * @file ak_ble_gatt.h
 * @brief BLE GATT transport backend for the Agentic Keyboard product firmware.
 *
 * Phase-2 wireless transport: the board is a BLE peripheral exposing Tuya's
 * built-in common command service. Current TuyaOpen defaults to V2 service
 * 0xFD50 with 128-bit write/notify characteristics; V1 builds use service
 * 0x1910 with 0x2B11/0x2B10. Both carry the same JSONL protocol as UART0.
 * Ported from the real-hardware-verified FP-2 ble_gatt_link probe.
 *
 * Interface shape mirrors ak_uart exactly so tuya_app_main.c can #define the
 * ak_transport_* aliases against either backend:
 *   - ak_ble_gatt_init()  : bring up BLE and advertise "Agentic Keyboard-XXXXXX"
 *                           with the service selected by TAL,
 *                           register the event callback. Returns once the init
 *                           call is issued; the stack signals readiness
 *                           asynchronously via TAL_BLE_STACK_INIT.
 *   - ak_ble_gatt_write() : NOTIFY the bytes when a central is subscribed;
 *                           dropped otherwise (mirrors FP-2 emit_frame).
 *   - ak_ble_gatt_read()  : drain bytes received on the command write characteristic from an
 *                           internal ring buffer, so ak_rx consumes them
 *                           exactly like UART RX.
 *
 * BLE greets on notify-subscribe rather than at init: ak_ble_gatt_take_greet()
 * is a one-shot latch the app polls so it emits hello_ack at the right time.
 */

#ifndef AK_BLE_GATT_H
#define AK_BLE_GATT_H

#include <stdbool.h>
#include <stdint.h>

#include "tal_bluetooth_def.h"
#include "tuya_cloud_types.h"

/* Keep the application's advertisement aligned with tal_bluetooth.c. The SDK
 * defaults to service version 2 when no global override is supplied. */
#ifndef TAL_BLE_SERVICE_VERSION
#define TAL_BLE_SERVICE_VERSION 2
#endif
#if TAL_BLE_SERVICE_VERSION == 2
#define AK_BLE_GATT_SERVICE_UUID TAL_BLE_CMD_SERVICE_UUID_V2
#elif TAL_BLE_SERVICE_VERSION == 1
#define AK_BLE_GATT_SERVICE_UUID TAL_BLE_CMD_SERVICE_UUID_V1
#else
#error "Unsupported TAL_BLE_SERVICE_VERSION"
#endif

/** Bring up BLE, advertise the selected TAL command service, and register callbacks. */
OPERATE_RET ak_ble_gatt_init(void);

/**
 * Take ownership of an already-initialized BLE stack without registering the
 * common GATT service again. Cloud DUAL mode uses this after netmgr_init(),
 * because a second tal_ble_bt_init() would reset TAL's saved characteristic
 * handles while the T5 service-add guard skips repopulating them.
 */
OPERATE_RET ak_ble_gatt_takeover(void);

/** Drain up to `length` received bytes from the RX ring. Returns byte count. */
int ak_ble_gatt_read(uint8_t *data, uint32_t length);

/** NOTIFY `length` bytes to the subscribed central, split to negotiated-MTU
 *  payloads; drops when not subscribed. Returns bytes sent. */
int ak_ble_gatt_write(const void *data, uint32_t length);

/**
 * One-shot greet latch. Returns true exactly once after each notify-subscribe
 * event, so the app can send hello_ack when the link goes live rather than at
 * init. Returns false otherwise.
 */
bool ak_ble_gatt_take_greet(void);

/**
 * Keep the Agentic Keyboard-XXXXXX advertisement alive while disconnected. The Tuya cloud
 * netmgr also initializes its BLE manager and may stop advertising after MQTT
 * connects; the product loop calls this periodically to restore AK ownership.
 */
void ak_ble_gatt_maintain(uint32_t now_ms);

/** Link state for the app header. */
typedef enum {
    AK_BLE_LINK_ADVERTISING = 0, /* not connected — advertising as Agentic Keyboard-XXXXXX */
    AK_BLE_LINK_CONNECTED,       /* central connected, notify not yet subscribed */
    AK_BLE_LINK_LIVE,            /* connected AND subscribed to notify (link live) */
} ak_ble_link_state_t;

/** Current BLE link state, for the app to render in its header. */
ak_ble_link_state_t ak_ble_gatt_link_state(void);

#endif /* AK_BLE_GATT_H */
