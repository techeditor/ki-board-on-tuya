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
#include "ak_font8x8.h"
#include "ak_asr.h"
#include "ak_cloud.h"
#include "ak_wifi.h"
#include "ak_input.h"
#include "ak_icons16.h"
#include "ak_session_icons.h"
#include "ak_protocol.h"
#include "ak_rx.h"
#include "ak_settings.h"
#include "ak_statuslcd.h"
#include "ak_voice_route.h"
#include "tal_api.h"
#include "tal_mutex.h"
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

static bool     s_transport_rx_is_usb = false;
static bool     s_ak_ble_active       = false;
static uint32_t s_ak_ble_retry_at_ms  = 0U;

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
    if (s_ak_ble_active) {
        (void)ak_ble_gatt_write(data, length);
    }
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
    if (!s_ak_ble_active) {
        return 0;
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

#if AK_TRANSPORT_DUAL
#define AK_BLE_RUNTIME_ACTIVE() (s_ak_ble_active)
#else
#define AK_BLE_RUNTIME_ACTIVE() (true)
#endif

#define APP_TASK_STACK_SIZE       (5U * 1024U)
#define POLL_INTERVAL_MS          5U
#define HEARTBEAT_INTERVAL_MS     2000U
#define USB_LINK_TIMEOUT_MS       7000U
#define FRAME_BUFFER_SIZE         192U
#define SYSTEM_INFO_FRAME_BUFFER_SIZE 384U
#define AK_CHAT_SLOT_COUNT        3U
#define ASR_FRAME_BUFFER_SIZE     2048U
#define CORE_INIT_ATTEMPTS        3U
#define CORE_INIT_RETRY_MS        250U
#define FATAL_RESTART_DELAY_MS    2000U
#ifndef PROJECT_VERSION
#define PROJECT_VERSION "0.0.0-dev"
#endif
#define APP_FW_VERSION PROJECT_VERSION

static THREAD_HANDLE  s_app_thread = NULL;
static MUTEX_HANDLE   s_tx_mutex   = NULL;
static uint32_t       s_sequence   = 0;
static ak_rx_reader_t s_rx;
/* ASR can contain 255 bytes and JSON control escapes can expand each byte to
 * six characters. Keep the large frame in BSS instead of callback stacks. */
static char s_asr_tx_frame[ASR_FRAME_BUFFER_SIZE];
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
static volatile bool            s_wifi_connected    = false;
static volatile bool            s_wifi_status_dirty = false;
static volatile ak_wifi_state_t s_wifi_lcd_state    = AK_WIFI_DISCONNECTED;
#endif

/* ASR routing keeps a user preference separate from the effective K4 route.
 * Default preference is Tuya cloud ASR. If cloud readiness drops, K4 falls
 * back to PC ASR immediately; it returns to cloud when readiness recovers,
 * unless the user explicitly selected PC ASR. */
#define AK_VOICE_ROUTE_CHECK_MS 500U
static ak_voice_route_t s_voice_route;
static bool             s_voice_route_initialized;
static uint32_t         s_voice_route_last_check_ms;
/* True only for automatic cloud -> PC degradation, not a manual PC choice. */
static bool s_voice_fallback_active;
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
#define ASR_RECOGNIZE_TIMEOUT_MS 15000U

static ak_voice_ui_state_t s_voice_ui_state = AK_VOICE_UI_IDLE;
static uint32_t            s_voice_ui_state_since_ms;
/* ASR callbacks run on the AI event thread. They copy one complete result and
 * publish the volatile flag last; the app thread consumes it before drawing. */
static char                s_asr_pending_text[ASR_TRANSCRIPT_MAX];
static volatile bool       s_asr_result_pending;
static volatile bool       s_asr_result_is_error;
static bool                s_lcd_redraw_pending;
static uint32_t            s_lcd_audio_guard_until_ms;

static ak_settings_model_t s_settings;
static bool                s_settings_active;
static bool                s_settings_confirmed;
static bool                s_center_long_consumed;
#endif

/* K4 is the push-to-talk key on the expansion board (see hardware pinmap). */
#define AK_PTT_KEY_ID "k4"

static void transport_write_frame_unlocked(const char *frame, size_t capacity, int length)
{
    if (frame == NULL || capacity == 0U || length <= 0) {
        PR_WARN("transport frame rejected: invalid buffer/length");
        return;
    }
    if ((size_t)length >= capacity) {
        PR_WARN("transport frame too large: required=%d capacity=%u", length,
                (unsigned)capacity);
        return;
    }
    int written = ak_transport_write(frame, (uint32_t)length);
    if (written != length) {
        PR_WARN("transport short write: %d/%d", written, length);
    }
}

static bool transport_lock(void)
{
    if (s_tx_mutex == NULL || tal_mutex_lock(s_tx_mutex) != OPRT_OK) {
        PR_ERR("transport TX lock failed");
        return false;
    }
    return true;
}

static void transport_unlock(void)
{
    if (tal_mutex_unlock(s_tx_mutex) != OPRT_OK) {
        PR_ERR("transport TX unlock failed");
    }
}

static void transport_write_frame_sized(const char *frame, size_t capacity, int length)
{
    if (!transport_lock()) {
        return;
    }
    transport_write_frame_unlocked(frame, capacity, length);
    transport_unlock();
}

#define transport_write_frame(frame, length) \
    transport_write_frame_sized((frame), sizeof(frame), (length))

/* Sequence allocation and the matching write share one critical section, so
 * callback threads cannot race the app loop or transmit sequence N+1 first. */
static bool transport_begin_sequenced(uint32_t *sequence)
{
    if (sequence == NULL || !transport_lock()) {
        return false;
    }
    *sequence = ++s_sequence;
    return true;
}

static void transport_end_sequenced_sized(const char *frame, size_t capacity, int length)
{
    transport_write_frame_unlocked(frame, capacity, length);
    transport_unlock();
}

#define transport_end_sequenced(frame, length) \
    transport_end_sequenced_sized((frame), sizeof(frame), (length))

/* UART0 is the diagnostics/trusted-Companion channel. In DUAL mode this frame
 * intentionally stays on UART0 instead of being mirrored to BLE. */
static void send_system_info_uart(void)
{
#if AK_TRANSPORT_UART || AK_TRANSPORT_DUAL
    const char *ble_name  = "";
    const char *ble_state = "disabled";
#if AK_TRANSPORT_DUAL
    ak_ble_link_state_t link = ak_ble_gatt_link_state();
    ble_name                 = ak_ble_gatt_device_name();
    ble_state                = ak_ble_gatt_link_state_str(link);
#endif

    char frame[SYSTEM_INFO_FRAME_BUFFER_SIZE];
    if (!transport_lock()) {
        return;
    }
    uint32_t sequence = ++s_sequence;
    int length = ak_proto_system_info(
        frame, sizeof(frame), sequence, APP_FW_VERSION, AK_TRANSPORT_NAME,
        ak_wifi_current_ssid(), ak_wifi_state_str(ak_wifi_current_state()),
        ble_name, ble_state,
        ak_asr_available() ? "ready" : (ak_cloud_compiled() ? "unavailable" : "disabled"),
        ak_license_present() ? "present" : "missing");
    if (length > 0 && length < (int)sizeof(frame)) {
        int written = ak_uart_write(frame, (uint32_t)length);
        if (written != length) {
            PR_WARN("UART0 system info short write: %d/%d", written, length);
        }
    } else {
        PR_WARN("UART0 system info frame too large: %d", length);
    }
    transport_unlock();
#endif
}

/* Explicit development diagnostic. Keep the reply on the physically trusted
 * UART0 path even in DUAL builds; never mirror License metadata to BLE. */
static void send_license_info_uart(void)
{
#if AK_TRANSPORT_UART || AK_TRANSPORT_DUAL
    ak_license_debug_info_t info;
    (void)ak_license_debug_read(&info);
    PR_NOTICE("license read: source=%s uuid=%s authkey=****%s", info.source, info.uuid,
              info.authkey_tail);

    char frame[FRAME_BUFFER_SIZE];
    if (!transport_lock()) {
        return;
    }
    uint32_t sequence = ++s_sequence;
    int length = ak_proto_license_info(frame, sizeof(frame), sequence, info.source, info.uuid,
                                       info.authkey_tail);
    if (length > 0 && length < (int)sizeof(frame)) {
        int written = ak_uart_write(frame, (uint32_t)length);
        if (written != length) {
            PR_WARN("UART0 license info short write: %d/%d", written, length);
        }
    } else {
        PR_WARN("UART0 license info frame too large: %d", length);
    }
    transport_unlock();
#endif
}

typedef OPERATE_RET (*ak_core_init_fn_t)(void);

static OPERATE_RET core_init_with_retry(const char *name, ak_core_init_fn_t init_fn)
{
    OPERATE_RET rt = OPRT_INVALID_PARM;
    for (uint32_t attempt = 1U; attempt <= CORE_INIT_ATTEMPTS; attempt++) {
        rt = init_fn();
        if (rt == OPRT_OK) {
            return OPRT_OK;
        }
        PR_ERR("%s init failed: %d (attempt %u/%u)", name, rt,
               (unsigned)attempt, (unsigned)CORE_INIT_ATTEMPTS);
        if (attempt < CORE_INIT_ATTEMPTS) {
            tal_system_sleep(CORE_INIT_RETRY_MS);
        }
    }
    return rt;
}

static OPERATE_RET tx_mutex_init(void)
{
    return tal_mutex_create_init(&s_tx_mutex);
}

static void restart_after_fatal_init(const char *component, OPERATE_RET rt)
{
    PR_ERR("fatal %s initialization failure: %d; restarting in %ums", component, rt,
           (unsigned)FATAL_RESTART_DELAY_MS);
    tal_system_sleep(FATAL_RESTART_DELAY_MS);
    tal_system_reset();
    /* A platform reset should not return. Do not let the app task silently die
     * if a port returns unexpectedly. */
    while (1) {
        tal_system_sleep(1000U);
    }
}

/* Forward declarations: PTT / voice helpers are defined below but referenced by
 * on_input_event and send_hello_ack above them. */
static void on_ptt_event(ak_input_event_t event);
static void send_voice_source(void);
#if AK_ENABLE_DISPLAY
static void send_device_settings(void);
static void ak_voice_ui_set_state(ak_voice_ui_state_t state);
static void ak_voice_ui_queue_result(const char *text, bool is_error);
static void ak_voice_ui_tick(uint32_t now_ms);
static void ak_select_session_key(const char *id);
static void ak_render_status(void);
static void ak_settings_draw(void);
static bool ak_settings_consume_input(ak_input_kind_t kind, const char *id,
                                      ak_input_event_t event, uint32_t held_ms);
#endif

static void ak_emit_input_frame(ak_input_kind_t kind, const char *id,
                                ak_input_event_t event, uint32_t held_ms)
{
    char     frame[FRAME_BUFFER_SIZE];
    int      length;
    uint32_t sequence;

    if (!transport_begin_sequenced(&sequence)) {
        return;
    }
    if (kind == AK_INPUT_KIND_KEY) {
        length = ak_proto_key(frame, sizeof(frame), sequence, id,
                              ak_input_event_str(event), held_ms);
    } else {
        length = ak_proto_nav(frame, sizeof(frame), sequence, id,
                              ak_input_event_str(event), held_ms);
    }
    transport_end_sequenced(frame, length);
    PR_DEBUG("input %s %s held=%ums", id, ak_input_event_str(event), held_ms);
}

/* Input event -> local settings UI or JSONL frame on the active transport. */
static void on_input_event(ak_input_kind_t kind, const char *id,
                           ak_input_event_t event, uint32_t held_ms)
{
#if AK_ENABLE_DISPLAY
    if (ak_settings_consume_input(kind, id, event, held_ms)) {
        return;
    }
    if (kind == AK_INPUT_KIND_KEY && event == AK_INPUT_EVENT_DOWN) {
        ak_select_session_key(id);
    }
#endif

    ak_emit_input_frame(kind, id, event, held_ms);

    /* K4 = push-to-talk. Cloud consumes DOWN/LONG/UP as one latched HOLD
     * interaction; system still uses immediate DOWN/UP Windows dictation. */
    if (kind == AK_INPUT_KIND_KEY && strcmp(id, AK_PTT_KEY_ID) == 0 &&
        (event == AK_INPUT_EVENT_DOWN || event == AK_INPUT_EVENT_LONG ||
         event == AK_INPUT_EVENT_UP)) {
        /* Double-click on K4 (cloud route) sounds the "long-press to talk"
         * prompt, mirroring FP-3. Feeding runs on every edge; a hold fires LONG
         * which the detector excludes, so hold-to-talk never trips it. */
        if (ak_dclick_feed(&s_ptt_dclick, event, tal_system_get_millisecond(), held_ms) &&
            s_voice_route.effective == AK_VOICE_CLOUD && ak_asr_available()) {
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
    /* Follow the greeting with board-owned runtime state so Companion controls
     * reflect the AK immediately after connecting or reloading the link. */
    send_voice_source();
#if AK_ENABLE_DISPLAY
    send_device_settings();
#endif
    send_system_info_uart();
}

/* Emit a voice.start / voice.stop edge (PC companion runs its own ASR). */
static void send_voice(const char *state)
{
    char     frame[FRAME_BUFFER_SIZE];
    uint32_t sequence;
    if (!transport_begin_sequenced(&sequence)) {
        return;
    }
    transport_end_sequenced(frame, ak_proto_voice(frame, sizeof(frame), sequence, state));
}

/* Report the board's current voice routing so the companion UI reflects it. */
static void send_voice_source(void)
{
    char     frame[FRAME_BUFFER_SIZE];
    uint32_t sequence;
    if (!transport_begin_sequenced(&sequence)) {
        return;
    }
    transport_end_sequenced(
        frame, ak_proto_voice_source(frame, sizeof(frame), sequence, ak_voice_source_str(s_voice_route.effective)));
}

#if AK_ENABLE_DISPLAY
static void send_device_settings(void)
{
    char     frame[FRAME_BUFFER_SIZE];
    uint32_t sequence;
    if (!transport_begin_sequenced(&sequence)) {
        return;
    }
    transport_end_sequenced(
        frame, ak_proto_device_settings(frame, sizeof(frame), sequence,
                                        s_settings.volume, s_settings.brightness));
}
#endif

/* Cloud-ASR result callback: forward recognized text to the PC as an `asr`
 * frame. Registered with ak_asr_init; a no-op path when the stub is compiled. */
static void on_asr_text(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }
#if AK_ENABLE_DISPLAY
    ak_voice_ui_queue_result(text, false);
#endif
    uint32_t sequence;
    if (!transport_begin_sequenced(&sequence)) {
        return;
    }
    transport_end_sequenced(
        s_asr_tx_frame,
        ak_proto_asr(s_asr_tx_frame, sizeof(s_asr_tx_frame), sequence, text));
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
    uint32_t sequence;
    if (!transport_begin_sequenced(&sequence)) {
        return;
    }
    transport_end_sequenced(frame, ak_proto_asr_error(frame, sizeof(frame), sequence, code));
}

/* Wi-Fi provisioning status callback: report state to the PC as a wifi_status
 * frame. Registered with ak_wifi_init; the stub reports "failed" immediately. */
static void on_wifi_status(ak_wifi_state_t state, const char *ssid)
{
    char frame[FRAME_BUFFER_SIZE];
#if AK_ENABLE_DISPLAY
    s_wifi_connected = (state == AK_WIFI_CONNECTED);
    s_wifi_lcd_state = state;
    /* Publish dirty last so the app loop reads a complete state update. */
    s_wifi_status_dirty = true;
#endif
    uint32_t sequence;
    if (!transport_begin_sequenced(&sequence)) {
        return;
    }
    transport_end_sequenced(frame,
                            ak_proto_wifi_status(frame, sizeof(frame), sequence,
                                                 ak_wifi_state_str(state), ssid));
    send_system_info_uart();
}

/* K4 push-to-talk routing. The route is decided on DOWN and held until UP:
 * - system: emit voice.start immediately, then voice.stop on UP;
 * - cloud: wait for LONG, inject FP-3 HOLD long-press, then release on UP.
 * A short cloud press intentionally starts no ASR session, matching FP-3. */
static void on_ptt_event(ak_input_event_t event)
{
    if (event == AK_INPUT_EVENT_DOWN) {
        s_ptt_cloud_route   = (s_voice_route.effective == AK_VOICE_CLOUD && ak_asr_available());
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
    char     frame[FRAME_BUFFER_SIZE];
    uint32_t sequence;

    if (!transport_begin_sequenced(&sequence)) {
        return;
    }
    transport_end_sequenced(frame, ak_proto_ping(frame, sizeof(frame), sequence, uptime_ms));
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

#define LCD_SIDEBAR_W             64U
#define LCD_SESSION_ROWS          AK_CHAT_SLOT_COUNT
#define LCD_SESSION_ROW_H         67U
#define LCD_SESSION_ALIAS_CHARS   7U
#define LCD_SESSION_ALIAS_BYTES   (LCD_SESSION_ALIAS_CHARS + 1U)
#define LCD_CHAT_X                (LCD_SIDEBAR_W + 6U)
#define LCD_CHAT_TEXT_COLS      29U
#define LCD_HISTORY_Y             62U
#define LCD_HISTORY_LINE_ADVANCE  10U
#define LCD_HISTORY_PARAGRAPH_GAP 6U
#define LCD_HISTORY_CAPACITY      10U
#define LCD_HISTORY_TEXT_BYTES    128U
#define LCD_HISTORY_VISIBLE_LINES 16U
#define LCD_STATUS_BAR_H        16U
#define LCD_STATUS_BAR_Y        (AK_STATUSLCD_H - LCD_STATUS_BAR_H)
#define LCD_STATUS_TEXT_BYTES   40U
#define LCD_SESSION_SELECTED_BG AK_RGB565(0x8EU, 0x48U, 0xFFU)
#define LCD_AGENT_RESPONSE_BG   AK_RGB565(0x6FU, 0x36U, 0xCCU)

typedef struct {
    char    agent[32];
    char    state[16];
    char    history[LCD_HISTORY_CAPACITY][LCD_HISTORY_TEXT_BYTES];
    bool    history_from_agent[LCD_HISTORY_CAPACITY];
    uint8_t history_count;
} ak_session_ui_t;

static ak_session_ui_t s_sessions[LCD_SESSION_ROWS] = {
    {.agent = "-", .state = "idle"},
    {.agent = "-", .state = "idle"},
    {.agent = "-", .state = "idle"},
};
static char     s_session_labels[LCD_SESSION_ROWS][LCD_SESSION_ALIAS_BYTES] = {"A", "B", "C"};
static uint8_t  s_selected_session;
static char     s_status_bar_text[LCD_STATUS_TEXT_BYTES] = "Starting system...";
static uint16_t s_status_bar_color = AK_COL_CYAN;

static void ak_status_bar_set(const char *text, uint16_t color)
{
    (void)snprintf(s_status_bar_text, sizeof(s_status_bar_text), "%s",
                   text == NULL ? "" : text);
    s_status_bar_color = color;
}

static void ak_status_bar_set_wifi(ak_wifi_state_t state)
{
    switch (state) {
    case AK_WIFI_CONNECTING:
        ak_status_bar_set("WiFi connecting...", AK_COL_CYAN);
        break;
    case AK_WIFI_CONNECTED:
        ak_status_bar_set("WiFi connected", AK_COL_GREEN);
        break;
    case AK_WIFI_FAILED:
        ak_status_bar_set("Warning: WiFi failed", AK_COL_RED);
        break;
    case AK_WIFI_DISCONNECTED:
    default:
        ak_status_bar_set("Warning: WiFi disconnected", AK_COL_ORANGE);
        break;
    }
}

static void ak_status_bar_draw(void)
{
    const char *text  = s_voice_fallback_active ? "Tuya ASR unavailable -> PC ASR" : s_status_bar_text;
    uint16_t    color = s_voice_fallback_active ? AK_COL_ORANGE : s_status_bar_color;

    ak_gfx_fill_rect(0U, LCD_STATUS_BAR_Y, AK_STATUSLCD_W,
                     LCD_STATUS_BAR_H, AK_COL_BLACK);
    ak_gfx_fill_rect(0U, LCD_STATUS_BAR_Y, AK_STATUSLCD_W,
                     1U, LCD_SESSION_SELECTED_BG);
    ak_gfx_text_thin(4U, (uint16_t)(LCD_STATUS_BAR_Y + 4U), text, color, AK_COL_BLACK, 1);
}

/* Consume one UTF-8 code point. The 8x8 font is ASCII-only, so each unsupported
 * non-ASCII code point becomes one missing-glyph square. */
static char ak_history_next_glyph(const char **cursor)
{
    const unsigned char *p = (const unsigned char *)*cursor;
    unsigned char c = *p++;

    if (c >= 0x80U) {
        while ((*p & 0xC0U) == 0x80U) {
            p++;
        }
        *cursor = (const char *)p;
        return (char)AK_FONT_MISSING_GLYPH;
    }
    *cursor = (const char *)p;
    return (c >= 0x20U && c < 0x7FU) ? (char)c : ' ';
}

/* Keep the ten most recent text updates for each Chat, preserving whether
 * each entry came from local ASR or Companion Agent output. */
static void ak_chat_history_add(ak_session_ui_t *session, const char *text, bool from_agent)
{
    if (session == NULL || text == NULL || text[0] == '\0') {
        return;
    }
    if (session->history_count == LCD_HISTORY_CAPACITY) {
        memmove(session->history[0], session->history[1],
                (LCD_HISTORY_CAPACITY - 1U) * LCD_HISTORY_TEXT_BYTES);
        memmove(&session->history_from_agent[0], &session->history_from_agent[1],
                (LCD_HISTORY_CAPACITY - 1U) * sizeof(session->history_from_agent[0]));
        session->history_count--;
    }
    (void)snprintf(session->history[session->history_count], LCD_HISTORY_TEXT_BYTES,
                   "%s", text);
    session->history_from_agent[session->history_count] = from_agent;
    session->history_count++;
}

typedef struct {
    char text[LCD_CHAT_TEXT_COLS + 1U];
    bool paragraph_start;
    bool from_agent;
} ak_history_line_t;

static void ak_history_line_push(ak_history_line_t *lines, uint8_t *count,
                                 const char *text, uint16_t length,
                                 bool paragraph_start, bool from_agent)
{
    if (*count == LCD_HISTORY_VISIBLE_LINES) {
        memmove(&lines[0], &lines[1],
                (LCD_HISTORY_VISIBLE_LINES - 1U) * sizeof(lines[0]));
        (*count)--;
    }

    ak_history_line_t *line = &lines[*count];
    memcpy(line->text, text, length);
    line->text[length]    = '\0';
    line->paragraph_start = paragraph_start;
    line->from_agent      = from_agent;
    (*count)++;
}

/* Wrap one history entry at word boundaries. An overlong word is split at the
 * right edge; each unsupported UTF-8 code point remains one square glyph. */
static void ak_history_wrap_entry(const char *text, bool from_agent, ak_history_line_t *lines, uint8_t *line_count)
{
    char glyphs[LCD_HISTORY_TEXT_BYTES];
    const char *p = text;
    uint16_t glyph_count = 0U;

    while (*p != '\0' && glyph_count + 1U < sizeof(glyphs)) {
        glyphs[glyph_count++] = ak_history_next_glyph(&p);
    }
    glyphs[glyph_count] = '\0';

    uint16_t start = 0U;
    bool first_line = true;
    while (start < glyph_count) {
        while (start < glyph_count && glyphs[start] == ' ') {
            start++;
        }
        if (start == glyph_count) {
            break;
        }

        uint16_t remaining = (uint16_t)(glyph_count - start);
        uint16_t length = remaining > LCD_CHAT_TEXT_COLS
                              ? LCD_CHAT_TEXT_COLS
                              : remaining;
        if (length < remaining) {
            uint16_t split = length;
            while (split > 0U && glyphs[start + split] != ' ') {
                split--;
            }
            if (split > 0U) {
                length = split;
            }
        }
        while (length > 0U && glyphs[start + length - 1U] == ' ') {
            length--;
        }
        if (length == 0U) {
            start++;
            continue;
        }

        ak_history_line_push(lines, line_count, &glyphs[start], length, first_line, from_agent);
        first_line = false;
        start = (uint16_t)(start + length);
    }
}

static void ak_chat_history_draw(const ak_session_ui_t *session)
{
    ak_history_line_t lines[LCD_HISTORY_VISIBLE_LINES];
    uint8_t line_count = 0U;

    for (uint8_t i = 0U; i < session->history_count; i++) {
        ak_history_wrap_entry(session->history[i], session->history_from_agent[i], lines, &line_count);
    }
    if (line_count == 0U) {
        return;
    }

    /* Select the newest suffix that fits above the status bar. Wrapped lines
     * advance 10 px; a new history entry adds another 6 px paragraph gap. */
    uint8_t first = (uint8_t)(line_count - 1U);
    uint16_t used_height = 8U;
    while (first > 0U) {
        uint16_t advance = LCD_HISTORY_LINE_ADVANCE;
        if (lines[first].paragraph_start) {
            advance = (uint16_t)(advance + LCD_HISTORY_PARAGRAPH_GAP);
        }
        if ((uint16_t)(used_height + advance) >
            (uint16_t)(LCD_STATUS_BAR_Y - LCD_HISTORY_Y)) {
            break;
        }
        used_height = (uint16_t)(used_height + advance);
        first--;
    }

    uint16_t y = LCD_HISTORY_Y;
    for (uint8_t i = first; i < line_count; i++) {
        if (i > first) {
            y = (uint16_t)(y + LCD_HISTORY_LINE_ADVANCE);
            if (lines[i].paragraph_start) {
                y = (uint16_t)(y + LCD_HISTORY_PARAGRAPH_GAP);
            }
        }
        uint16_t bg = lines[i].from_agent ? LCD_AGENT_RESPONSE_BG : AK_COL_BLACK;
        ak_gfx_text_thin(LCD_CHAT_X, y, lines[i].text, AK_COL_WHITE, bg, 1);
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

#define LCD_SETTINGS_ROW_X 12U
#define LCD_SETTINGS_ROW_W 296U
#define LCD_SETTINGS_BAR_W 272U

static void ak_settings_outline(uint16_t x, uint16_t y, uint16_t w,
                                uint16_t h, uint16_t color)
{
    ak_gfx_fill_rect(x, y, w, 1U, color);
    ak_gfx_fill_rect(x, (uint16_t)(y + h - 1U), w, 1U, color);
    ak_gfx_fill_rect(x, y, 1U, h, color);
    ak_gfx_fill_rect((uint16_t)(x + w - 1U), y, 1U, h, color);
}

static void ak_settings_value_row(uint16_t y, ak_settings_item_t item,
                                  const char *label, uint8_t value)
{
    bool selected = s_settings.selected == item;
    uint16_t bg = selected ? LCD_SESSION_SELECTED_BG : AK_COL_BLACK;
    uint16_t fg = selected ? AK_COL_WHITE : AK_COL_CYAN;
    char value_text[12];

    ak_gfx_fill_rect(LCD_SETTINGS_ROW_X, y, LCD_SETTINGS_ROW_W, 48U, bg);
    if (!selected) {
        ak_settings_outline(LCD_SETTINGS_ROW_X, y, LCD_SETTINGS_ROW_W, 48U,
                            AK_COL_GRAY);
    }
    ak_gfx_text_thin(24U, (uint16_t)(y + 8U), label,
                     selected ? AK_COL_WHITE : AK_COL_GRAY, bg, 1);
    (void)snprintf(value_text, sizeof(value_text), "< %3u%% >", value);
    ak_gfx_text_thin(232U, (uint16_t)(y + 8U), value_text, fg, bg, 1);
    if (selected && s_settings_confirmed) {
        ak_gfx_text_thin(160U, (uint16_t)(y + 8U), "Applied",
                         AK_COL_WHITE, bg, 1);
    }

    ak_gfx_fill_rect(24U, (uint16_t)(y + 31U), LCD_SETTINGS_BAR_W, 6U,
                     selected ? AK_COL_BLACK : AK_COL_GRAY);
    ak_gfx_fill_rect(24U, (uint16_t)(y + 31U),
                     (uint16_t)((uint32_t)LCD_SETTINGS_BAR_W * value / 100U),
                     6U, selected ? AK_COL_WHITE : LCD_SESSION_SELECTED_BG);
}

static void ak_settings_draw(void)
{
    if (!ak_statuslcd_ready()) {
        return;
    }
    uint32_t now_ms = tal_system_get_millisecond();
    if ((int32_t)(s_lcd_audio_guard_until_ms - now_ms) > 0 ||
        ak_asr_audio_busy()) {
        s_lcd_redraw_pending = true;
        return;
    }

    ak_gfx_clear(AK_COL_BLACK);
    ak_gfx_fill_rect(0U, 0U, AK_STATUSLCD_W, 22U, LCD_SESSION_SELECTED_BG);
    ak_gfx_text(6U, 7U, "Settings", AK_COL_BLACK,
                LCD_SESSION_SELECTED_BG, 1);
    ak_gfx_text_thin(16U, 27U, "Device Settings", AK_COL_GRAY,
                     AK_COL_BLACK, 1);

    ak_settings_value_row(42U, AK_SETTINGS_ITEM_VOLUME, "Volume",
                          s_settings.volume);
    ak_settings_value_row(98U, AK_SETTINGS_ITEM_BRIGHTNESS, "Brightness",
                          s_settings.brightness);

    bool exit_selected = s_settings.selected == AK_SETTINGS_ITEM_EXIT;
    uint16_t exit_bg = exit_selected ? LCD_SESSION_SELECTED_BG : AK_COL_BLACK;
    ak_gfx_fill_rect(LCD_SETTINGS_ROW_X, 158U, LCD_SETTINGS_ROW_W, 38U,
                     exit_bg);
    if (!exit_selected) {
        ak_settings_outline(LCD_SETTINGS_ROW_X, 158U, LCD_SETTINGS_ROW_W, 38U,
                            AK_COL_GRAY);
    }
    ak_gfx_text_thin(136U, 173U, "Exit",
                     exit_selected ? AK_COL_WHITE : AK_COL_GRAY, exit_bg, 1);

    ak_gfx_fill_rect(0U, 207U, AK_STATUSLCD_W, 33U, AK_COL_BLACK);
    ak_gfx_fill_rect(0U, 207U, AK_STATUSLCD_W, 1U,
                     LCD_SESSION_SELECTED_BG);
    ak_gfx_text_thin(4U, 213U, "Up/Down Select  Left/Right Adjust",
                     AK_COL_CYAN, AK_COL_BLACK, 1);
    ak_gfx_text_thin(4U, 227U, "Center Confirm", AK_COL_GRAY,
                     AK_COL_BLACK, 1);
    ak_statuslcd_flush();
    s_lcd_redraw_pending = false;
}

static OPERATE_RET ak_settings_apply_selected(void)
{
    if (s_settings.selected == AK_SETTINGS_ITEM_VOLUME) {
        return ak_asr_set_volume(s_settings.volume);
    }
    if (s_settings.selected == AK_SETTINGS_ITEM_BRIGHTNESS) {
        return ak_statuslcd_set_brightness(s_settings.brightness);
    }
    return OPRT_OK;
}

/* The center DOWN edge is delayed: a short release is replayed to the companion,
 * while a LONG opens settings without leaking a false center click to the PC. */
static bool ak_settings_consume_input(ak_input_kind_t kind, const char *id,
                                      ak_input_event_t event, uint32_t held_ms)
{
    if (!s_settings_active) {
        if (kind != AK_INPUT_KIND_NAV || strcmp(id, "center") != 0) {
            return false;
        }
        if (event == AK_INPUT_EVENT_DOWN) {
            s_center_long_consumed = false;
            return true;
        }
        if (event == AK_INPUT_EVENT_LONG) {
            s_center_long_consumed = true;
            s_settings_active = true;
            s_settings_confirmed = false;
            ak_settings_draw();
            return true;
        }
        if (event == AK_INPUT_EVENT_UP) {
            if (s_center_long_consumed) {
                s_center_long_consumed = false;
                return true;
            }
            ak_emit_input_frame(AK_INPUT_KIND_NAV, "center",
                                AK_INPUT_EVENT_DOWN, 0U);
            return false;
        }
        return true;
    }

    /* Settings is modal: prevent K1-K5/PTT and nav events from reaching the
     * companion or changing the hidden work screen. */
    if (kind != AK_INPUT_KIND_NAV) {
        return true;
    }
    if (strcmp(id, "center") == 0) {
        if (event == AK_INPUT_EVENT_DOWN) {
            s_center_long_consumed = false;
        } else if (event == AK_INPUT_EVENT_LONG) {
            s_center_long_consumed = true;
        } else if (event == AK_INPUT_EVENT_UP) {
            if (s_center_long_consumed) {
                s_center_long_consumed = false;
                return true;
            }
            ak_settings_result_t result =
                ak_settings_handle(&s_settings, AK_SETTINGS_ACTION_CONFIRM);
            if (result == AK_SETTINGS_RESULT_EXIT) {
                s_settings_active = false;
                s_settings_confirmed = false;
                ak_render_status();
                return true;
            }
            s_settings_confirmed = ak_settings_apply_selected() == OPRT_OK;
            ak_settings_draw();
        }
        return true;
    }
    if (event != AK_INPUT_EVENT_DOWN) {
        return true;
    }

    ak_settings_action_t action;
    if (strcmp(id, "up") == 0) {
        action = AK_SETTINGS_ACTION_UP;
    } else if (strcmp(id, "down") == 0) {
        action = AK_SETTINGS_ACTION_DOWN;
    } else if (strcmp(id, "left") == 0) {
        action = AK_SETTINGS_ACTION_LEFT;
    } else if (strcmp(id, "right") == 0) {
        action = AK_SETTINGS_ACTION_RIGHT;
    } else {
        return true;
    }

    ak_settings_result_t result = ak_settings_handle(&s_settings, action);
    s_settings_confirmed = false;
    if (result == AK_SETTINGS_RESULT_CHANGED) {
        OPERATE_RET rt = ak_settings_apply_selected();
        if (rt != OPRT_OK) {
            PR_WARN("settings apply failed: item=%d rt=%d",
                    (int)s_settings.selected, rt);
        } else {
            send_device_settings();
        }
    }
    ak_settings_draw();
    (void)held_ms;
    return true;
}

static void ak_session_label_set(uint8_t slot, const char *alias)
{
    if (slot >= LCD_SESSION_ROWS) {
        return;
    }
    if (alias == NULL || alias[0] == '\0') {
        s_session_labels[slot][0] = (char)('A' + slot);
        s_session_labels[slot][1] = '\0';
        return;
    }

    const char *cursor = alias;
    uint8_t length = 0U;
    while (*cursor != '\0' && length < LCD_SESSION_ALIAS_CHARS) {
        s_session_labels[slot][length++] = ak_history_next_glyph(&cursor);
    }
    s_session_labels[slot][length] = '\0';
}

static void ak_draw_session_sidebar(void)
{
    for (uint16_t i = 0; i < LCD_SESSION_ROWS; i++) {
        bool selected = (i == s_selected_session);
        uint16_t y = (uint16_t)(22U + i * LCD_SESSION_ROW_H);
        uint16_t h = (i + 1U == LCD_SESSION_ROWS)
                         ? (uint16_t)(LCD_STATUS_BAR_Y - y)
                         : LCD_SESSION_ROW_H;
        uint16_t bg = selected ? LCD_SESSION_SELECTED_BG : AK_COL_BLACK;
        uint16_t fg = selected ? AK_COL_WHITE : AK_COL_GRAY;
        uint16_t label_width = (uint16_t)(strlen(s_session_labels[i]) * 8U);
        uint16_t label_x = label_width < LCD_SIDEBAR_W
                               ? (uint16_t)((LCD_SIDEBAR_W - label_width) / 2U)
                               : 0U;

        ak_gfx_fill_rect(0, y, LCD_SIDEBAR_W - 1U, h, bg);
        ak_gfx_icon(16U, (uint16_t)(y + 8U), AK_SESSION_ICON_KIRO_GHOST,
                    AK_SESSION_ICON_W, AK_SESSION_ICON_H, fg, bg);
        ak_gfx_text_thin(label_x, (uint16_t)(y + 50U),
                         s_session_labels[i], fg, bg, 1);
        if (i > 0U) {
            ak_gfx_fill_rect(0, y, LCD_SIDEBAR_W, 1U, AK_COL_GRAY);
        }
    }
    ak_gfx_fill_rect(LCD_SIDEBAR_W - 1U, 22U, 1U,
                     AK_STATUSLCD_H - 22U, AK_COL_GRAY);
}

/* Render the selected A/B/C session and its independent ten-entry history. */
static void ak_render_status(void)
{
    if (!ak_statuslcd_ready()) {
        return;
    }
    uint32_t now_ms = tal_system_get_millisecond();
    if ((int32_t)(s_lcd_audio_guard_until_ms - now_ms) > 0 ||
        ak_asr_audio_busy()) {
        s_lcd_redraw_pending = true;
        return;
    }
    if (s_settings_active) {
        ak_settings_draw();
        return;
    }
    const ak_session_ui_t *session = &s_sessions[s_selected_session];
    char     link_text[32];
    uint16_t link_color = ak_link_header(link_text, sizeof(link_text));
    char     line[48];

    bool usb_connected = false;
    bool ble_connected = false;
#if AK_TRANSPORT_UART || AK_TRANSPORT_DUAL
    usb_connected = s_pc_connected;
#endif
#if AK_TRANSPORT_BLE_GATT || AK_TRANSPORT_DUAL
    ble_connected = (ak_ble_gatt_link_state() != AK_BLE_LINK_ADVERTISING);
#endif

    ak_statuslcd_title_links("Ki-board for Kiro Crew", link_color, s_wifi_connected,
                             usb_connected, ble_connected);
    /* 22 title glyphs end at x=182; the thin version fits before WiFi at x=256. */
    ak_gfx_text_thin(186U, 7U, "v" APP_FW_VERSION,
                     AK_COL_BLACK, link_color, 1);
    ak_draw_session_sidebar();

    (void)snprintf(line, sizeof(line), "Session %s: %s",
                   s_session_labels[s_selected_session],
                   session->agent[0] ? session->agent : "-");
    ak_gfx_text_thin(LCD_CHAT_X, 29U, line, AK_COL_WHITE, AK_COL_BLACK, 1);
    (void)snprintf(line, sizeof(line), "Status: %s",
                   session->state[0] ? session->state : "-");
    ak_gfx_text_thin(LCD_CHAT_X, 43U, line,
                     ak_status_color(session->state), AK_COL_BLACK, 1);
    ak_chat_history_draw(session);
    ak_status_bar_draw();
    ak_statuslcd_flush();
    s_lcd_redraw_pending = false;
}

static void ak_refresh_link_header(void)
{
    ak_render_status();
}

static void ak_select_session_key(const char *id)
{
    uint8_t selected;

    if (strcmp(id, "k1") == 0) {
        selected = 0U;
    } else if (strcmp(id, "k2") == 0) {
        selected = 1U;
    } else if (strcmp(id, "k3") == 0) {
        selected = 2U;
    } else {
        return;
    }

    s_selected_session = selected;
    char feedback[32];
    (void)snprintf(feedback, sizeof(feedback), "Selected Session %s",
                   s_session_labels[selected]);
    ak_status_bar_set(feedback, AK_COL_CYAN);
    ak_render_status();
}

static void ak_voice_ui_set_state(ak_voice_ui_state_t state)
{
    s_voice_ui_state = state;
    s_voice_ui_state_since_ms = tal_system_get_millisecond();
    switch (state) {
    case AK_VOICE_UI_LISTENING:
        ak_status_bar_set("Listening...", AK_COL_RED);
        break;
    case AK_VOICE_UI_RECOGNIZING:
        ak_status_bar_set("Recognizing...", AK_COL_YELLOW);
        break;
    case AK_VOICE_UI_IDLE:
    default:
        ak_status_bar_set("Ready", AK_COL_GREEN);
        break;
    }
    ak_render_status();
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
        if (!is_error) {
            ak_chat_history_add(&s_sessions[s_selected_session], s_asr_pending_text, false);
            ak_status_bar_set("ASR input received", AK_COL_GREEN);
        } else if (strcmp(s_asr_pending_text, "asr_empty") == 0) {
            ak_status_bar_set("Warning: no speech", AK_COL_ORANGE);
        } else {
            ak_status_bar_set("Warning: ASR failed", AK_COL_RED);
        }
        redraw = true;
        s_asr_result_pending = false;
        s_voice_ui_state = AK_VOICE_UI_IDLE;
        s_voice_ui_state_since_ms = now_ms;
    }

    if (s_voice_ui_state == AK_VOICE_UI_RECOGNIZING &&
        (uint32_t)(now_ms - s_voice_ui_state_since_ms) >= ASR_RECOGNIZE_TIMEOUT_MS) {
        s_voice_ui_state = AK_VOICE_UI_IDLE;
        s_voice_ui_state_since_ms = now_ms;
        ak_status_bar_set("Warning: ASR timeout", AK_COL_ORANGE);
        redraw = true;
    }

    if (redraw) {
        ak_refresh_link_header();
    }
}

#endif /* AK_ENABLE_DISPLAY */

/* Reconcile the preferred ASR engine with current Tuya cloud readiness.
 * Transitions are edge-triggered: source frames, LCD redraws, and local audio
 * prompts happen once per state change rather than once per polling interval. */
static void ak_voice_route_update(bool announce)
{
    static bool       cloud_report_initialized;
    static bool       last_cloud_available;
    bool              was_initialized = s_voice_route_initialized;
    bool              was_fallback    = s_voice_route.fallback;
    ak_voice_source_t previous        = s_voice_route.effective;
    bool              cloud_available = ak_asr_available();
    bool              cloud_status_changed =
        cloud_report_initialized && last_cloud_available != cloud_available;
    bool              changed = ak_voice_route_refresh(&s_voice_route, cloud_available);

    last_cloud_available     = cloud_available;
    cloud_report_initialized = true;

    s_voice_fallback_active = s_voice_route.fallback;
    if (!was_initialized || changed) {
        if (s_voice_route.fallback) {
            PR_WARN("Tuya ASR unavailable; falling back to PC ASR");
#if AK_ENABLE_DISPLAY
            ak_status_bar_set("Tuya ASR unavailable -> PC ASR", AK_COL_ORANGE);
#endif
        } else if (s_voice_route.effective == AK_VOICE_CLOUD) {
            PR_NOTICE("Tuya ASR %s; using board microphone", was_fallback ? "restored" : "ready");
#if AK_ENABLE_DISPLAY
            ak_status_bar_set(was_fallback ? "Tuya ASR restored" : "Tuya ASR ready", AK_COL_GREEN);
#endif
        } else {
            PR_NOTICE("PC ASR selected explicitly");
#if AK_ENABLE_DISPLAY
            ak_status_bar_set("PC ASR selected", AK_COL_CYAN);
#endif
        }

        /* Cloud/Wi-Fi is expected to be unavailable during initial startup.
         * Announce only a real ready -> unavailable transition, not the initial
         * fallback selected before association and AI-agent startup complete. */
        bool play_failure_prompt =
            announce && was_initialized && !was_fallback &&
            s_voice_route.fallback;
        bool play_recovery_prompt =
            announce && was_initialized && was_fallback &&
            s_voice_route.effective == AK_VOICE_CLOUD;

#if AK_ENABLE_DISPLAY
        /* Finish the status frame before starting asynchronous MP3 playback.
         * LCD redraws requested while audio is active are deferred below. */
        ak_refresh_link_header();
        if (play_failure_prompt || play_recovery_prompt) {
            s_lcd_audio_guard_until_ms =
                tal_system_get_millisecond() + 250U;
            /* Redraw once more after playback to recover from any panel-state
             * disturbance caused by the speaker/audio DMA startup. */
            s_lcd_redraw_pending = true;
        }
#endif
        if (play_failure_prompt) {
            ak_asr_play_availability_prompt(false);
        } else if (play_recovery_prompt) {
            ak_asr_play_availability_prompt(true);
        }
        if (was_initialized && previous != s_voice_route.effective) {
            send_voice_source();
        }
    }
    if (cloud_status_changed) {
        send_system_info_uart();
    }
    s_voice_route_initialized = true;
}

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
            ak_voice_source_t before = s_voice_route.effective;
            ak_voice_route_set_preference(&s_voice_route, want);
            PR_NOTICE("voice preference -> %s (from PC config)", ak_voice_source_str(want));
            ak_voice_route_update(true);
            /* Echo unchanged effective state too: a requested cloud route may
             * remain system while Tuya ASR is unavailable. Route transitions
             * are already reported by ak_voice_route_update(). */
            if (before == s_voice_route.effective) {
                send_voice_source();
            }
        }
        send_ack(seq);
    } else if (strcmp(type, "device_settings") == 0) {
#if AK_ENABLE_DISPLAY
        uint32_t value = 0U;
        bool received = false;
        bool changed = false;

        if (ak_json_uint(line, "volume", &value)) {
            uint8_t volume = (uint8_t)(value > 100U ? 100U : value);
            received = true;
            if (ak_asr_set_volume(volume) == OPRT_OK) {
                s_settings.volume = volume;
                changed = true;
            } else {
                PR_WARN("device settings: volume apply failed");
            }
        }
        if (ak_json_uint(line, "brightness", &value)) {
            uint8_t brightness = (uint8_t)(value > 100U ? 100U : value);
            if (brightness < AK_SETTINGS_BRIGHTNESS_MIN) {
                brightness = AK_SETTINGS_BRIGHTNESS_MIN;
            }
            received = true;
            if (ak_statuslcd_set_brightness(brightness) == OPRT_OK) {
                s_settings.brightness = brightness;
                changed = true;
            } else {
                PR_WARN("device settings: brightness apply failed");
            }
        }
        if (changed) {
            if (s_settings_active) {
                ak_settings_draw();
            } else {
                ak_render_status();
            }
        }
        if (received) {
            send_device_settings();
        }
#endif
    } else if (strcmp(type, "bindings") == 0) {
#if AK_ENABLE_DISPLAY
        static const char *fields[LCD_SESSION_ROWS] = {
            "alias_a", "alias_b", "alias_c"
        };
        bool changed = false;
        for (uint8_t i = 0U; i < LCD_SESSION_ROWS; i++) {
            char alias[LCD_SESSION_ALIAS_BYTES] = "";
            if (ak_json_str(line, fields[i], alias, sizeof(alias))) {
                ak_session_label_set(i, alias);
                changed = true;
            }
        }
        if (changed) {
            PR_NOTICE("binding aliases: A=%s B=%s C=%s",
                      s_session_labels[0], s_session_labels[1],
                      s_session_labels[2]);
            ak_render_status();
        }
#endif
    } else if (strcmp(type, "wifi") == 0) {
        /* Wi-Fi credentials are sensitive and may only enter through the
         * physically trusted USB/UART transport. */
        if (!ak_transport_rx_is_usb()) {
            PR_WARN("wifi config rejected: USB transport required");
            return;
        }
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
        /* UUID/AuthKey writes are sensitive and may only enter through the
         * physically trusted USB/UART transport. */
        if (!ak_transport_rx_is_usb()) {
            PR_WARN("license write rejected: USB transport required");
            return;
        }
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
            char     frame[FRAME_BUFFER_SIZE];
            uint32_t sequence;
            if (transport_begin_sequenced(&sequence)) {
                transport_end_sequenced(frame,
                                        ak_proto_license_status(frame, sizeof(frame), sequence,
                                                                ak_license_result_str(lr)));
            }
        }
        send_ack(seq);
        send_system_info_uart();
    } else if (strcmp(type, "license_read") == 0) {
        if (!ak_transport_rx_is_usb()) {
            PR_WARN("license read rejected: USB transport required");
            return;
        }
        uint32_t seq = 0;
        ak_json_uint(line, "seq", &seq);
        send_license_info_uart();
        send_ack(seq);
    } else if (strcmp(type, "display") == 0) {
        uint32_t chat_slot = 0U;
        char     agent[32] = "";
        char     state[16] = "";
        char     text[128] = "";
        if (!ak_json_uint(line, "chat_slot", &chat_slot) || chat_slot < 1U || chat_slot > AK_CHAT_SLOT_COUNT) {
            PR_WARN("display rejected: chat_slot must be 1..%u", (unsigned)AK_CHAT_SLOT_COUNT);
            return;
        }
        ak_json_str(line, "agent_name", agent, sizeof(agent));
        ak_json_str(line, "state", state, sizeof(state));
        ak_json_str(line, "text", text, sizeof(text));
        uint8_t session_index = (uint8_t)(chat_slot - 1U);
        PR_NOTICE("display: chat=%c agent=%s state=%s", (char)('A' + session_index), agent, state);
#if AK_ENABLE_DISPLAY
        ak_session_ui_t *session = &s_sessions[session_index];
        snprintf(session->agent, sizeof(session->agent), "%s", agent);
        snprintf(session->state, sizeof(session->state), "%s", state);
        ak_chat_history_add(session, text, true);
        if (session_index == s_selected_session) {
            ak_render_status();
        }
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
    PR_NOTICE("Transport: %s (BLE GATT peripheral, service 0x%04X)",
              AK_TRANSPORT_NAME, (unsigned)AK_BLE_GATT_SERVICE_UUID);
#endif
    PR_NOTICE("Inputs: 5 keys + 5-way nav, debounce=%ums long=%ums", (unsigned)AK_FSM_DEBOUNCE_MS,
              (unsigned)AK_FSM_LONG_PRESS_MS);

    rt = core_init_with_retry("transport", ak_transport_init);
    if (rt != OPRT_OK) {
        restart_after_fatal_init("transport", rt);
    }
    rt = core_init_with_retry("input", ak_input_init);
    if (rt != OPRT_OK) {
        restart_after_fatal_init("input", rt);
    }
    rt = core_init_with_retry("transport TX mutex", tx_mutex_init);
    if (rt != OPRT_OK) {
        restart_after_fatal_init("transport TX mutex", rt);
    }
    ak_dclick_init(&s_ptt_dclick);
    ak_voice_route_init(&s_voice_route);
#if AK_ENABLE_DISPLAY
    ak_settings_init(&s_settings);
    s_settings.volume = ak_asr_get_volume();
#endif

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
        ak_status_bar_set("LCD ready", AK_COL_GREEN);
        ak_render_status();
    }
#endif

#if AK_ENABLE_DISPLAY
    ak_status_bar_set("Starting cloud...", AK_COL_CYAN);
    ak_render_status();
#endif

    /* Tuya IoT / cloud bring-up (stub unless AK_ASR_BOARD_CLOUD is set). MUST run
     * BEFORE ak_asr_init: ai_chat_init needs an initialized IoT / AI-service
     * context (tuya_iot_init + netmgr), or it dereferences a NULL handle and
     * hard-faults at boot (the black-screen crash). No-op on the v1/BLE profiles. */
    OPERATE_RET cloud_rt = ak_cloud_init();
#if AK_ENABLE_DISPLAY
    ak_status_bar_set(cloud_rt == OPRT_OK ? "Cloud initialized"
                                          : "Warning: cloud init failed",
                      cloud_rt == OPRT_OK ? AK_COL_GREEN : AK_COL_RED);
    ak_render_status();
#endif
#if AK_TRANSPORT_DUAL
    /* The T5 exposes one global BLE callback set. While unactivated, leave it
     * with Tuya BLE/SoftAP provisioning; UART0 remains available to Companion.
     * Once activation succeeds the app loop takes over the already-created GATT
     * service without a second tal_ble_bt_init(). */
    OPERATE_RET ble_rt = OPRT_OK;
#if AK_ASR_BOARD_CLOUD
    if (cloud_rt == OPRT_OK && ak_cloud_network_ready() &&
        !ak_cloud_activated()) {
        PR_NOTICE("dual: AK BLE deferred; Tuya BLE/SoftAP provisioning active (UART0 available)");
    } else if (ak_cloud_network_ready()) {
        ble_rt = ak_ble_gatt_takeover();
        s_ak_ble_active = (ble_rt == OPRT_OK);
    } else {
        ble_rt = ak_ble_gatt_init();
        s_ak_ble_active = (ble_rt == OPRT_OK);
    }
#else
    ble_rt = ak_ble_gatt_init();
    s_ak_ble_active = (ble_rt == OPRT_OK);
#endif
    if (ble_rt != OPRT_OK) {
        PR_ERR("dual: delayed AK BLE setup failed: %d (UART remains available)", ble_rt);
#if AK_ENABLE_DISPLAY
        ak_status_bar_set("Warning: BLE init failed", AK_COL_ORANGE);
        ak_render_status();
#endif
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
#if AK_ENABLE_DISPLAY
        ak_status_bar_set("Starting ASR...", AK_COL_CYAN);
        ak_render_status();
#endif
        OPERATE_RET asr_rt = ak_asr_init(on_asr_text, on_asr_error);
        (void)asr_rt;
#if AK_ENABLE_DISPLAY
        if (asr_rt == OPRT_OK) {
            s_settings.volume = ak_asr_get_volume();
        }
        ak_status_bar_set(asr_rt == OPRT_OK ? "ASR initialized"
                                            : "Warning: ASR init failed",
                          asr_rt == OPRT_OK ? AK_COL_GREEN : AK_COL_RED);
        ak_render_status();
#endif
    }
    PR_NOTICE("Voice preference: %s (cloud backend %s)", ak_voice_source_str(s_voice_route.preferred),
              ak_asr_available() ? "ready" : "unavailable");

    /* Wi-Fi provisioning (netmgr in the cloud profile, stub otherwise). The PC
     * pushes credentials over a `wifi` frame; results return as wifi_status. */
#if AK_ENABLE_DISPLAY
    ak_status_bar_set("Starting WiFi monitor...", AK_COL_CYAN);
    ak_render_status();
#endif
    OPERATE_RET wifi_rt = ak_wifi_init(on_wifi_status);
    (void)wifi_rt;
    PR_NOTICE("Wi-Fi provisioning: %s", ak_wifi_available() ? "ready" : "stub/off");
#if AK_ENABLE_DISPLAY
    if (wifi_rt != OPRT_OK) {
        ak_status_bar_set("Warning: WiFi init failed", AK_COL_RED);
    } else {
        ak_status_bar_set_wifi(s_wifi_lcd_state);
    }
    ak_render_status();
#endif

    /* Start the Tuya IoT task (tuya_iot_start + a dedicated tuya_iot_yield loop)
     * once everything is initialized. No-op on v1/BLE; runs off the input/RX
     * loop so key latency is unaffected. Skipped implicitly if cloud init failed
     * (ak_cloud_start returns an error, logged and non-fatal). */
    if (cloud_rt == OPRT_OK && ak_cloud_start() != OPRT_OK) {
        PR_ERR("cloud start failed; device will not reach the Tuya cloud");
    }

    /* Default preference is Tuya ASR. Resolve it now: if the cloud/AI agent is
     * not ready yet, use PC ASR immediately and monitor for recovery below. */
    ak_voice_route_update(true);

    now                         = tal_system_get_millisecond();
    last_heartbeat              = now;
    s_voice_route_last_check_ms = now;

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

        if ((uint32_t)(now - s_voice_route_last_check_ms) >= AK_VOICE_ROUTE_CHECK_MS) {
            s_voice_route_last_check_ms = now;
            ak_voice_route_update(true);
        }

#if AK_ENABLE_DISPLAY
        ak_voice_ui_tick(now);
        if (s_lcd_redraw_pending &&
            (int32_t)(s_lcd_audio_guard_until_ms - now) <= 0 &&
            !ak_asr_audio_busy()) {
            ak_render_status();
        }
        if (s_wifi_status_dirty) {
            s_wifi_status_dirty = false;
            ak_wifi_state_t wifi_state = s_wifi_lcd_state;
            ak_status_bar_set_wifi(wifi_state);
            ak_refresh_link_header();
        }
#endif

#if AK_TRANSPORT_DUAL && AK_ASR_BOARD_CLOUD
        if (!s_ak_ble_active && ak_cloud_network_ready() &&
            ak_cloud_activated() &&
            (s_ak_ble_retry_at_ms == 0U ||
             (int32_t)(now - s_ak_ble_retry_at_ms) >= 0)) {
            OPERATE_RET takeover_rt = ak_ble_gatt_takeover();
            if (takeover_rt == OPRT_OK) {
                s_ak_ble_active = true;
                PR_NOTICE("dual: Tuya activation complete; AK BLE transport active");
            } else {
                s_ak_ble_retry_at_ms = now + 1000U;
                PR_WARN("dual: AK BLE takeover after activation failed: %d; retrying in 1s",
                        takeover_rt);
            }
        }
#endif

#if AK_TRANSPORT_BLE_GATT
        if (AK_BLE_RUNTIME_ACTIVE()) {
            ak_ble_gatt_maintain(now);
        }
#endif

#if AK_TRANSPORT_GREET_ON_SUBSCRIBE
        if (AK_BLE_RUNTIME_ACTIVE()) {
            /* When a central subscribes to notifications, the link is live: greet. */
            if (ak_ble_gatt_take_greet()) {
                send_hello_ack();
            }
            /* Report BLE advertising/connected/live transitions on UART0 and
             * refresh the LCD header when present. */
            {
                static ak_ble_link_state_t s_last_link = (ak_ble_link_state_t)-1;
                ak_ble_link_state_t        link = ak_ble_gatt_link_state();
                if (link != s_last_link) {
                    s_last_link = link;
                    send_system_info_uart();
#if AK_ENABLE_DISPLAY
                    ak_refresh_link_header();
#endif
                }
            }
        }
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

    OPERATE_RET rt = OPRT_COM_ERROR;
    for (uint32_t attempt = 1U; attempt <= CORE_INIT_ATTEMPTS; attempt++) {
        s_app_thread = NULL;
        rt = tal_thread_create_and_start(&s_app_thread, NULL, NULL,
                                         app_thread, NULL, &thread_cfg);
        if (rt == OPRT_OK) {
            return;
        }
        PR_ERR("app thread create failed: %d (attempt %u/%u)", rt,
               (unsigned)attempt, (unsigned)CORE_INIT_ATTEMPTS);
        if (attempt < CORE_INIT_ATTEMPTS) {
            tal_system_sleep(CORE_INIT_RETRY_MS);
        }
    }
    restart_after_fatal_init("app thread", rt);
}
