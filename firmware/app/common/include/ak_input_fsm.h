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
/* K1-K3: long press = switch Crew instance, so it fires sooner than the other
 * keys (K4 clear, K6 menu, EC11 settings and the K5 talk key keep 800 ms). */
#ifndef AK_FSM_LONG_PRESS_K13_MS
#define AK_FSM_LONG_PRESS_K13_MS 500U
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
    uint32_t long_ms; /* this input's long-press threshold; 0 = AK_FSM_LONG_PRESS_MS */
} ak_input_fsm_t;

/** Long-press threshold in ms for a key id: AK_FSM_LONG_PRESS_K13_MS for
 *  "k1".."k3", AK_FSM_LONG_PRESS_MS for everything else. */
uint32_t ak_input_long_ms_for_id(const char *id);

/** Override one input's long-press threshold (0 = back to the default).
 *  Call after ak_input_fsm_init(), which resets it. */
void ak_input_fsm_set_long_ms(ak_input_fsm_t *fsm, uint32_t long_ms);

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

/* --- Gesture classifier: single / double / long ---------------------------
 * Layered on the FSM event stream. Every physical operation on a key produces
 * exactly ONE gesture; the three are mutually exclusive:
 *
 *   single  pressed and released before the key's long threshold, and no second
 *           press starts within AK_GESTURE_DOUBLE_WINDOW_MS of that release.
 *           Reported when the window expires, so it lags the release by the
 *           window. A key with no double binding (ak_gesture_set_double_bound)
 *           reports it on the release itself.
 *   double  a second press starts within the window after the first release
 *           and is itself released before the long threshold. Reported on the
 *           second release; no single is reported for that pair.
 *   long    held for AK_FSM_LONG_PRESS_MS. Reported immediately (not on
 *           release); the release that follows reports nothing.
 *
 * Ordering: a short press followed inside the window by a press that turns
 * into a hold is two operations, reported single THEN long. feed() returns the
 * single on the LONG edge and the long is returned by the next tick().
 *
 * Usage: feed() every FSM event and call tick() every poll, so a pending
 * single can expire with no further input. Both return one resolved gesture
 * or AK_GESTURE_NONE. Keys that need raw edges (voice push-to-talk) bypass it. */
#ifndef AK_GESTURE_DOUBLE_WINDOW_MS
#define AK_GESTURE_DOUBLE_WINDOW_MS 250U /* first release -> second press */
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
    bool     second_down;    /* a second press started inside the window */
    bool     press_had_long; /* the current press already resolved as long */
    bool     owed_long;      /* long to report on the next tick (after a single) */
    bool     double_bound;   /* the key has a double binding: single waits out the window */
    uint32_t first_held_ms;  /* press time of the pending single */
    uint32_t owed_held_ms;   /* press time of the owed long */
    uint32_t res_held_ms;    /* press time of the gesture resolved last */
    uint32_t res_wait_ms;    /* how long that gesture waited after its deciding release */
} ak_gesture_state_t;

/** Reset a gesture classifier to idle. */
void ak_gesture_init(ak_gesture_state_t *g);

/**
 * Tell the classifier whether the key has a double binding in the current
 * keymap (default true). With no double binding a short release is reported
 * as single at once, with no wait for the window. Call before each feed(); a
 * single already waiting out the window is not affected.
 */
void ak_gesture_set_double_bound(ak_gesture_state_t *g, bool bound);

/** Of the gesture feed()/tick() resolved last: the press time that decided it
 *  and how long it waited after that press was released (0 for an immediate
 *  single, a double or a long). Reported in the key frame. */
void ak_gesture_last(const ak_gesture_state_t *g, uint32_t *held_ms, uint32_t *wait_ms);

/** Feed one FSM event (DOWN/UP/LONG/NONE); returns a gesture resolved now. */
ak_gesture_t ak_gesture_feed(ak_gesture_state_t *g, ak_input_event_t event,
                             uint32_t now_ms, uint32_t held_ms);

/** Call every poll: returns an expired single or an owed long, else NONE. */
ak_gesture_t ak_gesture_tick(ak_gesture_state_t *g, uint32_t now_ms);

/** Wire / keymap name of a gesture: "single" / "double" / "long" ("" for NONE). */
const char *ak_gesture_str(ak_gesture_t g);

#endif /* AK_INPUT_FSM_H */
