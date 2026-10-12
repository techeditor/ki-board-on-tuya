/**
 * @file ak_input.c
 * @brief GPIO scan for 5 keys + 5-way nav; wraps the pure FSM per input.
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

static ak_input_t s_inputs[] = {
    {AK_INPUT_KIND_KEY, "k1", AK_KEY1_PIN, {0}},         {AK_INPUT_KIND_KEY, "k2", AK_KEY2_PIN, {0}},
    {AK_INPUT_KIND_KEY, "k3", AK_KEY3_PIN, {0}},         {AK_INPUT_KIND_KEY, "k4", AK_KEY4_PIN, {0}},
    {AK_INPUT_KIND_KEY, "k5", AK_KEY5_PIN, {0}},         {AK_INPUT_KIND_NAV, "up", AK_NAV_UP_PIN, {0}},
    {AK_INPUT_KIND_NAV, "down", AK_NAV_DOWN_PIN, {0}},   {AK_INPUT_KIND_NAV, "left", AK_NAV_LEFT_PIN, {0}},
    {AK_INPUT_KIND_NAV, "right", AK_NAV_RIGHT_PIN, {0}}, {AK_INPUT_KIND_NAV, "center", AK_NAV_CENTER_PIN, {0}},
};

/* Active-low wiring: pressed when the pin reads the active (low) level. */
static bool level_is_pressed(TUYA_GPIO_LEVEL_E level)
{
    return level == AK_KEY_ACTIVE_LEVEL;
}

OPERATE_RET ak_input_init(void)
{
    TUYA_GPIO_BASE_CFG_T input_cfg = {
        .mode   = TUYA_GPIO_PULLUP,
        .direct = TUYA_GPIO_INPUT,
        .level  = AK_KEY_INACTIVE_LEVEL,
    };
    uint32_t now = tal_system_get_millisecond();

    for (uint32_t i = 0; i < CNTSOF(s_inputs); i++) {
        TUYA_GPIO_LEVEL_E level;
        OPERATE_RET       rt = tkl_gpio_init(s_inputs[i].pin, &input_cfg);
        if (rt != OPRT_OK) {
            return rt;
        }
        rt = tkl_gpio_read(s_inputs[i].pin, &level);
        if (rt != OPRT_OK) {
            return rt;
        }
        ak_input_fsm_init(&s_inputs[i].fsm, level_is_pressed(level), now);
    }
    return OPRT_OK;
}

void ak_input_poll(uint32_t now_ms, ak_input_event_cb cb)
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
}
