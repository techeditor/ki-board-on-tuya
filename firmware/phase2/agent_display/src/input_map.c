/**
 * @file input_map.c
 * @brief Pure key/nav -> intent mapping (see input_map.h). SDK-free.
 */

#include "input_map.h"

#include <string.h>

static bool set(ak_event_t *out, ak_event_type_t type, int dir)
{
    out->type = type;
    out->dir  = dir;
    return true;
}

bool input_map_translate(bool is_nav, const char *id, ak_input_event_t event, uint32_t held_ms, ak_event_t *out)
{
    if (out == NULL || id == NULL) {
        return false;
    }
    out->type = AK_EVT_NONE;
    out->dir  = 0;

    bool is_short = (event == AK_INPUT_EVENT_UP) && (held_ms < AK_FSM_LONG_PRESS_MS);
    bool is_long  = (event == AK_INPUT_EVENT_LONG);

    if (!is_nav) {
        /* Mechanical keys k1..k5. */
        if (strcmp(id, "k3") == 0) {
            if (is_long) {
                return set(out, AK_EVT_NOTIF_CLEAR, 0);
            }
            if (is_short) {
                return set(out, AK_EVT_NOTIF_DISMISS_LATEST, 0);
            }
            return false;
        }
        if (!is_short) {
            return false;
        }
        if (strcmp(id, "k1") == 0) {
            return set(out, AK_EVT_STATUS_CYCLE, +1);
        }
        if (strcmp(id, "k2") == 0) {
            return set(out, AK_EVT_NOTIF_ADD, 0);
        }
        if (strcmp(id, "k4") == 0) {
            return set(out, AK_EVT_CONNECT_TOGGLE, 0);
        }
        if (strcmp(id, "k5") == 0) {
            return set(out, AK_EVT_ONESHOT_DEMO, 0);
        }
        return false;
    }

    /* Five-way navigation switch (short press only). */
    if (!is_short) {
        return false;
    }
    if (strcmp(id, "up") == 0) {
        return set(out, AK_EVT_ANIM_CYCLE, -1);
    }
    if (strcmp(id, "down") == 0) {
        return set(out, AK_EVT_ANIM_CYCLE, +1);
    }
    if (strcmp(id, "left") == 0) {
        return set(out, AK_EVT_STATUS_CYCLE, -1);
    }
    if (strcmp(id, "right") == 0) {
        return set(out, AK_EVT_STATUS_CYCLE, +1);
    }
    if (strcmp(id, "center") == 0) {
        return set(out, AK_EVT_SLEEP_TOGGLE, 0);
    }
    return false;
}
