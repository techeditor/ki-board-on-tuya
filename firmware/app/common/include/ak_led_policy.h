/**
 * @file ak_led_policy.h
 * @brief LED power setting (Off / On / Auto) and the Auto idle timer, as pure
 *        logic. Times are milliseconds from a free-running uint32 clock
 *        passed in by the caller (wrap-safe).
 *
 * The policy decides WHAT the chain shows; ak_led_power.h decides WHEN the
 * switch (P18) may change and a frame may go out (DIN low before the switch
 * drops, settle before the first frame). ak_leds.c combines the two:
 *
 *   output SHOW : send the effect buffer; an all-zero buffer powers down
 *   output DARK : send zeros and power down; the buffer is kept, so the
 *                 current effect comes back on wake / mode change
 *   output HOLD : send the effect buffer and keep the chain powered even
 *                 when it is all zero
 *
 *   Off  -> DARK always (every effect request is ignored)
 *   On   -> HOLD always (chain powered all the time)
 *   Auto -> SHOW, or DARK once no activity for the configured timeout. Any
 *           activity (key, EC11, voice, a status the Companion / Crew wants
 *           shown) wakes it. Waking only changes the output: the caller
 *           still handles the event that woke it.
 * The hardware test page sets an override that forces SHOW in every mode.
 */

#ifndef AK_LED_POLICY_H
#define AK_LED_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    AK_LED_MODE_OFF = 0,
    AK_LED_MODE_ON,
    AK_LED_MODE_AUTO,
    AK_LED_MODE_COUNT,
} ak_led_mode_t;

typedef enum {
    AK_LED_OUT_SHOW = 0,
    AK_LED_OUT_DARK,
    AK_LED_OUT_HOLD,
} ak_led_output_t;

#define AK_LED_MODE_DEFAULT        AK_LED_MODE_AUTO
#define AK_LED_TIMEOUT_COUNT       4U /* 30 s / 60 s / 5 min / 15 min */
#define AK_LED_TIMEOUT_IDX_DEFAULT 1U /* 60 s */
#define AK_LED_POLICY_BLOB_LEN     3U /* version, mode, timeout index */
#define AK_LED_POLICY_BLOB_VER     1U

typedef struct {
    ak_led_mode_t mode;
    uint8_t       timeout_idx;
    uint32_t      last_activity_ms;
    bool          idle;     /* Auto timed out: chain dark until activity */
    bool          override; /* hardware test page: SHOW in every mode */
} ak_led_policy_t;

void ak_led_policy_init(ak_led_policy_t *p, ak_led_mode_t mode, uint8_t timeout_idx, uint32_t now_ms);

/** Change the mode (counts as activity). Returns true when the output changed. */
bool ak_led_policy_set_mode(ak_led_policy_t *p, ak_led_mode_t mode, uint32_t now_ms);

/** Change the Auto timeout (restarts the idle timer). Out of range is ignored. */
void ak_led_policy_set_timeout(ak_led_policy_t *p, uint8_t timeout_idx, uint32_t now_ms);

/** Activity happened. Returns true when it woke the chain (output changed). */
bool ak_led_policy_activity(ak_led_policy_t *p, uint32_t now_ms);

/** Advance the idle timer. Returns true when Auto has just gone idle. */
bool ak_led_policy_tick(ak_led_policy_t *p, uint32_t now_ms);

/** Test page override on/off (restarts the idle timer). Returns true when the output changed. */
bool ak_led_policy_set_override(ak_led_policy_t *p, bool on, uint32_t now_ms);

ak_led_output_t ak_led_policy_output(const ak_led_policy_t *p);

/** Whether the frame handed to ak_led_power is "lit" (power wanted), given
 * whether the effect buffer has any non-zero channel. */
bool ak_led_policy_frame_lit(ak_led_output_t out, bool buffer_lit);

uint32_t    ak_led_policy_timeout_ms(uint8_t timeout_idx);
const char *ak_led_policy_timeout_str(uint8_t timeout_idx); /* "30 s", "60 s", "5 min", "15 min" */
const char *ak_led_mode_str(ak_led_mode_t mode);            /* "Off", "On", "Auto" */
const char *ak_led_mode_wire_str(ak_led_mode_t mode);       /* "off", "on", "auto" (device_settings_state) */

/** KV blob <-> setting. from_blob rejects any other length, version or value. */
void ak_led_policy_to_blob(ak_led_mode_t mode, uint8_t timeout_idx, uint8_t blob[AK_LED_POLICY_BLOB_LEN]);
bool ak_led_policy_from_blob(const uint8_t *blob, size_t len, ak_led_mode_t *mode, uint8_t *timeout_idx);

#endif /* AK_LED_POLICY_H */
