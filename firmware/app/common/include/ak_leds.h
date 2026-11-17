/**
 * @file ak_leds.h
 * @brief 7x WS2812 RGB chain (v0.3) over the official TuyaOpen SPI pixel driver.
 *
 * Data line is SPI1_MOSI = P4 (AK_WS2812_*). Requires CONFIG_ENABLE_LEDS_PIXEL=y
 * so tdd_pixel_ws2812 is compiled in. The chain order is GRB on the real board
 * (RGB_ORDER swaps red<->green, verified during bring-up).
 *
 * Kept intentionally small: the product UI only needs this for the on-device
 * test page today, so it exposes "set one LED", "set all", "clear", and a
 * refresh. Brightness is capped low (7x white would be ~255 mA).
 */

#ifndef AK_LEDS_H
#define AK_LEDS_H

#include <stdbool.h>
#include <stdint.h>

#include "ak_board_io.h"
#include "tuya_cloud_types.h"

#define AK_LEDS_COUNT AK_WS2812_COUNT

/** Register + open the WS2812 chain. Safe to call once; returns OPRT_OK on
 *  success. On failure ak_leds_available() stays false and all setters no-op. */
OPERATE_RET ak_leds_init(void);

/** Whether the pixel device opened and drawing is usable. */
bool ak_leds_available(void);

/** Set one LED (index 0..AK_LEDS_COUNT-1) to an 8-bit RGB value. Buffered;
 *  call ak_leds_show() to push to the chain. Out-of-range index is ignored. */
void ak_leds_set(uint32_t index, uint8_t r, uint8_t g, uint8_t b);

/** Set one LED from a colour-wheel hue (0..AK_LEDS_HUE_STEPS-1) at `level`
 *  brightness (pre-cap). Buffered. Used by the hardware test page so EC11
 *  rotation can sweep the colour while K1-K6 light their own LED. */
#define AK_LEDS_HUE_STEPS 24U
void ak_leds_set_hue(uint32_t index, int32_t hue, uint8_t level);

/** Set every LED to the same RGB value (buffered). */
void ak_leds_set_all(uint8_t r, uint8_t g, uint8_t b);

/** Turn every LED off (buffered). */
void ak_leds_clear(void);

/** Push the buffered colours to the physical chain. */
OPERATE_RET ak_leds_show(void);

/**
 * Drive the chain power switch and send a frame that ak_leds_show() had to
 * defer (chain still powering up, or the minimum frame gap). Call from every
 * loop that calls ak_leds_show(), at least every few ms.
 */
void ak_leds_tick(uint32_t now_ms);

/* ---- LED power setting (ak_led_policy.h) ---------------------------------
 * Off / On / Auto plus the Auto idle timeout. ak_leds_tick() runs the idle
 * timer; every function that changes what the chain should show re-sends the
 * kept effect buffer through the power sequence. */
#include "ak_led_policy.h"

/** Apply the setting (boot and Settings commit). Counts as activity. */
void ak_leds_set_policy(ak_led_mode_t mode, uint8_t timeout_idx);

/** Activity that should wake the chain in Auto (key, EC11, voice, a status to
 *  show). Never consumes the event: the caller still handles it. */
void ak_leds_activity(void);

/** Hardware test page: show frames in every mode while on. */
void ak_leds_set_test_override(bool on);

/** Commanded LED_PWR_EN (P18) level: true = high (chain powered). */
bool ak_leds_power_enabled(void);

/** Chain power state for the test page: "OFF", "SETTLING", "ON", "STOPPING". */
const char *ak_leds_power_state_str(void);

#endif /* AK_LEDS_H */
