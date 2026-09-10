/**
 * @file showcase_map.c
 * @brief Pure key/nav -> showcase-intent mapping (see showcase_map.h). SDK-free.
 */

#include "showcase_map.h"

#include <string.h>

static bool set_intent(showcase_intent_t *out, showcase_intent_type_t type, int arg, int dir)
{
    out->type = type;
    out->arg  = arg;
    out->dir  = dir;
    return true;
}

bool showcase_map_translate(bool is_nav, const char *id, ak_input_event_t event, uint32_t held_ms,
                            showcase_intent_t *out)
{
    if (out == NULL || id == NULL) {
        return false;
    }
    out->type = SC_INTENT_NONE;
    out->arg  = 0;
    out->dir  = 0;

    bool is_short = (event == AK_INPUT_EVENT_UP) && (held_ms < AK_FSM_LONG_PRESS_MS);

    /* This probe only uses short presses; DOWN/LONG produce nothing. */
    if (!is_short) {
        return false;
    }

    if (!is_nav) {
        /* Mechanical keys k1..k5. */
        if (strcmp(id, "k1") == 0) {
            return set_intent(out, SC_INTENT_FOCUS_SLOT, 0, 0);
        }
        if (strcmp(id, "k2") == 0) {
            return set_intent(out, SC_INTENT_FOCUS_SLOT, 1, 0);
        }
        if (strcmp(id, "k3") == 0) {
            return set_intent(out, SC_INTENT_FOCUS_SLOT, 2, 0);
        }
        if (strcmp(id, "k4") == 0) {
            return set_intent(out, SC_INTENT_PTT_TOGGLE, 0, 0);
        }
        if (strcmp(id, "k5") == 0) {
            return set_intent(out, SC_INTENT_SLOT_CYCLE, 0, 0);
        }
        return false;
    }

    /* Five-way navigation switch. */
    if (strcmp(id, "left") == 0) {
        return set_intent(out, SC_INTENT_SCENE_STEP, 0, -1);
    }
    if (strcmp(id, "right") == 0) {
        return set_intent(out, SC_INTENT_SCENE_STEP, 0, +1);
    }
    if (strcmp(id, "center") == 0) {
        return set_intent(out, SC_INTENT_NOTIF_TOGGLE, 0, 0);
    }
    if (strcmp(id, "up") == 0 || strcmp(id, "down") == 0) {
        return set_intent(out, SC_INTENT_CONN_TOGGLE, 0, 0);
    }
    return false;
}

const char *showcase_intent_name(showcase_intent_type_t type)
{
    switch (type) {
    case SC_INTENT_FOCUS_SLOT:
        return "FOCUS_SLOT";
    case SC_INTENT_PTT_TOGGLE:
        return "PTT_TOGGLE";
    case SC_INTENT_SLOT_CYCLE:
        return "SLOT_CYCLE";
    case SC_INTENT_SCENE_STEP:
        return "SCENE_STEP";
    case SC_INTENT_NOTIF_TOGGLE:
        return "NOTIF_TOGGLE";
    case SC_INTENT_CONN_TOGGLE:
        return "CONN_TOGGLE";
    case SC_INTENT_NONE:
    default:
        return "NONE";
    }
}
