/**
 * @file tuya_app_main.c
 * @brief Validate T5 Wi-Fi scanning and BLE advertising without credentials.
 */

#include <limits.h>
#include <string.h>

#include "bringup_uart.h"
#include "tal_api.h"
#include "tal_bluetooth.h"
#include "tal_wifi.h"
#include "tkl_output.h"

#define APP_TASK_STACK_SIZE   (6U * 1024U)
#define WIFI_SCAN_INTERVAL_MS 10000U

static THREAD_HANDLE s_app_thread = NULL;

static uint8_t s_adv_data[] = {
    0x02,
    0x01,
    0x06,
};

static uint8_t s_scan_response[] = {
    0x0B, 0x09, 'A', 'K', '-', 'B', 'r', 'i', 'n', 'g', 'u', 'p',
};

static void ble_event_callback(TAL_BLE_EVT_PARAMS_T *event)
{
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case TAL_BLE_STACK_INIT:
        if (event->ble_event.init == 0) {
            TAL_BLE_DATA_T adv = {
                .len    = sizeof(s_adv_data),
                .p_data = s_adv_data,
            };
            TAL_BLE_DATA_T response = {
                .len    = sizeof(s_scan_response),
                .p_data = s_scan_response,
            };

            tal_ble_advertising_data_set(&adv, &response);
            tal_ble_advertising_start(TUYAOS_BLE_DEFAULT_ADV_PARAM);
            bringup_uart_printf("{\"v\":1,\"type\":\"bringup\",\"payload\":{\"name\":\"wireless_selftest\","
                                "\"ble\":\"advertising\"}}\n");
        } else {
            bringup_uart_printf("{\"v\":1,\"type\":\"bringup\",\"payload\":{\"name\":\"wireless_selftest\","
                                "\"ble\":\"init_error\",\"result\":%d}}\n",
                                event->ble_event.init);
        }
        break;

    case TAL_BLE_EVT_PERIPHERAL_CONNECT:
        bringup_uart_printf("{\"v\":1,\"type\":\"bringup\",\"payload\":{\"name\":\"wireless_selftest\","
                            "\"ble\":\"connected\",\"result\":%d}}\n",
                            event->ble_event.connect.result);
        break;

    case TAL_BLE_EVT_DISCONNECT:
        bringup_uart_printf("{\"v\":1,\"type\":\"bringup\",\"payload\":{\"name\":\"wireless_selftest\","
                            "\"ble\":\"disconnected\",\"reason\":%u}}\n",
                            event->ble_event.disconnect.reason);
        tal_ble_advertising_start(TUYAOS_BLE_DEFAULT_ADV_PARAM);
        break;

    default:
        break;
    }
}

static void wifi_scan_once(void)
{
    AP_IF_S    *access_points = NULL;
    uint32_t    count         = 0;
    int         best_rssi     = INT_MIN;
    OPERATE_RET rt            = tal_wifi_all_ap_scan(&access_points, &count);

    if (rt != OPRT_OK) {
        bringup_uart_printf("{\"v\":1,\"type\":\"bringup\",\"payload\":{\"name\":\"wireless_selftest\","
                            "\"wifi\":\"scan_error\",\"result\":%d}}\n",
                            rt);
        return;
    }

    for (uint32_t index = 0; index < count; index++) {
        if ((int)access_points[index].rssi > best_rssi) {
            best_rssi = access_points[index].rssi;
        }
    }

    bringup_uart_printf("{\"v\":1,\"type\":\"bringup\",\"payload\":{\"name\":\"wireless_selftest\","
                        "\"wifi\":\"scan_ok\",\"ap_count\":%u,\"best_rssi\":%d}}\n",
                        count, count == 0 ? 0 : best_rssi);

    tal_wifi_release_ap(access_points);
}

static void user_main(void)
{
    OPERATE_RET rt;

    tal_log_init(TAL_LOG_LEVEL_DEBUG, 2048, (TAL_LOG_OUTPUT_CB)tkl_log_output);
    PR_NOTICE("======== Agentic Keyboard bring-up: wireless_selftest ========");

    rt = bringup_uart_init();
    if (rt != OPRT_OK) {
        PR_ERR("UART0 init failed: %d", rt);
        return;
    }

    tal_kv_init(&(tal_kv_cfg_t){
        .seed = "agentic-bringup",
        .key  = "wireless-selftest",
    });
    tal_sw_timer_init();
    tal_workq_init();

    rt = tal_ble_bt_init(TAL_BLE_ROLE_PERIPERAL, ble_event_callback);
    if (rt != OPRT_OK) {
        PR_ERR("BLE init failed: %d", rt);
    }

    bringup_uart_printf("{\"v\":1,\"type\":\"bringup\",\"payload\":{\"name\":\"wireless_selftest\","
                        "\"status\":\"ready\",\"ble_name\":\"AK-Bringup\"}}\n");

    while (1) {
        wifi_scan_once();
        tal_system_sleep(WIFI_SCAN_INTERVAL_MS);
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
        .thrdname   = "wireless_test",
    };

    tal_thread_create_and_start(&s_app_thread, NULL, NULL, app_thread, NULL, &thread_cfg);
}
