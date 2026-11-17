/**
 * @file ak_led_policy.c
 * @brief LED power setting and Auto idle timer (pure). See ak_led_policy.h.
 */

#include "ak_led_policy.h"

#include <string.h>

static const uint32_t k_timeout_ms[AK_LED_TIMEOUT_COUNT] = {30000U, 60000U, 300000U, 900000U};
static const char *const k_timeout_str[AK_LED_TIMEOUT_COUNT] = {"30 s", "60 s", "5 min", "15 min"};

void ak_led_policy_init(ak_led_policy_t *p, ak_led_mode_t mode, uint8_t timeout_idx, uint32_t now_ms)
{
    p->mode             = mode < AK_LED_MODE_COUNT ? mode : AK_LED_MODE_DEFAULT;
    p->timeout_idx      = timeout_idx < AK_LED_TIMEOUT_COUNT ? timeout_idx : AK_LED_TIMEOUT_IDX_DEFAULT;
    p->last_activity_ms = now_ms;
    p->idle             = false;
    p->override         = false;
}

ak_led_output_t ak_led_policy_output(const ak_led_policy_t *p)
{
    if (p->override) {
        return AK_LED_OUT_SHOW;
    }
    switch (p->mode) {
    case AK_LED_MODE_OFF:
        return AK_LED_OUT_DARK;
    case AK_LED_MODE_ON:
        return AK_LED_OUT_HOLD;
    default:
        return p->idle ? AK_LED_OUT_DARK : AK_LED_OUT_SHOW;
    }
}

bool ak_led_policy_activity(ak_led_policy_t *p, uint32_t now_ms)
{
    ak_led_output_t before = ak_led_policy_output(p);
    p->last_activity_ms    = now_ms;
    p->idle                = false;
    return ak_led_policy_output(p) != before;
}

bool ak_led_policy_set_mode(ak_led_policy_t *p, ak_led_mode_t mode, uint32_t now_ms)
{
    if (mode >= AK_LED_MODE_COUNT) {
        return false;
    }
    ak_led_output_t before = ak_led_policy_output(p);
    p->mode                = mode;
    p->last_activity_ms    = now_ms;
    p->idle                = false;
    return ak_led_policy_output(p) != before;
}

void ak_led_policy_set_timeout(ak_led_policy_t *p, uint8_t timeout_idx, uint32_t now_ms)
{
    if (timeout_idx < AK_LED_TIMEOUT_COUNT) {
        p->timeout_idx      = timeout_idx;
        p->last_activity_ms = now_ms;
    }
}

bool ak_led_policy_tick(ak_led_policy_t *p, uint32_t now_ms)
{
    if (p->mode != AK_LED_MODE_AUTO || p->idle || p->override) {
        return false;
    }
    if ((uint32_t)(now_ms - p->last_activity_ms) < k_timeout_ms[p->timeout_idx]) {
        return false;
    }
    p->idle = true;
    return true;
}

bool ak_led_policy_set_override(ak_led_policy_t *p, bool on, uint32_t now_ms)
{
    ak_led_output_t before = ak_led_policy_output(p);
    p->override            = on;
    p->last_activity_ms    = now_ms;
    p->idle                = false;
    return ak_led_policy_output(p) != before;
}

bool ak_led_policy_frame_lit(ak_led_output_t out, bool buffer_lit)
{
    switch (out) {
    case AK_LED_OUT_DARK:
        return false;
    case AK_LED_OUT_HOLD:
        return true;
    default:
        return buffer_lit;
    }
}

uint32_t ak_led_policy_timeout_ms(uint8_t timeout_idx)
{
    return k_timeout_ms[timeout_idx < AK_LED_TIMEOUT_COUNT ? timeout_idx : AK_LED_TIMEOUT_IDX_DEFAULT];
}

const char *ak_led_policy_timeout_str(uint8_t timeout_idx)
{
    return k_timeout_str[timeout_idx < AK_LED_TIMEOUT_COUNT ? timeout_idx : AK_LED_TIMEOUT_IDX_DEFAULT];
}

const char *ak_led_mode_str(ak_led_mode_t mode)
{
    switch (mode) {
    case AK_LED_MODE_OFF:
        return "Off";
    case AK_LED_MODE_ON:
        return "On";
    default:
        return "Auto";
    }
}

const char *ak_led_mode_wire_str(ak_led_mode_t mode)
{
    switch (mode) {
    case AK_LED_MODE_OFF:
        return "off";
    case AK_LED_MODE_ON:
        return "on";
    default:
        return "auto";
    }
}

bool ak_led_mode_from_wire(const char *s, ak_led_mode_t *out)
{
    static const ak_led_mode_t modes[] = {AK_LED_MODE_OFF, AK_LED_MODE_ON, AK_LED_MODE_AUTO};
    if (s == NULL || out == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        if (strcmp(s, ak_led_mode_wire_str(modes[i])) == 0) {
            *out = modes[i];
            return true;
        }
    }
    return false;
}

bool ak_led_policy_timeout_from_s(uint32_t seconds, uint8_t *idx_out)
{
    if (idx_out == NULL) {
        return false;
    }
    for (uint8_t i = 0U; i < AK_LED_TIMEOUT_COUNT; i++) {
        if (k_timeout_ms[i] == seconds * 1000U && seconds <= 3600U) {
            *idx_out = i;
            return true;
        }
    }
    return false;
}

void ak_led_policy_to_blob(ak_led_mode_t mode, uint8_t timeout_idx, uint8_t blob[AK_LED_POLICY_BLOB_LEN])
{
    blob[0] = AK_LED_POLICY_BLOB_VER;
    blob[1] = (uint8_t)mode;
    blob[2] = timeout_idx;
}

bool ak_led_policy_from_blob(const uint8_t *blob, size_t len, ak_led_mode_t *mode, uint8_t *timeout_idx)
{
    if (blob == NULL || len != AK_LED_POLICY_BLOB_LEN || blob[0] != AK_LED_POLICY_BLOB_VER ||
        blob[1] >= (uint8_t)AK_LED_MODE_COUNT || blob[2] >= AK_LED_TIMEOUT_COUNT) {
        return false;
    }
    *mode        = (ak_led_mode_t)blob[1];
    *timeout_idx = blob[2];
    return true;
}
