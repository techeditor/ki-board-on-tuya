/**
 * @file input_map.h
 * @brief Pure mapping from physical key/nav events to ak_event_t intents.
 *
 * SDK-free and stateless so it can be unit-tested on the host. The cycling /
 * toggle / counter state lives in app_state, not here — this only classifies a
 * single input event into an intent.
 *
 * Mapping (see .kiro/specs/feature-probes-agent-display/design.md §5):
 *   short press = UP with held_ms < AK_FSM_LONG_PRESS_MS; long = LONG event.
 *   k1 short  -> STATUS_CYCLE +1        k2 short -> NOTIF_ADD
 *   k3 short  -> NOTIF_DISMISS_LATEST    k3 long  -> NOTIF_CLEAR
 *   k4 short  -> CONNECT_TOGGLE          k5 short -> ONESHOT_DEMO
 *   nav up    -> ANIM_CYCLE -1           nav down  -> ANIM_CYCLE +1
 *   nav left  -> STATUS_CYCLE -1         nav right -> STATUS_CYCLE +1
 *   nav center-> SLEEP_TOGGLE
 */

#ifndef INPUT_MAP_H
#define INPUT_MAP_H

#include <stdbool.h>
#include <stdint.h>

#include "ak_event.h"
#include "ak_input_fsm.h"

/**
 * Translate one input event into an intent.
 *
 * @param is_nav    true for the 5-way nav switch, false for a mechanical key.
 * @param id        "k1".."k5" for keys; "up"/"down"/"left"/"right"/"center" for nav.
 * @param event     debounced FSM event (DOWN/UP/LONG).
 * @param held_ms   press duration (valid on UP/LONG).
 * @param out       filled with the intent when the return value is true.
 * @return true if an intent was produced, false otherwise.
 */
bool input_map_translate(bool is_nav, const char *id, ak_input_event_t event, uint32_t held_ms, ak_event_t *out);

#endif /* INPUT_MAP_H */
