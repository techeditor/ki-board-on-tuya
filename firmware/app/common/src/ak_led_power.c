/**
 * @file ak_led_power.c
 * @brief WS2812 chain power switch state machine (pure). See ak_led_power.h.
 */

#include "ak_led_power.h"

#include <stddef.h>

static bool before(uint32_t now, uint32_t deadline)
{
    return (int32_t)(deadline - now) > 0;
}

static bool gap_ok(const ak_led_power_t *p, uint32_t now)
{
    return !p->sent_once || (uint32_t)(now - p->last_send_ms) >= AK_LED_FRAME_GAP_MS;
}

static void switch_off(ak_led_power_t *p, uint32_t now)
{
    p->enable        = false;
    p->state         = AK_LED_PWR_OFF;
    p->off_since_ms  = now;
    p->was_on        = true;
    p->frame_pending = false; /* only a dark frame can be pending here */
}

void ak_led_power_init(ak_led_power_t *p)
{
    p->state         = AK_LED_PWR_OFF;
    p->enable        = false;
    p->frame_pending = false;
    p->pending_lit   = false;
    p->deadline_ms   = 0;
    p->last_send_ms  = 0;
    p->sent_once     = false;
    p->off_since_ms  = 0;
    p->was_on        = false;
}

bool ak_led_power_tick(ak_led_power_t *p, uint32_t now)
{
    switch (p->state) {
    case AK_LED_PWR_OFF:
        if (!p->frame_pending) {
            return false;
        }
        if (!p->pending_lit) {
            p->frame_pending = false; /* already dark: nothing to send */
            return false;
        }
        if (p->was_on && before(now, p->off_since_ms + AK_LED_OFF_HOLD_MS)) {
            return false; /* the switch is still turning off */
        }
        /* DIN is idle low (nothing is sent while OFF); now raise the switch. */
        p->enable      = true;
        p->state       = AK_LED_PWR_SETTLING;
        p->deadline_ms = now + AK_LED_SETTLE_MS;
        return false;

    case AK_LED_PWR_SETTLING:
        if (p->frame_pending && !p->pending_lit) {
            switch_off(p, now); /* went dark before anything was shown */
            return false;
        }
        if (before(now, p->deadline_ms)) {
            return false;
        }
        p->state = AK_LED_PWR_ON;
        return p->frame_pending && gap_ok(p, now);

    case AK_LED_PWR_ON:
        return p->frame_pending && gap_ok(p, now);

    case AK_LED_PWR_STOPPING:
        if (p->frame_pending && p->pending_lit) {
            p->state = AK_LED_PWR_ON; /* lit again before the switch dropped */
            return gap_ok(p, now);
        }
        if (!before(now, p->deadline_ms)) {
            switch_off(p, now);
        }
        return false;

    default:
        return false;
    }
}

bool ak_led_power_frame(ak_led_power_t *p, bool lit, uint32_t now)
{
    p->frame_pending = true;
    p->pending_lit   = lit;
    return ak_led_power_tick(p, now);
}

void ak_led_power_sent(ak_led_power_t *p, uint32_t now)
{
    bool lit         = p->pending_lit;
    p->last_send_ms  = now;
    p->sent_once     = true;
    p->frame_pending = false;
    if (!lit) {
        /* Dark frame is on the wire: drop the switch once it has left. */
        p->state       = AK_LED_PWR_STOPPING;
        p->deadline_ms = now + AK_LED_FRAME_GAP_MS;
    }
}

bool ak_led_power_din_may_drive(const ak_led_power_t *p)
{
    return p->state == AK_LED_PWR_ON || p->state == AK_LED_PWR_STOPPING;
}
