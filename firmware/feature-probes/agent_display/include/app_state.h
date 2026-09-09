/**
 * @file app_state.h
 * @brief Display state that consumes ak_event_t intents and drives the scene
 *        + notification UI (FP-1 T5).
 *
 * Holds the notification store, the current Agent status, sleep/connection
 * flags, and the cycling indices. This is the single place that maps intents to
 * concrete animations and notification operations, so a future event source
 * (FP-3 serial JSONL) can reuse it unchanged.
 */

#ifndef APP_STATE_H
#define APP_STATE_H

#include "tuya_cloud_types.h"

#include "ak_event.h"

/** Initialise state (idle, connected, empty notifications). */
OPERATE_RET app_state_init(void);

/** Apply one intent: update the scene and/or notification UI accordingly. */
void app_state_apply(const ak_event_t *evt);

#endif /* APP_STATE_H */
