/**
 * @file tuya_app_main.c
 * @brief FP-2 ble-gatt-link — wireless board<->companion link over BLE GATT.
 *
 * The MVP wireless transport. The board is a BLE peripheral exposing Tuya's
 * fixed command service (service 0x1910): it NOTIFYs protocol-v1 JSONL frames
 * (shared/protocol/protocol.md) on char 0x2B10 and RECEIVES companion writes on
 * char 0x2B11. Same JSONL protocol as the UART backend, so the companion parser
 * is reused unchanged — this probe just swaps the wire from USB to BLE.
 *
 * What it validates:
 *   - advertise as "AK-KB", accept a central (PC companion / nRF Connect),
 *   - negotiate MTU, detect notify subscription,
 *   - key/nav presses -> JSONL frames NOTIFIED over the air,
 *   - companion writes -> received, logged, counted.
 *
 * UART0 (P10/P11, 115200 8N1) mirrors every frame + link state as JSONL so the
 * link is observable on serial during bring-up; the Tuya debug log stays on
 * UART1 @ 460800.
 */

#include <stdio.h>
#include <string.h>

#include "ak_board_io.h"
#include "ak_input.h"
#include "ak_protocol.h"
#include "ak_statuslcd.h"
#include "ak_uart.h"
#include "ak_feedback.h"

#include "tal_api.h"
#include "tal_bluetooth.h"
#include "tkl_output.h"

#define APP_TASK_STACK_SIZE (6U * 1024U)
#define POLL_INTERVAL_MS    5U
#define PING_INTERVAL_MS    5000U
#define SERVER_MTU          247U
#define BLE_NAME            "AK-KB"

static THREAD_HANDLE       s_app_thread = NULL;
static uint32_t            s_seq        = 0;
static bool                s_connected  = false;
static bool                s_subscribed = false;
static TAL_BLE_PEER_INFO_T s_peer;
static uint32_t            s_tx_frames = 0;
static uint32_t            s_rx_writes = 0;

/* Advertising: flags + incomplete 16-bit service UUID list (0x1910). */
static uint8_t s_adv_data[] = {
    0x02, 0x01, 0x06,       /* flags: LE General Discoverable, BR/EDR not supported */
    0x03, 0x03, 0x10, 0x19, /* complete list of 16-bit service UUIDs: 0x1910 */
};

/* Scan response: complete local name "AK-KB". */
static uint8_t s_scan_rsp[] = {
    0x06, 0x09, 'A', 'K', '-', 'K', 'B',
};

/* ------------------------------------------------------------ helpers */

static void uart_line(const char *buf, int len)
{
    if (len < 0) {
        len = (int)strlen(buf);
    }
    if (len > 0) {
        ak_uart_write(buf, (uint32_t)len);
    }
}

/* Send one JSONL frame: always to UART0 (observability); over BLE when live. */
static void emit_frame(const char *buf, int len)
{
    if (len <= 0) {
        return;
    }
    uart_line(buf, len);

    if (s_connected && s_subscribed) {
        TAL_BLE_DATA_T data = {.len = (uint16_t)len, .p_data = (uint8_t *)buf};
        if (tal_ble_server_common_send(&data) == OPRT_OK) {
            s_tx_frames++;
            ak_feedback_event("ble", "notify sent");
        }
    }
}

static void start_advertising(void)
{
    TAL_BLE_DATA_T adv = {.len = sizeof(s_adv_data), .p_data = s_adv_data};
    TAL_BLE_DATA_T rsp = {.len = sizeof(s_scan_rsp), .p_data = s_scan_rsp};
    tal_ble_advertising_data_set(&adv, &rsp);
    tal_ble_advertising_start(TUYAOS_BLE_DEFAULT_ADV_PARAM);
}

/* ------------------------------------------------------------ LCD status */

static char s_last_evt[40] = "-";

static void draw_status(void)
{
    const char *state;
    uint16_t    bar;
    char        line[40];

    if (!ak_statuslcd_ready()) {
        return;
    }
    if (s_connected && s_subscribed) {
        state = "LIVE";
        bar   = AK_COL_GREEN;
    } else if (s_connected) {
        state = "CONNECTED";
        bar   = AK_COL_DKBLUE;
    } else {
        state = "ADVERTISING";
        bar   = AK_COL_ORANGE;
    }

    ak_statuslcd_title("FP-2 ble-gatt-link", bar);
    ak_gfx_text(6, AK_STATUSLCD_CONTENT_Y + 4, "BLE " BLE_NAME, AK_COL_GRAY, AK_COL_BLACK, 1);
    ak_gfx_text(6, AK_STATUSLCD_CONTENT_Y + 18, state, AK_COL_YELLOW, AK_COL_BLACK, 2);

    (void)snprintf(line, sizeof(line), "sub:%s  MTU:%u", s_subscribed ? "yes" : "no", (unsigned)SERVER_MTU);
    ak_gfx_text(6, AK_STATUSLCD_CONTENT_Y + 44, line, AK_COL_CYAN, AK_COL_BLACK, 1);
    (void)snprintf(line, sizeof(line), "TX:%u  RX:%u", (unsigned)s_tx_frames, (unsigned)s_rx_writes);
    ak_gfx_text(6, AK_STATUSLCD_CONTENT_Y + 58, line, AK_COL_CYAN, AK_COL_BLACK, 1);

    (void)snprintf(line, sizeof(line), "Last: %s", s_last_evt);
    ak_gfx_text(6, AK_STATUSLCD_CONTENT_Y + 76, line, AK_COL_GREEN, AK_COL_BLACK, 1);

    ak_gfx_text(6, AK_STATUSLCD_H - 12, "keys/nav=notify  write->RX", AK_COL_GRAY, AK_COL_BLACK, 1);
    ak_statuslcd_flush();
}

/* ------------------------------------------------------------ input */

static void on_input(ak_input_kind_t kind, const char *id, ak_input_event_t event, uint32_t held_ms)
{
    char        buf[192];
    const char *action = ak_input_event_str(event);
    int         len;

    if (action[0] == '\0') {
        return;
    }
    s_seq++;
    if (kind == AK_INPUT_KIND_KEY) {
        len = ak_proto_key(buf, sizeof(buf), s_seq, id, action, held_ms);
    } else {
        len = ak_proto_nav(buf, sizeof(buf), s_seq, id, action, held_ms);
    }
    emit_frame(buf, len);

    (void)snprintf(s_last_evt, sizeof(s_last_evt), "%s %s", id, action);
    draw_status();
}

/* ------------------------------------------------------------ BLE events */

static void ble_event_cb(TAL_BLE_EVT_PARAMS_T *event)
{
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case TAL_BLE_STACK_INIT:
        if (event->ble_event.init == 0) {
            start_advertising();
            PR_NOTICE("BLE ready, advertising as %s", BLE_NAME);
            uart_line("{\"v\":1,\"type\":\"link\",\"payload\":{\"ble\":\"advertising\",\"name\":\"" BLE_NAME "\"}}\n",
                      -1); /* len computed below */
            ak_feedback_status("ble_gatt_link", "advertising");
            ak_feedback_ready("ble_gatt_link");
            draw_status();
        } else {
            PR_ERR("BLE init failed: %d", event->ble_event.init);
        }
        break;

    case TAL_BLE_EVT_PERIPHERAL_CONNECT: {
        s_peer      = event->ble_event.connect.peer;
        s_connected = true;
        char buf[96];
        int  len = snprintf(buf, sizeof(buf),
                            "{\"v\":1,\"type\":\"link\",\"payload\":{\"ble\":\"connected\",\"result\":%d}}\n",
                            event->ble_event.connect.result);
        uart_line(buf, len);
        ak_feedback_event("ble", "central connected");
        draw_status();
        break;
    }

    case TAL_BLE_EVT_MTU_REQUEST:
        tal_ble_server_exchange_mtu_reply(s_peer, SERVER_MTU);
        break;

    case TAL_BLE_EVT_SUBSCRIBE:
        s_subscribed = event->ble_event.subscribe.cur_notify ? true : false;
        if (s_subscribed) {
            char buf[128];
            int  len = ak_proto_hello_ack(buf, sizeof(buf), PROJECT_VERSION, "ble-gatt");
            emit_frame(buf, len);
            ak_feedback_event("ble", "notify subscribed; hello_ack sent");
        }
        draw_status();
        break;

    case TAL_BLE_EVT_WRITE_REQ: {
        TAL_BLE_DATA_T *rx = &event->ble_event.write_report.report;
        s_rx_writes++;
        /* Echo the received bytes back over UART as an observable rx frame. */
        char     buf[224];
        uint32_t n   = (rx->len < sizeof(buf) - 48) ? rx->len : (uint32_t)(sizeof(buf) - 48);
        int      len = snprintf(buf, sizeof(buf), "{\"v\":1,\"type\":\"link_rx\",\"payload\":{\"len\":%u,\"data\":\"",
                                (unsigned)rx->len);
        for (uint32_t i = 0; i < n && len < (int)sizeof(buf) - 8; i++) {
            uint8_t c  = rx->p_data[i];
            buf[len++] = (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') ? (char)c : '.';
        }
        len += snprintf(buf + len, sizeof(buf) - len, "\"}}\n");
        uart_line(buf, len);
        {
            char m[40];
            (void)snprintf(m, sizeof(m), "rx %u bytes", (unsigned)rx->len);
            ak_feedback_event("ble", m);
        }
        (void)snprintf(s_last_evt, sizeof(s_last_evt), "rx %u bytes", (unsigned)rx->len);
        draw_status();
        break;
    }

    case TAL_BLE_EVT_DISCONNECT: {
        uint8_t reason = event->ble_event.disconnect.reason;
        s_connected    = false;
        s_subscribed   = false;
        char buf[112];
        int  len = snprintf(buf, sizeof(buf),
                            "{\"v\":1,\"type\":\"link\",\"payload\":{\"ble\":\"disconnected\",\"reason\":%u}}\n",
                            (unsigned)reason);
        uart_line(buf, len);
        (void)snprintf(s_last_evt, sizeof(s_last_evt), "disc reason 0x%02X", (unsigned)reason);
        {
            char m[40];
            (void)snprintf(m, sizeof(m), "disconnected reason 0x%02X", (unsigned)reason);
            ak_feedback_event("ble", m);
        }
        ak_feedback_status("ble_gatt_link", "advertising");
        start_advertising();
        draw_status();
        break;
    }

    default:
        break;
    }
}

/* ------------------------------------------------------------ main */

static void user_main(void)
{
    OPERATE_RET rt;

    tal_log_init(TAL_LOG_LEVEL_DEBUG, 2048, (TAL_LOG_OUTPUT_CB)tkl_log_output);
    PR_NOTICE("======== Agentic Keyboard FP-2: ble-gatt-link ========");
    PR_NOTICE("Built: %s %s", __DATE__, __TIME__);

    /* Open UART0 once via the feedback layer: ak_feedback_init() calls
     * ak_uart_init() internally (single open) AND arms the feedback ready flag,
     * so both this probe's own uart_line()/emit_frame() writes and the
     * ak_feedback_*() emitters share the ONE open UART0 — no double-open. */
    rt = ak_feedback_init();
    if (rt != OPRT_OK) {
        PR_ERR("UART0 init failed: %d", rt);
        return;
    }
    rt = ak_input_init();
    if (rt != OPRT_OK) {
        PR_ERR("Input init failed: %d", rt);
        return;
    }

    rt = ak_statuslcd_init();
    if (rt != OPRT_OK) {
        PR_ERR("Status LCD init failed: %d (continuing headless)", rt);
    }
    draw_status();

    tal_kv_init(&(tal_kv_cfg_t){.seed = "agentic-fp2", .key = "ble-gatt-link"});
    tal_sw_timer_init();
    tal_workq_init();

    rt = tal_ble_bt_init(TAL_BLE_ROLE_PERIPERAL, ble_event_cb);
    if (rt != OPRT_OK) {
        PR_ERR("BLE init failed: %d", rt);
        return;
    }

    {
        char buf[128];
        int  len = ak_proto_hello_ack(buf, sizeof(buf), PROJECT_VERSION, "ble-gatt");
        uart_line(buf, len);
    }

    uint32_t last_ping = tal_system_get_millisecond();

    while (1) {
        uint32_t now = tal_system_get_millisecond();

        ak_input_poll(now, on_input);

        if ((uint32_t)(now - last_ping) >= PING_INTERVAL_MS) {
            last_ping = now;
            char buf[128];
            int  len = ak_proto_ping(buf, sizeof(buf), ++s_seq, now);
            /* ping is also a liveness beacon for the link state on UART. */
            uart_line(buf, len);
            ak_feedback_heartbeat(now);
            if (s_connected && s_subscribed) {
                TAL_BLE_DATA_T d = {.len = (uint16_t)len, .p_data = (uint8_t *)buf};
                if (tal_ble_server_common_send(&d) == OPRT_OK) {
                    s_tx_frames++;
                }
            }
            PR_DEBUG("link: conn=%d sub=%d tx=%u rx=%u", s_connected, s_subscribed, (unsigned)s_tx_frames,
                     (unsigned)s_rx_writes);
            draw_status();
        }

        tal_system_sleep(POLL_INTERVAL_MS);
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
        .thrdname   = "fp2_ble_link",
    };
    tal_thread_create_and_start(&s_app_thread, NULL, NULL, app_thread, NULL, &thread_cfg);
}
