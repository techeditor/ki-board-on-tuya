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
 * Per-phase debounced, table-driven decoder — kept in sync with the bring-up
 * decoder in firmware/bringup/input_scan. Each A/B phase is debounced
 * INDEPENDENTLY (candidate level must hold ENC_DEBOUNCE_MS before it is taken
 * as stable); the 2-bit stable Gray code (A<<1)|B is then run through a
 * transition table. Debouncing each phase separately means A and B never flip
 * in the same decode step, so the code always moves between ADJACENT Gray
 * states — this both rejects contact bounce AND avoids the lost-step bug a
 * single raw read per poll has when both phases change inside one poll window.
 * One mechanical detent is a full 4-quarter cycle, so a step is emitted when
 * the accumulator reaches +/-4. The table maps illegal (diagonal) transitions
 * to 0.
 */
#ifndef AK_ENC_DEBOUNCE_MS
#define AK_ENC_DEBOUNCE_MS 2U
#endif

static const int8_t QUAD_TABLE[16] = {
    0, -1, +1, 0,
    +1, 0, 0, -1,
    -1, 0, 0, +1,
    0, +1, -1, 0,
};

typedef struct {
    TUYA_GPIO_LEVEL_E stable;         /* debounced level */
    TUYA_GPIO_LEVEL_E candidate;      /* last raw read */
    uint32_t          candidate_since;/* when the candidate first appeared */
} ak_enc_phase_t;

static ak_enc_phase_t s_enc_a;
static ak_enc_phase_t s_enc_b;
static uint8_t        s_enc_code     = 0U; /* last stable (A<<1)|B */
static int32_t        s_enc_subcount = 0;  /* accumulated quarter-steps */

static uint8_t phase_bit(const ak_enc_phase_t *p)
{
    return (p->stable == TUYA_GPIO_LEVEL_HIGH) ? 1U : 0U;
}

/* Debounce one phase; returns true when its stable level changed this tick. */
static bool ak_enc_scan_phase(ak_enc_phase_t *p, TUYA_GPIO_NUM_E pin, uint32_t now_ms)
{
    TUYA_GPIO_LEVEL_E raw;
    if (tkl_gpio_read(pin, &raw) != OPRT_OK) {
        return false;
    }
    if (raw != p->candidate) {
        p->candidate       = raw;
        p->candidate_since = now_ms;
        return false;
    }
    if (raw == p->stable || (uint32_t)(now_ms - p->candidate_since) < AK_ENC_DEBOUNCE_MS) {
        return false;
    }
    p->stable = raw;
    return true;
}

static void ak_enc_phase_init(ak_enc_phase_t *p, TUYA_GPIO_NUM_E pin)
{
    TUYA_GPIO_LEVEL_E level = TUYA_GPIO_LEVEL_HIGH;
    (void)tkl_gpio_read(pin, &level);
    p->stable          = level;
    p->candidate       = level;
    p->candidate_since = 0U;
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
    ak_enc_phase_init(&s_enc_a, AK_ENC_A_PIN);
    ak_enc_phase_init(&s_enc_b, AK_ENC_B_PIN);
    s_enc_code     = (uint8_t)((phase_bit(&s_enc_a) << 1) | phase_bit(&s_enc_b));
    s_enc_subcount = 0;
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

    /* EC11 rotation: debounce each phase, then step the Gray code. Because the
     * phases are debounced independently they move one at a time, so prev->new
     * is always an adjacent transition (never a dropped diagonal). */
    bool changed = false;
    changed |= ak_enc_scan_phase(&s_enc_a, AK_ENC_A_PIN, now_ms);
    changed |= ak_enc_scan_phase(&s_enc_b, AK_ENC_B_PIN, now_ms);
    if (changed) {
        uint8_t new_code = (uint8_t)((phase_bit(&s_enc_a) << 1) | phase_bit(&s_enc_b));
        uint8_t index    = (uint8_t)((s_enc_code << 2) | new_code);
        s_enc_subcount += QUAD_TABLE[index & 0x0FU];
        s_enc_code = new_code;
    }
    if (s_enc_subcount >= 4 || s_enc_subcount <= -4) {
        int32_t detents = s_enc_subcount / 4;
        s_enc_subcount -= detents * 4;
        if (detents != 0 && enc_cb != NULL) {
            enc_cb(detents);
        }
    }
}
