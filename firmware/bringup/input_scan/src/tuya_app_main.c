/**
 * @file tuya_app_main.c
 * @brief Validate all mechanical keys and five-way navigation inputs.
 */

#include <stdbool.h>
#include <string.h>

#include "agentic_board_io.h"
#include "bringup_uart.h"
#include "tal_api.h"
#include "tkl_gpio.h"
#include "tkl_output.h"

#define APP_TASK_STACK_SIZE    (5U * 1024U)
#define INPUT_POLL_INTERVAL_MS 5U
#define INPUT_DEBOUNCE_MS      20U
#define HEARTBEAT_INTERVAL_MS  2000U

typedef enum {
    INPUT_KIND_KEY,
    INPUT_KIND_NAV,
} INPUT_KIND_E;

typedef struct {
    INPUT_KIND_E      kind;
    const char       *id;
    TUYA_GPIO_NUM_E   pin;
    TUYA_GPIO_LEVEL_E stable;
    TUYA_GPIO_LEVEL_E candidate;
    uint32_t          candidate_since;
    uint32_t          pressed_since;
} INPUT_STATE_T;

static THREAD_HANDLE s_app_thread = NULL;
static uint32_t      s_sequence   = 0;

static INPUT_STATE_T s_inputs[] = {
    {INPUT_KIND_KEY, "k1", AK_KEY1_PIN, AK_KEY_INACTIVE_LEVEL, AK_KEY_INACTIVE_LEVEL, 0, 0},
    {INPUT_KIND_KEY, "k2", AK_KEY2_PIN, AK_KEY_INACTIVE_LEVEL, AK_KEY_INACTIVE_LEVEL, 0, 0},
    {INPUT_KIND_KEY, "k3", AK_KEY3_PIN, AK_KEY_INACTIVE_LEVEL, AK_KEY_INACTIVE_LEVEL, 0, 0},
    {INPUT_KIND_KEY, "k4", AK_KEY4_PIN, AK_KEY_INACTIVE_LEVEL, AK_KEY_INACTIVE_LEVEL, 0, 0},
    {INPUT_KIND_KEY, "k5", AK_KEY5_PIN, AK_KEY_INACTIVE_LEVEL, AK_KEY_INACTIVE_LEVEL, 0, 0},
    {INPUT_KIND_NAV, "up", AK_NAV_UP_PIN, AK_KEY_INACTIVE_LEVEL, AK_KEY_INACTIVE_LEVEL, 0, 0},
    {INPUT_KIND_NAV, "down", AK_NAV_DOWN_PIN, AK_KEY_INACTIVE_LEVEL, AK_KEY_INACTIVE_LEVEL, 0, 0},
    {INPUT_KIND_NAV, "left", AK_NAV_LEFT_PIN, AK_KEY_INACTIVE_LEVEL, AK_KEY_INACTIVE_LEVEL, 0, 0},
    {INPUT_KIND_NAV, "right", AK_NAV_RIGHT_PIN, AK_KEY_INACTIVE_LEVEL, AK_KEY_INACTIVE_LEVEL, 0, 0},
    {INPUT_KIND_NAV, "center", AK_NAV_CENTER_PIN, AK_KEY_INACTIVE_LEVEL, AK_KEY_INACTIVE_LEVEL, 0, 0},
};

static OPERATE_RET inputs_init(void)
{
    TUYA_GPIO_BASE_CFG_T input_cfg = {
        .mode   = TUYA_GPIO_PULLUP,
        .direct = TUYA_GPIO_INPUT,
        .level  = AK_KEY_INACTIVE_LEVEL,
    };

    for (uint32_t index = 0; index < CNTSOF(s_inputs); index++) {
        TUYA_GPIO_LEVEL_E level;
        OPERATE_RET       rt = tkl_gpio_init(s_inputs[index].pin, &input_cfg);
        if (rt != OPRT_OK) {
            return rt;
        }
        rt = tkl_gpio_read(s_inputs[index].pin, &level);
        if (rt != OPRT_OK) {
            return rt;
        }
        s_inputs[index].stable          = level;
        s_inputs[index].candidate       = level;
        s_inputs[index].candidate_since = tal_system_get_millisecond();
    }
    return OPRT_OK;
}

static void send_input_event(INPUT_STATE_T *input, const char *action, uint32_t held_ms)
{
    s_sequence++;
    if (input->kind == INPUT_KIND_KEY) {
        bringup_uart_printf("{\"v\":1,\"seq\":%u,\"type\":\"key\",\"payload\":{\"id\":\"%s\","
                            "\"action\":\"%s\",\"held_ms\":%u}}\n",
                            s_sequence, input->id, action, held_ms);
    } else {
        bringup_uart_printf("{\"v\":1,\"seq\":%u,\"type\":\"nav\",\"payload\":{\"dir\":\"%s\","
                            "\"action\":\"%s\",\"held_ms\":%u}}\n",
                            s_sequence, input->id, action, held_ms);
    }
}

static void scan_input(INPUT_STATE_T *input, uint32_t now)
{
    TUYA_GPIO_LEVEL_E raw;
    if (tkl_gpio_read(input->pin, &raw) != OPRT_OK) {
        return;
    }

    if (raw != input->candidate) {
        input->candidate       = raw;
        input->candidate_since = now;
        return;
    }
    if (raw == input->stable || (uint32_t)(now - input->candidate_since) < INPUT_DEBOUNCE_MS) {
        return;
    }

    input->stable = raw;
    if (raw == AK_KEY_ACTIVE_LEVEL) {
        input->pressed_since = now;
        send_input_event(input, "down", 0);
    } else {
        uint32_t held_ms = input->pressed_since == 0 ? 0 : (uint32_t)(now - input->pressed_since);
        send_input_event(input, "up", held_ms);
        input->pressed_since = 0;
    }
}

static void user_main(void)
{
    OPERATE_RET rt;
    uint32_t    last_heartbeat;

    tal_log_init(TAL_LOG_LEVEL_DEBUG, 2048, (TAL_LOG_OUTPUT_CB)tkl_log_output);
    PR_NOTICE("======== Agentic Keyboard bring-up: input_scan ========");

    rt = bringup_uart_init();
    if (rt != OPRT_OK) {
        PR_ERR("UART0 init failed: %d", rt);
        return;
    }
    rt = inputs_init();
    if (rt != OPRT_OK) {
        PR_ERR("Input init failed: %d", rt);
        return;
    }

    bringup_uart_printf("{\"v\":1,\"type\":\"bringup\",\"payload\":{\"name\":\"input_scan\","
                        "\"status\":\"ready\",\"inputs\":10,\"debounce_ms\":%u}}\n",
                        INPUT_DEBOUNCE_MS);
    last_heartbeat = tal_system_get_millisecond();

    while (1) {
        uint32_t now = tal_system_get_millisecond();
        for (uint32_t index = 0; index < CNTSOF(s_inputs); index++) {
            scan_input(&s_inputs[index], now);
        }
        if ((uint32_t)(now - last_heartbeat) >= HEARTBEAT_INTERVAL_MS) {
            last_heartbeat = now;
            bringup_uart_printf("{\"v\":1,\"type\":\"bringup\",\"payload\":{\"name\":\"input_scan\","
                                "\"status\":\"alive\",\"uptime_ms\":%u}}\n",
                                now);
        }
        tal_system_sleep(INPUT_POLL_INTERVAL_MS);
    }
}

static void app_thread(void *arg)
{
    (void)arg;
    user_main();
    tal_thread_delete(s_app_thread);
    s_app_thread = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T thread_cfg = {
        .stackDepth = APP_TASK_STACK_SIZE,
        .priority   = THREAD_PRIO_1,
        .thrdname   = "input_scan",
    };

    tal_thread_create_and_start(&s_app_thread, NULL, NULL, app_thread, NULL, &thread_cfg);
}
