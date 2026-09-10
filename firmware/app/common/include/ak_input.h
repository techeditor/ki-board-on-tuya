/**
 * @file ak_input.h
 * @brief GPIO scan for the 5 mechanical keys + 5-way nav, on top of the
 *        pure debounce/long-press FSM. Poll from the app loop; events are
 *        delivered through a callback.
 */

#ifndef AK_INPUT_H
#define AK_INPUT_H

#include <stdint.h>

#include "ak_input_fsm.h"
#include "tuya_cloud_types.h"

typedef enum {
    AK_INPUT_KIND_KEY = 0, /* mechanical key: id = "k1".."k5" */
    AK_INPUT_KIND_NAV,     /* nav switch:     id = "up"/"down"/"left"/"right"/"center" */
} ak_input_kind_t;

typedef void (*ak_input_event_cb)(ak_input_kind_t kind, const char *id, ak_input_event_t event, uint32_t held_ms);

/** Configure all input GPIOs (pull-up, input) and seed the FSMs. */
OPERATE_RET ak_input_init(void);

/** Read every input once and dispatch any events via cb. Call each poll tick. */
void ak_input_poll(uint32_t now_ms, ak_input_event_cb cb);

#endif /* AK_INPUT_H */
