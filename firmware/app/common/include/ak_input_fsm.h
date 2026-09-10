/**
 * @file ak_input_fsm.h
 * @brief Pure debounce + long-press state machine for one digital input.
 *
 * Hardware-independent so it can be unit-tested on the host (see
 * firmware/app/test/). Feed it the raw "pressed" boolean and a millisecond
 * timestamp each poll; it returns at most one event per call.
 *
 * Event sequence for a normal long press:
 *   DOWN (on debounced press) -> LONG (once, at LONG_PRESS_MS while held) -> UP.
 * A short press emits DOWN then UP with no LONG.
 */

#ifndef AK_INPUT_FSM_H
#define AK_INPUT_FSM_H

#include <stdbool.h>
#include <stdint.h>

#ifndef AK_FSM_DEBOUNCE_MS
#define AK_FSM_DEBOUNCE_MS 20U
#endif
#ifndef AK_FSM_LONG_PRESS_MS
#define AK_FSM_LONG_PRESS_MS 800U
#endif

typedef enum {
    AK_INPUT_EVENT_NONE = 0,
    AK_INPUT_EVENT_DOWN,
    AK_INPUT_EVENT_UP,
    AK_INPUT_EVENT_LONG,
} ak_input_event_t;

typedef struct {
    bool     stable_pressed;
    bool     cand_pressed;
    uint32_t cand_since;
    uint32_t pressed_since;
    bool     long_fired;
} ak_input_fsm_t;

/** Initialise from the input's current raw level (no event emitted). */
void ak_input_fsm_init(ak_input_fsm_t *fsm, bool initial_pressed, uint32_t now_ms);

/**
 * Advance the state machine. Returns the event produced this tick (or NONE).
 * When the event is UP or LONG, *held_ms_out (if non-NULL) is the press
 * duration in milliseconds.
 */
ak_input_event_t ak_input_fsm_update(ak_input_fsm_t *fsm, bool raw_pressed, uint32_t now_ms, uint32_t *held_ms_out);

/** Protocol action string ("down"/"up"/"long"/"") for an event. */
const char *ak_input_event_str(ak_input_event_t event);

#endif /* AK_INPUT_FSM_H */
