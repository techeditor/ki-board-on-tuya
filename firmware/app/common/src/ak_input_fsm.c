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
    fsm->long_ms        = 0;
}

uint32_t ak_input_long_ms_for_id(const char *id)
{
    if (id != NULL && id[0] == 'k' && id[1] >= '1' && id[1] <= '3' && id[2] == '\0') {
        return AK_FSM_LONG_PRESS_K13_MS;
    }
    return AK_FSM_LONG_PRESS_MS;
}

void ak_input_fsm_set_long_ms(ak_input_fsm_t *fsm, uint32_t long_ms)
{
    fsm->long_ms = long_ms;
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
    uint32_t long_ms = fsm->long_ms != 0U ? fsm->long_ms : AK_FSM_LONG_PRESS_MS;
    if (fsm->stable_pressed && !fsm->long_fired && (uint32_t)(now_ms - fsm->pressed_since) >= long_ms) {
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
    g->pending_single = false;
    g->first_up_ms    = 0;
    g->second_down    = false;
    g->press_had_long = false;
    g->owed_long      = false;
    g->double_bound   = true;
    g->first_held_ms  = 0;
    g->owed_held_ms   = 0;
    g->res_held_ms    = 0;
    g->res_wait_ms    = 0;
}

void ak_gesture_set_double_bound(ak_gesture_state_t *g, bool bound)
{
    g->double_bound = bound;
}

void ak_gesture_last(const ak_gesture_state_t *g, uint32_t *held_ms, uint32_t *wait_ms)
{
    *held_ms = g->res_held_ms;
    *wait_ms = g->res_wait_ms;
}

static void resolved(ak_gesture_state_t *g, uint32_t held_ms, uint32_t wait_ms)
{
    g->res_held_ms = held_ms;
    g->res_wait_ms = wait_ms;
}

static bool window_open(const ak_gesture_state_t *g, uint32_t now_ms)
{
    return (uint32_t)(now_ms - g->first_up_ms) <= AK_GESTURE_DOUBLE_WINDOW_MS;
}

ak_gesture_t ak_gesture_feed(ak_gesture_state_t *g, ak_input_event_t event,
                             uint32_t now_ms, uint32_t held_ms)
{
    switch (event) {
    case AK_INPUT_EVENT_DOWN:
        g->press_had_long = false;
        if (g->pending_single) {
            if (window_open(g, now_ms)) {
                g->second_down = true; /* double candidate; single held back */
                return AK_GESTURE_NONE;
            }
            /* Window lapsed before tick() saw it: the earlier press was a
             * single, and this press starts a fresh operation. */
            g->pending_single = false;
            resolved(g, g->first_held_ms, (uint32_t)(now_ms - g->first_up_ms));
            return AK_GESTURE_SINGLE;
        }
        return AK_GESTURE_NONE;

    case AK_INPUT_EVENT_LONG:
        g->press_had_long = true;
        if (g->pending_single) {
            /* Short press, then a hold: report the single first, the long on
             * the next tick, so the PC sees them in physical order. */
            g->pending_single = false;
            g->second_down    = false;
            g->owed_long      = true;
            g->owed_held_ms   = held_ms;
            resolved(g, g->first_held_ms, (uint32_t)(now_ms - g->first_up_ms));
            return AK_GESTURE_SINGLE;
        }
        resolved(g, held_ms, 0);
        return AK_GESTURE_LONG;

    case AK_INPUT_EVENT_UP:
        if (g->press_had_long) {
            g->press_had_long = false; /* release of a long: already reported */
            return AK_GESTURE_NONE;
        }
        if (g->pending_single && g->second_down) {
            g->pending_single = false;
            g->second_down    = false;
            resolved(g, held_ms, 0);
            return AK_GESTURE_DOUBLE;
        }
        if (!g->double_bound && !g->pending_single) {
            resolved(g, held_ms, 0); /* nothing to wait for: single on release */
            return AK_GESTURE_SINGLE;
        }
        g->pending_single = true; /* first short release: open the window */
        g->second_down    = false;
        g->first_up_ms    = now_ms;
        g->first_held_ms  = held_ms;
        return AK_GESTURE_NONE;

    default:
        return AK_GESTURE_NONE;
    }
}

ak_gesture_t ak_gesture_tick(ak_gesture_state_t *g, uint32_t now_ms)
{
    if (g->owed_long) {
        g->owed_long = false;
        resolved(g, g->owed_held_ms, 0);
        return AK_GESTURE_LONG;
    }
    if (g->pending_single && !g->second_down && !window_open(g, now_ms)) {
        g->pending_single = false;
        resolved(g, g->first_held_ms, (uint32_t)(now_ms - g->first_up_ms));
        return AK_GESTURE_SINGLE;
    }
    return AK_GESTURE_NONE;
}

const char *ak_gesture_str(ak_gesture_t g)
{
    switch (g) {
    case AK_GESTURE_SINGLE:
        return "single";
    case AK_GESTURE_DOUBLE:
        return "double";
    case AK_GESTURE_LONG:
        return "long";
    default:
        return "";
    }
}
