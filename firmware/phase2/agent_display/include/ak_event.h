/**
 * @file ak_event.h
 * @brief SDK-free "intent" event model — the boundary between an event source
 *        and the display state (R6.1).
 *
 * The input mapper (input_map.c) translates physical key/nav events into these
 * high-level intents; app_state.c resolves them against the current display
 * state (which concrete animation, which notification). Keeping intents free of
 * any concrete animation id (and of SDK headers) means:
 *   - the mapper is a pure function and host-unit-testable;
 *   - P2-3 can drive the SAME app_state from serial JSONL / real Kiro events by
 *     emitting these intents (or richer ones) instead of key presses.
 */

#ifndef AK_EVENT_H
#define AK_EVENT_H

typedef enum {
    AK_EVT_NONE = 0,
    AK_EVT_STATUS_CYCLE,         /* cycle main Agent status; dir = +1/-1        */
    AK_EVT_ANIM_CYCLE,           /* cycle through ALL animations (debug); dir   */
    AK_EVT_SLEEP_TOGGLE,         /* idle <-> sleeping                           */
    AK_EVT_ONESHOT_DEMO,         /* play a one-shot (happy/alert), then fall back*/
    AK_EVT_NOTIF_ADD,            /* inject a demo notification                  */
    AK_EVT_NOTIF_DISMISS_LATEST, /* dismiss the newest notification             */
    AK_EVT_NOTIF_CLEAR,          /* clear all notifications                     */
    AK_EVT_CONNECT_TOGGLE,       /* connected <-> disconnected                  */
} ak_event_type_t;

typedef struct {
    ak_event_type_t type;
    int             dir; /* +1 / -1 for the *_CYCLE intents; 0 otherwise */
} ak_event_t;

#endif /* AK_EVENT_H */
