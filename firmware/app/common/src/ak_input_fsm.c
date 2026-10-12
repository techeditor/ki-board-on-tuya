/**
 * @file ak_input_fsm.c
 * @brief Pure debounce + long-press state machine (host-testable, no SDK deps).
 */

#include "ak_input_fsm.h"

#include <stddef.h>

void ak_input_fsm_init(ak_input_fsm_t *fsm, bool initial_pressed, uint32_t now_ms)
{
    fsm->stable_pressed = initial_pressed;
    fsm->cand_pressed   = initial_pressed;
    fsm->cand_since     = now_ms;
    fsm->pressed_since  = 0;
    fsm->long_fired     = false;
}

ak_input_event_t ak_input_fsm_update(ak_input_fsm_t *fsm, bool raw_pressed, uint32_t now_ms, uint32_t *held_ms_out)
{
    if (held_ms_out != NULL) {
        *held_ms_out = 0;
    }

    /* A change in the raw level restarts the debounce window. */
    if (raw_pressed != fsm->cand_pressed) {
        fsm->cand_pressed = raw_pressed;
        fsm->cand_since   = now_ms;
        return AK_INPUT_EVENT_NONE;
    }

    /* Candidate differs from the committed level: commit once it settles. */
    if (fsm->cand_pressed != fsm->stable_pressed) {
        if ((uint32_t)(now_ms - fsm->cand_since) < AK_FSM_DEBOUNCE_MS) {
            return AK_INPUT_EVENT_NONE;
        }
        fsm->stable_pressed = fsm->cand_pressed;
        if (fsm->stable_pressed) {
            fsm->pressed_since = now_ms;
            fsm->long_fired    = false;
            return AK_INPUT_EVENT_DOWN;
        }
        if (held_ms_out != NULL) {
            *held_ms_out = (uint32_t)(now_ms - fsm->pressed_since);
        }
        fsm->pressed_since = 0;
        return AK_INPUT_EVENT_UP;
    }

    /* Steady state: emit LONG once when the hold crosses the threshold. */
    if (fsm->stable_pressed && !fsm->long_fired && (uint32_t)(now_ms - fsm->pressed_since) >= AK_FSM_LONG_PRESS_MS) {
        fsm->long_fired = true;
        if (held_ms_out != NULL) {
            *held_ms_out = (uint32_t)(now_ms - fsm->pressed_since);
        }
        return AK_INPUT_EVENT_LONG;
    }

    return AK_INPUT_EVENT_NONE;
}

const char *ak_input_event_str(ak_input_event_t event)
{
    switch (event) {
    case AK_INPUT_EVENT_DOWN:
        return "down";
    case AK_INPUT_EVENT_UP:
        return "up";
    case AK_INPUT_EVENT_LONG:
        return "long";
    default:
        return "";
    }
}

void ak_dclick_init(ak_dclick_t *dc)
{
    dc->armed          = false;
    dc->last_up_ms     = 0;
    dc->press_had_long = false;
}

bool ak_dclick_feed(ak_dclick_t *dc, ak_input_event_t event, uint32_t now_ms, uint32_t held_ms)
{
    (void)held_ms;

    switch (event) {
    case AK_INPUT_EVENT_DOWN:
        /* New press starts; assume short until a LONG says otherwise. */
        dc->press_had_long = false;
        return false;

    case AK_INPUT_EVENT_LONG:
        /* This press is a hold, not a click: it can neither open nor complete a
         * double-click, and it cancels any pending first click. */
        dc->press_had_long = true;
        dc->armed          = false;
        return false;

    case AK_INPUT_EVENT_UP:
        if (dc->press_had_long) {
            /* Long-press release: never part of a double-click. */
            dc->press_had_long = false;
            dc->armed          = false;
            return false;
        }
        if (dc->armed && (uint32_t)(now_ms - dc->last_up_ms) <= AK_DCLICK_WINDOW_MS) {
            /* Second short release inside the window -> double-click. */
            dc->armed = false;
            return true;
        }
        /* First short release (or the window lapsed): arm and wait for a second. */
        dc->armed      = true;
        dc->last_up_ms = now_ms;
        return false;

    default:
        return false;
    }
}

void ak_gesture_init(ak_gesture_state_t *g)
{
    g->pending_single  = false;
    g->first_up_ms     = 0;
    g->press_had_long  = false;
    g->deferred_single = false;
}

ak_gesture_t ak_gesture_feed(ak_gesture_state_t *g, ak_input_event_t event,
                             uint32_t now_ms, uint32_t held_ms)
{
    (void)held_ms;

    switch (event) {
    case AK_INPUT_EVENT_DOWN:
        /* New press begins; assume short until a LONG says otherwise. */
        g->press_had_long = false;
        return AK_GESTURE_NONE;

    case AK_INPUT_EVENT_LONG:
        /* Long press resolves immediately. If a single was still pending (a
         * short click immediately followed by a long press on the same key),
         * that click was real and is owed: defer it to the next tick rather
         * than dropping it. The LONG is returned now. */
        g->press_had_long = true;
        if (g->pending_single) {
            g->pending_single  = false;
            g->deferred_single = true;
        }
        return AK_GESTURE_LONG;

    case AK_INPUT_EVENT_UP:
        if (g->press_had_long) {
            /* Release of a long press: already resolved as LONG, consume it. */
            g->press_had_long = false;
            return AK_GESTURE_NONE;
        }
        if (g->pending_single &&
            (uint32_t)(now_ms - g->first_up_ms) <= AK_GESTURE_DCLICK_MS) {
            /* Second short release inside the window -> double. */
            g->pending_single = false;
            return AK_GESTURE_DOUBLE;
        }
        /* First short release: arm the window; SINGLE resolves on tick. */
        g->pending_single = true;
        g->first_up_ms    = now_ms;
        return AK_GESTURE_NONE;

    default:
        return AK_GESTURE_NONE;
    }
}

ak_gesture_t ak_gesture_tick(ak_gesture_state_t *g, uint32_t now_ms)
{
    /* A single preempted by a following long press is owed first. */
    if (g->deferred_single) {
        g->deferred_single = false;
        return AK_GESTURE_SINGLE;
    }
    if (g->pending_single &&
        (uint32_t)(now_ms - g->first_up_ms) > AK_GESTURE_DCLICK_MS) {
        g->pending_single = false;
        return AK_GESTURE_SINGLE;
    }
    return AK_GESTURE_NONE;
}
