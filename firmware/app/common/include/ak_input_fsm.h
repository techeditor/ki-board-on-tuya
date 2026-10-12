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

/* --- Double-click detector -------------------------------------------------
 * A tiny, pure, host-testable helper layered ON TOP of the FSM's event stream.
 * It does NOT change the FSM's DOWN/UP/LONG output; feed each emitted event
 * into ak_dclick_feed() and it returns true on the second short press of a
 * double click. Only short presses count: a press that fired LONG (held past
 * the long-press threshold) resets the sequence, so long-press-to-talk never
 * registers as a double-click click.
 *
 * Rationale: the FSM emits at most one event per tick and reports UP as soon as
 * a press releases, so double-click cannot live inside it without delaying
 * every single-press UP. Keeping it separate leaves the record/hold-to-talk
 * path (DOWN -> LONG -> UP) untouched. */
#ifndef AK_DCLICK_WINDOW_MS
#define AK_DCLICK_WINDOW_MS 400U /* max gap between the two release edges */
#endif

typedef struct {
    bool     armed;         /* a first short press has been seen */
    uint32_t last_up_ms;    /* timestamp of that first short-press release */
    bool     press_had_long;/* the current press already fired LONG */
} ak_dclick_t;

/** Reset a double-click detector to the idle (disarmed) state. */
void ak_dclick_init(ak_dclick_t *dc);

/**
 * Feed one FSM event. Returns true exactly once when a double-click completes
 * (the second short-press release within AK_DCLICK_WINDOW_MS of the first).
 * DOWN starts a press, LONG marks it as a long press (excluded), UP closes it.
 * held_ms is the FSM's release duration (used only for robustness; the LONG
 * marker is authoritative). now_ms is the current tick time.
 */
bool ak_dclick_feed(ak_dclick_t *dc, ak_input_event_t event, uint32_t now_ms, uint32_t held_ms);

/* --- Gesture classifier (single / double / long, deferred) ------------------
 * Layered on the FSM event stream like ak_dclick, but it emits a FINAL gesture
 * for EVERY press, not just double-clicks: SINGLE, DOUBLE, or LONG. A short
 * release is held for AK_GESTURE_DCLICK_MS to see whether a second short press
 * follows (DOUBLE); if none arrives in the window, SINGLE is emitted. LONG is
 * emitted immediately on the FSM LONG edge and never participates in a double.
 *
 * This is "scheme X" (deferred single): the single-click is delayed by up to
 * the double-click window so single vs double can be told apart cleanly. Use it
 * for keys whose single and double actions differ and must not both fire (K1-K3:
 * single = switch chat, double = switch instance). Keys that need an immediate
 * edge (K5 push-to-talk) should NOT use this — poll the raw FSM events instead.
 *
 * Usage: feed every FSM event into ak_gesture_feed(), AND call ak_gesture_tick()
 * every poll so a pending single can time out when no further event arrives.
 * Both return the resolved gesture (or AK_GESTURE_NONE). */
#ifndef AK_GESTURE_DCLICK_MS
#define AK_GESTURE_DCLICK_MS 250U /* window to wait for a second short press */
#endif

typedef enum {
    AK_GESTURE_NONE = 0,
    AK_GESTURE_SINGLE,
    AK_GESTURE_DOUBLE,
    AK_GESTURE_LONG,
} ak_gesture_t;

typedef struct {
    bool     pending_single; /* a first short release is waiting out the window */
    uint32_t first_up_ms;    /* when that release happened */
    bool     press_had_long; /* current press already fired LONG */
} ak_gesture_state_t;

/** Reset a gesture classifier to idle. */
void ak_gesture_init(ak_gesture_state_t *g);

/**
 * Feed one FSM event (DOWN/UP/LONG/NONE). Returns the resolved gesture when one
 * completes this call (DOUBLE on the second short release, LONG on a long-press
 * edge), else AK_GESTURE_NONE. A first short release does NOT resolve here — it
 * arms the window and resolves later via ak_gesture_tick().
 */
ak_gesture_t ak_gesture_feed(ak_gesture_state_t *g, ak_input_event_t event,
                             uint32_t now_ms, uint32_t held_ms);

/**
 * Call every poll tick. Returns AK_GESTURE_SINGLE once when a pending first
 * short release has waited out AK_GESTURE_DCLICK_MS with no second press, else
 * AK_GESTURE_NONE.
 */
ak_gesture_t ak_gesture_tick(ak_gesture_state_t *g, uint32_t now_ms);

#endif /* AK_INPUT_FSM_H */
