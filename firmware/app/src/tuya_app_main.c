/**
 * @file tuya_app_main.c
 * @brief Agentic Keyboard for Kiro -- product firmware entry.
 *
 * Phase-1 (wired) flow so far:
 *   - open the selected transport (UART0),
 *   - announce capabilities (hello_ack),
 *   - scan 5 keys + 5-way nav and emit key/nav events (JSONL protocol v1),
 *   - emit a periodic ping heartbeat.
 *
 * The debounce/long-press logic lives in the pure, host-tested ak_input_fsm;
 * frame formatting lives in the pure, host-tested ak_protocol. RX/ack parsing,
 * LCD/LVGL, BLE and board cloud ASR come in later tasks; the transport backend
 * is selected at build time via ak_features.h.
 */

#include <string.h>

#include "ak_board_io.h"
#include "ak_display.h"
#include "ak_features.h"
#include "ak_input.h"
#include "ak_protocol.h"
#include "ak_rx.h"
#include "ak_statuslcd.h"
#include "tal_api.h"
#include "tkl_output.h"

#if AK_TRANSPORT_UART
#include "ak_uart.h"
#define ak_transport_init  ak_uart_init
#define ak_transport_write ak_uart_write
#define ak_transport_read  ak_uart_read
#define AK_TRANSPORT_NAME  "uart"
#elif AK_TRANSPORT_BLE_GATT
#include "ak_ble_gatt.h"
#define ak_transport_init               ak_ble_gatt_init
#define ak_transport_write              ak_ble_gatt_write
#define ak_transport_read               ak_ble_gatt_read
#define AK_TRANSPORT_NAME               "ble_gatt"
/* BLE greets on notify-subscribe, not at init (see ak_ble_gatt_take_greet). */
#define AK_TRANSPORT_GREET_ON_SUBSCRIBE 1
#else
#error "Select exactly one AK_TRANSPORT_* backend in the build profile"
#endif

#ifndef AK_TRANSPORT_GREET_ON_SUBSCRIBE
#define AK_TRANSPORT_GREET_ON_SUBSCRIBE 0
#endif

#define APP_TASK_STACK_SIZE   (5U * 1024U)
#define POLL_INTERVAL_MS      5U
#define HEARTBEAT_INTERVAL_MS 2000U
#define FRAME_BUFFER_SIZE     192U
#define APP_FW_VERSION        "0.1.0"

static THREAD_HANDLE  s_app_thread = NULL;
static uint32_t       s_sequence   = 0;
static ak_rx_reader_t s_rx;

static void transport_write_frame(const char *frame, int length)
{
    if (length <= 0 || length >= (int)FRAME_BUFFER_SIZE) {
        return;
    }
    int written = ak_transport_write(frame, (uint32_t)length);
    if (written != length) {
        PR_WARN("transport short write: %d/%d", written, length);
    }
}

/* Input event -> key/nav JSONL frame on the active transport. */
static void on_input_event(ak_input_kind_t kind, const char *id, ak_input_event_t event, uint32_t held_ms)
{
    char        frame[FRAME_BUFFER_SIZE];
    const char *action = ak_input_event_str(event);
    int         length;

    s_sequence++;
    if (kind == AK_INPUT_KIND_KEY) {
        length = ak_proto_key(frame, sizeof(frame), s_sequence, id, action, held_ms);
    } else {
        length = ak_proto_nav(frame, sizeof(frame), s_sequence, id, action, held_ms);
    }
    transport_write_frame(frame, length);
    PR_DEBUG("input %s %s held=%ums", id, action, held_ms);
}

static void send_hello_ack(void)
{
    char frame[FRAME_BUFFER_SIZE];
    int  length = ak_proto_hello_ack(frame, sizeof(frame), APP_FW_VERSION, AK_TRANSPORT_NAME);
    transport_write_frame(frame, length);
}

static void send_heartbeat(uint32_t uptime_ms)
{
    char frame[FRAME_BUFFER_SIZE];
    int  length;

    s_sequence++;
    length = ak_proto_ping(frame, sizeof(frame), s_sequence, uptime_ms);
    transport_write_frame(frame, length);
}

static void send_pong(void)
{
    char frame[FRAME_BUFFER_SIZE];
    transport_write_frame(frame, ak_proto_pong(frame, sizeof(frame)));
}

static void send_ack(uint32_t seq)
{
    char frame[FRAME_BUFFER_SIZE];
    transport_write_frame(frame, ak_proto_ack(frame, sizeof(frame), seq));
}

#if AK_ENABLE_DISPLAY
/* Map a Kiro Crew agent state string to a title-bar color. */
static uint16_t ak_status_color(const char *state)
{
    if (strcmp(state, "running") == 0) {
        return AK_COL_GREEN;
    } else if (strcmp(state, "tool") == 0) {
        return AK_COL_ORANGE;
    } else if (strcmp(state, "idle") == 0) {
        return AK_COL_GRAY;
    } else if (strcmp(state, "received") == 0) {
        return AK_COL_CYAN;
    }
    return AK_COL_DKBLUE;
}

/* Word-blind wrap of ASCII text into the content area; non-ASCII -> '.'. */
static void ak_status_wrap(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t max_lines)
{
    const uint16_t cols   = 36; /* ~ (320-12)/8 chars per line at scale 1 */
    const uint16_t line_h = 10;
    char           buf[40];
    uint16_t       col = 0, ln = 0;

    for (const char *p = s; *p && ln < max_lines; ++p) {
        unsigned char c = (unsigned char)*p;
        buf[col++]      = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        if (col >= cols) {
            buf[col] = '\0';
            ak_gfx_text(x, (uint16_t)(y + ln * line_h), buf, fg, AK_COL_BLACK, 1);
            col = 0;
            ln++;
        }
    }
    if (col > 0 && ln < max_lines) {
        buf[col] = '\0';
        ak_gfx_text(x, (uint16_t)(y + ln * line_h), buf, fg, AK_COL_BLACK, 1);
    }
}

/* Render a Kiro Crew agent-status screen from a `display` frame. */
static void ak_render_status(const char *agent, const char *state, const char *text)
{
    if (!ak_statuslcd_ready()) {
        return;
    }
    uint16_t color = ak_status_color(state);
    char     line[48];

    ak_statuslcd_title("Kiro Crew", color);
    (void)snprintf(line, sizeof(line), "agent: %s", agent[0] ? agent : "-");
    ak_gfx_text(6, AK_STATUSLCD_CONTENT_Y + 2, line, AK_COL_WHITE, AK_COL_BLACK, 1);
    (void)snprintf(line, sizeof(line), "state: %s", state[0] ? state : "-");
    ak_gfx_text(6, AK_STATUSLCD_CONTENT_Y + 16, line, color, AK_COL_BLACK, 1);
    ak_status_wrap(6, AK_STATUSLCD_CONTENT_Y + 34, text, AK_COL_YELLOW, 14);
    ak_statuslcd_flush();
}
#endif /* AK_ENABLE_DISPLAY */

/* Handle one inbound JSONL line (PC -> board). Unknown types are ignored. */
static void on_rx_line(const char *line)
{
    char type[16];
    if (!ak_json_str(line, "type", type, sizeof(type))) {
        return;
    }
    if (strcmp(type, "hello") == 0) {
        send_hello_ack();
    } else if (strcmp(type, "ping") == 0) {
        send_pong();
    } else if (strcmp(type, "config") == 0) {
        uint32_t seq = 0;
        ak_json_uint(line, "seq", &seq);
        send_ack(seq);
    } else if (strcmp(type, "display") == 0) {
        char agent[32] = "";
        char state[16] = "";
        char text[128] = "";
        ak_json_str(line, "agent_name", agent, sizeof(agent));
        ak_json_str(line, "state", state, sizeof(state));
        ak_json_str(line, "text", text, sizeof(text));
        PR_NOTICE("display: agent=%s state=%s", agent, state);
#if AK_ENABLE_DISPLAY
        ak_render_status(agent, state, text);
#endif
    }
    /* ack / pong / unknown: ignore per protocol */
}

static void user_main(void)
{
    OPERATE_RET rt;
    uint32_t    now;
    uint32_t    last_heartbeat;

    tal_log_init(TAL_LOG_LEVEL_DEBUG, 2048, (TAL_LOG_OUTPUT_CB)tkl_log_output);

    PR_NOTICE("======== Agentic Keyboard for Kiro: product fw %s ========", APP_FW_VERSION);
    PR_NOTICE("Board: expansion baseboard v1.2 (WS2812=P%u/SPI1, LCD_BL=P%u)", (unsigned)AK_WS2812_DATA_PIN,
              (unsigned)AK_LCD_BACKLIGHT_PIN);
#if AK_TRANSPORT_UART
    PR_NOTICE("Transport: %s (UART%u RX=P%u TX=P%u @%u 8N1)", AK_TRANSPORT_NAME, (unsigned)AK_UART_PORT,
              (unsigned)AK_UART_RX_PIN, (unsigned)AK_UART_TX_PIN, (unsigned)AK_UART_BAUDRATE);
#else
    PR_NOTICE("Transport: %s (BLE GATT peripheral, service 0x1910)", AK_TRANSPORT_NAME);
#endif
    PR_NOTICE("Inputs: 5 keys + 5-way nav, debounce=%ums long=%ums", (unsigned)AK_FSM_DEBOUNCE_MS,
              (unsigned)AK_FSM_LONG_PRESS_MS);

    rt = ak_transport_init();
    if (rt != OPRT_OK) {
        PR_ERR("transport init failed: %d", rt);
        return;
    }
    rt = ak_input_init();
    if (rt != OPRT_OK) {
        PR_ERR("input init failed: %d", rt);
        return;
    }

#if AK_ENABLE_DISPLAY
    /* Status LCD (shared ak_statuslcd + ak_gfx, no LVGL): renders Kiro Crew
       agent status from inbound `display` frames. Falls back to headless. */
    rt = ak_statuslcd_init();
    if (rt != OPRT_OK) {
        PR_WARN("status LCD init failed: %d (continuing headless)", rt);
    } else {
        ak_render_status("-", "idle", "waiting for Kiro Crew");
    }
#endif

    now            = tal_system_get_millisecond();
    last_heartbeat = now;

    ak_rx_reader_init(&s_rx);
#if !AK_TRANSPORT_GREET_ON_SUBSCRIBE
    /* Wired transports are up as soon as init returns: greet immediately. */
    send_hello_ack();
#else
    /* BLE greets on notify-subscribe instead (polled in the loop below). */
    PR_NOTICE("Transport is BLE; hello_ack deferred until notify-subscribe");
#endif
    PR_NOTICE("Firmware ready; scanning inputs, heartbeat every %ums", HEARTBEAT_INTERVAL_MS);

    while (1) {
        now = tal_system_get_millisecond();

#if AK_TRANSPORT_GREET_ON_SUBSCRIBE
        /* When a central subscribes to notifications, the link is live: greet. */
        if (ak_ble_gatt_take_greet()) {
            send_hello_ack();
        }
#endif

        ak_input_poll(now, on_input_event);

        uint8_t rx[64];
        int     got = ak_transport_read(rx, sizeof(rx));
        if (got > 0) {
            ak_rx_reader_feed(&s_rx, rx, (size_t)got, on_rx_line);
        }

        if ((uint32_t)(now - last_heartbeat) >= HEARTBEAT_INTERVAL_MS) {
            last_heartbeat = now;
            send_heartbeat(now);
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
        .thrdname   = "ak_app",
    };

    tal_thread_create_and_start(&s_app_thread, NULL, NULL, app_thread, NULL, &thread_cfg);
}
