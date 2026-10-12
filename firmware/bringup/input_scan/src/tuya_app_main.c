/**
 * @file tuya_app_main.c
 * @brief Validate all six mechanical keys (K1-K6) and the EC11 rotary encoder.
 *
 * v0.3 replaced the five-way joystick with an EC11 incremental encoder, so this
 * bring-up now reports:
 *   - keys K1-K6  : {"type":"key","payload":{"id":"k1".."k6","action":"down|up","held_ms":N}}
 *   - encoder turn: {"type":"enc","payload":{"dir":"cw|ccw","steps":N}}
 *   - encoder push: {"type":"key","payload":{"id":"enc_sw","action":"down|up","held_ms":N}}
 *
 * The EC11 produces two quadrature phases A/B. We debounce each phase, build the
 * 2-bit Gray code (A<<1 | B) and feed consecutive codes through a standard
 * quadrature transition table; valid transitions accumulate +/-1 "sub-steps",
 * and one detent (datasheet: 15 pulses/360deg, full-quadrature) is emitted once
 * four sub-steps in the same direction have accrued. Phase A is used as the
 * reference as the datasheet recommends.
 */

#include <stdbool.h>
#include <string.h>

#include "agentic_board_io.h"
#include "bringup_uart.h"
#include "tal_api.h"
#include "tkl_gpio.h"
#include "tkl_output.h"

#define APP_TASK_STACK_SIZE    (5U * 1024U)
#define INPUT_POLL_INTERVAL_MS 2U
#define KEY_DEBOUNCE_MS        20U
#define ENC_DEBOUNCE_MS        2U
#define ENC_SUBSTEPS_PER_DETENT 4
#define HEARTBEAT_INTERVAL_MS  2000U

/* ---- Mechanical keys (K1-K6 + encoder push) ------------------------------ */

typedef struct {
    const char       *id;
    TUYA_GPIO_NUM_E   pin;
    TUYA_GPIO_LEVEL_E active;
    TUYA_GPIO_LEVEL_E stable;
    TUYA_GPIO_LEVEL_E candidate;
    uint32_t          candidate_since;
    uint32_t          pressed_since;
} KEY_STATE_T;

static KEY_STATE_T s_keys[] = {
    {"k1", AK_KEY1_PIN, AK_KEY_ACTIVE_LEVEL, 0, 0, 0, 0},
    {"k2", AK_KEY2_PIN, AK_KEY_ACTIVE_LEVEL, 0, 0, 0, 0},
    {"k3", AK_KEY3_PIN, AK_KEY_ACTIVE_LEVEL, 0, 0, 0, 0},
    {"k4", AK_KEY4_PIN, AK_KEY_ACTIVE_LEVEL, 0, 0, 0, 0},
    {"k5", AK_KEY5_PIN, AK_KEY_ACTIVE_LEVEL, 0, 0, 0, 0},
    {"k6", AK_KEY6_PIN, AK_KEY_ACTIVE_LEVEL, 0, 0, 0, 0},
    {"enc_sw", AK_EC11_SW_PIN, AK_EC11_SW_ACTIVE_LEVEL, 0, 0, 0, 0},
};

/* ---- EC11 quadrature phases A/B ------------------------------------------ */

typedef struct {
    TUYA_GPIO_LEVEL_E stable;
    TUYA_GPIO_LEVEL_E candidate;
    uint32_t          candidate_since;
} PHASE_STATE_T;

static PHASE_STATE_T s_phase_a;
static PHASE_STATE_T s_phase_b;
static uint8_t       s_enc_code; /* last stable 2-bit code: (A<<1)|B */
static int32_t       s_enc_accum; /* signed sub-step accumulator */

static THREAD_HANDLE s_app_thread = NULL;
static uint32_t      s_sequence   = 0;

/*
 * Quadrature transition table indexed by (prev_code<<2 | new_code).
 * +1 = one sub-step CW, -1 = one sub-step CCW, 0 = no change / illegal (noise).
 */
static const int8_t s_quad_table[16] = {
    0, -1, +1, 0, +1, 0, 0, -1, -1, 0, 0, +1, 0, +1, -1, 0,
};

static OPERATE_RET gpio_input_init(TUYA_GPIO_NUM_E pin, TUYA_GPIO_LEVEL_E *level)
{
    TUYA_GPIO_BASE_CFG_T input_cfg = {
        .mode   = TUYA_GPIO_PULLUP,
        .direct = TUYA_GPIO_INPUT,
        .level  = TUYA_GPIO_LEVEL_HIGH,
    };
    OPERATE_RET rt = tkl_gpio_init(pin, &input_cfg);
    if (rt != OPRT_OK) {
        return rt;
    }
    return tkl_gpio_read(pin, level);
}

/* EC11 A/B carry external 10k pull-ups + RC on the board (EC11I-152 datasheet),
 * so the internal pull must be OFF — use a plain floating input. Keep this in
 * sync with firmware/app ak_input.c. SW stays an active-low key (pull-up). */
static OPERATE_RET ec11_phase_init(TUYA_GPIO_NUM_E pin, TUYA_GPIO_LEVEL_E *level)
{
    TUYA_GPIO_BASE_CFG_T input_cfg = {
        .mode   = TUYA_GPIO_FLOATING,
        .direct = TUYA_GPIO_INPUT,
        .level  = TUYA_GPIO_LEVEL_HIGH,
    };
    OPERATE_RET rt = tkl_gpio_init(pin, &input_cfg);
    if (rt != OPRT_OK) {
        return rt;
    }
    return tkl_gpio_read(pin, level);
}

static OPERATE_RET inputs_init(void)
{
    OPERATE_RET       rt;
    TUYA_GPIO_LEVEL_E level;
    uint32_t          now = tal_system_get_millisecond();

    for (uint32_t index = 0; index < CNTSOF(s_keys); index++) {
        rt = gpio_input_init(s_keys[index].pin, &level);
        if (rt != OPRT_OK) {
            return rt;
        }
        s_keys[index].stable          = level;
        s_keys[index].candidate       = level;
        s_keys[index].candidate_since = now;
    }

    rt = ec11_phase_init(AK_EC11_A_PIN, &level);
    if (rt != OPRT_OK) {
        return rt;
    }
    s_phase_a.stable = s_phase_a.candidate = level;
    s_phase_a.candidate_since              = now;

    rt = ec11_phase_init(AK_EC11_B_PIN, &level);
    if (rt != OPRT_OK) {
        return rt;
    }
    s_phase_b.stable = s_phase_b.candidate = level;
    s_phase_b.candidate_since              = now;

    s_enc_code  = (uint8_t)(((s_phase_a.stable == TUYA_GPIO_LEVEL_HIGH ? 1U : 0U) << 1) |
                           (s_phase_b.stable == TUYA_GPIO_LEVEL_HIGH ? 1U : 0U));
    s_enc_accum = 0;
    return OPRT_OK;
}

static void send_key_event(const char *id, const char *action, uint32_t held_ms)
{
    s_sequence++;
    bringup_uart_printf("{\"v\":1,\"seq\":%u,\"type\":\"key\",\"payload\":{\"id\":\"%s\","
                        "\"action\":\"%s\",\"held_ms\":%u}}\n",
                        s_sequence, id, action, held_ms);
}

static void send_enc_event(const char *dir, uint32_t steps)
{
    s_sequence++;
    bringup_uart_printf("{\"v\":1,\"seq\":%u,\"type\":\"enc\",\"payload\":{\"dir\":\"%s\","
                        "\"steps\":%u}}\n",
                        s_sequence, dir, steps);
}

static void scan_key(KEY_STATE_T *key, uint32_t now)
{
    TUYA_GPIO_LEVEL_E raw;
    if (tkl_gpio_read(key->pin, &raw) != OPRT_OK) {
        return;
    }

    if (raw != key->candidate) {
        key->candidate       = raw;
        key->candidate_since = now;
        return;
    }
    if (raw == key->stable || (uint32_t)(now - key->candidate_since) < KEY_DEBOUNCE_MS) {
        return;
    }

    key->stable = raw;
    if (raw == key->active) {
        key->pressed_since = now;
        send_key_event(key->id, "down", 0);
    } else {
        uint32_t held_ms = key->pressed_since == 0 ? 0 : (uint32_t)(now - key->pressed_since);
        send_key_event(key->id, "up", held_ms);
        key->pressed_since = 0;
    }
}

/* Debounce one encoder phase; returns true if its stable level changed. */
static bool scan_phase(PHASE_STATE_T *phase, TUYA_GPIO_NUM_E pin, uint32_t now)
{
    TUYA_GPIO_LEVEL_E raw;
    if (tkl_gpio_read(pin, &raw) != OPRT_OK) {
        return false;
    }
    if (raw != phase->candidate) {
        phase->candidate       = raw;
        phase->candidate_since = now;
        return false;
    }
    if (raw == phase->stable || (uint32_t)(now - phase->candidate_since) < ENC_DEBOUNCE_MS) {
        return false;
    }
    phase->stable = raw;
    return true;
}

static void scan_encoder(uint32_t now)
{
    bool changed = false;
    changed |= scan_phase(&s_phase_a, AK_EC11_A_PIN, now);
    changed |= scan_phase(&s_phase_b, AK_EC11_B_PIN, now);
    if (!changed) {
        return;
    }

    uint8_t new_code = (uint8_t)(((s_phase_a.stable == TUYA_GPIO_LEVEL_HIGH ? 1U : 0U) << 1) |
                                 (s_phase_b.stable == TUYA_GPIO_LEVEL_HIGH ? 1U : 0U));
    int8_t  delta    = s_quad_table[(s_enc_code << 2) | new_code];
    s_enc_code       = new_code;
    if (delta == 0) {
        return;
    }

    s_enc_accum += delta;
    if (s_enc_accum >= ENC_SUBSTEPS_PER_DETENT) {
        s_enc_accum -= ENC_SUBSTEPS_PER_DETENT;
        send_enc_event("cw", 1);
    } else if (s_enc_accum <= -ENC_SUBSTEPS_PER_DETENT) {
        s_enc_accum += ENC_SUBSTEPS_PER_DETENT;
        send_enc_event("ccw", 1);
    }
}

static void user_main(void)
{
    OPERATE_RET rt;
    uint32_t    last_heartbeat;

    tal_log_init(TAL_LOG_LEVEL_DEBUG, 2048, (TAL_LOG_OUTPUT_CB)tkl_log_output);
    PR_NOTICE("======== Agentic Keyboard bring-up: input_scan (v0.3: 6 keys + EC11) ========");

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

    /* 6 keys + encoder push + encoder turn = 8 logical inputs. */
    bringup_uart_printf("{\"v\":1,\"type\":\"bringup\",\"payload\":{\"name\":\"input_scan\","
                        "\"status\":\"ready\",\"keys\":6,\"encoder\":1,\"inputs\":8,"
                        "\"key_debounce_ms\":%u}}\n",
                        KEY_DEBOUNCE_MS);
    last_heartbeat = tal_system_get_millisecond();

    while (1) {
        uint32_t now = tal_system_get_millisecond();
        for (uint32_t index = 0; index < CNTSOF(s_keys); index++) {
            scan_key(&s_keys[index], now);
        }
        scan_encoder(now);
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
