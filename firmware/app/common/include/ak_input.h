/**
 * @file ak_input.h
 * @brief GPIO scan for the 6 mechanical keys (K1..K6) + EC11 rotary encoder,
 *        on top of the pure debounce/long-press FSM and a quadrature decoder.
 *        Poll from the app loop; events are delivered through callbacks.
 *
 * v0.3: the WS-1002 5-way joystick is replaced by an EC11 encoder. Rotation is
 * reported as a signed detent count through ak_encoder_event_cb (NOT a key/nav
 * event); the encoder push switch is reported as an ordinary key with id "enc".
 */

#ifndef AK_INPUT_H
#define AK_INPUT_H

#include <stdint.h>

#include "ak_input_fsm.h"
#include "tuya_cloud_types.h"

typedef enum {
    AK_INPUT_KIND_KEY = 0, /* mechanical key: id = "k1".."k6", or "enc" (EC11 push) */
} ak_input_kind_t;

/* Key / push-switch event (K1..K6 and the EC11 push "enc"). */
typedef void (*ak_input_event_cb)(ak_input_kind_t kind, const char *id, ak_input_event_t event, uint32_t held_ms);

/* EC11 rotation event: `detents` is a signed step count accumulated this poll
 * (positive = clockwise, negative = counter-clockwise). Emitted only when the
 * net count for the tick is non-zero. */
typedef void (*ak_encoder_event_cb)(int32_t detents);

/** Configure all input GPIOs (keys pull-up; EC11 A/B plain input, external
 *  pull-ups on board) and seed the FSMs / quadrature decoder. */
OPERATE_RET ak_input_init(void);

/**
 * Read every input once and dispatch events. Call each poll tick.
 * @param now_ms  current millisecond tick.
 * @param cb      key/push-switch callback (may be NULL).
 * @param enc_cb  encoder-rotation callback (may be NULL).
 */
void ak_input_poll(uint32_t now_ms, ak_input_event_cb cb, ak_encoder_event_cb enc_cb);

#endif /* AK_INPUT_H */
