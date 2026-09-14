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
#include "ak_asr.h"
#include "ak_cloud.h"
#include "ak_wifi.h"
#include "ak_input.h"
#include "ak_protocol.h"
#include "ak_rx.h"
#include "ak_statuslcd.h"
#include "tal_api.h"
#include "tkl_output.h"

#if AK_TRANSPORT_DUAL
/* DUAL: UART0 + BLE GATT at the same time. Every frame goes to BOTH backends —
 * ak_uart_write always writes UART0; ak_ble_gatt_write NOTIFYs only while a
 * central is subscribed and drops otherwise — so "send on the connected link"
 * falls out naturally. Reads poll both and merge into the one ak_rx assembler. */
#include "ak_ble_gatt.h"
#include "ak_uart.h"
#define AK_TRANSPORT_NAME "dual"
/* BLE still greets on notify-subscribe; UART greets immediately (both fire). */
#define AK_TRANSPORT_GREET_ON_SUBSCRIBE 1

static bool s_transport_rx_is_usb = false;

static bool ak_transport_rx_is_usb(void)
{
    return s_transport_rx_is_usb;
}

static OPERATE_RET ak_transport_init(void)
{
    /* netmgr_init() also calls tal_ble_bt_init() and replaces the single TAL
     * callback. Bring UART up now; initialize AK BLE after cloud/netmgr below. */
    OPERATE_RET ru = ak_uart_init();
    if (ru != OPRT_OK) {
        PR_WARN("dual: UART init failed: %d; continuing to delayed BLE init", ru);
    }
    return OPRT_OK;
}

static int ak_transport_write(const void *data, uint32_t length)
{
    /* Write to both; report the UART count (BLE silently drops when unsubscribed
     * so its short-write is not an error worth surfacing). */
    int wu = ak_uart_write(data, length);
    (void)ak_ble_gatt_write(data, length);
    return wu;
}

static int ak_transport_read(uint8_t *data, uint32_t length)
{
    /* Prefer UART bytes this tick; if none, drain BLE. The caller feeds whatever
     * it gets into the single ak_rx line assembler, so alternating sources are
     * fine as long as complete lines are not interleaved (they are not: the PC
     * talks over exactly one link at a time). */
    int n = ak_uart_read(data, length);
    if (n > 0) {
        s_transport_rx_is_usb = true;
        return n;
    }
    n = ak_ble_gatt_read(data, length);
    if (n > 0) {
        s_transport_rx_is_usb = false;
    }
    return n;
}
#elif AK_TRANSPORT_UART
#include "ak_uart.h"
#define ak_transport_init  ak_uart_init
#define ak_transport_write ak_uart_write
#define ak_transport_read  ak_uart_read
#define AK_TRANSPORT_NAME  "uart"
static bool ak_transport_rx_is_usb(void)
{
    return true;
}
#elif AK_TRANSPORT_BLE_GATT
#include "ak_ble_gatt.h"
#define ak_transport_init               ak_ble_gatt_init
#define ak_transport_write              ak_ble_gatt_write
#define ak_transport_read               ak_ble_gatt_read
#define AK_TRANSPORT_NAME               "ble_gatt"
/* BLE greets on notify-subscribe, not at init (see ak_ble_gatt_take_greet). */
#define AK_TRANSPORT_GREET_ON_SUBSCRIBE 1
static bool ak_transport_rx_is_usb(void)
{
    return false;
}
#else
#error "Select exactly one AK_TRANSPORT_* backend in the build profile"
#endif

#ifndef AK_TRANSPORT_GREET_ON_SUBSCRIBE
#define AK_TRANSPORT_GREET_ON_SUBSCRIBE 0
#endif

#define APP_TASK_STACK_SIZE    (5U * 1024U)
#define POLL_INTERVAL_MS       5U
#define HEARTBEAT_INTERVAL_MS  2000U
#define USB_LINK_TIMEOUT_MS    7000U
#define FRAME_BUFFER_SIZE      192U
#define APP_FW_VERSION         "0.1.0"

static THREAD_HANDLE  s_app_thread = NULL;
static uint32_t       s_sequence   = 0;
static ak_rx_reader_t s_rx;
/* UART/USB: hello marks the companion live; every later UART frame (including
 * heartbeat pong) refreshes liveness. A graceful bye or heartbeat timeout
 * clears the link, so an unplug/crash cannot leave the USB icon stuck on. */
static bool s_pc_connected = false;
#if AK_TRANSPORT_UART || AK_TRANSPORT_DUAL
static uint32_t s_usb_last_rx_ms;
#endif

#if AK_ENABLE_DISPLAY
/* netmgr callbacks can run outside the app loop. Cache WiFi state there and
 * let the app loop perform the frame-buffer redraw. */
static volatile bool s_wifi_connected    = false;
static volatile bool s_wifi_status_dirty = false;
#endif

/* Which ASR path a K4 (PTT) press uses. "system" = board only emits
 * voice.start; the PC companion runs its own ASR (Win+H). "cloud" = board mic +
 * Tuya cloud ASR (requires AK_ASR_BOARD_CLOUD + authorized/networked device).
 * Selectable from EITHER end: the PC pushes a `config` frame, or the board
 * toggles locally and reports a `voice_source` frame. Defaults to system since
 * the cloud backend is a compile-time option that is off by default. */
typedef enum { AK_VOICE_SYSTEM = 0, AK_VOICE_CLOUD } ak_voice_source_t;
static ak_voice_source_t s_voice_source = AK_VOICE_SYSTEM;
/* Route is latched on K4 DOWN. Cloud waits for the LONG event (matching FP-3
 * HOLD semantics); system emits start immediately and always receives its UP. */
static bool s_ptt_cloud_route   = false;
static bool s_ptt_cloud_started = false;
/* Double-click detector for K4: a double-click in the cloud ASR route sounds
 * the "long-press to talk" prompt tone (FP-3 plays this on K4 double-click).
 * It observes the same DOWN/UP/LONG stream and never affects the hold-to-talk
 * capture path. */
static ak_dclick_t s_ptt_dclick;

#if AK_ENABLE_DISPLAY
typedef enum {
    AK_VOICE_UI_IDLE = 0,
    AK_VOICE_UI_LISTENING,
    AK_VOICE_UI_RECOGNIZING,
} ak_voice_ui_state_t;

#define ASR_TRANSCRIPT_MAX       256U
#define ASR_BLINK_INTERVAL_MS    350U
#define ASR_SCROLL_INTERVAL_MS   1000U
#define ASR_RECOGNIZE_TIMEOUT_MS 15000U

static ak_voice_ui_state_t s_voice_ui_state = AK_VOICE_UI_IDLE;
static uint32_t            s_voice_ui_state_since_ms;
static uint32_t            s_voice_ui_last_blink_ms;
static uint32_t            s_asr_last_scroll_ms;
static uint16_t            s_asr_scroll_line;
static bool                s_voice_ui_blink_on = true;
static char                s_asr_transcript[ASR_TRANSCRIPT_MAX] = "Waiting for Tuya ASR...";
/* ASR callbacks run on the AI event thread. They copy one complete result and
 * publish the volatile flag last; the app thread consumes it before drawing. */
static char                s_asr_pending_text[ASR_TRANSCRIPT_MAX];
static volatile bool       s_asr_result_pending;
static volatile bool       s_asr_result_is_error;
#endif

/* K4 is the push-to-talk key on the expansion board (see hardware pinmap). */
#define AK_PTT_KEY_ID "k4"

static const char *ak_voice_source_str(ak_voice_source_t s)
{
    return (s == AK_VOICE_CLOUD) ? "cloud" : "system";
}

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

/* Forward declarations: PTT / voice helpers are defined below but referenced by
 * on_input_event and send_hello_ack above them. */
static void on_ptt_event(ak_input_event_t event);
static void send_voice_source(void);
#if AK_ENABLE_DISPLAY
static void ak_voice_ui_set_state(ak_voice_ui_state_t state);
static void ak_voice_ui_queue_result(const char *text, bool is_error);
static void ak_voice_ui_tick(uint32_t now_ms);
#endif

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

    /* K4 = push-to-talk. Cloud consumes DOWN/LONG/UP as one latched HOLD
     * interaction; system still uses immediate DOWN/UP Windows dictation. */
    if (kind == AK_INPUT_KIND_KEY && strcmp(id, AK_PTT_KEY_ID) == 0 &&
        (event == AK_INPUT_EVENT_DOWN || event == AK_INPUT_EVENT_LONG ||
         event == AK_INPUT_EVENT_UP)) {
        /* Double-click on K4 (cloud route) sounds the "long-press to talk"
         * prompt, mirroring FP-3. Feeding runs on every edge; a hold fires LONG
         * which the detector excludes, so hold-to-talk never trips it. */
        if (ak_dclick_feed(&s_ptt_dclick, event, tal_system_get_millisecond(), held_ms) &&
            s_voice_source == AK_VOICE_CLOUD && ak_asr_available()) {
            ak_asr_play_prompt();
        }
        on_ptt_event(event);
    }
}

static void send_hello_ack(void)
{
    char frame[FRAME_BUFFER_SIZE];
    int  length = ak_proto_hello_ack(frame, sizeof(frame), APP_FW_VERSION, AK_TRANSPORT_NAME);
    transport_write_frame(frame, length);
    /* Follow the greeting with the current voice routing so the companion UI
     * reflects the board's state as soon as a link comes up. */
    send_voice_source();
}

/* Emit a voice.start / voice.stop edge (PC companion runs its own ASR). */
static void send_voice(const char *state)
{
    char frame[FRAME_BUFFER_SIZE];
    s_sequence++;
    transport_write_frame(frame, ak_proto_voice(frame, sizeof(frame), s_sequence, state));
}

/* Report the board's current voice routing so the companion UI reflects it. */
static void send_voice_source(void)
{
    char frame[FRAME_BUFFER_SIZE];
    s_sequence++;
    transport_write_frame(frame,
                          ak_proto_voice_source(frame, sizeof(frame), s_sequence,
                                                ak_voice_source_str(s_voice_source)));
}

/* Cloud-ASR result callback: forward recognized text to the PC as an `asr`
 * frame. Registered with ak_asr_init; a no-op path when the stub is compiled. */
static void on_asr_text(const char *text)
{
    char frame[FRAME_BUFFER_SIZE];
    if (text == NULL || text[0] == '\0') {
        return;
    }
#if AK_ENABLE_DISPLAY
    ak_voice_ui_queue_result(text, false);
#endif
    s_sequence++;
    transport_write_frame(frame, ak_proto_asr(frame, sizeof(frame), s_sequence, text));
}

/* Cloud-ASR failure callback: forward "asr_error" / "asr_empty" to the PC as an
 * `asr` error frame so a press that produced no text is visible, not silently
 * dropped. Registered with ak_asr_init; no-op path when the stub is compiled. */
static void on_asr_error(const char *code)
{
    char frame[FRAME_BUFFER_SIZE];
    if (code == NULL || code[0] == '\0') {
        return;
    }
#if AK_ENABLE_DISPLAY
    ak_voice_ui_queue_result(code, true);
#endif
    s_sequence++;
    transport_write_frame(frame, ak_proto_asr_error(frame, sizeof(frame), s_sequence, code));
}

/* Wi-Fi provisioning status callback: report state to the PC as a wifi_status
 * frame. Registered with ak_wifi_init; the stub reports "failed" immediately. */
static void on_wifi_status(ak_wifi_state_t state, const char *ssid)
{
    char frame[FRAME_BUFFER_SIZE];
#if AK_ENABLE_DISPLAY
    s_wifi_connected = (state == AK_WIFI_CONNECTED);
    s_wifi_status_dirty = true;
#endif
    s_sequence++;
    transport_write_frame(frame,
                          ak_proto_wifi_status(frame, sizeof(frame), s_sequence,
                                               ak_wifi_state_str(state), ssid));
}

/* K4 push-to-talk routing. The route is decided on DOWN and held until UP:
 * - system: emit voice.start immediately, then voice.stop on UP;
 * - cloud: wait for LONG, inject FP-3 HOLD long-press, then release on UP.
 * A short cloud press intentionally starts no ASR session, matching FP-3. */
static void on_ptt_event(ak_input_event_t event)
{
    if (event == AK_INPUT_EVENT_DOWN) {
        s_ptt_cloud_route =
            (s_voice_source == AK_VOICE_CLOUD && ak_asr_available());
        s_ptt_cloud_started = false;
        if (s_ptt_cloud_route) {
#if AK_ENABLE_DISPLAY
            /* Show feedback from the physical press immediately. Capture starts
             * on LONG below, matching the official HOLD state machine. */
            ak_voice_ui_set_state(AK_VOICE_UI_LISTENING);
#endif
        } else {
            send_voice("start");
        }
        return;
    }

    if (event == AK_INPUT_EVENT_LONG) {
        if (s_ptt_cloud_route && !s_ptt_cloud_started) {
            ak_asr_start();
            s_ptt_cloud_started = true;
        }
        return;
    }

    if (event == AK_INPUT_EVENT_UP) {
        if (s_ptt_cloud_route) {
            if (s_ptt_cloud_started) {
                ak_asr_stop();
#if AK_ENABLE_DISPLAY
                ak_voice_ui_set_state(AK_VOICE_UI_RECOGNIZING);
#endif
            } else {
#if AK_ENABLE_DISPLAY
                /* A short cloud press never opened a HOLD session. */
                ak_voice_ui_set_state(AK_VOICE_UI_IDLE);
#endif
            }
        } else {
            send_voice("stop");
        }
        s_ptt_cloud_route = false;
        s_ptt_cloud_started = false;
    }
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

#define ASR_PANEL_Y       116U
#define ASR_TEXT_Y        140U
#define ASR_TEXT_COLS     38U /* 38 glyphs * 8 px = 304 px */
#define ASR_VISIBLE_LINES 10U
#define ASR_LINE_H        10U

static const char *ak_voice_ui_label(void)
{
    switch (s_voice_ui_state) {
    case AK_VOICE_UI_LISTENING:
        return "LISTENING";
    case AK_VOICE_UI_RECOGNIZING:
        return "RECOGNIZING";
    case AK_VOICE_UI_IDLE:
    default:
        return "IDLE";
    }
}

static uint16_t ak_voice_ui_color(void)
{
    switch (s_voice_ui_state) {
    case AK_VOICE_UI_LISTENING:
        return AK_COL_RED;
    case AK_VOICE_UI_RECOGNIZING:
        return AK_COL_YELLOW;
    case AK_VOICE_UI_IDLE:
    default:
        return AK_COL_GREEN;
    }
}

/* Draw a small filled circle using horizontal spans. */
static void ak_voice_ui_dot(uint16_t cx, uint16_t cy, uint16_t color)
{
    const int radius = 6;
    for (int dy = -radius; dy <= radius; dy++) {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= radius * radius) {
            dx++;
        }
        ak_gfx_fill_rect((uint16_t)((int)cx - dx), (uint16_t)((int)cy + dy),
                         (uint16_t)(2 * dx + 1), 1, color);
    }
}

/* Consume one UTF-8 code point. The built-in 8x8 font is ASCII-only, so a
 * non-ASCII code point is shown as one '?' rather than one '?' per UTF-8 byte. */
static char ak_asr_next_glyph(const char **cursor)
{
    const unsigned char *p = (const unsigned char *)*cursor;
    unsigned char c = *p++;

    if (c >= 0x80U) {
        while ((*p & 0xC0U) == 0x80U) {
            p++;
        }
        *cursor = (const char *)p;
        return '?';
    }
    *cursor = (const char *)p;
    return (c >= 0x20U && c < 0x7FU) ? (char)c : ' ';
}

static uint16_t ak_asr_line_count(const char *text)
{
    uint16_t lines = 1;
    uint16_t col = 0;
    const char *p = text;

    while (*p != '\0') {
        if (*p == '\n') {
            p++;
            lines++;
            col = 0;
            continue;
        }
        (void)ak_asr_next_glyph(&p);
        col++;
        if (col >= ASR_TEXT_COLS && *p != '\0') {
            lines++;
            col = 0;
        }
    }
    return lines;
}

static void ak_asr_draw_line(uint16_t line_index, uint16_t scroll_line,
                             const char *line)
{
    if (line_index < scroll_line || line_index >= scroll_line + ASR_VISIBLE_LINES) {
        return;
    }
    uint16_t visible = (uint16_t)(line_index - scroll_line);
    ak_gfx_text(6, (uint16_t)(ASR_TEXT_Y + visible * ASR_LINE_H), line,
                AK_COL_WHITE, AK_COL_BLACK, 1);
}

static void ak_asr_draw_transcript(void)
{
    char line[ASR_TEXT_COLS + 1U];
    uint16_t col = 0;
    uint16_t line_index = 0;
    const char *p = s_asr_transcript;

    while (*p != '\0') {
        if (*p == '\n') {
            p++;
            line[col] = '\0';
            ak_asr_draw_line(line_index++, s_asr_scroll_line, line);
            col = 0;
            continue;
        }
        line[col++] = ak_asr_next_glyph(&p);
        if (col >= ASR_TEXT_COLS) {
            line[col] = '\0';
            ak_asr_draw_line(line_index++, s_asr_scroll_line, line);
            col = 0;
        }
    }
    if (col > 0 || line_index == 0) {
        line[col] = '\0';
        ak_asr_draw_line(line_index, s_asr_scroll_line, line);
    }
}

/* Compose the connection-status header text + color for the title bar.
 * Header reads "Companion: <state>". USB reflects whether the PC companion has
 * greeted (Connected green / Disconnected orange). BLE reflects the live link
 * state: ADVERTISING (orange) -> CONNECTED (blue) -> LIVE (green).
 * `out` must hold at least 28 bytes. Returns the title-bar color. */
static uint16_t ak_link_header(char *out, uint32_t out_len)
{
#if AK_TRANSPORT_DUAL
    /* Show both links. Green if EITHER is up (USB greeted or BLE live). */
    const char *usb = s_pc_connected ? "USB" : "usb";
    ak_ble_link_state_t bs = ak_ble_gatt_link_state();
    const char *ble = (bs == AK_BLE_LINK_LIVE) ? "BLE"
                    : (bs == AK_BLE_LINK_CONNECTED) ? "ble*"
                    : "adv";
    (void)snprintf(out, out_len, "Companion: %s | %s", usb, ble);
    bool up = s_pc_connected || (bs == AK_BLE_LINK_LIVE);
    return up ? AK_COL_GREEN : AK_COL_ORANGE;
#elif AK_TRANSPORT_UART
    if (s_pc_connected) {
        (void)snprintf(out, out_len, "Companion: USB Connected");
        return AK_COL_GREEN;
    }
    (void)snprintf(out, out_len, "Companion: USB Disconnected");
    return AK_COL_ORANGE;
#else
    switch (ak_ble_gatt_link_state()) {
    case AK_BLE_LINK_LIVE:
        (void)snprintf(out, out_len, "Companion: BLE LIVE");
        return AK_COL_GREEN;
    case AK_BLE_LINK_CONNECTED:
        (void)snprintf(out, out_len, "Companion: BLE CONN");
        return AK_COL_DKBLUE;
    case AK_BLE_LINK_ADVERTISING:
    default:
        (void)snprintf(out, out_len, "Companion: BLE ADV");
        return AK_COL_ORANGE;
    }
#endif
}

/* Render a Kiro Crew agent-status screen from a `display` frame. The title bar
 * doubles as the connection-status header (transport + BLE link state). */
static void ak_render_status(const char *agent, const char *state, const char *text)
{
    if (!ak_statuslcd_ready()) {
        return;
    }
    char     header[32];
    uint16_t link_color = ak_link_header(header, sizeof(header));
    char     line[48];
    uint16_t voice_color = ak_voice_ui_color();

    bool usb_connected = false;
    bool ble_connected = false;
#if AK_TRANSPORT_UART || AK_TRANSPORT_DUAL
    usb_connected = s_pc_connected;
#endif
#if AK_TRANSPORT_BLE_GATT || AK_TRANSPORT_DUAL
    ble_connected = (ak_ble_gatt_link_state() != AK_BLE_LINK_ADVERTISING);
#endif

    /* Title bar = connection-status header. Right side shows WiFi, USB and BLE
     * link state. The voice dot sits just left of the three icons. */
    ak_statuslcd_title_links(header, link_color, s_wifi_connected, usb_connected,
                             ble_connected);
    ak_voice_ui_dot(244, 11,
                    (s_voice_ui_state == AK_VOICE_UI_LISTENING && !s_voice_ui_blink_on)
                        ? link_color
                        : voice_color);

    (void)snprintf(line, sizeof(line), "agent: %s", agent[0] ? agent : "-");
    ak_gfx_text(6, AK_STATUSLCD_CONTENT_Y + 2, line, AK_COL_WHITE, AK_COL_BLACK, 1);
    (void)snprintf(line, sizeof(line), "state: %s", state[0] ? state : "-");
    ak_gfx_text(6, AK_STATUSLCD_CONTENT_Y + 16, line, ak_status_color(state), AK_COL_BLACK, 1);
    (void)snprintf(line, sizeof(line), "voice: %s%s", ak_voice_source_str(s_voice_source),
                   (s_voice_source == AK_VOICE_CLOUD && !ak_asr_available()) ? " (stub)" : "");
    ak_gfx_text(6, AK_STATUSLCD_CONTENT_Y + 30, line, AK_COL_CYAN, AK_COL_BLACK, 1);
    (void)snprintf(line, sizeof(line), "ASR: %s", ak_voice_ui_label());
    ak_gfx_text(6, AK_STATUSLCD_CONTENT_Y + 44, line, voice_color, AK_COL_BLACK, 1);
    ak_status_wrap(6, AK_STATUSLCD_CONTENT_Y + 58, text, AK_COL_GRAY, 3);

    /* Fixed lower-half transcript panel. New results reset to line zero; long
     * results advance one wrapped line per second in ak_voice_ui_tick(). */
    ak_gfx_fill_rect(0, ASR_PANEL_Y, AK_STATUSLCD_W, 1, AK_COL_GRAY);
    ak_gfx_text(6, ASR_PANEL_Y + 6, "Tuya ASR transcript", AK_COL_CYAN, AK_COL_BLACK, 1);
    ak_asr_draw_transcript();
    ak_statuslcd_flush();
}

/* Redraw just the connection-status header, preserving the last agent status
 * content. Cheap enough to call on every BLE link-state change. */
static char s_last_agent[32] = "-";
static char s_last_state[16] = "idle";
static char s_last_text[128] = "waiting for Kiro Crew";

static void ak_refresh_link_header(void)
{
    ak_render_status(s_last_agent, s_last_state, s_last_text);
}

static void ak_voice_ui_set_state(ak_voice_ui_state_t state)
{
    s_voice_ui_state = state;
    s_voice_ui_state_since_ms = tal_system_get_millisecond();
    s_voice_ui_last_blink_ms = s_voice_ui_state_since_ms;
    s_voice_ui_blink_on = true;
    ak_refresh_link_header();
}

static void ak_voice_ui_queue_result(const char *text, bool is_error)
{
    if (text == NULL) {
        return;
    }
    (void)snprintf(s_asr_pending_text, sizeof(s_asr_pending_text), "%s", text);
    s_asr_result_is_error = is_error;
    /* Publish last: the app loop never reads a partially copied result. */
    s_asr_result_pending = true;
}

static void ak_voice_ui_tick(uint32_t now_ms)
{
    bool redraw = false;

    if (s_asr_result_pending) {
        bool is_error = s_asr_result_is_error;
        if (is_error && strcmp(s_asr_pending_text, "asr_empty") == 0) {
            (void)snprintf(s_asr_transcript, sizeof(s_asr_transcript),
                           "No speech recognized.");
        } else if (is_error) {
            (void)snprintf(s_asr_transcript, sizeof(s_asr_transcript),
                           "Recognition failed.");
        } else {
            (void)snprintf(s_asr_transcript, sizeof(s_asr_transcript), "%s",
                           s_asr_pending_text);
        }
        s_asr_result_pending = false;
        s_asr_scroll_line = 0;
        s_asr_last_scroll_ms = now_ms;
        s_voice_ui_state = AK_VOICE_UI_IDLE;
        s_voice_ui_state_since_ms = now_ms;
        s_voice_ui_blink_on = true;
        redraw = true;
    }

    if (s_voice_ui_state == AK_VOICE_UI_LISTENING &&
        (uint32_t)(now_ms - s_voice_ui_last_blink_ms) >= ASR_BLINK_INTERVAL_MS) {
        s_voice_ui_last_blink_ms = now_ms;
        s_voice_ui_blink_on = !s_voice_ui_blink_on;
        redraw = true;
    }

    if (s_voice_ui_state == AK_VOICE_UI_RECOGNIZING &&
        (uint32_t)(now_ms - s_voice_ui_state_since_ms) >= ASR_RECOGNIZE_TIMEOUT_MS) {
        (void)snprintf(s_asr_transcript, sizeof(s_asr_transcript),
                       "Recognition timed out.");
        s_voice_ui_state = AK_VOICE_UI_IDLE;
        s_voice_ui_state_since_ms = now_ms;
        s_asr_scroll_line = 0;
        redraw = true;
    }

    uint16_t total_lines = ak_asr_line_count(s_asr_transcript);
    uint16_t max_scroll = (total_lines > ASR_VISIBLE_LINES)
                              ? (uint16_t)(total_lines - ASR_VISIBLE_LINES)
                              : 0;
    if (max_scroll > 0 &&
        (uint32_t)(now_ms - s_asr_last_scroll_ms) >= ASR_SCROLL_INTERVAL_MS) {
        s_asr_last_scroll_ms = now_ms;
        s_asr_scroll_line = (s_asr_scroll_line >= max_scroll)
                                ? 0
                                : (uint16_t)(s_asr_scroll_line + 1U);
        redraw = true;
    }

    if (redraw) {
        ak_refresh_link_header();
    }
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
        /* Only a greeting received over UART proves the USB companion link is
         * up. In DUAL mode a BLE hello must not light the USB icon. */
        if (ak_transport_rx_is_usb() && !s_pc_connected) {
            s_pc_connected = true;
#if AK_ENABLE_DISPLAY
            ak_refresh_link_header();
#endif
        }
    } else if (strcmp(type, "ping") == 0) {
        send_pong();
    } else if (strcmp(type, "bye") == 0) {
        /* The PC companion is disconnecting gracefully. Plain UART gives no
         * hardware disconnect edge, so this frame is how the board learns the
         * link is going down -> flip the header to "USB Disconnected". */
        if (ak_transport_rx_is_usb() && s_pc_connected) {
            s_pc_connected = false;
#if AK_ENABLE_DISPLAY
            ak_refresh_link_header();
#endif
        }
    } else if (strcmp(type, "config") == 0) {
        uint32_t seq = 0;
        ak_json_uint(line, "seq", &seq);
        /* The PC selects the voice path here: {"voice_engine":"cloud"|"system"}
         * (the companion mirrors its choice to the board via config). */
        char engine[16] = "";
        if (ak_json_str(line, "voice_engine", engine, sizeof(engine))) {
            ak_voice_source_t want =
                (strcmp(engine, "cloud") == 0) ? AK_VOICE_CLOUD : AK_VOICE_SYSTEM;
            if (want != s_voice_source) {
                s_voice_source = want;
                PR_NOTICE("voice source -> %s (from PC config)", ak_voice_source_str(want));
#if AK_ENABLE_DISPLAY
                ak_refresh_link_header();
#endif
            }
            /* Echo the resulting source so the UI reflects the applied value. */
            send_voice_source();
        }
        send_ack(seq);
    } else if (strcmp(type, "wifi") == 0) {
        /* PC provisions Wi-Fi: {"type":"wifi","payload":{"ssid":"..","pass":".."}}.
         * Apply via ak_wifi (netmgr) so the board connects + cloud-activates;
         * the connect result comes back asynchronously as a wifi_status frame. */
        uint32_t seq  = 0;
        char     ssid[64] = "";
        char     pass[80] = "";
        ak_json_uint(line, "seq", &seq);
        ak_json_str(line, "ssid", ssid, sizeof(ssid));
        ak_json_str(line, "pass", pass, sizeof(pass));
        PR_NOTICE("wifi config: ssid=%s (pass %u chars)", ssid, (unsigned)strlen(pass));
        ak_wifi_connect(ssid, pass);
        send_ack(seq);
    } else if (strcmp(type, "license") == 0) {
        /* PC provisions the device license: {"type":"license","payload":
         * {"uuid":"..","authkey":".."}}. Written to persistent storage; PID
         * stays compile-time. Never echo the secret back — only a status. */
        uint32_t seq          = 0;
        char     uuid[40]     = "";
        char     authkey[48]  = "";
        ak_json_uint(line, "seq", &seq);
        ak_json_str(line, "uuid", uuid, sizeof(uuid));
        ak_json_str(line, "authkey", authkey, sizeof(authkey));
        PR_NOTICE("license write: uuid len %u, authkey len %u", (unsigned)strlen(uuid),
                  (unsigned)strlen(authkey));
        ak_license_result_t lr = ak_license_write(uuid, authkey);
        {
            char frame[FRAME_BUFFER_SIZE];
            s_sequence++;
            transport_write_frame(frame, ak_proto_license_status(frame, sizeof(frame), s_sequence,
                                                                  ak_license_result_str(lr)));
        }
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
        snprintf(s_last_agent, sizeof(s_last_agent), "%s", agent);
        snprintf(s_last_state, sizeof(s_last_state), "%s", state);
        snprintf(s_last_text, sizeof(s_last_text), "%s", text);
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
    ak_dclick_init(&s_ptt_dclick);

#if AK_ENABLE_DISPLAY
    /* Status LCD FIRST — before any AI / Wi-Fi bring-up. The board-side cloud
     * ASR (ai_chat_init) and netmgr can block or fault at boot when the device
     * is not yet provisioned (no License / no Wi-Fi), so initializing them
     * before the display would leave the screen black. Bring the LCD up and
     * paint the boot screen first; the display must never depend on the cloud
     * stack. (shared ak_statuslcd + ak_gfx, no LVGL; headless fallback.) */
    rt = ak_statuslcd_init();
    if (rt != OPRT_OK) {
        PR_WARN("status LCD init failed: %d (continuing headless)", rt);
    } else {
        ak_render_status("-", "idle", "waiting for Kiro Crew");
    }
#endif

    /* Tuya IoT / cloud bring-up (stub unless AK_ASR_BOARD_CLOUD is set). MUST run
     * BEFORE ak_asr_init: ai_chat_init needs an initialized IoT / AI-service
     * context (tuya_iot_init + netmgr), or it dereferences a NULL handle and
     * hard-faults at boot (the black-screen crash). No-op on the v1/BLE profiles. */
    OPERATE_RET cloud_rt = ak_cloud_init();
#if AK_TRANSPORT_DUAL
    /* Cloud netmgr has already initialized TAL's common GATT service. Do NOT
     * call tal_ble_bt_init() a second time: T5's one-shot service guard would
     * leave TAL's rewritten characteristic handles invalid, making every
     * notify fail even though AK-KB connects. Non-cloud DUAL still needs the
     * normal first-time BLE initialization. */
#if AK_ASR_BOARD_CLOUD
    OPERATE_RET ble_rt = ak_ble_gatt_takeover();
#else
    OPERATE_RET ble_rt = ak_ble_gatt_init();
#endif
    if (ble_rt != OPRT_OK) {
        PR_ERR("dual: delayed AK BLE setup failed: %d (UART remains available)", ble_rt);
    }
#endif
    if (cloud_rt != OPRT_OK) {
        PR_ERR("cloud init failed: %d — skipping ai_chat_init to avoid a fault; "
               "board-side cloud ASR unavailable (keyboard still works)",
               cloud_rt);
    } else {
        /* Board-side cloud ASR backend (stub unless AK_ASR_BOARD_CLOUD is set).
         * Registers the recognized-text callback that emits `asr` frames. Only
         * reached once the IoT context exists, so ai_chat_init cannot fault. */
        ak_asr_init(on_asr_text, on_asr_error);
    }
    PR_NOTICE("Voice source: %s (cloud backend %s)", ak_voice_source_str(s_voice_source),
              ak_asr_available() ? "ready" : "stub/off");

    /* Wi-Fi provisioning (netmgr in the cloud profile, stub otherwise). The PC
     * pushes credentials over a `wifi` frame; results return as wifi_status. */
    ak_wifi_init(on_wifi_status);
    PR_NOTICE("Wi-Fi provisioning: %s", ak_wifi_available() ? "ready" : "stub/off");

    /* Start the Tuya IoT task (tuya_iot_start + a dedicated tuya_iot_yield loop)
     * once everything is initialized. No-op on v1/BLE; runs off the input/RX
     * loop so key latency is unaffected. Skipped implicitly if cloud init failed
     * (ak_cloud_start returns an error, logged and non-fatal). */
    if (cloud_rt == OPRT_OK && ak_cloud_start() != OPRT_OK) {
        PR_ERR("cloud start failed; device will not reach the Tuya cloud");
    }

    now            = tal_system_get_millisecond();
    last_heartbeat = now;

    ak_rx_reader_init(&s_rx);
#if AK_TRANSPORT_DUAL
    /* DUAL: UART0 is up immediately, so greet now over both (BLE drops it until
     * subscribed); BLE additionally greets on notify-subscribe in the loop. */
    send_hello_ack();
#elif !AK_TRANSPORT_GREET_ON_SUBSCRIBE
    /* Wired transports are up as soon as init returns: greet immediately. */
    send_hello_ack();
#else
    /* BLE greets on notify-subscribe instead (polled in the loop below). */
    PR_NOTICE("Transport is BLE; hello_ack deferred until notify-subscribe");
#endif
    PR_NOTICE("Firmware ready; scanning inputs, heartbeat every %ums", HEARTBEAT_INTERVAL_MS);

    while (1) {
        now = tal_system_get_millisecond();

#if AK_ENABLE_DISPLAY
        ak_voice_ui_tick(now);
        if (s_wifi_status_dirty) {
            s_wifi_status_dirty = false;
            ak_refresh_link_header();
        }
#endif

#if AK_TRANSPORT_BLE_GATT
        ak_ble_gatt_maintain(now);
#endif

#if AK_TRANSPORT_GREET_ON_SUBSCRIBE
        /* When a central subscribes to notifications, the link is live: greet. */
        if (ak_ble_gatt_take_greet()) {
            send_hello_ack();
        }
        /* Refresh the connection-status header when the BLE link state changes
         * (advertising -> connected -> live, or back to advertising on drop). */
#if AK_ENABLE_DISPLAY
        {
            static ak_ble_link_state_t s_last_link = (ak_ble_link_state_t)-1;
            ak_ble_link_state_t        link = ak_ble_gatt_link_state();
            if (link != s_last_link) {
                s_last_link = link;
                ak_refresh_link_header();
            }
        }
#endif
#endif

        ak_input_poll(now, on_input_event);

        uint8_t rx[64];
        int     got = ak_transport_read(rx, sizeof(rx));
        if (got > 0) {
#if AK_TRANSPORT_UART || AK_TRANSPORT_DUAL
            if (ak_transport_rx_is_usb()) {
                /* The companion answers the board's 2 s ping with pong, so any
                 * UART traffic proves the USB protocol link is still alive. */
                s_usb_last_rx_ms = now;
            }
#endif
            ak_rx_reader_feed(&s_rx, rx, (size_t)got, on_rx_line);
        }

#if AK_TRANSPORT_UART || AK_TRANSPORT_DUAL
        if (s_pc_connected &&
            (uint32_t)(now - s_usb_last_rx_ms) >= USB_LINK_TIMEOUT_MS) {
            s_pc_connected = false;
            PR_NOTICE("USB companion timed out; marking link disconnected");
#if AK_ENABLE_DISPLAY
            ak_refresh_link_header();
#endif
        }
#endif

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
