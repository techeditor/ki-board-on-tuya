/**
 * @file ak_input.c
 * @brief GPIO scan for 6 keys (K1..K6) + the EC11 push switch via the pure FSM,
 *        plus a quadrature decoder for EC11 A/B rotation.
 *
 * v0.3: replaces the v0.2 5-way joystick with an EC11 rotary encoder. Keys and
 * the encoder push ("enc") use the debounce/long-press FSM; A/B rotation uses a
 * table-driven quadrature decoder that emits one signed step per mechanical
 * detent (EC11I-152: 15 detents / 360deg, one full Gray-code cycle per detent).
 */

#include "ak_input.h"

#include "ak_board_io.h"
#include "tal_api.h"
#include "tkl_gpio.h"

typedef struct {
    ak_input_kind_t kind;
    const char     *id;
    TUYA_GPIO_NUM_E pin;
    ak_input_fsm_t  fsm;
} ak_input_t;

/* Keys + the EC11 push switch. All active-low with an internal pull-up. */
static ak_input_t s_inputs[] = {
    {AK_INPUT_KIND_KEY, "k1", AK_KEY1_PIN, {0}},
    {AK_INPUT_KIND_KEY, "k2", AK_KEY2_PIN, {0}},
    {AK_INPUT_KIND_KEY, "k3", AK_KEY3_PIN, {0}},
    {AK_INPUT_KIND_KEY, "k4", AK_KEY4_PIN, {0}},
    {AK_INPUT_KIND_KEY, "k5", AK_KEY5_PIN, {0}},
    {AK_INPUT_KIND_KEY, "k6", AK_KEY6_PIN, {0}},
    {AK_INPUT_KIND_KEY, AK_ENC_SW_ID, AK_ENC_SW_PIN, {0}},
};

/* Active-low wiring: pressed when the pin reads the active (low) level. */
static bool level_is_pressed(TUYA_GPIO_LEVEL_E level)
{
    return level == AK_KEY_ACTIVE_LEVEL;
}

/* ── EC11 quadrature decoder ──────────────────────────────────────────────
 * Standard table-driven decoder. The 2-bit state is (A<<1)|B. On each A/B edge
 * we look up the transition in a table that yields +1 / -1 / 0, and accumulate
 * quarter-steps; one mechanical detent is a full 4-quarter cycle, so we emit a
 * step when the accumulator reaches +/-4. The table rejects illegal (bouncing)
 * transitions as 0, which debounces the contacts without a timer.
 */
static const int8_t QUAD_TABLE[16] = {
    0, -1, +1, 0,
    +1, 0, 0, -1,
    -1, 0, 0, +1,
    0, +1, -1, 0,
};

static uint8_t s_enc_prev_state = 0U; /* last (A<<1)|B */
static int32_t s_enc_subcount   = 0;  /* accumulated quarter-steps */

static uint8_t ak_enc_read_state(void)
{
    TUYA_GPIO_LEVEL_E a = AK_KEY_INACTIVE_LEVEL;
    TUYA_GPIO_LEVEL_E b = AK_KEY_INACTIVE_LEVEL;
    (void)tkl_gpio_read(AK_ENC_A_PIN, &a);
    (void)tkl_gpio_read(AK_ENC_B_PIN, &b);
    uint8_t sa = (a == TUYA_GPIO_LEVEL_HIGH) ? 1U : 0U;
    uint8_t sb = (b == TUYA_GPIO_LEVEL_HIGH) ? 1U : 0U;
    return (uint8_t)((sa << 1) | sb);
}

OPERATE_RET ak_input_init(void)
{
    TUYA_GPIO_BASE_CFG_T key_cfg = {
        .mode   = TUYA_GPIO_PULLUP,
        .direct = TUYA_GPIO_INPUT,
        .level  = AK_KEY_INACTIVE_LEVEL,
    };
    /* EC11 A/B carry external 10k pull-ups + RC per the datasheet; use a plain
     * floating input and let the board set the idle level. */
    TUYA_GPIO_BASE_CFG_T enc_ab_cfg = {
        .mode   = TUYA_GPIO_FLOATING,
        .direct = TUYA_GPIO_INPUT,
        .level  = TUYA_GPIO_LEVEL_HIGH,
    };
    uint32_t now = tal_system_get_millisecond();

    for (uint32_t i = 0; i < CNTSOF(s_inputs); i++) {
        TUYA_GPIO_LEVEL_E level;
        OPERATE_RET       rt = tkl_gpio_init(s_inputs[i].pin, &key_cfg);
        if (rt != OPRT_OK) {
            return rt;
        }
        rt = tkl_gpio_read(s_inputs[i].pin, &level);
        if (rt != OPRT_OK) {
            return rt;
        }
        ak_input_fsm_init(&s_inputs[i].fsm, level_is_pressed(level), now);
    }

    OPERATE_RET rt = tkl_gpio_init(AK_ENC_A_PIN, &enc_ab_cfg);
    if (rt != OPRT_OK) {
        return rt;
    }
    rt = tkl_gpio_init(AK_ENC_B_PIN, &enc_ab_cfg);
    if (rt != OPRT_OK) {
        return rt;
    }
    s_enc_prev_state = ak_enc_read_state();
    s_enc_subcount   = 0;
    return OPRT_OK;
}

void ak_input_poll(uint32_t now_ms, ak_input_event_cb cb, ak_encoder_event_cb enc_cb)
{
    for (uint32_t i = 0; i < CNTSOF(s_inputs); i++) {
        TUYA_GPIO_LEVEL_E level;
        if (tkl_gpio_read(s_inputs[i].pin, &level) != OPRT_OK) {
            continue;
        }

        uint32_t         held  = 0;
        ak_input_event_t event = ak_input_fsm_update(&s_inputs[i].fsm, level_is_pressed(level), now_ms, &held);
        if (event != AK_INPUT_EVENT_NONE && cb != NULL) {
            cb(s_inputs[i].kind, s_inputs[i].id, event, held);
        }
    }

    /* EC11 rotation: fold every A/B transition into net detents this tick. */
    uint8_t state = ak_enc_read_state();
    if (state != s_enc_prev_state) {
        uint8_t index = (uint8_t)((s_enc_prev_state << 2) | state);
        s_enc_subcount += QUAD_TABLE[index & 0x0FU];
        s_enc_prev_state = state;
    }
    if (s_enc_subcount >= 4 || s_enc_subcount <= -4) {
        int32_t detents = s_enc_subcount / 4;
        s_enc_subcount -= detents * 4;
        if (detents != 0 && enc_cb != NULL) {
            enc_cb(detents);
        }
    }
}
