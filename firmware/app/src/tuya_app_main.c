/**
 * @file tuya_app_main.c
 * @brief Agentic Keyboard for Kiro -- product firmware entry.
 *
 * Phase-1 (wired) flow so far:
 *   - open the selected transport (UART0),
 *   - announce capabilities (hello_ack),
 *   - scan 6 keys + the EC11 encoder and emit key/encoder events (JSONL v1),
 *   - emit a periodic ping heartbeat.
 *
 * The debounce/long-press logic lives in the pure, host-tested ak_input_fsm;
 * frame formatting lives in the pure, host-tested ak_protocol. RX/ack parsing,
 * LCD/LVGL, BLE and board cloud ASR come in later tasks; the transport backend
 * is selected at build time via ak_features.h.
 */

#include <string.h>

#include "ak_board_io.h"
#include "ak_battery.h"
#include "ak_leds.h"
#include "ak_features.h"
#include "ak_font8x8.h"
#include "ak_asr.h"
#include "ak_cloud.h"
#include "ak_wifi.h"
#include "ak_input.h"
#include "ak_keymap.h"
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
/* DUAL firmware supports UART0 and BLE GATT, while Companion selects exactly
 * one PC transport. Before selection, UART remains available and AK BLE may
 * advertise. A USB hello locks this boot to UART and disables AK BLE; a BLE
 * subscription keeps UART idle. Reads still share one ak_rx assembler, but
 * Companion's disconnect-before-switch rule prevents source interleaving. */
#include "ak_ble_gatt.h"
#include "ak_uart.h"
#define AK_TRANSPORT_NAME "dual"
/* BLE still greets on notify-subscribe; UART greets immediately (both fire). */
#define AK_TRANSPORT_GREET_ON_SUBSCRIBE 1

static bool     s_transport_rx_is_usb = false;
static bool     s_ak_ble_active       = false;
static bool     s_usb_selected        = false;
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

/* Runtime key-role table (v0.3 defaults; overridable by a Companion keymap
 * frame + cached in TAL KV). Replaces the old hard-coded AK_PTT_KEY_ID. */
static ak_keymap_t s_keymap;
/* TAL KV key holding the Companion-provided keymap override blob (plan A). */
#define AK_KEYMAP_KV_KEY "ak_keymap"
/* Deferred single/double classifiers for the CHAT keys (K1-K3). Index by the
 * keymap slot (0..2); other slots are unused. Scheme X: single is delayed to
 * tell it apart from a double, then reported as key.action "click"/"double". */
static ak_gesture_state_t s_key_gesture[AK_KEYMAP_SLOTS];

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
/* Post-ASR pending-confirm state (firmware-led, scheme "乙"): after a successful
 * recognition the text is already in the PC composer; the firmware enters this
 * state so K4/K6 become edit keys (cancel_char / cancel_all / confirm, sent to
 * the Companion to inject) and LED4/LED6 give the red-breathing / green-blink
 * hints. Cleared on confirm, cancel-all, or a timeout. */
static bool                s_pending_confirm;
static uint32_t            s_pending_confirm_since_ms;
#define PENDING_CONFIRM_TIMEOUT_MS 20000U
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

/* On-device hardware test page (entered from Settings -> Test hardware).
 * Modal like settings: swallows input, drives WS2812, shows live block state,
 * and is left by a long-press of the EC11 push. */
static bool     s_test_active;
static uint8_t  s_test_key_mask;      /* bit i set while K(i+1) is held; bit 6 = enc */
static int32_t  s_test_enc_count;     /* net EC11 detents since entering */
static uint32_t s_test_ws_phase;      /* WS2812 effect animation phase */
/* LED effect mode in the test page (K1-K6 pick, EC11 tunes), like the bringup
 * full_selftest. SOLID/COMET/BREATHE/RAINBOW/CHASE/BLINK = 0..5. */
static int      s_test_fx;            /* current effect 0..5 */
static int32_t  s_test_fx_param[6];   /* per-effect EC11 param (hue or speed) */
#define AK_TEST_FX_COUNT   6
#define AK_TEST_FX_LEVEL   48
#define AK_TEST_FX_SPEED_MIN 1
#define AK_TEST_FX_SPEED_MAX 10
static const char *const s_test_fx_names[AK_TEST_FX_COUNT] = {
    "SOLID", "COMET", "BREATHE", "RAINBOW", "CHASE", "BLINK",
};
#endif

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
    if (s_ak_ble_active) {
        ak_ble_link_state_t link = ak_ble_gatt_link_state();
        ble_name                 = ak_ble_gatt_device_name();
        ble_state                = ak_ble_gatt_link_state_str(link);
    }
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

/* Emit a human-readable transport lifecycle line on UART0. PR_NOTICE goes to
 * UART1 and is not visible to Companion, so every LCD transport prompt also
 * gets this JSONL log frame on the trusted diagnostics channel. */
static void send_log_uart(const char *tag, const char *message)
{
#if AK_TRANSPORT_UART || AK_TRANSPORT_DUAL
    char frame[FRAME_BUFFER_SIZE];
    if (!transport_lock()) {
        return;
    }
    uint32_t sequence = ++s_sequence;
    int length = ak_proto_log(frame, sizeof(frame), sequence, tag, message);
    if (length > 0 && length < (int)sizeof(frame)) {
        int written = ak_uart_write(frame, (uint32_t)length);
        if (written != length) {
            PR_WARN("UART0 log short write: %d/%d", written, length);
        }
    } else {
        PR_WARN("UART0 log frame too large: %d", length);
    }
    transport_unlock();
#else
    (void)tag;
    (void)message;
#endif
}


static const char *boot_reset_reason_str(TUYA_RESET_REASON_E reason)
{
    switch (reason) {
    case TUYA_RESET_REASON_POWERON:
        return "power-on";
    case TUYA_RESET_REASON_HW_WDOG:
        return "hardware-watchdog";
    case TUYA_RESET_REASON_FAULT:
        return "access-fault";
    case TUYA_RESET_REASON_SW_WDOG:
        return "software-watchdog";
    case TUYA_RESET_REASON_SOFTWARE:
        return "software-reset";
    case TUYA_RESET_REASON_DEEPSLEEP:
        return "deep-sleep";
    case TUYA_RESET_REASON_EXTERNAL:
        return "external-reset";
    case TUYA_RESET_REASON_CRASH:
        return "software-crash";
    case TUYA_RESET_REASON_FLASH:
        return "flash-failure";
    case TUYA_RESET_REASON_FATAL:
        return "fatal-error";
    case TUYA_RESET_REASON_BROWNOUT:
        return "brownout";
    case TUYA_RESET_REASON_BOOTLOADER:
        return "bootloader";
    case TUYA_RESET_REASON_UNKNOWN:
    default:
        return "unknown";
    }
}

static volatile bool s_activation_ui_pending;
static char          s_activation_ui_state[32];

static void send_activation_status_uart(const char *state, const char *qr_url)
{
#if AK_TRANSPORT_UART || AK_TRANSPORT_DUAL
    char frame[SYSTEM_INFO_FRAME_BUFFER_SIZE];
    if (!transport_lock()) {
        return;
    }
    uint32_t sequence = ++s_sequence;
    int length = ak_proto_activation_status(frame, sizeof(frame), sequence,
                                            state, qr_url);
    if (length > 0 && length < (int)sizeof(frame)) {
        int written = ak_uart_write(frame, (uint32_t)length);
        if (written != length) {
            PR_WARN("UART0 activation status short write: %d/%d", written, length);
        }
    } else {
        PR_WARN("UART0 activation status frame too large: %d", length);
    }
    transport_unlock();
#else
    (void)state;
    (void)qr_url;
#endif
}

static void on_cloud_activation(const char *state, const char *qr_url)
{
    (void)snprintf(s_activation_ui_state, sizeof(s_activation_ui_state), "%s",
                   state == NULL ? "unknown" : state);
    s_activation_ui_pending = true;
    send_activation_status_uart(s_activation_ui_state, qr_url);
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
static void ak_pending_confirm_enter(uint32_t now_ms);
static void ak_pending_confirm_exit(void);
static void ak_pending_confirm_tick(uint32_t now_ms);
static void ak_status_bar_set(const char *text, uint16_t color);
static void dispatch_edit_action(const char *id, const char *action);
static void ak_select_session_key(const char *id);
static void ak_render_status(void);
static void ak_settings_draw(void);
static bool ak_settings_consume_input(ak_input_kind_t kind, const char *id,
                                      ak_input_event_t event, uint32_t held_ms);
/* Returns true when the encoder rotation was consumed by the modal settings UI
 * (so it must not be forwarded to the companion as an encoder frame). */
static bool ak_settings_consume_encoder(int32_t detents);
/* On-device test page (entered from Settings; runs its own blocking loop). */
static void ak_test_run(void);
#endif

static void ak_emit_input_frame(ak_input_kind_t kind, const char *id,
                                ak_input_event_t event, uint32_t held_ms)
{
    char     frame[FRAME_BUFFER_SIZE];
    int      length;
    uint32_t sequence;

    (void)kind; /* only AK_INPUT_KIND_KEY remains in v0.3 */
    if (!transport_begin_sequenced(&sequence)) {
        return;
    }
    length = ak_proto_key(frame, sizeof(frame), sequence, id,
                          ak_input_event_str(event), held_ms);
    transport_end_sequenced(frame, length);
    PR_DEBUG("input %s %s held=%ums", id, ak_input_event_str(event), held_ms);
}

/* EC11 rotation -> encoder frame (signed detent count) on the active transport. */
static void ak_emit_encoder_frame(int32_t detents)
{
    char     frame[FRAME_BUFFER_SIZE];
    int      length;
    uint32_t sequence;

    if (detents == 0 || !transport_begin_sequenced(&sequence)) {
        return;
    }
    length = ak_proto_encoder(frame, sizeof(frame), sequence, detents);
    transport_end_sequenced(frame, length);
    PR_DEBUG("encoder detents=%d", (int)detents);
}

/* Emit a classified gesture (click/double) for a deferred key as a key.action
 * frame, and give CHAT keys their local session highlight on a single click.
 * Called from on_input_event (DOUBLE) and the main loop's gesture tick (SINGLE
 * timeout). role lets the CHAT keys drive the on-screen A/B/C selection. */
static void dispatch_key_gesture(const char *id, ak_key_role_t role, ak_gesture_t g)
{
    const char *action;
    switch (g) {
    case AK_GESTURE_SINGLE: action = "click";  break;
    case AK_GESTURE_DOUBLE: action = "double"; break;
    default:                return; /* NONE / LONG not produced for these keys */
    }

#if AK_ENABLE_DISPLAY
    /* A single click on a CHAT key focuses its A/B/C session locally (double =
     * switch instance, which is a Companion-side Ctrl+N; nothing local). */
    if (role == AK_KEY_ROLE_CHAT && g == AK_GESTURE_SINGLE) {
        ak_select_session_key(id);
    }
#else
    (void)role;
#endif

    char     frame[FRAME_BUFFER_SIZE];
    uint32_t sequence;
    if (!transport_begin_sequenced(&sequence)) {
        return;
    }
    transport_end_sequenced(
        frame, ak_proto_key(frame, sizeof(frame), sequence, id, action, 0U));
    PR_DEBUG("gesture %s %s", id, action);
}

/* Emit a post-ASR edit intent (cancel_char / cancel_all / confirm) as a key
 * frame; the Companion injects the matching PC edit key. */
static void dispatch_edit_action(const char *id, const char *action)
{
    char     frame[FRAME_BUFFER_SIZE];
    uint32_t sequence;
    if (!transport_begin_sequenced(&sequence)) {
        return;
    }
    transport_end_sequenced(
        frame, ak_proto_key(frame, sizeof(frame), sequence, id, action, 0U));
    PR_DEBUG("edit %s %s", id, action);
}

/* Input event -> local settings UI or JSONL frame on the active transport. */
static void on_input_event(ak_input_kind_t kind, const char *id,
                           ak_input_event_t event, uint32_t held_ms)
{
#if AK_ENABLE_DISPLAY
    if (ak_settings_consume_input(kind, id, event, held_ms)) {
        return;
    }
#endif

    if (kind != AK_INPUT_KIND_KEY) {
        return;
    }

    ak_key_role_t         role    = ak_keymap_role(&s_keymap, id);
    ak_key_gesture_mode_t gesture = ak_keymap_gesture(&s_keymap, id);

    /* CHAT keys (K1-K3): deferred single/double. Feed the classifier here;
     * ak_gesture_tick() in the main loop resolves a lone single. The resolved
     * gesture becomes a key.action "click"/"double" frame (see dispatch_key_
     * gesture); raw down/up are NOT forwarded for these keys. */
    if (gesture == AK_KEY_GESTURE_SINGLE_DOUBLE) {
        int slot = ak_keymap_slot_for_id(id);
        if (slot >= 0) {
            ak_gesture_t g = ak_gesture_feed(&s_key_gesture[slot], event,
                                             tal_system_get_millisecond(), held_ms);
            dispatch_key_gesture(id, role, g);
        }
        return;
    }

    /* Pending-confirm (post-ASR): K4/K6 become edit keys. Intercept BEFORE the
     * raw edge is forwarded so no stray down/up leaks while confirming.
     * K4 (cancel): long = cancel_all (+exit), click = cancel_char.
     * K6 (confirm): click = confirm (+exit). A LONG sets a per-event flag so the
     * following UP is not also treated as a click. */
#if AK_ENABLE_DISPLAY
    if (s_pending_confirm &&
        (role == AK_KEY_ROLE_CANCEL || role == AK_KEY_ROLE_CONFIRM)) {
        /* Per-key long-press guard (K4 and K6 tracked separately) so one key's
         * long press cannot suppress the other's click, and a long press is
         * never mistaken for a click on release. */
        static bool s_pc_long_fired[AK_KEYMAP_SLOTS];
        int slot = ak_keymap_slot_for_id(id);
        bool *long_fired = (slot >= 0) ? &s_pc_long_fired[slot] : &s_pc_long_fired[0];
        if (event == AK_INPUT_EVENT_DOWN) {
            *long_fired = false;
        } else if (event == AK_INPUT_EVENT_LONG) {
            /* Any long press marks this press so the following UP is NOT a click.
             * K4 long = cancel_all (+exit); K6 long has no action but must still
             * block the release from being read as a confirm. */
            *long_fired = true;
            if (role == AK_KEY_ROLE_CANCEL) {
                dispatch_edit_action(id, "cancel_all");
                ak_pending_confirm_exit();
                ak_status_bar_set("Input cancelled", AK_COL_ORANGE);
            }
        } else if (event == AK_INPUT_EVENT_UP) {
            if (!*long_fired) {
                if (role == AK_KEY_ROLE_CANCEL) {
                    dispatch_edit_action(id, "cancel_char"); /* delete one char */
                } else { /* CONFIRM */
                    dispatch_edit_action(id, "confirm");
                    ak_pending_confirm_exit();
                    ak_status_bar_set("Input confirmed", AK_COL_GREEN);
                }
            }
            *long_fired = false;
        }
        ak_render_status();
        return; /* swallow: no raw edge while in pending-confirm */
    }
#endif

    /* EDGE keys: forward the raw edge as before. */
    ak_emit_input_frame(kind, id, event, held_ms);

    /* VOICE role (push-to-talk; K5 by default). Cloud consumes DOWN/LONG/UP as
     * one latched HOLD; system emits immediate DOWN/UP dictation. */
    if (role == AK_KEY_ROLE_VOICE &&
        (event == AK_INPUT_EVENT_DOWN || event == AK_INPUT_EVENT_LONG ||
         event == AK_INPUT_EVENT_UP)) {
        if (ak_dclick_feed(&s_ptt_dclick, event, tal_system_get_millisecond(), held_ms) &&
            s_voice_route.effective == AK_VOICE_CLOUD && ak_asr_available()) {
            ak_asr_play_prompt();
        }
        on_ptt_event(event);
    }
}

/* EC11 rotation -> test page / settings UI when modal, else an encoder frame. */
static void on_encoder_event(int32_t detents)
{
#if AK_ENABLE_DISPLAY
    if (ak_settings_consume_encoder(detents)) {
        return;
    }
#endif
    ak_emit_encoder_frame(detents);
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
    send_activation_status_uart(ak_cloud_activation_state(),
                                ak_cloud_binding_qr_url());
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
    const char *log_message;
    switch (state) {
    case AK_WIFI_CONNECTING:
        log_message = "WiFi connecting...";
        break;
    case AK_WIFI_CONNECTED:
        log_message = "WiFi connected";
        break;
    case AK_WIFI_FAILED:
        log_message = "Warning: WiFi failed";
        break;
    case AK_WIFI_DISCONNECTED:
    default:
        log_message = "Warning: WiFi disconnected";
        break;
    }
    send_log_uart("wifi", log_message);
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
#define LCD_HEADER_DISCONNECTED_BG AK_RGB565(0xFFU, 0xE6U, 0x99U)
#define LCD_HEADER_CONNECTED_BG    AK_RGB565(0x65U, 0xFFU, 0x65U)

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
    const char *text;
    uint16_t    color;

    /* Priority: pairing mode > voice fallback > last status message. When the
     * device is awaiting Wi-Fi provisioning the user must know to open the Tuya
     * app / companion, so that hint outranks everything else. */
    if (ak_cloud_provisioning()) {
        text  = "Pairing mode: use Tuya app";
        color = AK_COL_ORANGE;
    } else if (s_voice_fallback_active) {
        text  = "Tuya ASR unavailable -> PC ASR";
        color = AK_COL_ORANGE;
    } else {
        text  = s_status_bar_text;
        color = s_status_bar_color;
    }

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
    bool editing  = selected && s_settings.editing;
    uint16_t bg = selected ? LCD_SESSION_SELECTED_BG : AK_COL_BLACK;
    uint16_t fg = editing ? AK_COL_YELLOW : (selected ? AK_COL_WHITE : AK_COL_CYAN);
    char value_text[12];

    ak_gfx_fill_rect(LCD_SETTINGS_ROW_X, y, LCD_SETTINGS_ROW_W, 48U, bg);
    if (!selected) {
        ak_settings_outline(LCD_SETTINGS_ROW_X, y, LCD_SETTINGS_ROW_W, 48U,
                            AK_COL_GRAY);
    } else if (editing) {
        /* A bright outline signals "rotate now adjusts this value". */
        ak_settings_outline(LCD_SETTINGS_ROW_X, y, LCD_SETTINGS_ROW_W, 48U,
                            AK_COL_YELLOW);
    }
    ak_gfx_text_thin(24U, (uint16_t)(y + 8U), label,
                     selected ? AK_COL_WHITE : AK_COL_GRAY, bg, 1);
    (void)snprintf(value_text, sizeof(value_text), "< %3u%% >", value);
    ak_gfx_text_thin(232U, (uint16_t)(y + 8U), value_text, fg, bg, 1);
    if (editing) {
        ak_gfx_text_thin(160U, (uint16_t)(y + 8U), "EDIT",
                         AK_COL_YELLOW, bg, 1);
    } else if (selected && s_settings_confirmed) {
        ak_gfx_text_thin(160U, (uint16_t)(y + 8U), "Applied",
                         AK_COL_WHITE, bg, 1);
    }

    ak_gfx_fill_rect(24U, (uint16_t)(y + 31U), LCD_SETTINGS_BAR_W, 6U,
                     selected ? AK_COL_BLACK : AK_COL_GRAY);
    ak_gfx_fill_rect(24U, (uint16_t)(y + 31U),
                     (uint16_t)((uint32_t)LCD_SETTINGS_BAR_W * value / 100U),
                     6U, editing ? AK_COL_YELLOW
                                 : (selected ? AK_COL_WHITE : LCD_SESSION_SELECTED_BG));
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

    ak_settings_value_row(40U, AK_SETTINGS_ITEM_VOLUME, "Volume",
                          s_settings.volume);
    ak_settings_value_row(92U, AK_SETTINGS_ITEM_BRIGHTNESS, "Brightness",
                          s_settings.brightness);

    /* Test row (enter the on-device hardware test page). */
    bool test_selected = s_settings.selected == AK_SETTINGS_ITEM_TEST;
    uint16_t test_bg = test_selected ? LCD_SESSION_SELECTED_BG : AK_COL_BLACK;
    ak_gfx_fill_rect(LCD_SETTINGS_ROW_X, 146U, LCD_SETTINGS_ROW_W, 26U, test_bg);
    if (!test_selected) {
        ak_settings_outline(LCD_SETTINGS_ROW_X, 146U, LCD_SETTINGS_ROW_W, 26U,
                            AK_COL_GRAY);
    }
    ak_gfx_text_thin(120U, 155U, "Test hardware",
                     test_selected ? AK_COL_WHITE : AK_COL_GRAY, test_bg, 1);

    bool exit_selected = s_settings.selected == AK_SETTINGS_ITEM_EXIT;
    uint16_t exit_bg = exit_selected ? LCD_SESSION_SELECTED_BG : AK_COL_BLACK;
    ak_gfx_fill_rect(LCD_SETTINGS_ROW_X, 176U, LCD_SETTINGS_ROW_W, 26U,
                     exit_bg);
    if (!exit_selected) {
        ak_settings_outline(LCD_SETTINGS_ROW_X, 176U, LCD_SETTINGS_ROW_W, 26U,
                            AK_COL_GRAY);
    }
    ak_gfx_text_thin(136U, 185U, "Exit",
                     exit_selected ? AK_COL_WHITE : AK_COL_GRAY, exit_bg, 1);

    ak_gfx_fill_rect(0U, 207U, AK_STATUSLCD_W, 33U, AK_COL_BLACK);
    ak_gfx_fill_rect(0U, 207U, AK_STATUSLCD_W, 1U,
                     LCD_SESSION_SELECTED_BG);
    if (s_settings.editing) {
        ak_gfx_text_thin(4U, 213U, "Rotate: adjust value",
                         AK_COL_YELLOW, AK_COL_BLACK, 1);
        ak_gfx_text_thin(4U, 227U, "Click: confirm & exit edit", AK_COL_GRAY,
                         AK_COL_BLACK, 1);
    } else {
        ak_gfx_text_thin(4U, 213U, "Rotate: select item",
                         AK_COL_CYAN, AK_COL_BLACK, 1);
        ak_gfx_text_thin(4U, 227U, "Click: edit value / run Test / Exit",
                         AK_COL_GRAY, AK_COL_BLACK, 1);
    }
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
    /* v0.3: the modal settings UI is driven by the EC11 encoder only.
     *   - enter    : long-press the encoder push ("enc")
     *   - navigate : rotate (handled in ak_settings_consume_encoder -> UP/DOWN)
     *   - adjust    : short-press "enc" advances to the next item; the value is
     *                 applied live as you rotate, so there is no separate commit
     *   - exit      : short-press "enc" on the EXIT item, or long-press anywhere
     */
    if (!s_settings_active) {
        if (kind != AK_INPUT_KIND_KEY || strcmp(id, AK_ENC_SW_ID) != 0) {
            return false;
        }
        if (event == AK_INPUT_EVENT_DOWN) {
            s_center_long_consumed = false;
            return true;
        }
        if (event == AK_INPUT_EVENT_LONG) {
            s_center_long_consumed = true;
            ak_pending_confirm_exit(); /* drop pending-confirm + its hint LEDs */
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
            /* A plain short press of the encoder push is forwarded as a normal
             * "enc" key event (id "enc") so the companion can bind it. */
            ak_emit_input_frame(AK_INPUT_KIND_KEY, AK_ENC_SW_ID,
                                AK_INPUT_EVENT_DOWN, 0U);
            return false;
        }
        return true;
    }

    /* Settings is modal: swallow every key so K1-K6 / PTT never reach the
     * companion or change the hidden work screen. */
    if (kind != AK_INPUT_KIND_KEY) {
        return true;
    }
    if (strcmp(id, AK_ENC_SW_ID) == 0) {
        if (event == AK_INPUT_EVENT_DOWN) {
            s_center_long_consumed = false;
        } else if (event == AK_INPUT_EVENT_LONG) {
            /* Long-press no longer does anything in settings: every transition
             * is driven by a short click so a held encoder cannot re-trigger
             * the item you just left (see also the test-page exit). Mark it so
             * the following UP is swallowed, not treated as a click. */
            s_center_long_consumed = true;
        } else if (event == AK_INPUT_EVENT_UP) {
            if (s_center_long_consumed) {
                s_center_long_consumed = false;
                return true;
            }
            /* Short click = CONFIRM. The model decides what that means for the
             * current item/mode: value item -> enter/leave edit; TEST/EXIT ->
             * execute directly. */
            ak_settings_result_t result =
                ak_settings_handle(&s_settings, AK_SETTINGS_ACTION_CONFIRM);
            switch (result) {
            case AK_SETTINGS_RESULT_EXIT:
                s_settings_active = false;
                s_settings_confirmed = false;
                ak_render_status();
                break;
            case AK_SETTINGS_RESULT_ENTER_TEST:
                ak_test_run();
                break;
            case AK_SETTINGS_RESULT_ENTER_EDIT:
            case AK_SETTINGS_RESULT_CONFIRMED:
            default:
                s_settings_confirmed =
                    (result == AK_SETTINGS_RESULT_CONFIRMED);
                ak_settings_draw();
                break;
            }
        }
        return true;
    }
    (void)held_ms;
    return true;
}

/* EC11 rotation inside the modal settings UI. The two-level model decides what
 * rotation means: while editing a value it adjusts it (one step per detent);
 * otherwise it moves the selection. Positive detents = DOWN/increase, negative
 * = UP/decrease. Returns true iff settings is active (so no frame is sent). */
static bool ak_settings_consume_encoder(int32_t detents)
{
    if (!s_settings_active || detents == 0) {
        return s_settings_active;
    }
    ak_settings_action_t action =
        detents > 0 ? AK_SETTINGS_ACTION_DOWN : AK_SETTINGS_ACTION_UP;
    int32_t steps = detents > 0 ? detents : -detents;
    for (int32_t i = 0; i < steps; i++) {
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
    }
    ak_settings_draw();
    return true;
}

/* ───────────────────────── On-device test page ─────────────────────────
 * Modal hardware test, entered from Settings -> "Test hardware". Shows live
 * state for six blocks (keys, EC11, WS2812, battery, WiFi, BLE), runs a WS2812
 * chase, and is left by a LONG-press of the EC11 push. While active it swallows
 * all input so nothing leaks to the companion or the work screen.
 */
#define AK_TEST_TICK_MS 120U

static uint32_t s_test_uart_rx; /* bytes drained from UART0 while testing */

/* Map a key id ("k1".."k6" / "enc") to a bit in s_test_key_mask. */
static int ak_test_key_bit(const char *id)
{
    if (id == NULL) {
        return -1;
    }
    if (strcmp(id, AK_ENC_SW_ID) == 0) {
        return 6;
    }
    if (id[0] == 'k' && id[1] >= '1' && id[1] <= '6' && id[2] == '\0') {
        return id[1] - '1';
    }
    return -1;
}

/* Fixed Y positions of each dynamic row, so a changed value repaints only its
 * own line instead of the whole 320x240 frame (the old per-event full redraw is
 * what starved the input poll and made keys/long-press feel dead). */
#define AK_TEST_Y_KEYS   28U
#define AK_TEST_Y_ENC    54U
#define AK_TEST_Y_WS     72U
#define AK_TEST_Y_BAT    90U
#define AK_TEST_Y_UART   108U
#define AK_TEST_Y_WIFI   126U
#define AK_TEST_Y_BLE    144U

/* Draw the one static key cell (held = green). */
static void ak_test_draw_key_cell(int i, bool held)
{
    static const char *const k_lbl[7] = {"1", "2", "3", "4", "5", "6", "E"};
    uint16_t x  = (uint16_t)(60U + i * 34U);
    uint16_t bg = held ? AK_COL_GREEN : AK_COL_DKBLUE;
    uint16_t fg = held ? AK_COL_BLACK : AK_COL_WHITE;
    ak_gfx_fill_rect(x, AK_TEST_Y_KEYS, 28U, 18U, bg);
    ak_gfx_text(x + 10U, AK_TEST_Y_KEYS + 5U, k_lbl[i], fg, bg, 1);
}

/* Repaint only the key row from the current mask (partial redraw). */
static void ak_test_draw_keys(void)
{
    for (int i = 0; i < 7; i++) {
        ak_test_draw_key_cell(i, (s_test_key_mask & (1U << i)) != 0U);
    }
    ak_statuslcd_flush();
}

/* Repaint only the EC11 detent-count line. */
static void ak_test_draw_enc(void)
{
    char line[48];
    ak_gfx_fill_rect(0U, AK_TEST_Y_ENC, AK_STATUSLCD_W, 14U, AK_COL_BLACK);
    (void)snprintf(line, sizeof(line), "FX %s  %s %ld  (EC11 %+ld)",
                   s_test_fx_names[s_test_fx],
                   s_test_fx == 0 ? "hue" : "spd",
                   (long)s_test_fx_param[s_test_fx], (long)s_test_enc_count);
    ak_gfx_text_thin(6U, AK_TEST_Y_ENC, line, AK_COL_CYAN, AK_COL_BLACK, 1);
    ak_statuslcd_flush();
}

/* Draw the full static test-page frame ONCE on entry. */
static void ak_test_draw_frame(void)
{
    char line[48];

    ak_gfx_clear(AK_COL_BLACK);
    ak_gfx_fill_rect(0U, 0U, AK_STATUSLCD_W, 22U, LCD_SESSION_SELECTED_BG);
    ak_gfx_text(6U, 7U, "Hardware Test", AK_COL_BLACK, LCD_SESSION_SELECTED_BG, 1);

    ak_gfx_text_thin(6U, 30U, "Keys", AK_COL_GRAY, AK_COL_BLACK, 1);
    for (int i = 0; i < 7; i++) {
        ak_test_draw_key_cell(i, false);
    }

    (void)snprintf(line, sizeof(line), "FX %s  %s %ld  (EC11 %+ld)",
                   s_test_fx_names[s_test_fx],
                   s_test_fx == 0 ? "hue" : "spd",
                   (long)s_test_fx_param[s_test_fx], (long)s_test_enc_count);
    ak_gfx_text_thin(6U, AK_TEST_Y_ENC, line, AK_COL_CYAN, AK_COL_BLACK, 1);

    (void)snprintf(line, sizeof(line), "WS2812 x%u: %s", (unsigned)AK_LEDS_COUNT,
                   ak_leds_available() ? "K1-K6 effect, EC11 tune" : "unavailable");
    ak_gfx_text_thin(6U, AK_TEST_Y_WS, line, ak_leds_available() ? AK_COL_GREEN : AK_COL_RED,
                     AK_COL_BLACK, 1);

    /* Battery/WiFi/BLE drawn by their periodic refreshers below; seed labels. */
    ak_gfx_text_thin(6U, AK_TEST_Y_BAT, "Battery: ...", AK_COL_WHITE, AK_COL_BLACK, 1);
    (void)snprintf(line, sizeof(line), "UART0 (%s): rx %lu bytes", AK_TRANSPORT_NAME,
                   (unsigned long)s_test_uart_rx);
    ak_gfx_text_thin(6U, AK_TEST_Y_UART, line, AK_COL_WHITE, AK_COL_BLACK, 1);
    ak_gfx_text_thin(6U, AK_TEST_Y_WIFI, "WiFi: ...", AK_COL_WHITE, AK_COL_BLACK, 1);
    ak_gfx_text_thin(6U, AK_TEST_Y_BLE, "BLE: ...", AK_COL_WHITE, AK_COL_BLACK, 1);

    ak_gfx_fill_rect(0U, 207U, AK_STATUSLCD_W, 33U, AK_COL_BLACK);
    ak_gfx_fill_rect(0U, 207U, AK_STATUSLCD_W, 1U, LCD_SESSION_SELECTED_BG);
    ak_gfx_text_thin(4U, 213U, "K1-K6 pick effect  EC11 tune", AK_COL_CYAN, AK_COL_BLACK, 1);
    ak_gfx_text_thin(4U, 227U, "Double-click EC11 to exit", AK_COL_GRAY, AK_COL_BLACK, 1);
    ak_statuslcd_flush();
}

/* Repaint the slow rows (battery/UART/WiFi/BLE) on the periodic tick. */
static void ak_test_draw_slow(void)
{
    char line[48];

    ak_gfx_fill_rect(0U, AK_TEST_Y_BAT, AK_STATUSLCD_W, 14U, AK_COL_BLACK);
    if (ak_battery_available()) {
        int32_t node_mv = ak_battery_read_mv();
        (void)snprintf(line, sizeof(line), "Battery: %u%%  (ADC %ld mV)",
                       (unsigned)ak_battery_percent(), (long)(node_mv < 0 ? 0 : node_mv));
    } else {
        (void)snprintf(line, sizeof(line), "Battery: ADC unavailable");
    }
    ak_gfx_text_thin(6U, AK_TEST_Y_BAT, line, AK_COL_WHITE, AK_COL_BLACK, 1);

    ak_gfx_fill_rect(0U, AK_TEST_Y_UART, AK_STATUSLCD_W, 14U, AK_COL_BLACK);
    (void)snprintf(line, sizeof(line), "UART0 (%s): rx %lu bytes", AK_TRANSPORT_NAME,
                   (unsigned long)s_test_uart_rx);
    ak_gfx_text_thin(6U, AK_TEST_Y_UART, line, AK_COL_WHITE, AK_COL_BLACK, 1);

    ak_gfx_fill_rect(0U, AK_TEST_Y_WIFI, AK_STATUSLCD_W, 14U, AK_COL_BLACK);
    (void)snprintf(line, sizeof(line), "WiFi: %s %s",
                   ak_wifi_state_str(ak_wifi_current_state()), ak_wifi_current_ssid());
    ak_gfx_text_thin(6U, AK_TEST_Y_WIFI, line, AK_COL_WHITE, AK_COL_BLACK, 1);

    ak_gfx_fill_rect(0U, AK_TEST_Y_BLE, AK_STATUSLCD_W, 14U, AK_COL_BLACK);
    (void)snprintf(line, sizeof(line), "BLE: %s",
                   ak_ble_gatt_link_state_str(ak_ble_gatt_link_state()));
    ak_gfx_text_thin(6U, AK_TEST_Y_BLE, line, AK_COL_WHITE, AK_COL_BLACK, 1);

    ak_statuslcd_flush();
}

/* ---- Test-page private input callbacks (used only inside ak_test_run) ----
 * These feed off the SAME ak_input_poll the main loop uses, but are routed here
 * only while the test page owns the loop. They update the live mask / detent
 * count and repaint ONLY the affected row. EXIT is a DOUBLE-CLICK of the EC11
 * push: a single click (and a long-press) must stay testable as ordinary key
 * input, so only a double-click leaves the page.
 */
static volatile bool s_test_exit_req;
static ak_dclick_t   s_test_enc_dclick;

/* --- Test-page LED effects (ported from bringup full_selftest) -------------
 * Six effects; K1-K6 pick one, EC11 tunes it (SOLID = hue, others = speed).
 * The chain is 7 LEDs. Rendered each animation frame from s_test_ws_phase. */

/* Scale 0..AK_TEST_FX_LEVEL hue colour by n/d into an RGB written to LED i. */
static void ak_test_fx_set_scaled(uint32_t i, int32_t hue, int32_t n, int32_t d)
{
    if (d <= 0) d = 1;
    if (n < 0)  n = 0;
    /* Resolve hue->rgb at full level, then scale. Reuse set_hue by scaling the
     * level argument (hue ramp is linear in level for our wheel). */
    uint8_t lvl = (uint8_t)((int32_t)AK_TEST_FX_LEVEL * n / d);
    ak_leds_set_hue(i, hue, lvl);
}

static void ak_test_leds_render(void)
{
    if (!ak_leds_available()) {
        return;
    }
    ak_leds_clear();
    int32_t  p   = s_test_fx_param[s_test_fx];
    uint32_t ph  = s_test_ws_phase;
    uint32_t n   = AK_LEDS_COUNT;

    switch (s_test_fx) {
    case 0: /* SOLID: whole chain one colour, EC11 sweeps hue (p). */
        for (uint32_t i = 0; i < n; i++) ak_leds_set_hue(i, p, AK_TEST_FX_LEVEL);
        break;
    case 1: /* COMET: bright head + fading tail. */
        for (uint32_t i = 0; i < n; i++) {
            int32_t dist = (int32_t)((ph + n - i) % n);
            ak_test_fx_set_scaled(i, (int32_t)ph, (int32_t)n - dist, (int32_t)n);
        }
        break;
    case 2: { /* BREATHE: whole chain fades up/down. */
        int32_t t   = (int32_t)(ph % 32U);
        int32_t lvl = (t < 16) ? t : (32 - t); /* 0..16 */
        for (uint32_t i = 0; i < n; i++)
            ak_test_fx_set_scaled(i, (int32_t)(ph / 32), lvl, 16);
        break;
    }
    case 3: /* RAINBOW: hue spread across the chain, scrolling. */
        for (uint32_t i = 0; i < n; i++)
            ak_leds_set_hue(i, (int32_t)(ph + i * (AK_LEDS_HUE_STEPS / n)), AK_TEST_FX_LEVEL);
        break;
    case 4: /* CHASE: one lit pixel marching -> verifies each LED in turn. */
        ak_leds_set_hue(ph % n, (int32_t)ph, AK_TEST_FX_LEVEL);
        break;
    default: /* BLINK: whole chain flashes on/off. */
        if (ph & 1U)
            for (uint32_t i = 0; i < n; i++) ak_leds_set_hue(i, (int32_t)(ph / 2), AK_TEST_FX_LEVEL);
        break;
    }
    (void)ak_leds_show();
}

/* ms between animation frames for the current effect (SOLID is static). */
static uint32_t ak_test_fx_frame_ms(void)
{
    if (s_test_fx == 0) {
        return 1000U;
    }
    int32_t speed = s_test_fx_param[s_test_fx];
    if (speed < AK_TEST_FX_SPEED_MIN) speed = AK_TEST_FX_SPEED_MIN;
    if (speed > AK_TEST_FX_SPEED_MAX) speed = AK_TEST_FX_SPEED_MAX;
    return (uint32_t)(360 - speed * 32); /* 1->328ms .. 10->40ms */
}

static void ak_test_input_cb(ak_input_kind_t kind, const char *id,
                             ak_input_event_t event, uint32_t held_ms)
{
    (void)kind;
    int bit = ak_test_key_bit(id);
    if (bit < 0) {
        return;
    }

    if (event == AK_INPUT_EVENT_DOWN) {
        s_test_key_mask |= (uint8_t)(1U << bit);
        /* K1-K6 pick one of the six LED effects (bit 0..5). */
        if (bit >= 0 && bit < AK_TEST_FX_COUNT) {
            s_test_fx = bit;
            ak_test_leds_render();
        }
        ak_test_draw_keys();
    } else if (event == AK_INPUT_EVENT_UP) {
        s_test_key_mask &= (uint8_t)~(1U << bit);
        ak_test_draw_keys();
    }

    /* Double-click of the EC11 push exits. Feeding every enc edge lets the
     * detector see DOWN/LONG/UP; a hold fires LONG which it excludes, so
     * hold-to-test never trips the exit. */
    if (bit == 6) {
        if (ak_dclick_feed(&s_test_enc_dclick, event,
                           tal_system_get_millisecond(), held_ms)) {
            s_test_exit_req = true;
        }
    }
}

static void ak_test_enc_cb(int32_t detents)
{
    s_test_enc_count += detents;
    /* EC11 tunes the current effect: SOLID = hue, others = speed (clamped). */
    if (s_test_fx == 0) {
        s_test_fx_param[0] += detents;
    } else {
        int32_t v = s_test_fx_param[s_test_fx] + detents;
        if (v < AK_TEST_FX_SPEED_MIN) v = AK_TEST_FX_SPEED_MIN;
        if (v > AK_TEST_FX_SPEED_MAX) v = AK_TEST_FX_SPEED_MAX;
        s_test_fx_param[s_test_fx] = v;
    }
    ak_test_draw_enc();
    ak_test_leds_render();
}

/* Blocking, self-contained test page. Entered from Settings -> "Test hardware".
 * Owns its own poll/redraw loop so key latency is not starved by the main
 * loop's full-screen redraws. The dedicated IoT thread keeps the cloud link
 * alive while this runs; UART0 is drained each tick so its RX buffer never
 * overflows and the UART block shows live byte counts. Leaves on an EC11
 * DOUBLE-CLICK (single click/long-press stay testable) and restores settings.
 */
static void ak_test_run(void)
{
    s_test_exit_req     = false;
    s_test_key_mask     = 0U;
    s_test_enc_count    = 0;
    s_test_ws_phase     = 0U;
    s_test_uart_rx      = 0U;
    s_test_fx           = 0;            /* start on SOLID */
    s_test_fx_param[0]  = 0;            /* SOLID hue */
    for (int fi = 1; fi < AK_TEST_FX_COUNT; fi++) {
        s_test_fx_param[fi] = 5;       /* mid speed */
    }
    s_settings_active   = false;
    ak_dclick_init(&s_test_enc_dclick);

    ak_test_draw_frame();
    ak_test_draw_slow();
    ak_test_leds_render();

    uint32_t last_tick = tal_system_get_millisecond();
    uint32_t last_fx_ms = last_tick;

    while (!s_test_exit_req) {
        uint32_t now = tal_system_get_millisecond();

        /* Tight input polling -> keys feel immediate. */
        ak_input_poll(now, ak_test_input_cb, ak_test_enc_cb);

        /* Keep draining UART0 so its buffer never overflows; count the bytes. */
        uint8_t rx[64];
        int     got = ak_transport_read(rx, sizeof(rx));
        if (got > 0) {
            s_test_uart_rx += (uint32_t)got;
#if AK_TRANSPORT_UART || AK_TRANSPORT_DUAL
            /* Companion pings keep arriving; refresh the USB liveness stamp so
             * the main loop does not falsely time the link out on exit. */
            if (ak_transport_rx_is_usb()) {
                s_usb_last_rx_ms = now;
            }
#endif
        }

        /* Periodic: repaint the slow status rows (120ms). */
        if ((uint32_t)(now - last_tick) >= AK_TEST_TICK_MS) {
            last_tick = now;
            ak_test_draw_slow();
        }

        /* Advance the LED effect animation at the effect's own frame rate. */
        if ((uint32_t)(now - last_fx_ms) >= ak_test_fx_frame_ms()) {
            last_fx_ms = now;
            s_test_ws_phase++;
            ak_test_leds_render();
        }

        tal_system_sleep(POLL_INTERVAL_MS);
    }

    /* Clean up and return to the settings page the user came from. */
    if (ak_leds_available()) {
        ak_leds_clear();
        (void)ak_leds_show();
    }
    s_test_active        = false;
    s_settings_active    = true;
    s_settings_confirmed = false;
    ak_settings_draw();
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
    char     line[48];

    bool usb_connected = false;
    bool ble_connected = false;
#if AK_TRANSPORT_UART || AK_TRANSPORT_DUAL
    usb_connected = s_pc_connected;
#endif
#if AK_TRANSPORT_BLE_GATT || AK_TRANSPORT_DUAL
#if AK_TRANSPORT_DUAL
    ble_connected = s_ak_ble_active &&
                    (ak_ble_gatt_link_state() != AK_BLE_LINK_ADVERTISING);
#else
    ble_connected = (ak_ble_gatt_link_state() != AK_BLE_LINK_ADVERTISING);
#endif
#endif

    bool companion_connected = usb_connected || ble_connected;
    uint16_t header_color = companion_connected ? LCD_HEADER_CONNECTED_BG
                                                : LCD_HEADER_DISCONNECTED_BG;

    /* Header contract: fixed title + firmware version + WiFi/USB/BLE icons.
     * WiFi only updates its icon. Background is pale yellow while neither PC
     * transport is connected and light green after USB or BLE connects. */
    ak_statuslcd_header("Ki-board", header_color,
                        s_wifi_connected, usb_connected, ble_connected,
                        ak_battery_percent(), ak_battery_available());
    /* Title "Ki-board" ends at x=70; the thin version sits just after it, well
     * left of the shifted WiFi icon (x=214) and the battery gauge (x>=282). */
    ak_gfx_text_thin(76U, 7U, "v" APP_FW_VERSION,
                     AK_COL_BLACK, header_color, 1);
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

/* ---- Post-ASR pending-confirm (firmware-led) -----------------------------
 * Enter after a successful recognition; LED4 (K4=cancel) breathes red and LED6
 * (K6=confirm) blinks green to cue the gestures. ak_pending_confirm_tick drives
 * the animation; exit clears both LEDs. The lit LEDs are indices 3 (K4) and 5
 * (K6) per the LEDn<->Kn mapping (§0.5). */
#define AK_PC_LED_K4 3U /* LED4 under K4 */
#define AK_PC_LED_K6 5U /* LED6 under K6 */

static void ak_pending_confirm_enter(uint32_t now_ms)
{
    s_pending_confirm          = true;
    s_pending_confirm_since_ms = now_ms;
}

static void ak_pending_confirm_exit(void)
{
    if (!s_pending_confirm) {
        return;
    }
    s_pending_confirm = false;
    if (ak_leds_available()) {
        ak_leds_set(AK_PC_LED_K4, 0, 0, 0);
        ak_leds_set(AK_PC_LED_K6, 0, 0, 0);
        (void)ak_leds_show();
    }
}

/* Animate the two hint LEDs while in pending-confirm: K4 red breathing (long =
 * cancel-all), K6 green single blink (click = confirm). Also times the state
 * out. Called from the main loop. */
static void ak_pending_confirm_tick(uint32_t now_ms)
{
    if (!s_pending_confirm) {
        return;
    }
    if ((uint32_t)(now_ms - s_pending_confirm_since_ms) >= PENDING_CONFIRM_TIMEOUT_MS) {
        ak_pending_confirm_exit();
        return;
    }
    if (!ak_leds_available()) {
        return;
    }
    uint32_t t = now_ms - s_pending_confirm_since_ms;
    /* K4 red breathing: triangle 0..48..0 over ~1.6s. */
    uint32_t bp    = t % 1600U;
    uint32_t level = bp < 800U ? (bp * 48U / 800U) : ((1600U - bp) * 48U / 800U);
    ak_leds_set(AK_PC_LED_K4, (uint8_t)level, 0, 0);
    /* K6 green single blink: ~150ms on at the start of each 1000ms period. */
    bool on = (t % 1000U) < 150U;
    ak_leds_set(AK_PC_LED_K6, 0, on ? 48U : 0U, 0);
    (void)ak_leds_show();
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
            ak_status_bar_set("Confirm: K6 / Cancel: K4", AK_COL_GREEN);
            /* Text is now in the PC composer; enter pending-confirm so K4/K6
             * become edit keys and the hint LEDs light. A new result arriving
             * while a previous one is still pending (user re-dictated without
             * confirming) cleanly supersedes it: exit the old state first so the
             * hint LEDs and animation phase reset rather than carrying over. */
            if (s_pending_confirm) {
                ak_pending_confirm_exit();
            }
            ak_pending_confirm_enter(now_ms);
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


/* Keep user-visible transport feedback consistent: every state edge is shown
 * in the LCD status bar and emitted as a UART0 JSONL log frame. Header text is
 * only the persistent link summary, not the event notification. */
static void ak_transport_feedback(const char *text, uint16_t color)
{
    send_log_uart("transport", text);
#if AK_ENABLE_DISPLAY
    ak_status_bar_set(text, color);
    ak_render_status();
#else
    (void)color;
#endif
}
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

#if AK_ENABLE_DISPLAY
        /* Finish the status frame before starting asynchronous MP3 playback.
         * Recovery audio is already emitted by Tuya's AI mode when it connects;
         * do not enqueue the same NETWORK_CONNECTED alert a second time. */
        ak_refresh_link_header();
        if (play_failure_prompt) {
            s_lcd_audio_guard_until_ms =
                tal_system_get_millisecond() + 250U;
            /* Redraw once more after playback to recover from any panel-state
             * disturbance caused by the speaker/audio DMA startup. */
            s_lcd_redraw_pending = true;
        }
#endif
        if (play_failure_prompt) {
            ak_asr_play_availability_prompt(false);
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
        /* Only a greeting received over UART proves the USB companion link is
         * up. In DUAL mode a BLE hello must not light the USB icon. */
        if (ak_transport_rx_is_usb()) {
#if AK_TRANSPORT_DUAL
            /* Companion chooses exactly one PC transport. A USB hello locks
             * this boot to USB; stop only AK-owned advertising. During Tuya
             * provisioning s_ak_ble_active is false, so phone-pairing BLE is
             * intentionally untouched. USB hot-unplug does not auto-switch. */
            if (!s_usb_selected) {
                s_usb_selected = true;
                if (s_ak_ble_active) {
                    ak_ble_gatt_suspend_for_usb();
                    s_ak_ble_active = false;
                }
                PR_NOTICE("dual: Companion selected USB; AK BLE disabled for this boot");
            }
#endif
            if (!s_pc_connected) {
                s_pc_connected = true;
                ak_transport_feedback("Companion connected via USB", AK_COL_GREEN);
                send_system_info_uart();
            }
        }
        /* Respond only after the selected transport is locked, so a USB hello
         * is never mirrored to BLE during the transition. */
        send_hello_ack();
    } else if (strcmp(type, "ping") == 0) {
        send_pong();
    } else if (strcmp(type, "bye") == 0) {
        /* The PC companion is disconnecting gracefully. Plain UART gives no
         * hardware disconnect edge, so this frame is how the board learns the
         * link is going down -> flip the header to "USB Disconnected". */
        if (ak_transport_rx_is_usb() && s_pc_connected) {
            s_pc_connected = false;
            ak_transport_feedback("USB disconnected; reconnect USB", AK_COL_ORANGE);
            send_system_info_uart();
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
    } else if (strcmp(type, "keymap") == 0) {
        /* Plan A: Companion downlinks per-key roles; apply to the live table,
         * cache the result in TAL KV, and ack. Omitted keys keep their current
         * role (partial update); unknown role strings are ignored. */
        uint32_t seq = 0;
        ak_json_uint(line, "seq", &seq);
        static const char *const slot_ids[AK_KEYMAP_SLOTS] = {
            "k1", "k2", "k3", "k4", "k5", "k6", "enc",
        };
        bool changed = false;
        for (int i = 0; i < AK_KEYMAP_SLOTS; i++) {
            char role_str[16] = "";
            if (ak_json_str(line, slot_ids[i], role_str, sizeof(role_str))) {
                ak_key_role_t role;
                if (ak_keymap_role_from_str(role_str, &role)) {
                    if (ak_keymap_apply_role(&s_keymap, slot_ids[i], role)) {
                        changed = true;
                    }
                } else {
                    PR_WARN("keymap: ignoring unknown role '%s' for %s",
                            role_str, slot_ids[i]);
                }
            }
        }
        if (changed) {
            /* Re-init the CHAT keys' gesture classifiers: a role change can
             * switch a slot between edge and single/double detection. */
            for (int gi = 0; gi < AK_KEYMAP_SLOTS; gi++) {
                ak_gesture_init(&s_key_gesture[gi]);
            }
            uint8_t blob[AK_KEYMAP_BLOB_SIZE];
            unsigned n = ak_keymap_to_blob(&s_keymap, blob, sizeof(blob));
            if (n == AK_KEYMAP_BLOB_SIZE) {
                if (tal_kv_set(AK_KEYMAP_KV_KEY, blob, n) == OPRT_OK) {
                    PR_NOTICE("keymap: applied + cached %u-byte blob", n);
                } else {
                    PR_WARN("keymap: KV cache write failed (applied in RAM)");
                }
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
    } else if (strcmp(type, "reset_netcfg") == 0) {
        /* Companion "enter pairing" — clear Wi-Fi/cloud binding and reboot into
         * provisioning. Destructive, so it may only enter through the physically
         * trusted USB/UART transport (same gate as wifi/license). The License
         * (UUID/AuthKey) is preserved; only Wi-Fi + cloud binding are cleared. */
        if (!ak_transport_rx_is_usb()) {
            PR_WARN("reset_netcfg rejected: USB transport required");
            return;
        }
        uint32_t seq = 0;
        ak_json_uint(line, "seq", &seq);
        /* ACK first so the companion sees the request was accepted before the
         * device reboots and the link drops. */
        send_ack(seq);
        OPERATE_RET rr = ak_cloud_enter_pairing();
        if (rr != OPRT_OK) {
            PR_WARN("reset_netcfg: enter pairing failed or unsupported: %d", rr);
        } else {
            PR_NOTICE("reset_netcfg: entering pairing mode (reboot follows)");
        }
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
    PR_NOTICE("Board: expansion baseboard v0.3 (WS2812=P%u/SPI1 x%u, LCD_BL=P%u)",
              (unsigned)AK_WS2812_DATA_PIN, (unsigned)AK_WS2812_COUNT,
              (unsigned)AK_LCD_BACKLIGHT_PIN);
#if AK_TRANSPORT_UART
    PR_NOTICE("Transport: %s (UART%u RX=P%u TX=P%u @%u 8N1)", AK_TRANSPORT_NAME, (unsigned)AK_UART_PORT,
              (unsigned)AK_UART_RX_PIN, (unsigned)AK_UART_TX_PIN, (unsigned)AK_UART_BAUDRATE);
#else
    PR_NOTICE("Transport: %s (BLE GATT peripheral, service 0x%04X)",
              AK_TRANSPORT_NAME, (unsigned)AK_BLE_GATT_SERVICE_UUID);
#endif
    PR_NOTICE("Inputs: 6 keys + EC11 encoder, debounce=%ums long=%ums", (unsigned)AK_FSM_DEBOUNCE_MS,
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
    {
        char *detail = NULL;
        TUYA_RESET_REASON_E reason = tal_system_get_reset_reason(&detail);
        char message[80];
        (void)snprintf(message, sizeof(message), "Boot reset: %s (%d)%s%s",
                       boot_reset_reason_str(reason), (int)reason,
                       detail != NULL ? " - " : "",
                       detail != NULL ? detail : "");
        send_log_uart("boot", message);
        PR_NOTICE("%s", message);
    }
    ak_cloud_set_activation_callback(on_cloud_activation);
    ak_dclick_init(&s_ptt_dclick);
    ak_keymap_init_defaults(&s_keymap);
    /* Plan A: a Companion-provided keymap cached in TAL KV overrides the
     * defaults so a disconnected board keeps the last map. A missing/corrupt
     * blob leaves the v0.3 defaults in place. */
    {
        uint8_t *kv_buf = NULL;
        size_t   kv_len = 0;
        if (tal_kv_get(AK_KEYMAP_KV_KEY, &kv_buf, &kv_len) == OPRT_OK && kv_buf != NULL) {
            if (ak_keymap_from_blob(&s_keymap, kv_buf, (unsigned)kv_len)) {
                PR_NOTICE("keymap: loaded Companion override from KV");
            } else {
                PR_WARN("keymap: KV blob invalid, using v0.3 defaults");
            }
            tal_kv_free(kv_buf);
        }
    }
    for (int gi = 0; gi < AK_KEYMAP_SLOTS; gi++) {
        ak_gesture_init(&s_key_gesture[gi]);
    }
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
    /* Battery fuel gauge (ADC). Optional: on failure the header omits the
     * gauge and everything else keeps working. */
    if (ak_battery_init() != OPRT_OK) {
        PR_WARN("battery ADC init failed; header gauge disabled");
    }
    /* WS2812 chain (used by the on-device test page). Optional. */
    if (ak_leds_init() != OPRT_OK) {
        PR_WARN("WS2812 init failed; test page LED block disabled");
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
    /* The T5 exposes one global BLE callback set. While the Tuya device is
     * unactivated (the SDK's provisioning state), leave BLE with Tuya for the
     * phone app; UART0 remains available to Companion. Once provisioning has
     * completed, start AK BLE so Companion can choose BLE. If Companion chooses
     * USB, its hello frame stops AK advertising and locks this boot to USB.
     * Wi-Fi/MQTT availability is deliberately not part of this decision. */
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
        ak_transport_feedback("Warning: BLE init failed", AK_COL_ORANGE);
        send_system_info_uart();
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
        if (s_activation_ui_pending) {
            char state[sizeof(s_activation_ui_state)];
            (void)snprintf(state, sizeof(state), "%s", s_activation_ui_state);
            s_activation_ui_pending = false;
            if (strcmp(state, "scan_qr") == 0) {
                ak_status_bar_set("Scan QR in Companion", AK_COL_CYAN);
            } else if (strcmp(state, "token_received") == 0) {
                ak_status_bar_set("Activating Tuya device...", AK_COL_CYAN);
            } else if (strcmp(state, "activated") == 0) {
                ak_status_bar_set("Tuya device activated", AK_COL_GREEN);
            } else if (strcmp(state, "cloud_ready") == 0) {
                ak_status_bar_set("Tuya Cloud ready", AK_COL_GREEN);
            } else if (strcmp(state, "cloud_disconnected") == 0) {
                ak_status_bar_set("Tuya Cloud disconnected", AK_COL_ORANGE);
            } else if (strcmp(state, "resetting") == 0) {
                ak_status_bar_set("Resetting network...", AK_COL_ORANGE);
            }
            ak_render_status();
        }
#endif

        /* Pairing is an SDK lifecycle state, not merely an LCD concern. Report
         * each edge to UART0 even in a display-disabled build; the common
         * feedback helper updates the status bar when display is present. */
        {
            static bool s_provisioning_shown = false;
            bool        provisioning = ak_cloud_provisioning();
            if (provisioning != s_provisioning_shown) {
                s_provisioning_shown = provisioning;
                ak_transport_feedback(
                    provisioning ? "Pairing mode: use Tuya app"
                                 : "Provisioning complete",
                    provisioning ? AK_COL_ORANGE : AK_COL_GREEN);
                send_system_info_uart();
            }
        }

#if AK_TRANSPORT_DUAL && AK_ASR_BOARD_CLOUD
        if (!s_ak_ble_active && !s_usb_selected &&
            ak_cloud_network_ready() && ak_cloud_activated() &&
            (s_ak_ble_retry_at_ms == 0U ||
             (int32_t)(now - s_ak_ble_retry_at_ms) >= 0)) {
            OPERATE_RET takeover_rt = ak_ble_gatt_takeover();
            if (takeover_rt == OPRT_OK) {
                s_ak_ble_active = true;
                PR_NOTICE("dual: provisioning complete; AK BLE available for Companion");
            } else {
                s_ak_ble_retry_at_ms = now + 1000U;
                PR_WARN("dual: AK BLE takeover after provisioning failed: %d; retrying in 1s",
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
            /* Report every BLE advertising/connected/live edge through both
             * LCD status bar and UART0 log, then emit the structured snapshot. */
            {
                static ak_ble_link_state_t s_last_link = (ak_ble_link_state_t)-1;
                ak_ble_link_state_t        link = ak_ble_gatt_link_state();
                if (link != s_last_link) {
                    ak_ble_link_state_t previous = s_last_link;
                    s_last_link = link;
                    switch (link) {
                    case AK_BLE_LINK_LIVE:
                        ak_transport_feedback("Companion connected via BLE", AK_COL_GREEN);
                        break;
                    case AK_BLE_LINK_CONNECTED:
                        ak_transport_feedback("BLE connected; subscribing...", AK_COL_DKBLUE);
                        break;
                    case AK_BLE_LINK_ADVERTISING:
                    default:
                        ak_transport_feedback(
                            previous == (ak_ble_link_state_t)-1
                                ? "BLE advertising for Companion"
                                : "BLE disconnected; advertising",
                            AK_COL_ORANGE);
                        break;
                    }
                    send_system_info_uart();
                }
            }
        }
#endif

        ak_input_poll(now, on_input_event, on_encoder_event);

        /* Resolve a pending single-click on the CHAT keys once its double-click
         * window lapses with no second press (scheme X). */
        for (int gi = 0; gi < AK_KEYMAP_SLOTS; gi++) {
            if (s_keymap.entries[gi].gesture != AK_KEY_GESTURE_SINGLE_DOUBLE) {
                continue;
            }
            ak_gesture_t g = ak_gesture_tick(&s_key_gesture[gi], now);
            if (g != AK_GESTURE_NONE) {
                const char *id = ak_keymap_id_for_slot(gi);
                dispatch_key_gesture(id, s_keymap.entries[gi].role, g);
            }
        }

#if AK_ENABLE_DISPLAY
        /* Animate the post-ASR pending-confirm hint LEDs + time the state out. */
        ak_pending_confirm_tick(now);
#endif

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
            ak_transport_feedback("USB timed out; reconnect USB", AK_COL_ORANGE);
            send_system_info_uart();
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
