/**
 * @file ak_provision.h
 * @brief Pairing window: a short, physically confirmed period in which the
 *        Companion may provision the device over USB. Pure logic; the caller
 *        passes the time, so it is host-testable.
 *
 * The window is opened on the device (Settings -> Pairing mode -> K6 / knob
 * confirm), or at boot after a physical reset that asked for it. Outside the
 * window, and for any line that did not arrive over UART, wifi / license /
 * license_read / reset_netcfg are refused (config-ui-design §4, FW-01/FW-12).
 */

#ifndef AK_PROVISION_H
#define AK_PROVISION_H

#include <stdbool.h>
#include <stdint.h>

#include "ak_rx.h"

#define AK_PROV_WINDOW_MS            180000U /* Q2: 180 s */
#define AK_PROV_CONFIRM_MS           10000U  /* unanswered confirm dialog = Cancel */
#define AK_PROV_CLOSE_AFTER_READY_MS 5000U   /* Q2: close 5 s after cloud_ready */
#define AK_PROV_OVERWRITE_MS         60000U  /* License overwrite permission, one use */
#define AK_PROV_RESET_CONFIRM_MS     10000U  /* unanswered reset dialog = Cancel */

typedef enum {
    AK_PROV_CLOSED = 0,
    AK_PROV_CONFIRMING, /* "Enter pairing mode?" dialog on the LCD */
    AK_PROV_OPEN,
} ak_prov_state_t;

typedef enum {
    AK_PROV_CMD_WIFI = 0,
    AK_PROV_CMD_LICENSE,
    AK_PROV_CMD_LICENSE_READ,
    AK_PROV_CMD_RESET,
    AK_PROV_CMD_BLE_PAIR, /* create the BLE pairing key (USB, window open) */
} ak_prov_cmd_t;

typedef enum {
    AK_PROV_ALLOW = 0,
    AK_PROV_DENY_SOURCE, /* line did not arrive over UART */
    AK_PROV_DENY_CLOSED, /* window is not open */
    AK_PROV_DENY_EXISTS, /* a License is already stored and overwrite is not armed */
    AK_PROV_DENY_BUSY,   /* a reset is already waiting for the device confirm */
    AK_PROV_NEEDS_DEVICE_CONFIRM, /* reset: show the confirm dialog, run on K6 */
} ak_prov_verdict_t;

typedef struct {
    ak_prov_state_t state;
    uint32_t        deadline_ms;    /* end of CONFIRMING or OPEN */
    bool            choice_confirm; /* dialog highlight: false = Cancel (default) */
    bool            overwrite_armed; /* user allowed one License overwrite */
    uint32_t        overwrite_deadline_ms;
    bool            reset_pending; /* Companion reset_netcfg waits for K6 */
    uint32_t        reset_deadline_ms;
    bool            unpair_pending; /* Companion ble_unpair waits for K6 */
    uint32_t        unpair_deadline_ms;
    bool            rebind_pending; /* Settings "Unbind & re-pair" waits for K6 */
    uint32_t        rebind_deadline_ms;
} ak_provision_t;

void ak_provision_init(ak_provision_t *p);

/** Settings -> Pairing mode: show the confirm dialog (Cancel preselected). */
void ak_provision_request(ak_provision_t *p, uint32_t now_ms);

/** Move the dialog highlight (knob rotation). No effect outside CONFIRMING. */
void ak_provision_select(ak_provision_t *p, bool confirm);

/** K6 (or knob click on Confirm): CONFIRMING -> OPEN. Returns true if opened. */
bool ak_provision_confirm(ak_provision_t *p, uint32_t now_ms);

/** Knob click: run the highlighted choice. Returns true if the window opened. */
bool ak_provision_click(ak_provision_t *p, uint32_t now_ms);

/** K4 / Cancel / Exit pairing mode: back to CLOSED from any state. */
void ak_provision_close(ak_provision_t *p);

/** Open directly (boot after a physical reset that asked for the window). */
void ak_provision_open(ak_provision_t *p, uint32_t now_ms);

/** Cloud reached cloud_ready: shorten an open window to 5 s. */
void ak_provision_on_cloud_ready(ak_provision_t *p, uint32_t now_ms);

/** Expire the dialog or window. Returns true when the state changed. */
bool ak_provision_tick(ak_provision_t *p, uint32_t now_ms);

bool ak_provision_is_open(const ak_provision_t *p, uint32_t now_ms);

/** Whole seconds left in the current state (rounded up), 0 when CLOSED. */
uint32_t ak_provision_remaining_s(const ak_provision_t *p, uint32_t now_ms);

/** Stable wire name: closed / confirming / open. */
const char *ak_provision_state_str(ak_prov_state_t state);

/**
 * May this provisioning command run now? Source is checked first, then the
 * window. A License write over an existing License also needs an armed
 * overwrite; reset_netcfg always needs a device confirm.
 */
ak_prov_verdict_t ak_provision_check(const ak_provision_t *p, ak_prov_cmd_t cmd, ak_rx_source_t source,
                                     bool license_present, uint32_t now_ms);

/** Credentials page K6 (window open): allow one License overwrite for 60 s. */
bool ak_provision_arm_overwrite(ak_provision_t *p, uint32_t now_ms);

/** True while an armed overwrite is still valid. */
bool ak_provision_overwrite_armed(const ak_provision_t *p, uint32_t now_ms);

/** A License write was attempted: the one-time permission is used up. */
void ak_provision_consume_overwrite(ak_provision_t *p);

/** Accepted reset_netcfg: wait for the device confirm (10 s). */
void ak_provision_reset_request(ak_provision_t *p, uint32_t now_ms);

/** True while the reset confirm dialog is up. */
bool ak_provision_reset_pending(const ak_provision_t *p, uint32_t now_ms);

/**
 * K6 (confirm = true) or K4 (false) on the reset dialog. Returns true only
 * when the reset should run now. The dialog is closed either way.
 */
bool ak_provision_reset_resolve(ak_provision_t *p, bool confirm, uint32_t now_ms);

/**
 * Companion `ble_unpair` (USB, FR-02): ask on the device (⑦, K6 / K1, 10 s).
 * Not tied to the pairing window. False when another system confirm is up.
 */
bool ak_provision_unpair_request(ak_provision_t *p, uint32_t now_ms);

/** True while the BLE unpair dialog is up. */
bool ak_provision_unpair_pending(const ak_provision_t *p, uint32_t now_ms);

/** K6 (true) or K1 (false). True only when the pairing should be cleared now. */
bool ak_provision_unpair_resolve(ak_provision_t *p, bool confirm, uint32_t now_ms);

/** Wire name of a refusal reason: source / closed ("" for ALLOW). */
const char *ak_provision_verdict_str(ak_prov_verdict_t verdict);

/* ── Unbind & re-pair (Settings, device-local) ──────────────────────────── */

typedef enum {
    AK_PAIR_ACTION_NEED_LICENSE = 0, /* no License: only USB can configure; just say so */
    AK_PAIR_ACTION_PAIRING,          /* not activated: same as Settings > Pairing mode */
    AK_PAIR_ACTION_REBIND,           /* activated: ask, then clear the Tuya binding */
} ak_pair_action_t;

/**
 * Settings > Pairing mode ("Change Wi-Fi" on an activated device), after K6:
 * restart into a boot window only when Tuya App pairing can run in it
 * (License present, not activated). An activated device changes Wi-Fi over
 * USB in a window opened at once: no restart, and the Tuya binding (devId,
 * keys) is untouched. Without a License the window also opens at once.
 */
bool ak_provision_pairing_needs_restart(bool license_present, bool activated);

/** What Settings > "Unbind & re-pair" does for this device. */
ak_pair_action_t ak_provision_rebind_action(bool license_present, bool activated);

/** Show the unbind confirm (⑦, K6 / K1, 10 s). False when another system
 *  confirm is up. */
bool ak_provision_rebind_request(ak_provision_t *p, uint32_t now_ms);

bool ak_provision_rebind_pending(const ak_provision_t *p, uint32_t now_ms);

/** K6 (true) or K1 (false). True only when the unbind should run now. */
bool ak_provision_rebind_resolve(ak_provision_t *p, bool confirm, uint32_t now_ms);

/**
 * May AK take the shared BLE callbacks over from Tuya now? Never while a
 * pairing window or its confirm is up (FR-12 B): BLE belongs to Tuya netcfg
 * for the whole window. With the window closed AK takes BLE whether or not the
 * device is activated, so a new board pairs with the Companion without the
 * Tuya cloud. An unactivated board reports `tuya_provisioning` from BIND_START
 * on every boot, so that flag only holds AK back on an activated board (an
 * unbind/reset in progress, reboot pending).
 */
bool ak_provision_ble_takeover_allowed(const ak_provision_t *p, bool activated, bool tuya_provisioning);

#endif /* AK_PROVISION_H */
