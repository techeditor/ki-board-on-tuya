/**
 * @file tuya_app_main.c
 * @brief Validate bidirectional UART0 communication through the CH340C bridge.
 */

#include "agentic_board_io.h"
#include "bringup_uart.h"
#include "tal_api.h"
#include "tkl_output.h"

#define APP_TASK_STACK_SIZE   (4U * 1024U)
#define HEARTBEAT_INTERVAL_MS 2000U
#define UART_READ_BUFFER_SIZE 64U

static THREAD_HANDLE s_app_thread = NULL;

static void user_main(void)
{
    OPERATE_RET rt;
    uint8_t     buffer[UART_READ_BUFFER_SIZE];
    uint32_t    last_heartbeat;

    tal_log_init(TAL_LOG_LEVEL_DEBUG, 1024, (TAL_LOG_OUTPUT_CB)tkl_log_output);

    PR_NOTICE("======== Agentic Keyboard bring-up: uart_loopback ========");
    PR_NOTICE("UART%u: RX=P%u TX=P%u, %u 8N1", (unsigned)AK_UART_PORT, (unsigned)AK_UART_RX_PIN,
              (unsigned)AK_UART_TX_PIN, (unsigned)AK_UART_BAUDRATE);

    rt = bringup_uart_init();
    if (rt != OPRT_OK) {
        PR_ERR("UART0 init failed: %d", rt);
        return;
    }

    bringup_uart_printf("{\"v\":1,\"type\":\"bringup\",\"payload\":{\"name\":\"uart_loopback\",\"status\":\"ready\","
                        "\"uart\":%u,\"baud\":%u}}\n",
                        (unsigned)AK_UART_PORT, (unsigned)AK_UART_BAUDRATE);
    last_heartbeat = tal_system_get_millisecond();

    while (1) {
        int read_length = bringup_uart_read(buffer, sizeof(buffer));
        if (read_length > 0) {
            bringup_uart_write(buffer, (uint32_t)read_length);
        }

        uint32_t now = tal_system_get_millisecond();
        if ((uint32_t)(now - last_heartbeat) >= HEARTBEAT_INTERVAL_MS) {
            last_heartbeat = now;
            bringup_uart_printf("{\"v\":1,\"type\":\"bringup\",\"payload\":{\"name\":\"uart_loopback\","
                                "\"status\":\"alive\",\"uptime_ms\":%u}}\n",
                                now);
        }
        tal_system_sleep(10);
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
        .thrdname   = "uart_loopback",
    };

    tal_thread_create_and_start(&s_app_thread, NULL, NULL, app_thread, NULL, &thread_cfg);
}
