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
    AK_INPUT_EVENT_CLICK,  /* confirmed single click (short press, not a double, window elapsed) */
    AK_INPUT_EVENT_DCLICK, /* confirmed double click */
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

/* --- Click / double-click gesture recognizer --------------------------------
 * A second pure helper, layered on the FSM event stream like ak_dclick but
 * emitting the gesture events themselves (CLICK / DCLICK) instead of a bool.
 * Used for K1-K3, whose "jump to chat" semantics must fire on the RELEASE of a
 * short press (CLICK) rather than on DOWN, so a long press (bind) never jumps.
 *
 *   - A short-press release goes "pending click" and waits AK_DCLICK_WINDOW_MS:
 *       a second short-press release inside the window -> DCLICK immediately
 *       (cancelling the pending click); the window lapsing -> CLICK.
 *   - A press that fired LONG cancels everything and produces no gesture.
 * Needs ak_gesture_tick() every poll so the window can expire and emit CLICK
 * (window expiry has no triggering input event). The FSM's own DOWN/UP/LONG
 * output is untouched; this is a strictly additive layer. */
typedef struct {
    bool     pending;        /* a short-press release is waiting out the window */
    uint32_t pending_since;  /* timestamp of that release */
    bool     press_had_long; /* the current press already fired LONG (excluded) */
} ak_gesture_t;

/** Reset a gesture recognizer to idle. */
void ak_gesture_init(ak_gesture_t *g);

/**
 * Feed one FSM event. Returns a gesture determinable at this instant
 * (normally DCLICK on the second short release), else AK_INPUT_EVENT_NONE.
 * held_ms is the FSM's release duration (robustness only).
 */
ak_input_event_t ak_gesture_feed(ak_gesture_t *g, ak_input_event_t event, uint32_t now_ms, uint32_t held_ms);

/** Call every poll tick. Returns CLICK when a pending click's window expires, else NONE. */
ak_input_event_t ak_gesture_tick(ak_gesture_t *g, uint32_t now_ms);

#endif /* AK_INPUT_FSM_H */
