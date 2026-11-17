/**
 * @file ak_combo.h
 * @brief Recovery key combo: hold K1 + K6 together for 5 s (pure, host-tested).
 *
 * The combo starts when the second of the two keys goes down while the other
 * is held. From then on every K1 / K6 event is swallowed until both are up,
 * so the keys cannot also resolve a single / double / long gesture, close a
 * menu item or confirm a dialog. The first key's DOWN has already been
 * handled; on every screen a K1 DOWN only cancels or goes back, and the
 * confirm dialogs act on K6 release, so pressing in either order is safe.
 * After AK_COMBO_HOLD_MS with both keys still held, ak_combo_tick() fires once.
 */

#ifndef AK_COMBO_H
#define AK_COMBO_H

#include <stdbool.h>
#include <stdint.h>

#include "ak_input_fsm.h"

#define AK_COMBO_HOLD_MS 5000U

typedef struct {
    bool     k1;     /* held */
    bool     k6;     /* held */
    bool     active; /* both went down; swallowing until both are up */
    bool     fired;
    uint32_t since_ms;
} ak_combo_t;

typedef enum {
    AK_COMBO_PASS = 0, /* not a combo key event: handle it normally */
    AK_COMBO_START,    /* combo began with this event: drop K1/K6 half-state, swallow it */
    AK_COMBO_SWALLOW,  /* part of a combo: swallow it */
} ak_combo_verdict_t;

void ak_combo_init(ak_combo_t *c);

/** Feed every key event ("k1".."k6", "enc"). */
ak_combo_verdict_t ak_combo_key(ak_combo_t *c, const char *id, ak_input_event_t event, uint32_t now_ms);

/** True once, when both keys have been held AK_COMBO_HOLD_MS. */
bool ak_combo_tick(ak_combo_t *c, uint32_t now_ms);

#endif /* AK_COMBO_H */
