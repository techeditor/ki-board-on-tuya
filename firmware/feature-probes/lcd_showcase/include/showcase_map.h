/**
 * @file showcase_map.h
 * @brief Pure key/nav -> showcase-intent mapping. SDK-free, stateless, host-testable.
 *
 * This is the FP-lcd-showcase analogue of FP-1's input_map.h. It only classifies
 * a single physical input event into a high-level "showcase intent"; the cycling
 * / focus / toggle STATE lives in showcase_state.c, not here.
 *
 * Mapping (see .kiro/specs/feature-probes-lcd-showcase/design.md, R3.1):
 *   short press = UP with held_ms < AK_FSM_LONG_PRESS_MS; long = LONG event.
 *   k1/k2/k3 short -> FOCUS_SLOT arg=0/1/2   (focus a K1..K3 agent slot)
 *   k4 short       -> PTT_TOGGLE             (PTT recording on/off)
 *   k5 short       -> SLOT_CYCLE             (focused slot status Idle->Run->Confirm->Idle)
 *   nav left       -> SCENE_STEP dir=-1      (previous showcase scene)
 *   nav right      -> SCENE_STEP dir=+1      (next showcase scene)
 *   nav center     -> NOTIF_TOGGLE           (inject / clear a demo notification)
 *   nav up/down    -> CONN_TOGGLE            (connected <-> disconnected)
 *
 * Note: intent, not action. FP-4 can produce the same showcase_intent_t from a
 * serial JSONL parser (real Kiro events) to drive the same HUD.
 */

#ifndef SHOWCASE_MAP_H
#define SHOWCASE_MAP_H

#include <stdbool.h>
#include <stdint.h>

#include "ak_input_fsm.h"

typedef enum {
    SC_INTENT_NONE = 0,
    SC_INTENT_FOCUS_SLOT, /* arg = 0/1/2                    */
    SC_INTENT_PTT_TOGGLE,
    SC_INTENT_SLOT_CYCLE,
    SC_INTENT_SCENE_STEP, /* dir = +1/-1                    */
    SC_INTENT_NOTIF_TOGGLE,
    SC_INTENT_CONN_TOGGLE,
} showcase_intent_type_t;

typedef struct {
    showcase_intent_type_t type;
    int                    arg; /* slot index for FOCUS_SLOT */
    int                    dir; /* +1/-1 for SCENE_STEP      */
} showcase_intent_t;

/**
 * Translate one input event into a showcase intent.
 *
 * @param is_nav  true for the 5-way nav switch, false for a mechanical key.
 * @param id      "k1".."k5" for keys; "up"/"down"/"left"/"right"/"center" for nav.
 * @param event   debounced FSM event (DOWN/UP/LONG).
 * @param held_ms press duration (valid on UP/LONG).
 * @param out     filled with the intent when the return value is true.
 * @return true if an intent was produced, false otherwise.
 */
bool showcase_map_translate(bool is_nav, const char *id, ak_input_event_t event, uint32_t held_ms,
                            showcase_intent_t *out);

/** Human-readable intent name (for logs). */
const char *showcase_intent_name(showcase_intent_type_t type);

#endif /* SHOWCASE_MAP_H */
