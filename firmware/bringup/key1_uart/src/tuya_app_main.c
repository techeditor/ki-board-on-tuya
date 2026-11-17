/**
 * @file tuya_app_main.c
 * @brief Validate KEY1=P24 and report debounced events over UART0=P10/P11.
 */

#include <stdio.h>
#include <string.h>

#include "agentic_board_io.h"
#include "bringup_uart.h"
#include "tal_api.h"
#include "tkl_gpio.h"
#include "tkl_output.h"

#define APP_TASK_STACK_SIZE   (4U * 1024U)
#define KEY_POLL_INTERVAL_MS  5U
#define KEY_DEBOUNCE_MS       20U
#define HEARTBEAT_INTERVAL_MS 2000U
#define FRAME_BUFFER_SIZE     192U

static THREAD_HANDLE s_app_thread = NULL;
static uint32_t      s_sequence   = 0;

static OPERATE_RET key1_init(TUYA_GPIO_LEVEL_E *initial_level)
{
    OPERATE_RET          rt      = OPRT_OK;
    TUYA_GPIO_BASE_CFG_T key_cfg = {
        .mode   = TUYA_GPIO_PULLUP,
        .direct = TUYA_GPIO_INPUT,
        .level  = AK_KEY_INACTIVE_LEVEL,
    };

    TUYA_CALL_ERR_RETURN(tkl_gpio_init(AK_KEY1_PIN, &key_cfg));
    return tkl_gpio_read(AK_KEY1_PIN, initial_level);
}

static void uart0_write_frame(const char *frame)
{
    uint32_t length  = (uint32_t)strlen(frame);
    int      written = bringup_uart_write(frame, length);

    if (written != (int)length) {
        PR_WARN("UART0 short write: %d/%u", written, length);
    }
}

static void send_ready(void)
{
    char frame[FRAME_BUFFER_SIZE];
    int  length = snprintf(frame, sizeof(frame),
                           "{\"v\":1,\"type\":\"bringup\",\"payload\":{\"name\":\"key1_uart\","
                            "\"status\":\"ready\",\"key_gpio\":%u,\"uart\":%u,\"baud\":%u}}\n",
                           (unsigned)AK_KEY1_PIN, (unsigned)AK_UART_PORT, (unsigned)AK_UART_BAUDRATE);

    if (length > 0 && length < (int)sizeof(frame)) {
        uart0_write_frame(frame);
    }
}

static void send_heartbeat(uint32_t uptime_ms)
{
    char frame[FRAME_BUFFER_SIZE];
    int  length = snprintf(frame, sizeof(frame),
                           "{\"v\":1,\"type\":\"bringup\",\"payload\":{\"name\":\"key1_uart\","
                            "\"status\":\"alive\",\"uptime_ms\":%u}}\n",
                           uptime_ms);

    if (length > 0 && length < (int)sizeof(frame)) {
        uart0_write_frame(frame);
    }
}

static void send_key_event(const char *action, uint32_t held_ms)
{
    char frame[FRAME_BUFFER_SIZE];
    int  length;

    s_sequence++;
    length = snprintf(frame, sizeof(frame),
                      "{\"v\":1,\"seq\":%u,\"type\":\"key\",\"payload\":{\"id\":\"k1\","
                      "\"action\":\"%s\",\"held_ms\":%u}}\n",
                      s_sequence, action, held_ms);

    if (length > 0 && length < (int)sizeof(frame)) {
        uart0_write_frame(frame);
    }
}

static void user_main(void)
{
    OPERATE_RET       rt;
    TUYA_GPIO_LEVEL_E raw_level = AK_KEY_INACTIVE_LEVEL;
    TUYA_GPIO_LEVEL_E candidate_level;
    TUYA_GPIO_LEVEL_E stable_level;
    uint32_t          now;
    uint32_t          candidate_since;
    uint32_t          pressed_since = 0;
    uint32_t          last_heartbeat;

    tal_log_init(TAL_LOG_LEVEL_DEBUG, 1024, (TAL_LOG_OUTPUT_CB)tkl_log_output);

    PR_NOTICE("======== Agentic Keyboard bring-up: key1_uart ========");
    PR_NOTICE("KEY1: P%u/J2.13, pull-up, active-low, debounce=%ums", (unsigned)AK_KEY1_PIN, KEY_DEBOUNCE_MS);
    PR_NOTICE("UART%u: RX=P%u TX=P%u, %u 8N1, no flow control", (unsigned)AK_UART_PORT, (unsigned)AK_UART_RX_PIN,
              (unsigned)AK_UART_TX_PIN, (unsigned)AK_UART_BAUDRATE);
    PR_NOTICE("Debug log remains on UART1 at 460800 baud");

    rt = bringup_uart_init();
    if (rt != OPRT_OK) {
        PR_ERR("UART0 init failed: %d", rt);
        return;
    }

    rt = key1_init(&raw_level);
    if (rt != OPRT_OK) {
        PR_ERR("KEY1 init failed: %d", rt);
        return;
    }

    stable_level    = raw_level;
    candidate_level = raw_level;
    now             = tal_system_get_millisecond();
    candidate_since = now;
    last_heartbeat  = now;

    send_ready();
    PR_NOTICE("Bring-up ready; initial KEY1 level=%d", stable_level);

    while (1) {
        rt = tkl_gpio_read(AK_KEY1_PIN, &raw_level);
        if (rt != OPRT_OK) {
            PR_ERR("KEY1 read failed: %d", rt);
            tal_system_sleep(100);
            continue;
        }

        now = tal_system_get_millisecond();

        if (raw_level != candidate_level) {
            candidate_level = raw_level;
            candidate_since = now;
        } else if (candidate_level != stable_level && (uint32_t)(now - candidate_since) >= KEY_DEBOUNCE_MS) {
            stable_level = candidate_level;

            if (stable_level == AK_KEY_ACTIVE_LEVEL) {
                pressed_since = now;
                send_key_event("down", 0);
                PR_NOTICE("KEY1 down");
            } else {
                uint32_t held_ms = pressed_since == 0 ? 0 : (uint32_t)(now - pressed_since);
                send_key_event("up", held_ms);
                PR_NOTICE("KEY1 up, held=%ums", held_ms);
                pressed_since = 0;
            }
        }

        if ((uint32_t)(now - last_heartbeat) >= HEARTBEAT_INTERVAL_MS) {
            last_heartbeat = now;
            send_heartbeat(now);
        }

        tal_system_sleep(KEY_POLL_INTERVAL_MS);
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
        .thrdname   = "key1_uart",
    };

    tal_thread_create_and_start(&s_app_thread, NULL, NULL, app_thread, NULL, &thread_cfg);
}
