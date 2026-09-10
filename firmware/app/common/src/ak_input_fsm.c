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
