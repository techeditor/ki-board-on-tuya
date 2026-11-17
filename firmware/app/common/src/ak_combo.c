/**
 * @file ak_combo.c
 * @brief Recovery key combo (K1 + K6, 5 s). See ak_combo.h.
 */

#include "ak_combo.h"

#include <stddef.h>
#include <string.h>

void ak_combo_init(ak_combo_t *c)
{
    if (c != NULL) {
        memset(c, 0, sizeof(*c));
    }
}

ak_combo_verdict_t ak_combo_key(ak_combo_t *c, const char *id, ak_input_event_t event, uint32_t now_ms)
{
    if (c == NULL || id == NULL) {
        return AK_COMBO_PASS;
    }
    bool is_k1 = strcmp(id, "k1") == 0;
    bool is_k6 = strcmp(id, "k6") == 0;
    if (!is_k1 && !is_k6) {
        return AK_COMBO_PASS;
    }
    bool *held = is_k1 ? &c->k1 : &c->k6;
    if (event == AK_INPUT_EVENT_DOWN) {
        *held = true;
    } else if (event == AK_INPUT_EVENT_UP) {
        *held = false;
    }

    if (c->active) {
        if (!c->k1 && !c->k6) {
            c->active = false; /* both released: the next press is normal again */
            c->fired  = false;
        }
        return AK_COMBO_SWALLOW;
    }
    if (event == AK_INPUT_EVENT_DOWN && c->k1 && c->k6) {
        c->active   = true;
        c->fired    = false;
        c->since_ms = now_ms;
        return AK_COMBO_START;
    }
    return AK_COMBO_PASS;
}

bool ak_combo_tick(ak_combo_t *c, uint32_t now_ms)
{
    if (c == NULL || !c->active || c->fired || !c->k1 || !c->k6 ||
        (uint32_t)(now_ms - c->since_ms) < AK_COMBO_HOLD_MS) {
        return false;
    }
    c->fired = true;
    return true;
}
