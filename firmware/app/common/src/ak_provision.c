/**
 * @file ak_provision.c
 * @brief Pairing window state machine (pure).
 */

#include "ak_provision.h"

#include <stddef.h>

/* Wrap-safe "now is before deadline" for uint32 millisecond clocks. */
static bool before(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(deadline_ms - now_ms) > 0;
}

void ak_provision_init(ak_provision_t *p)
{
    p->state                 = AK_PROV_CLOSED;
    p->deadline_ms           = 0U;
    p->choice_confirm        = false;
    p->overwrite_armed       = false;
    p->overwrite_deadline_ms = 0U;
    p->reset_pending         = false;
    p->reset_deadline_ms     = 0U;
    p->unpair_pending        = false;
    p->unpair_deadline_ms    = 0U;
    p->rebind_pending        = false;
    p->rebind_deadline_ms    = 0U;
}

void ak_provision_request(ak_provision_t *p, uint32_t now_ms)
{
    p->state          = AK_PROV_CONFIRMING;
    p->deadline_ms    = now_ms + AK_PROV_CONFIRM_MS;
    p->choice_confirm = false;
}

void ak_provision_select(ak_provision_t *p, bool confirm)
{
    if (p->state == AK_PROV_CONFIRMING) {
        p->choice_confirm = confirm;
    }
}

void ak_provision_open(ak_provision_t *p, uint32_t now_ms)
{
    p->state          = AK_PROV_OPEN;
    p->deadline_ms    = now_ms + AK_PROV_WINDOW_MS;
    p->choice_confirm = false;
}

bool ak_provision_confirm(ak_provision_t *p, uint32_t now_ms)
{
    if (p->state != AK_PROV_CONFIRMING || !before(now_ms, p->deadline_ms)) {
        return false;
    }
    ak_provision_open(p, now_ms);
    return true;
}

bool ak_provision_click(ak_provision_t *p, uint32_t now_ms)
{
    if (p->state != AK_PROV_CONFIRMING) {
        return false;
    }
    if (p->choice_confirm) {
        return ak_provision_confirm(p, now_ms);
    }
    ak_provision_close(p);
    return false;
}

void ak_provision_close(ak_provision_t *p)
{
    ak_provision_init(p);
}

void ak_provision_on_cloud_ready(ak_provision_t *p, uint32_t now_ms)
{
    if (p->state != AK_PROV_OPEN) {
        return;
    }
    uint32_t soon = now_ms + AK_PROV_CLOSE_AFTER_READY_MS;
    if (before(soon, p->deadline_ms)) {
        p->deadline_ms = soon;
    }
}

bool ak_provision_tick(ak_provision_t *p, uint32_t now_ms)
{
    if (p->state != AK_PROV_CLOSED && !before(now_ms, p->deadline_ms)) {
        ak_provision_close(p);
        return true;
    }
    bool changed = false;
    if (p->overwrite_armed && !before(now_ms, p->overwrite_deadline_ms)) {
        p->overwrite_armed = false;
        changed            = true;
    }
    if (p->reset_pending && !before(now_ms, p->reset_deadline_ms)) {
        p->reset_pending = false;
        changed          = true;
    }
    if (p->unpair_pending && !before(now_ms, p->unpair_deadline_ms)) {
        p->unpair_pending = false;
        changed           = true;
    }
    if (p->rebind_pending && !before(now_ms, p->rebind_deadline_ms)) {
        p->rebind_pending = false;
        changed           = true;
    }
    return changed;
}

bool ak_provision_arm_overwrite(ak_provision_t *p, uint32_t now_ms)
{
    if (!ak_provision_is_open(p, now_ms)) {
        return false;
    }
    p->overwrite_armed       = true;
    p->overwrite_deadline_ms = now_ms + AK_PROV_OVERWRITE_MS;
    return true;
}

bool ak_provision_overwrite_armed(const ak_provision_t *p, uint32_t now_ms)
{
    return p->overwrite_armed && before(now_ms, p->overwrite_deadline_ms) && ak_provision_is_open(p, now_ms);
}

void ak_provision_consume_overwrite(ak_provision_t *p)
{
    p->overwrite_armed = false;
}

void ak_provision_reset_request(ak_provision_t *p, uint32_t now_ms)
{
    p->reset_pending     = true;
    p->reset_deadline_ms = now_ms + AK_PROV_RESET_CONFIRM_MS;
}

bool ak_provision_reset_pending(const ak_provision_t *p, uint32_t now_ms)
{
    return p->reset_pending && before(now_ms, p->reset_deadline_ms) && ak_provision_is_open(p, now_ms);
}

bool ak_provision_reset_resolve(ak_provision_t *p, bool confirm, uint32_t now_ms)
{
    bool run         = confirm && ak_provision_reset_pending(p, now_ms);
    p->reset_pending = false;
    return run;
}

bool ak_provision_unpair_request(ak_provision_t *p, uint32_t now_ms)
{
    if (ak_provision_unpair_pending(p, now_ms) || ak_provision_reset_pending(p, now_ms) ||
        ak_provision_rebind_pending(p, now_ms) || p->state == AK_PROV_CONFIRMING) {
        return false; /* one system confirm at a time */
    }
    p->unpair_pending     = true;
    p->unpair_deadline_ms = now_ms + AK_PROV_RESET_CONFIRM_MS;
    return true;
}

bool ak_provision_unpair_pending(const ak_provision_t *p, uint32_t now_ms)
{
    return p->unpair_pending && before(now_ms, p->unpair_deadline_ms);
}

bool ak_provision_unpair_resolve(ak_provision_t *p, bool confirm, uint32_t now_ms)
{
    bool run          = confirm && ak_provision_unpair_pending(p, now_ms);
    p->unpair_pending = false;
    return run;
}

bool ak_provision_is_open(const ak_provision_t *p, uint32_t now_ms)
{
    return p->state == AK_PROV_OPEN && before(now_ms, p->deadline_ms);
}

bool ak_provision_pairing_needs_restart(bool license_present, bool activated)
{
    return license_present && !activated;
}

ak_pair_action_t ak_provision_rebind_action(bool license_present, bool activated)
{
    if (!license_present) {
        return AK_PAIR_ACTION_NEED_LICENSE;
    }
    return activated ? AK_PAIR_ACTION_REBIND : AK_PAIR_ACTION_PAIRING;
}

bool ak_provision_rebind_request(ak_provision_t *p, uint32_t now_ms)
{
    if (ak_provision_rebind_pending(p, now_ms) || ak_provision_unpair_pending(p, now_ms) ||
        ak_provision_reset_pending(p, now_ms) || p->state == AK_PROV_CONFIRMING) {
        return false; /* one system confirm at a time */
    }
    p->rebind_pending     = true;
    p->rebind_deadline_ms = now_ms + AK_PROV_RESET_CONFIRM_MS;
    return true;
}

bool ak_provision_rebind_pending(const ak_provision_t *p, uint32_t now_ms)
{
    return p->rebind_pending && before(now_ms, p->rebind_deadline_ms);
}

bool ak_provision_rebind_resolve(ak_provision_t *p, bool confirm, uint32_t now_ms)
{
    bool run          = confirm && ak_provision_rebind_pending(p, now_ms);
    p->rebind_pending = false;
    return run;
}

bool ak_provision_ble_takeover_allowed(const ak_provision_t *p, bool activated, bool tuya_provisioning)
{
    /* While a pairing window (or its confirm) is up, BLE stays with Tuya's
     * netcfg; AK takes it once the window has closed or run out, activated
     * or not. */
    return (p == NULL || p->state == AK_PROV_CLOSED) && !(activated && tuya_provisioning);
}

uint32_t ak_provision_remaining_s(const ak_provision_t *p, uint32_t now_ms)
{
    if (p->state == AK_PROV_CLOSED || !before(now_ms, p->deadline_ms)) {
        return 0U;
    }
    uint32_t left_ms = p->deadline_ms - now_ms;
    return (left_ms + 999U) / 1000U;
}

const char *ak_provision_state_str(ak_prov_state_t state)
{
    switch (state) {
    case AK_PROV_CONFIRMING:
        return "confirming";
    case AK_PROV_OPEN:
        return "open";
    case AK_PROV_CLOSED:
    default:
        return "closed";
    }
}

ak_prov_verdict_t ak_provision_check(const ak_provision_t *p, ak_prov_cmd_t cmd, ak_rx_source_t source,
                                     bool license_present, uint32_t now_ms)
{
    if (source != AK_RX_SRC_UART) {
        return AK_PROV_DENY_SOURCE;
    }
    if (p == NULL || !ak_provision_is_open(p, now_ms)) {
        return AK_PROV_DENY_CLOSED;
    }
    if (cmd == AK_PROV_CMD_LICENSE && license_present && !ak_provision_overwrite_armed(p, now_ms)) {
        return AK_PROV_DENY_EXISTS;
    }
    if (cmd == AK_PROV_CMD_RESET) {
        return ak_provision_reset_pending(p, now_ms) ? AK_PROV_DENY_BUSY : AK_PROV_NEEDS_DEVICE_CONFIRM;
    }
    return AK_PROV_ALLOW;
}

const char *ak_provision_verdict_str(ak_prov_verdict_t verdict)
{
    switch (verdict) {
    case AK_PROV_DENY_SOURCE:
        return "source";
    case AK_PROV_DENY_CLOSED:
        return "closed";
    case AK_PROV_DENY_EXISTS:
        return "exists";
    case AK_PROV_DENY_BUSY:
        return "busy";
    case AK_PROV_NEEDS_DEVICE_CONFIRM:
        return "confirm_on_device";
    case AK_PROV_ALLOW:
    default:
        return "";
    }
}
