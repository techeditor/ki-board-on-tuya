/**
 * @file ak_led_power.h
 * @brief WS2812 chain power switch (v0.3.2 LED_PWR_EN = P18) as pure logic.
 *
 * Sequence (firmware-review FW-05, FW-11):
 *   power on : DIN (P4) idle low -> LED_PWR_EN high -> wait for VDD_LED to
 *              settle (soft start, C15 1 µF ≈ 3.3 ms to 90 %) -> first frame.
 *              The settle wait also gives the chain far more than the 80 µs
 *              reset-low it needs before data.
 *   power off: last frame (all zero) -> wait until it has left the SPI and
 *              DIN is idle low -> LED_PWR_EN low. Re-powering waits for the
 *              switch to fully turn off first (C15 via R9, ≈160 ms).
 *   frames   : never closer than AK_LED_FRAME_GAP_MS (WS2812 reset ≥280 µs on
 *              v0.3.1, ≥80 µs on v0.3.2).
 * An all-zero frame powers the chain down; a non-zero frame powers it up.
 * On boards without the switch (lite, v0.3.1) P18 is an unconnected pin, so
 * the same image works on all three: the chain is always powered there and
 * only the first frame after "power on" is delayed by the settle time.
 * Times are milliseconds from a free-running uint32 clock (wrap-safe).
 */

#ifndef AK_LED_POWER_H
#define AK_LED_POWER_H

#include <stdbool.h>
#include <stdint.h>

/* VDD_LED soft start: τ ≈ 9.1 ms, 90 % at ≈3.3 ms with C15 = 1 µF; the study
 * asks for ≥5 ms. 10 ms leaves margin for C1 (47 µF) and a soft 5 V source. */
#define AK_LED_SETTLE_MS     10U
/* Switch turn-off (|Vgs| 4.4 V -> 0.9 V ≈ 160 ms) before the next power-on. */
#define AK_LED_OFF_HOLD_MS   200U
/* Minimum gap between frames: ≥300 µs required; the clock is 1 ms. The same
 * gap guarantees the last frame has been clocked out (7 LEDs ≈ 0.21 ms at
 * 6.5 MHz) before LED_PWR_EN drops. */
#define AK_LED_FRAME_GAP_MS  1U

typedef enum {
    AK_LED_PWR_OFF = 0,     /* switch off; DIN idle */
    AK_LED_PWR_SETTLING,    /* switch on, waiting for VDD_LED */
    AK_LED_PWR_ON,          /* frames may be sent */
    AK_LED_PWR_STOPPING,    /* last (all-zero) frame sent, switch drops after the gap */
} ak_led_pwr_state_t;

typedef struct {
    ak_led_pwr_state_t state;
    bool               enable;        /* LED_PWR_EN level to drive */
    bool               frame_pending; /* a frame waits for power / the gap */
    bool               pending_lit;   /* that frame has any non-zero channel */
    uint32_t           deadline_ms;   /* SETTLING end, or STOPPING switch-off time */
    uint32_t           last_send_ms;
    bool               sent_once;
    uint32_t           off_since_ms;  /* when the switch last went low */
    bool               was_on;        /* the switch has been on at least once */
} ak_led_power_t;

void ak_led_power_init(ak_led_power_t *p);

/**
 * A new frame is ready (`lit` = any channel non-zero). Returns true when the
 * caller must send it now; otherwise it is remembered and ak_led_power_tick()
 * says when to send it. Never returns true while the switch is off.
 */
bool ak_led_power_frame(ak_led_power_t *p, bool lit, uint32_t now_ms);

/**
 * Advance timers. Returns true when the remembered frame must be sent now.
 * Updates p->enable; the caller writes it to LED_PWR_EN after every call.
 */
bool ak_led_power_tick(ak_led_power_t *p, uint32_t now_ms);

/** The caller has just clocked a frame out on DIN. */
void ak_led_power_sent(ak_led_power_t *p, uint32_t now_ms);

/** True when a frame may be on the wire: DIN may only toggle in ON/STOPPING. */
bool ak_led_power_din_may_drive(const ak_led_power_t *p);

#endif /* AK_LED_POWER_H */
