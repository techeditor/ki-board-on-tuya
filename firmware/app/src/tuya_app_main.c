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
#include "ak_fonttest.h"
#include "ak_i18n.h"
#include "ak_text.h"
#include "ak_asr.h"
#include "ak_cloud.h"
#include "ak_cred.h"
#include "ak_wifi.h"
#include "ak_input.h"
#include "ak_keymap.h"
#include "ak_menu.h"
#include "ak_ui.h"
#include "ak_icons16.h"
#include "ak_session_icons.h"
#include "ak_protocol.h"
#include "ak_provision.h"
#include "ak_reset_netcfg.h"
#include "ak_rx.h"
#include "ak_validate.h"
#include "ak_settings.h"
#include "ak_statuslcd.h"
#include "ak_voice_route.h"
#include "ak_kv.h"
#include "ak_ota_link.h"
#include "tal_api.h"
#include "tal_mutex.h"
#include "tkl_output.h"

#if AK_TRANSPORT_DUAL || AK_TRANSPORT_BLE_GATT
#include "ak_ble_sec.h"
/* BLE uplink goes only through the secure channel (defined below). */
static int ble_secure_write(const void *data, uint32_t length);
#endif
#if AK_TRANSPORT_DUAL
/* DUAL firmware supports UART0 and BLE GATT, while Companion selects exactly
 * one PC transport. Before selection, UART remains available and AK BLE may
 * advertise. A USB hello locks this boot to UART and disables AK BLE; a BLE
 * subscription keeps UART idle. Each link has its own ak_rx line assembler and
 * every line carries its source, so bytes from the two links can never be
 * spliced into one command (FW-01). */
#include "ak_ble_gatt.h"
#include "ak_uart.h"
#define AK_TRANSPORT_NAME "dual"
#define AK_TRANSPORT_HAS_UART 1
#define AK_TRANSPORT_HAS_BLE  1
/* BLE still greets on notify-subscribe; UART greets immediately (both fire). */
#define AK_TRANSPORT_GREET_ON_SUBSCRIBE 1

static bool     s_ak_ble_active       = false;
static bool     s_usb_selected        = false;
#if AK_ASR_BOARD_CLOUD
static uint32_t s_ak_ble_retry_at_ms  = 0U;
#endif

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
        (void)ble_secure_write(data, length);
    }
    return wu;
}

static int ak_transport_read_uart(uint8_t *data, uint32_t length)
{
    return ak_uart_read(data, length);
}

static int ak_transport_read_ble(uint8_t *data, uint32_t length)
{
    return s_ak_ble_active ? ak_ble_gatt_read(data, length) : 0;
}
#elif AK_TRANSPORT_UART
#include "ak_uart.h"
#define ak_transport_init      ak_uart_init
#define ak_transport_write     ak_uart_write
#define ak_transport_read_uart ak_uart_read
#define AK_TRANSPORT_NAME      "uart"
#define AK_TRANSPORT_HAS_UART  1
#define AK_TRANSPORT_HAS_BLE   0
#elif AK_TRANSPORT_BLE_GATT
#include "ak_ble_gatt.h"
#define ak_transport_init               ak_ble_gatt_init
#define ak_transport_write              ble_secure_write
#define ak_transport_read_ble           ak_ble_gatt_read
#define AK_TRANSPORT_NAME               "ble_gatt"
#define AK_TRANSPORT_HAS_UART           0
#define AK_TRANSPORT_HAS_BLE            1
/* BLE greets on notify-subscribe, not at init (see ak_ble_gatt_take_greet). */
#define AK_TRANSPORT_GREET_ON_SUBSCRIBE 1
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
static ak_rx_reader_t s_rx_uart;
static ak_rx_reader_t s_rx_ble;
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
/* Voice mode (ASR / Chat), changed only on the device and kept in KV (Q6). */
#define AK_VOICE_MODE_KV_KEY "ak_vmode"
/* Interface language (FR-01), one byte (ak_lang_to_byte), kept in KV. */
#define AK_LANG_KV_KEY "ak_lang"
static ak_voice_mode_t s_voice_mode = AK_VOICE_MODE_ASR;
/* K5 pressed in Chat mode while the cloud is down: swallow its LONG/UP. */
static bool s_ptt_chat_blocked;
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
/* single/double/long classifier per NORMAL key and the EC11 push, indexed by
 * keymap slot (k1..k6, enc). */
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
/* The post-ASR pending-confirm is state ③ of s_ui (ak_ui times it out). The
 * text stays on the LCD (s_input_text): K4 edits it locally, K6 sends it in
 * the confirm frame, K1 cancels. Whether it also goes into the PC composer is
 * the Companion's choice (direct send by default, or keystroke injection). */
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

/* LCD menu (shared/protocol/protocol.md "LCD 菜单与按键灯光"). While it owns
 * the keys, K1-K6 / EC11 go to ak_menu instead of the keymap. A key whose
 * press the menu took keeps going to the menu until it is released, even if
 * the menu closed meanwhile (s_menu_swallow, bit = keymap slot). */
static ak_menu_t s_menu;
static uint8_t   s_menu_swallow;
/* Work state machine ①..⑦ (ak_ui.h). The post-ASR pending-confirm is ③. */
static ak_ui_t       s_ui;
static uint8_t       s_menu_parked;  /* ESC'd requests the Companion keeps (menu_parked) */
static volatile bool s_asr_for_menu; /* the running ASR answers a menu: never sent as `asr` */
static char          s_dictated[ASR_TRANSCRIPT_MAX]; /* ③ text of a dictated answer */
static char          s_input_text[ASR_TRANSCRIPT_MAX]; /* ③ text of a voice input */
/* Key lights last sent (ak_menu_lights / ak_session_lights). */
static ak_rgb_t  s_lights_shown[AK_LIGHT_COUNT];
static bool      s_lights_valid;
/* Short-lived status bar text (menu_close msg 2 s, notice 3 s). */
static char      s_status_tmp_text[AK_MENU_NOTICE_MAX + 1U];
static uint16_t  s_status_tmp_color;
static uint32_t  s_status_tmp_until_ms;
static bool      s_status_tmp_on;

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
/* Views inside the test page; a short EC11 click steps HW -> Font -> LED
 * power -> HW. Font view: rotation steps the candidate fonts (ak_fonttest.c).
 * LED power view: K1 / K2 switch the chain (P18) on / off by hand. */
typedef enum {
    AK_TEST_VIEW_HW = 0,
    AK_TEST_VIEW_FONT,
    AK_TEST_VIEW_LEDPWR,
    AK_TEST_VIEW_COUNT,
} ak_test_view_t;
static ak_test_view_t s_test_view;
static int      s_test_font_idx;
static bool     s_test_enc_long;      /* current EC11 press already fired LONG */

/* LED power setting (Settings > LED power / LED auto-off), kept in KV. */
#define AK_LED_POLICY_KV_KEY "ak_ledpwr"

static void ak_led_setting_load(void)
{
    uint8_t      *blob = NULL;
    size_t        len  = 0;
    ak_led_mode_t mode = AK_LED_MODE_DEFAULT;
    uint8_t       idx  = AK_LED_TIMEOUT_IDX_DEFAULT;
    if (ak_kv_get(AK_LED_POLICY_KV_KEY, &blob, &len) == OPRT_OK &&
        !ak_led_policy_from_blob(blob, len, &mode, &idx)) {
        PR_WARN("LED power: invalid KV value; using Auto / 60 s");
        mode = AK_LED_MODE_DEFAULT;
        idx  = AK_LED_TIMEOUT_IDX_DEFAULT;
    }
    if (blob != NULL) {
        ak_kv_free(blob);
    }
    s_settings.led_mode        = (uint8_t)mode;
    s_settings.led_timeout_idx = idx;
    ak_leds_set_policy(mode, idx);
    PR_NOTICE("LED power: %s, auto-off %s", ak_led_mode_str(mode), ak_led_policy_timeout_str(idx));
}

/* Settings commit: apply now, keep across reboots. */
static void ak_led_setting_commit(void)
{
    uint8_t blob[AK_LED_POLICY_BLOB_LEN];
    ak_led_mode_t mode = (ak_led_mode_t)s_settings.led_mode;
    ak_leds_set_policy(mode, s_settings.led_timeout_idx);
    ak_led_policy_to_blob(mode, s_settings.led_timeout_idx, blob);
    if (ak_kv_set(AK_LED_POLICY_KV_KEY, blob, sizeof(blob)) != OPRT_OK) {
        PR_WARN("LED power: KV write failed (applied until reboot)");
    }
    PR_NOTICE("LED power -> %s, auto-off %s", ak_led_mode_str(mode),
              ak_led_policy_timeout_str(s_settings.led_timeout_idx));
}
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

#if AK_TRANSPORT_DUAL || AK_TRANSPORT_BLE_GATT
/* ── BLE secure channel (ak_ble_sec.h) ──────────────────────────────────────
 * The session is only touched with the TX lock held: sealing runs inside
 * every (locked) transport write, the handshake runs on the app thread. */
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"

#define AK_BLE_KEY_KV_KEY "ak_ble_key"

static ak_ble_sec_t s_ble_sec;
static uint8_t      s_ble_key[AK_BLE_KEY_LEN];
static bool         s_ble_key_set;
static char         s_ble_tx_line[AK_BLE_CTR_LEN * 2U + 2048U * 2U]; /* sealed 2 KB frame + base64 */

/* CTR_DRBG seeded from mbedTLS entropy, which TuyaOpen feeds from the T5
 * TRNG (tuya_entropy_poll.c -> tal_system_get_random -> bk_rand). App
 * thread only. */
static int ak_secure_random(void *ctx, uint8_t *out, size_t len)
{
    static mbedtls_entropy_context  s_entropy;
    static mbedtls_ctr_drbg_context s_drbg;
    static bool                     s_seeded;
    (void)ctx;
    if (!s_seeded) {
        static const unsigned char pers[] = "ak-ble-pairing";
        mbedtls_entropy_init(&s_entropy);
        mbedtls_ctr_drbg_init(&s_drbg);
        if (mbedtls_ctr_drbg_seed(&s_drbg, mbedtls_entropy_func, &s_entropy, pers, sizeof(pers) - 1U) != 0) {
            PR_ERR("ble sec: random generator seeding failed");
            return -1;
        }
        s_seeded = true;
    }
    return mbedtls_ctr_drbg_random(&s_drbg, out, len) == 0 ? 0 : -1;
}

static int ble_secure_write(const void *data, uint32_t length)
{
    /* Caller holds the TX lock. Unpaired or unauthenticated: send nothing. */
    int n = ak_ble_sec_seal(&s_ble_sec, (const uint8_t *)data, length, s_ble_tx_line, sizeof(s_ble_tx_line));
    if (n <= 0) {
        return 0;
    }
    return ak_ble_gatt_write(s_ble_tx_line, (uint32_t)n) == n ? (int)length : 0;
}

/* Forget the BLE session (link down, new key, cleared pairing). */
static void ble_sec_drop(void)
{
    if (s_ble_sec.state == AK_BLE_SEC_IDLE) {
        return;
    }
    if (transport_lock()) {
        ak_ble_sec_reset(&s_ble_sec);
        transport_unlock();
    }
}

static void ble_key_load(void)
{
    uint8_t *blob = NULL;
    size_t   len  = 0;
    s_ble_key_set = false;
    if (ak_kv_get(AK_BLE_KEY_KV_KEY, &blob, &len) == OPRT_OK) {
        s_ble_key_set = ak_ble_key_from_blob(blob, len, s_ble_key);
        if (!s_ble_key_set) {
            PR_WARN("ble sec: stored pairing key invalid; BLE stays locked");
        }
    }
    if (blob != NULL) {
        mbedtls_platform_zeroize(blob, len);
        ak_kv_free(blob);
    }
    PR_NOTICE("ble sec: %s", s_ble_key_set ? "paired" : "not paired; BLE Companion link disabled");
}

/* Device-local: clear the pairing (Settings > Credentials). */
static void ble_key_clear(void)
{
    (void)ak_kv_del(AK_BLE_KEY_KV_KEY);
    mbedtls_platform_zeroize(s_ble_key, sizeof(s_ble_key));
    s_ble_key_set = false;
    if (transport_lock()) {
        ak_ble_sec_reset(&s_ble_sec);
        transport_unlock();
    }
    PR_NOTICE("ble sec: pairing cleared on the device");
}
#endif

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

/* Pairing window (config-ui-design §4). Owned by the app thread; the cloud
 * callback only reads s_prov_open (to decide whether a QR URL may leave the
 * device) and raises s_prov_cloud_ready_pending for the app loop. */
static ak_provision_t s_prov;
static volatile bool  s_prov_open;
static volatile bool  s_prov_cloud_ready_pending;
static uint32_t       s_prov_shown_s;
static uint32_t       s_prov_reset_seq; /* seq of the reset_netcfg waiting for K6 */
static bool           s_cred_page;      /* Settings > Credentials is showing */
#if AK_TRANSPORT_DUAL || AK_TRANSPORT_BLE_GATT
static bool           s_cred_unpair_armed; /* K1 pressed once on Credentials */
#endif

static void send_prov_status_uart(const char *rejected, const char *reason)
{
#if AK_TRANSPORT_UART || AK_TRANSPORT_DUAL
    char     frame[FRAME_BUFFER_SIZE];
    uint32_t now = tal_system_get_millisecond();
    if (!transport_lock()) {
        return;
    }
    uint32_t sequence = ++s_sequence;
    int      length   = ak_proto_prov_status(frame, sizeof(frame), sequence, ak_provision_state_str(s_prov.state),
                                             ak_provision_remaining_s(&s_prov, now), rejected, reason);
    if (length > 0 && length < (int)sizeof(frame)) {
        int written = ak_uart_write(frame, (uint32_t)length);
        if (written != length) {
            PR_WARN("UART0 prov status short write: %d/%d", written, length);
        }
    } else {
        PR_WARN("UART0 prov status frame too large: %d", length);
    }
    transport_unlock();
#else
    (void)rejected;
    (void)reason;
#endif
}

/* The binding QR URL contains the UUID, so it only leaves the device while
 * the pairing window is open (Q4). */
static const char *ak_prov_qr_url(const char *qr_url)
{
    return (s_prov_open && qr_url != NULL) ? qr_url : "";
}

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
    if (state != NULL && strcmp(state, "cloud_ready") == 0) {
        s_prov_cloud_ready_pending = true;
    }
    send_activation_status_uart(s_activation_ui_state, ak_prov_qr_url(qr_url));
}
/* Explicit development diagnostic, compiled only into debug profiles
 * (CONFIG_AK_LICENSE_DEBUG_READ=y). Keep the reply on the physically trusted
 * UART0 path even in DUAL builds; never mirror License metadata to BLE. */
#if AK_ENABLE_LICENSE_DEBUG_READ
static void send_license_info_uart(void)
{
#if AK_TRANSPORT_UART || AK_TRANSPORT_DUAL
    ak_license_debug_info_t info;
    (void)ak_license_debug_read(&info);

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
#endif

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
static void ak_language_apply(ak_lang_t lang, bool persist);
#if AK_ENABLE_DISPLAY
static void send_device_settings(void);
static void ak_voice_ui_set_state(ak_voice_ui_state_t state);
static void ak_voice_ui_queue_result(const char *text, bool is_error);
static void ak_voice_ui_tick(uint32_t now_ms);
static void ak_pending_confirm_enter(uint32_t now_ms);
static void ak_pending_confirm_exit(void);
static void ak_pending_confirm_tick(uint32_t now_ms);
static void ak_status_bar_set(const char *text, uint16_t color);
static void dispatch_gesture_for_slot(const char *id, ak_gesture_t g);
static void ak_select_session_key(const char *id);
static void ak_render_status(void);
static void ak_settings_draw(void);
static void ak_chat_scroll(int32_t detents);
static void ak_voice_mode_apply(ak_voice_mode_t mode, bool persist);
static void ak_prov_changed(void);
static void ak_prov_confirm_dialog(void);
static void ak_prov_reset_resolve(bool confirm);
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

/* Report one resolved gesture on a NORMAL key (or the EC11 push) as a key
 * frame: action = "single"/"double"/"long", plus bind = the resolved action
 * name when the gesture is bound to something that ran ("chat2",
 * "instance1", "cancel_char", "settings", ...). Exactly one frame per
 * physical operation; see shared/protocol/protocol.md "key". */
static void dispatch_gesture_frame(const char *id, ak_gesture_t g, const char *bind)
{
    char     frame[FRAME_BUFFER_SIZE];
    uint32_t sequence;
    if (!transport_begin_sequenced(&sequence)) {
        return;
    }
    transport_end_sequenced(frame, ak_proto_key_gesture(frame, sizeof(frame), sequence, id,
                                                        ak_gesture_str(g), bind));
    PR_DEBUG("gesture %s %s bind=%s", id, ak_gesture_str(g), bind != NULL ? bind : "-");
}

#if AK_ENABLE_DISPLAY
/* ───────────────────────── LCD menu glue ─────────────────────────
 * ak_ui (pure) is the work state machine, ak_menu (pure) the menu; this
 * sends their frames, keeps the menu's on-screen flag in step with the
 * state, drives the dictated answer and repaints. */

static void ak_gestures_reset(void)
{
    for (int gi = 0; gi < AK_KEYMAP_SLOTS; gi++) {
        ak_gesture_init(&s_key_gesture[gi]);
    }
}

/* ③ for the post-ASR text in the PC composer (not a dictated answer). */
static bool ak_input_confirm(void)
{
    return ak_ui_state(&s_ui) == AK_UI_CONFIRM && !s_ui.dictating;
}

static void ak_status_tmp_set(const char *text, uint16_t color, uint32_t span_ms)
{
    (void)snprintf(s_status_tmp_text, sizeof(s_status_tmp_text), "%s", text);
    ak_text_utf8_trim(s_status_tmp_text);
    s_status_tmp_color    = color;
    s_status_tmp_until_ms = tal_system_get_millisecond() + span_ms;
    s_status_tmp_on       = true;
}

/* Put the menu on / off screen to match the state machine. A menu that
 * (re)appears wakes Auto LED power and restarts the 300 ms guard. */
static void ak_ui_sync(uint32_t now)
{
    bool settings = s_settings_active || s_test_active;
    bool system   = ak_provision_reset_pending(&s_prov, now) || s_prov.state == AK_PROV_CONFIRMING;
#if AK_TRANSPORT_HAS_UART
    system = system || ak_ota_link_busy(); /* ⑦ firmware update: confirm, then progress */
#endif
    bool changed  = ak_ui_set_modals(&s_ui, settings, system);
    changed |= ak_ui_pump(&s_ui);
    if (s_menu.state == AK_MENU_OPEN) {
        bool hide = !ak_ui_menu_visible(&s_ui);
        if (s_menu.hidden != hide) {
            ak_menu_set_hidden(&s_menu, hide, now);
            if (!hide) {
                ak_leds_activity();
                ak_gestures_reset(); /* no half-classified gesture may fire under it */
            }
            changed = true;
        }
    }
    if (changed && !settings) {
        s_lcd_redraw_pending = true;
    }
}

static void ak_menu_handle(const ak_menu_out_t *out)
{
    static char frame[FRAME_BUFFER_SIZE + ASR_TRANSCRIPT_MAX * 6U]; /* 48 indexes, or a fully escaped answer; app thread only */
    uint32_t sequence;
    uint32_t now = tal_system_get_millisecond();

    switch (out->kind) {
    case AK_MENU_OUT_RESULT:
    case AK_MENU_OUT_DISMISS:
        if (transport_begin_sequenced(&sequence)) {
            int length = out->kind == AK_MENU_OUT_RESULT
                             ? ak_proto_menu_result(frame, sizeof(frame), sequence, s_menu.id, out->selected, out->n,
                                                    out->text)
                             : ak_proto_menu_dismissed(frame, sizeof(frame), sequence, s_menu.id, out->reason);
            transport_end_sequenced(frame, length);
        }
        PR_NOTICE("menu %s: %s n=%u", s_menu.id, out->kind == AK_MENU_OUT_RESULT ? "result" : out->reason,
                  (unsigned)out->n);
        ak_ui_menu_gone(&s_ui);
        break;
    case AK_MENU_OUT_DICTATE:
        /* The answer is the board's own cloud ASR text (ASR mode); in any
         * other route there is no text to send. */
        if (s_voice_route.effective == AK_VOICE_CLOUD && s_voice_mode == AK_VOICE_MODE_ASR && ak_asr_available()) {
            s_dictated[0] = '\0';
            ak_ui_dictate(&s_ui);
            ak_status_bar_set(ak_tr(AK_STR_ST_HOLD_TO_SPEAK), AK_COL_PURPLE);
        } else {
            ak_menu_dictate_cancel(&s_menu);
            ak_status_tmp_set(ak_tr(AK_STR_ST_DICTATION_NEEDS_CLOUD), AK_COL_YELLOW, AK_MENU_NOTICE_MS);
        }
        break;
    default:
        break;
    }
    ak_ui_sync(now);
    if (out->redraw || out->kind != AK_MENU_OUT_NONE) {
        ak_render_status();
    }
}

/* K1-K6 / EC11 push while a menu owns the keys (or still owns a key it saw
 * pressed). Menu keys act on press-and-release, without gesture
 * classification; nothing reaches the PC as a key frame. */
static bool ak_menu_route_key(const char *id, ak_input_event_t event)
{
    int slot = ak_keymap_slot_for_id(id);
    if (slot < 0) {
        return false;
    }
    uint8_t bit  = (uint8_t)(1U << slot);
    bool    owns = ak_menu_owns_keys(&s_menu);
    if (!owns && (s_menu_swallow & bit) == 0U) {
        return false;
    }
    ak_menu_out_t out;
    uint32_t      now = tal_system_get_millisecond();
    if (event == AK_INPUT_EVENT_DOWN) {
        s_menu_swallow |= bit;
        ak_menu_key(&s_menu, slot, true, now, &out);
        ak_menu_handle(&out);
    } else if (event == AK_INPUT_EVENT_UP) {
        s_menu_swallow &= (uint8_t)~bit;
        ak_menu_key(&s_menu, slot, false, now, &out);
        ak_menu_handle(&out);
    }
    return true;
}

/* ②/③ of a dictated answer: the voice key talks, K1 returns to the menu,
 * K6 (once there is text) sends it. Every other key does nothing and
 * nothing reaches the PC. Returns true when the key was handled here. */
static bool ak_dictation_route_key(const char *id, ak_input_event_t event)
{
    if (!s_ui.dictating || ak_keymap_is_voice(&s_keymap, id)) {
        return false; /* the voice key runs the normal PTT path */
    }
    if (event != AK_INPUT_EVENT_DOWN) {
        return true;
    }
    uint32_t now = tal_system_get_millisecond();
    if (strcmp(id, "k1") == 0) {
        s_asr_for_menu = false;
        ak_menu_dictate_cancel(&s_menu);
        ak_ui_dictate_back(&s_ui);
        ak_status_bar_set(ak_tr(AK_STR_ST_READY), AK_COL_GREEN);
        ak_ui_sync(now);
        ak_render_status();
    } else if (strcmp(id, "k6") == 0 && ak_ui_state(&s_ui) == AK_UI_CONFIRM) {
        ak_menu_out_t out;
        ak_menu_dictated(&s_menu, s_dictated, now, &out);
        if (out.kind == AK_MENU_OUT_NONE) {
            ak_ui_dictate_back(&s_ui); /* the menu went away meanwhile */
        }
        ak_menu_handle(&out);
    }
    return true;
}

/* Called for every inbound menu / menu_close / notice / menu_parked frame. */
static void ak_menu_on_frame(const char *type, const char *payload)
{
    uint32_t now = tal_system_get_millisecond();
    if (strcmp(type, "menu") == 0) {
        static ak_menu_page_t page;
        if (!ak_menu_parse(payload, &page)) {
            PR_WARN("menu rejected: malformed");
            return;
        }
        ak_menu_apply_t applied = ak_menu_on_page(&s_menu, &page, now);
        if (applied == AK_MENU_APPLY_REJECTED) {
            PR_WARN("menu %s page %u rejected: inconsistent with the open menu", page.id, (unsigned)page.page);
            return;
        }
        if (applied == AK_MENU_APPLY_OPENED) {
            if (s_ui.dictating) {
                s_asr_for_menu = false; /* the menu being dictated was replaced */
            }
            ak_ui_menu_arrived(&s_ui, ak_menu_is_model(&s_menu));
            /* Hide at once when it must wait (②/③/⑥/⑦), so no key reaches it. */
            ak_menu_set_hidden(&s_menu, !ak_ui_menu_visible(&s_ui), now);
            PR_NOTICE("menu %s opened (%u page(s)) state=%s", s_menu.id, (unsigned)s_menu.pages,
                      ak_ui_state_str(ak_ui_state(&s_ui)));
            ak_leds_activity(); /* a menu wakes Auto LED power (also when it waits: K4) */
            if (ak_ui_menu_visible(&s_ui)) {
                ak_gestures_reset();
            }
        }
        ak_ui_sync(now);
        ak_render_status();
    } else if (strcmp(type, "menu_close") == 0) {
        char id[AK_MENU_ID_MAX + 1U];
        char msg[AK_MENU_CLOSE_MSG_MAX + 1U];
        if (!ak_menu_parse_close(payload, id, msg)) {
            PR_WARN("menu_close rejected: malformed");
            return;
        }
        bool was_open = s_menu.state == AK_MENU_OPEN;
        if (!ak_menu_on_close(&s_menu, id)) {
            return; /* not the current menu */
        }
        if (was_open) {
            if (s_ui.dictating) {
                s_asr_for_menu = false;
            }
            ak_ui_menu_gone(&s_ui);
        }
        if (msg[0] != '\0') {
            ak_status_tmp_set(msg, AK_COL_CYAN, AK_MENU_CLOSE_MSG_MS);
        }
        ak_ui_sync(now);
        ak_render_status();
    } else if (strcmp(type, "menu_parked") == 0) {
        uint8_t count = 0U;
        if (!ak_menu_parse_parked(payload, &count)) {
            PR_WARN("menu_parked rejected: malformed");
            return;
        }
        if (count > s_menu_parked) {
            ak_leds_activity(); /* K4 starts breathing */
        }
        s_menu_parked = count;
    } else { /* notice */
        char              text[AK_MENU_NOTICE_MAX + 1U];
        ak_notice_level_t level;
        if (!ak_menu_parse_notice(payload, text, &level)) {
            PR_WARN("notice rejected: malformed");
            return;
        }
        ak_status_tmp_set(text,
                          level == AK_NOTICE_ERROR  ? AK_COL_RED
                          : level == AK_NOTICE_WARN ? AK_COL_YELLOW
                                                    : AK_COL_CYAN,
                          AK_MENU_NOTICE_MS);
        ak_render_status();
    }
}
#endif /* AK_ENABLE_DISPLAY */

/* Input event -> local settings UI or JSONL frame on the active transport.
 * v0.3 stage 乙: VOICE keys run the hold-to-talk edge path; every other key is
 * NORMAL and runs the single/double/long classifier, whose resolved gesture
 * selects the bound action from the keymap. */
static void on_input_event(ak_input_kind_t kind, const char *id,
                           ak_input_event_t event, uint32_t held_ms)
{
    /* Auto LED power: any key edge wakes the chain. The event that woke it
     * is still handled normally below (never swallowed). */
    ak_leds_activity();
#if AK_TRANSPORT_HAS_UART
    /* Firmware update is modal. While the device asks, K6 installs and K1
     * cancels (system confirm ⑦, default cancel after 10 s); during the
     * transfer and the debug UART probe every key is ignored. */
    if (ak_ota_link_busy()) {
        if (ak_ota_link_confirming() && kind == AK_INPUT_KIND_KEY && event == AK_INPUT_EVENT_DOWN &&
            (strcmp(id, "k6") == 0 || strcmp(id, "k1") == 0)) {
            ak_ota_link_confirm(strcmp(id, "k6") == 0, tal_system_get_millisecond());
        }
        return;
    }
#endif
    /* The reset confirm dialog is modal (⑦): only K6 (confirm) / K1 (cancel)
     * act, nothing else reaches the settings page or the Companion. */
    if (ak_provision_reset_pending(&s_prov, tal_system_get_millisecond())) {
        if (kind == AK_INPUT_KIND_KEY && event == AK_INPUT_EVENT_DOWN &&
            (strcmp(id, "k6") == 0 || strcmp(id, "k1") == 0)) {
            ak_prov_reset_resolve(strcmp(id, "k6") == 0);
        }
        return;
    }
#if AK_ENABLE_DISPLAY
    if (ak_settings_consume_input(kind, id, event, held_ms)) {
        return;
    }
#endif

    if (kind != AK_INPUT_KIND_KEY) {
        return;
    }

#if AK_ENABLE_DISPLAY
    /* An open LCD menu (④/⑤) takes K1-K6 + EC11 over from the keymap; a
     * dictated answer (②/③ for a menu) keeps them too. */
    if (ak_menu_route_key(id, event) || ak_dictation_route_key(id, event)) {
        return;
    }
#endif

    /* Chat mode: K4 only stops the spoken reply and never reaches the PC
     * (its cancel_char / cancel_all binds would edit the Crew composer). */
    if (s_voice_mode == AK_VOICE_MODE_CHAT && strcmp(id, "k4") == 0) {
        if (event == AK_INPUT_EVENT_DOWN) {
            ak_asr_interrupt();
        }
        return;
    }

    /* VOICE key (hold-to-talk; K5 by default): immediate-edge PTT. Cloud
     * consumes DOWN/LONG/UP as one latched HOLD; system emits DOWN/UP dictation.
     * No gesture classification. */
    if (ak_keymap_is_voice(&s_keymap, id)) {
#if AK_ENABLE_DISPLAY
        bool dictating = s_ui.dictating;
#else
        bool dictating = false;
#endif
        if (s_voice_mode != AK_VOICE_MODE_CHAT && !dictating) {
            ak_emit_input_frame(kind, id, event, held_ms); /* a dictated answer never reaches the PC */
        }
        if (event == AK_INPUT_EVENT_DOWN || event == AK_INPUT_EVENT_LONG ||
            event == AK_INPUT_EVENT_UP) {
            if (ak_dclick_feed(&s_ptt_dclick, event, tal_system_get_millisecond(), held_ms) &&
                s_voice_route.effective == AK_VOICE_CLOUD && ak_asr_available()) {
                ak_asr_play_prompt();
            }
            on_ptt_event(event);
        }
        return;
    }

    /* NORMAL key: feed the single/double/long classifier. The resolved gesture
     * picks the bound action; raw down/up are NOT forwarded. */
    int slot = ak_keymap_slot_for_id(id);
    if (slot < 0) {
        return;
    }
    ak_gesture_t g = ak_gesture_feed(&s_key_gesture[slot], event,
                                     tal_system_get_millisecond(), held_ms);
    dispatch_gesture_for_slot(id, g);
}

/* Resolve a classified gesture on a NORMAL key (or the EC11 push) to its bound
 * action, run whatever is local, and report the gesture with its bind.
 * Called from on_input_event and the main-loop gesture tick (a lone single
 * resolves after the double window, an owed long one tick after its single). */
static void dispatch_gesture_for_slot(const char *id, ak_gesture_t g)
{
    ak_gesture_slot_t gslot;
    switch (g) {
    case AK_GESTURE_SINGLE: gslot = AK_GESTURE_SLOT_SINGLE; break;
    case AK_GESTURE_DOUBLE: gslot = AK_GESTURE_SLOT_DOUBLE; break;
    case AK_GESTURE_LONG:   gslot = AK_GESTURE_SLOT_LONG;   break;
    default:                return; /* NONE: nothing resolved yet */
    }

#if AK_ENABLE_DISPLAY
    /* Settings is modal: a gesture that resolves after it opened (a pending
     * single timing out) must not reach the PC. Same for an open menu or a
     * dictated answer. */
    if (s_settings_active || ak_menu_owns_keys(&s_menu) || s_ui.dictating) {
        return;
    }
    /* EC11 long-press opens settings locally (v0.3 interaction §0.1). The
     * release that follows is swallowed by the settings handler. */
    if (strcmp(id, AK_ENC_SW_ID) == 0 && g == AK_GESTURE_LONG) {
        s_center_long_consumed = true;
        ak_pending_confirm_exit(); /* settings ends ② / ③ */
        s_settings_active    = true;
        s_settings_confirmed = false;
        ak_settings_draw();
        dispatch_gesture_frame(id, g, "settings");
        return;
    }
    /* ①: K4 single brings back the requests ESC took off the screen. */
    if (strcmp(id, "k4") == 0 && g == AK_GESTURE_SINGLE && ak_ui_k4_recalls(&s_ui, s_menu_parked)) {
        dispatch_gesture_frame(id, g, "menu_recall");
        return;
    }
    /* Chat mode: the EC11 click stays local, so it cannot become an Enter
     * that submits the Crew composer. */
    if (s_voice_mode == AK_VOICE_MODE_CHAT && strcmp(id, AK_ENC_SW_ID) == 0) {
        return;
    }
    /* ③: K1 cancels the input (reported as cancel_all so an injecting
     * Companion clears the composer too); its double / long do nothing. */
    if (ak_input_confirm() && strcmp(id, "k1") == 0) {
        if (g == AK_GESTURE_SINGLE) {
            dispatch_gesture_frame(id, g, "cancel_all");
            s_input_text[0] = '\0';
            ak_pending_confirm_exit();
            ak_status_bar_set(ak_tr(AK_STR_ST_INPUT_CANCELLED), AK_COL_ORANGE);
            ak_render_status();
        }
        return;
    }
#endif

    ak_action_t action = ak_keymap_action(&s_keymap, id, gslot);
    char        bind[16];
    uint8_t     n;

    if ((n = ak_action_chat_n(action)) != 0u) {
        snprintf(bind, sizeof(bind), "chat%u", (unsigned)n);
#if AK_ENABLE_DISPLAY
        if (g == AK_GESTURE_SINGLE) {
            ak_select_session_key(id); /* local A/B/C highlight */
        }
#endif
        dispatch_gesture_frame(id, g, bind);
        return;
    }
    if ((n = ak_action_instance_n(action)) != 0u) {
        snprintf(bind, sizeof(bind), "instance%u", (unsigned)n);
        dispatch_gesture_frame(id, g, bind);
        return;
    }
    /* Companion-run actions (K6 long / double by default). */
    if (action == AK_ACTION_MODEL_MENU || action == AK_ACTION_MODEL_TOGGLE) {
        dispatch_gesture_frame(id, g, ak_keymap_action_str(action));
        return;
    }

#if AK_ENABLE_DISPLAY
    /* Pending-confirm (post-ASR) edits are firmware-owned (design §0.2): they
     * only run, and only carry a bind, while the board awaits confirmation. */
    if (ak_input_confirm() &&
        (action == AK_ACTION_CANCEL_CHAR || action == AK_ACTION_CANCEL_ALL ||
         action == AK_ACTION_CONFIRM)) {
        if (action == AK_ACTION_CANCEL_CHAR) {
            dispatch_gesture_frame(id, g, "cancel_char");
            (void)ak_utf8_pop_last(s_input_text);
            s_ui.confirm_ms = tal_system_get_millisecond(); /* editing restarts the 20 s */
            if (s_input_text[0] == '\0') {
                ak_pending_confirm_exit();
                ak_status_bar_set(ak_tr(AK_STR_ST_INPUT_CLEARED), AK_COL_ORANGE);
            }
        } else if (action == AK_ACTION_CANCEL_ALL) {
            dispatch_gesture_frame(id, g, "cancel_all");
            s_input_text[0] = '\0';
            ak_pending_confirm_exit();
            ak_status_bar_set(ak_tr(AK_STR_ST_INPUT_CANCELLED), AK_COL_ORANGE);
        } else { /* CONFIRM: the text rides along for a direct send */
            uint32_t sequence;
            if (transport_begin_sequenced(&sequence)) {
                transport_end_sequenced(s_asr_tx_frame,
                                        ak_proto_key_gesture_text(s_asr_tx_frame, sizeof(s_asr_tx_frame), sequence,
                                                                  id, ak_gesture_str(g), "confirm", s_input_text));
            }
            s_input_text[0] = '\0';
            ak_pending_confirm_exit();
            ak_status_bar_set(ak_tr(AK_STR_ST_INPUT_CONFIRMED), AK_COL_GREEN);
        }
        ak_render_status();
        return;
    }
#endif

    /* Unbound, or a pending-confirm edit outside pending-confirm: report the
     * bare gesture so the Companion can apply its default rule. */
    dispatch_gesture_frame(id, g, NULL);
}

/* EC11 rotation -> test page / settings UI when modal, else an encoder frame. */
static void on_encoder_event(int32_t detents)
{
    /* Auto LED power: wake the chain; the rotation is still handled below. */
    ak_leds_activity();
#if AK_TRANSPORT_HAS_UART
    if (ak_ota_link_busy()) {
        return;
    }
#endif
    if (ak_provision_reset_pending(&s_prov, tal_system_get_millisecond())) {
        return;
    }
#if AK_ENABLE_DISPLAY
    if (ak_settings_consume_encoder(detents)) {
        return;
    }
    if (ak_menu_owns_keys(&s_menu)) {
        ak_menu_out_t out;
        ak_menu_encoder(&s_menu, detents, tal_system_get_millisecond(), &out);
        ak_menu_handle(&out);
        return;
    }
    if (s_ui.dictating) {
        return; /* a dictated answer: nothing reaches the PC */
    }
    if (s_voice_mode == AK_VOICE_MODE_CHAT) {
        ak_chat_scroll(detents); /* scroll the Chat page; nothing goes to the PC */
        return;
    }
#endif
    ak_emit_encoder_frame(detents);
}

static void send_hello_ack(void)
{
    char frame[FRAME_BUFFER_SIZE];
    int  length = ak_proto_hello_ack_caps(frame, sizeof(frame), APP_FW_VERSION, AK_TRANSPORT_NAME, AK_ENABLE_DISPLAY);
    transport_write_frame(frame, length);
    /* Follow the greeting with board-owned runtime state so Companion controls
     * reflect the AK immediately after connecting or reloading the link. */
    send_voice_source();
#if AK_ENABLE_DISPLAY
    send_device_settings();
#endif
    send_system_info_uart();
    send_activation_status_uart(ak_cloud_activation_state(),
                                ak_prov_qr_url(ak_cloud_binding_qr_url()));
    send_prov_status_uart(NULL, NULL);
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
    transport_end_sequenced(frame, ak_proto_voice_source(frame, sizeof(frame), sequence,
                                                         ak_voice_source_str(s_voice_route.effective),
                                                         ak_voice_mode_str(s_voice_mode)));
}

/* Chat turn -> Companion, display only (CP-01). UART0 only: an unencrypted
 * BLE link must not carry the conversation. */
static void send_tuya_chat_uart(const char *role, const char *text)
{
#if AK_TRANSPORT_UART || AK_TRANSPORT_DUAL
    static char frame[AK_CHAT_REPLY_MAX * 2U + 128U];
    if (!transport_lock()) {
        return;
    }
    uint32_t sequence = ++s_sequence;
    int      length   = ak_proto_tuya_chat(frame, sizeof(frame), sequence, role, text, true);
    if (length > 0 && length < (int)sizeof(frame)) {
        int written = ak_uart_write(frame, (uint32_t)length);
        if (written != length) {
            PR_WARN("UART0 tuya_chat short write: %d/%d", written, length);
        }
    } else {
        PR_WARN("UART0 tuya_chat frame too large: %d", length);
    }
    transport_unlock();
#else
    (void)role;
    (void)text;
#endif
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
        frame, ak_proto_device_settings(frame, sizeof(frame), sequence, s_settings.volume, s_settings.brightness,
                                        ak_led_mode_wire_str((ak_led_mode_t)s_settings.led_mode),
                                        ak_led_policy_timeout_ms(s_settings.led_timeout_idx) / 1000U,
                                        ak_lang_wire_str(ak_lang_get())));
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
    if (s_asr_for_menu) {
        return; /* a dictated menu answer: confirmed on the keyboard, sent in menu_result */
    }
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
    if (s_asr_for_menu) {
        return;
    }
#endif
    if (s_voice_mode == AK_VOICE_MODE_CHAT) {
        return; /* Chat mode sends no asr frames at all */
    }
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
    case AK_WIFI_RESTORED:
        log_message = "WiFi failed, restored previous network";
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
        ak_ptt_action_t act = ak_voice_mode_ptt_action(s_voice_mode, ak_asr_available(),
                                                       s_voice_route.effective == AK_VOICE_CLOUD);
        s_ptt_chat_blocked  = (act == AK_PTT_CHAT_UNAVAILABLE);
        s_ptt_cloud_route   = (act == AK_PTT_CLOUD_HOLD);
        s_ptt_cloud_started = false;
#if AK_ENABLE_DISPLAY
        if (s_ui.dictating && !(s_ptt_cloud_route && s_voice_mode == AK_VOICE_MODE_ASR)) {
            /* A dictated answer needs the board's own text; PC dictation would
             * type into whatever window is in front. Ignore this press. */
            s_ptt_chat_blocked = true;
            ak_status_tmp_set(ak_tr(AK_STR_ST_DICTATION_NEEDS_CLOUD), AK_COL_YELLOW, AK_MENU_NOTICE_MS);
            ak_render_status();
            return;
        }
        if (!s_ptt_chat_blocked) {
            s_asr_for_menu = s_ui.dictating;
            (void)ak_ui_voice_start(&s_ui); /* ① / ③ / dictating -> ② */
        }
#endif
        if (s_ptt_chat_blocked) {
            /* Chat never falls back to PC dictation (it would type into Crew). */
#if AK_ENABLE_DISPLAY
            ak_status_bar_set(ak_tr(AK_STR_ST_CHAT_NEEDS_CLOUD), AK_COL_ORANGE);
            ak_render_status();
#endif
            ak_asr_play_availability_prompt(false);
            return;
        }
        if (s_ptt_cloud_route && s_voice_mode == AK_VOICE_MODE_CHAT && ak_asr_audio_busy()) {
            ak_asr_interrupt(); /* talking over the reply stops it */
        }
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
        if (s_ptt_chat_blocked) {
            return;
        }
        if (s_ptt_cloud_route && !s_ptt_cloud_started) {
            ak_asr_start();
            s_ptt_cloud_started = true;
        }
        return;
    }

    if (event == AK_INPUT_EVENT_UP) {
        if (s_ptt_chat_blocked) {
            s_ptt_chat_blocked = false;
            return;
        }
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
                s_asr_for_menu = false;
                ak_ui_voice_end(&s_ui, AK_UI_VOICE_NONE, s_voice_mode == AK_VOICE_MODE_CHAT,
                                tal_system_get_millisecond());
                ak_ui_sync(tal_system_get_millisecond());
#endif
            }
        } else {
            send_voice("stop");
#if AK_ENABLE_DISPLAY
            /* PC dictation: the text goes straight into the PC window. */
            ak_ui_voice_end(&s_ui, AK_UI_VOICE_NONE, s_voice_mode == AK_VOICE_MODE_CHAT,
                            tal_system_get_millisecond());
            ak_ui_sync(tal_system_get_millisecond());
#endif
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
/* Map a Kiro Crew agent/slot state string to a status color.
 *   purple: unbound / default (empty, "-", "unbound")
 *   green : idle
 *   red   : running (busy)
 *   orange: waiting (needs user action)
 *   blue  : info (feedback available, no action needed)
 * Unknown states fall back to unbound (purple). */
static uint16_t ak_status_color(const char *state)
{
    if (state == NULL || state[0] == '\0' || strcmp(state, "-") == 0 ||
        strcmp(state, "unbound") == 0) {
        return AK_COL_PURPLE;
    } else if (strcmp(state, "idle") == 0) {
        return AK_COL_GREEN;
    } else if (strcmp(state, "running") == 0) {
        return AK_COL_RED;
    } else if (strcmp(state, "waiting") == 0) {
        return AK_COL_ORANGE;
    } else if (strcmp(state, "info") == 0) {
        return AK_COL_BLUE;
    }
    return AK_COL_PURPLE;
}

#define LCD_SIDEBAR_W             64U
#define LCD_SESSION_ROWS          AK_CHAT_SLOT_COUNT
#define LCD_SESSION_ROW_H         67U
#define LCD_SESSION_ALIAS_BYTES   16U /* UTF-8: up to 7 ASCII or 4 CJK characters fit the 60 px label */
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
#define LCD_STATUS_TEXT_BYTES   96U
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
    {.agent = "-", .state = ""},
    {.agent = "-", .state = ""},
    {.agent = "-", .state = ""},
};
static char     s_session_labels[LCD_SESSION_ROWS][LCD_SESSION_ALIAS_BYTES] = {"A", "B", "C"};
static uint8_t  s_selected_session;
static char     s_status_bar_text[LCD_STATUS_TEXT_BYTES] = "Starting system...";
static uint16_t s_status_bar_color = AK_COL_CYAN;

static void ak_status_bar_set(const char *text, uint16_t color)
{
    (void)snprintf(s_status_bar_text, sizeof(s_status_bar_text), "%s",
                   text == NULL ? "" : text);
    ak_text_utf8_trim(s_status_bar_text);
    s_status_bar_color = color;
}

static void ak_status_bar_set_wifi(ak_wifi_state_t state)
{
    switch (state) {
    case AK_WIFI_CONNECTING:
        ak_status_bar_set(ak_tr(AK_STR_ST_WIFI_CONNECTING), AK_COL_CYAN);
        break;
    case AK_WIFI_CONNECTED:
        ak_status_bar_set(ak_tr(AK_STR_ST_WIFI_CONNECTED), AK_COL_GREEN);
        break;
    case AK_WIFI_FAILED:
        ak_status_bar_set(ak_tr(AK_STR_ST_WIFI_FAILED), AK_COL_RED);
        break;
    case AK_WIFI_RESTORED:
        ak_status_bar_set(ak_tr(AK_STR_ST_WIFI_RESTORED), AK_COL_ORANGE);
        break;
    case AK_WIFI_DISCONNECTED:
    default:
        ak_status_bar_set(ak_tr(AK_STR_ST_WIFI_DISCONNECTED), AK_COL_ORANGE);
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
    static char window_text[48];
    uint32_t    now_ms = tal_system_get_millisecond();
    if (ak_provision_is_open(&s_prov, now_ms)) {
        uint32_t left = ak_provision_remaining_s(&s_prov, now_ms);
        if (ak_license_present()) {
            (void)snprintf(window_text, sizeof(window_text), ak_tr(AK_STR_ST_PAIR_WINDOW),
                           (unsigned)(left / 60U), (unsigned)(left % 60U));
        } else {
            /* No License yet: the window is the only time USB can write one, so say so. */
            (void)snprintf(window_text, sizeof(window_text), ak_tr(AK_STR_ST_PAIR_WRITE_LICENSE),
                           (unsigned)(left / 60U), (unsigned)(left % 60U));
        }
        text  = window_text;
        color = AK_COL_ORANGE;
    } else if (s_status_tmp_on) {
        text  = s_status_tmp_text; /* menu_close msg / notice, 2-3 s */
        color = s_status_tmp_color;
    } else if (ak_input_confirm() && s_ui.menu_waiting) {
        text  = ak_tr(AK_STR_ST_REQUEST_WAITING); /* ③ keeps K4 for cancel */
        color = AK_COL_YELLOW;
    } else if (ak_cloud_provisioning()) {
        text  = ak_tr(AK_STR_ST_PAIRING_TUYA_APP);
        color = AK_COL_ORANGE;
    } else if (s_voice_fallback_active) {
        text  = ak_tr(AK_STR_ST_ASR_FALLBACK);
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

/* One wrapped LCD line. UTF-8: 232 px holds at most 19 CJK characters (57
 * bytes); longer byte runs (4-byte code points drawn as boxes) are cut. */
#define LCD_CHAT_TEXT_W        (LCD_CHAT_TEXT_COLS * AK_TEXT_ASCII_W)
#define LCD_HISTORY_LINE_BYTES 64U
typedef struct {
    char text[LCD_HISTORY_LINE_BYTES + 1U];
    bool paragraph_start;
    bool from_agent;
} ak_history_line_t;

/* Line advance: 10 px for 8x8 ASCII lines, 14 px for lines drawn with the
 * 12 px UI font (Chinese / mixed). */
static uint16_t ak_history_line_advance(const ak_history_line_t *line)
{
    return ak_text_is_wide(line->text, sizeof(line->text)) ? 14U : LCD_HISTORY_LINE_ADVANCE;
}

static void ak_history_line_push(ak_history_line_t *lines, uint8_t *count, uint8_t capacity,
                                 const char *text, uint16_t length,
                                 bool paragraph_start, bool from_agent)
{
    if (*count == capacity) {
        memmove(&lines[0], &lines[1],
                (size_t)(capacity - 1U) * sizeof(lines[0]));
        (*count)--;
    }

    ak_history_line_t *line = &lines[*count];
    memcpy(line->text, text, length);
    line->text[length]    = '\0';
    line->paragraph_start = paragraph_start;
    line->from_agent      = from_agent;
    (*count)++;
}

/* Wrap one UTF-8 text into `lines` by pixel width (ak_text_wrap: spaces and
 * CJK boundaries; oldest lines drop out once `capacity` is reached). Control
 * characters become spaces. `glyphs` is caller scratch of `glyph_cap` bytes. */
static void ak_history_wrap_into(const char *text, bool from_agent, ak_history_line_t *lines, uint8_t *line_count,
                                 uint8_t capacity, char *glyphs, uint16_t glyph_cap)
{
    uint16_t n = 0U;
    for (const char *s = text; *s != '\0' && n + 1U < glyph_cap; s++) {
        glyphs[n++] = (unsigned char)*s < 0x20U ? ' ' : *s;
    }
    glyphs[n] = '\0';
    ak_text_utf8_trim(glyphs);

    const char *p          = glyphs;
    bool        first_line = true;
    while (*p != '\0') {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        size_t len  = ak_text_wrap(p, LCD_CHAT_TEXT_W);
        size_t take = len;
        if (take > LCD_HISTORY_LINE_BYTES) {
            take = LCD_HISTORY_LINE_BYTES;
            while (take > 0U && ((unsigned char)p[take] & 0xC0U) == 0x80U) {
                take--;
            }
            len = take;
        }
        while (take > 0U && p[take - 1U] == ' ') {
            take--;
        }
        if (take > 0U) {
            ak_history_line_push(lines, line_count, capacity, p, (uint16_t)take, first_line, from_agent);
            first_line = false;
        }
        p += len > 0U ? len : 1U;
    }
}

static void ak_history_wrap_entry(const char *text, bool from_agent, ak_history_line_t *lines, uint8_t *line_count)
{
    char glyphs[LCD_HISTORY_TEXT_BYTES];
    ak_history_wrap_into(text, from_agent, lines, line_count, LCD_HISTORY_VISIBLE_LINES, glyphs,
                         (uint16_t)sizeof(glyphs));
}

static void ak_chat_history_draw(const ak_session_ui_t *session)
{
    static ak_history_line_t lines[LCD_HISTORY_VISIBLE_LINES]; /* 1 KB: kept off the 5 KB task stack */
    uint8_t line_count = 0U;

    for (uint8_t i = 0U; i < session->history_count; i++) {
        ak_history_wrap_entry(session->history[i], session->history_from_agent[i], lines, &line_count);
    }
    if (line_count == 0U) {
        return;
    }

    /* Select the newest suffix that fits above the status bar. Wrapped lines
     * advance 10 px (14 px with CJK); a new history entry adds another 6 px
     * paragraph gap. */
    uint8_t first = (uint8_t)(line_count - 1U);
    uint16_t used_height = ak_text_is_wide(lines[first].text, sizeof(lines[first].text)) ? 12U : 8U;
    while (first > 0U) {
        uint16_t advance = ak_history_line_advance(&lines[first - 1U]);
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
            y = (uint16_t)(y + ak_history_line_advance(&lines[i - 1U]));
            if (lines[i].paragraph_start) {
                y = (uint16_t)(y + LCD_HISTORY_PARAGRAPH_GAP);
            }
        }
        uint16_t bg = lines[i].from_agent ? LCD_AGENT_RESPONSE_BG : AK_COL_BLACK;
        ak_gfx_text_thin(LCD_CHAT_X, y, lines[i].text, AK_COL_WHITE, bg, 1);
    }
}

/* ── Chat page (voice mode Chat, config-ui-design §5.2) ─────────────────── */
#define AK_CHAT_LINES_MAX 64U

static ak_asr_chat_t     s_chat_view;  /* last copy taken from ak_asr */
static ak_history_line_t s_chat_lines[AK_CHAT_LINES_MAX];
static uint8_t           s_chat_line_count;
static uint16_t          s_chat_scroll; /* lines scrolled up from the newest */
static uint32_t          s_chat_sent_turn;
static bool              s_chat_sent_reply;

#define LCD_CHAT_AREA_H ((uint16_t)(LCD_STATUS_BAR_Y - LCD_HISTORY_Y - 4U))

/* First line of the run ending before `end` that fits the Chat area
 * (line heights differ: 10 px ASCII, 14 px CJK). */
static uint8_t ak_chat_first_fitting(uint8_t end)
{
    uint16_t used  = 0U;
    uint8_t  first = end;
    while (first > 0U) {
        uint16_t h = ak_history_line_advance(&s_chat_lines[first - 1U]);
        if ((uint16_t)(used + h) > LCD_CHAT_AREA_H) {
            break;
        }
        used = (uint16_t)(used + h);
        first--;
    }
    return first;
}

/* Largest scroll (lines up from the newest) that still fills the area. */
static uint16_t ak_chat_max_scroll(void)
{
    uint16_t used = 0U;
    uint8_t  n    = 0U;
    while (n < s_chat_line_count) {
        uint16_t h = ak_history_line_advance(&s_chat_lines[n]);
        if ((uint16_t)(used + h) > LCD_CHAT_AREA_H) {
            break;
        }
        used = (uint16_t)(used + h);
        n++;
    }
    return (uint16_t)(s_chat_line_count - n);
}

static void ak_chat_rebuild_lines(void)
{
    static char entry[AK_CHAT_REPLY_MAX + 8U];
    static char glyphs[AK_CHAT_REPLY_MAX + 8U];
    s_chat_line_count = 0U;
    if (s_chat_view.user.len > 0U) {
        (void)snprintf(entry, sizeof(entry), ak_tr(AK_STR_CHAT_YOU), s_chat_view.user.text);
        ak_history_wrap_into(entry, false, s_chat_lines, &s_chat_line_count, AK_CHAT_LINES_MAX, glyphs,
                             (uint16_t)sizeof(glyphs));
    }
    if (s_chat_view.reply.len > 0U) {
        (void)snprintf(entry, sizeof(entry), ak_tr(AK_STR_CHAT_AI), s_chat_view.reply.text);
        ak_history_wrap_into(entry, true, s_chat_lines, &s_chat_line_count, AK_CHAT_LINES_MAX, glyphs,
                             (uint16_t)sizeof(glyphs));
    }
}

/* EC11 rotation on the Chat page: clockwise towards the newest line. */
static void ak_chat_scroll(int32_t detents)
{
    int32_t max = (int32_t)ak_chat_max_scroll();
    if (max < 0) {
        max = 0;
    }
    int32_t next = (int32_t)s_chat_scroll - detents;
    if (next < 0) {
        next = 0;
    }
    if (next > max) {
        next = max;
    }
    if ((uint16_t)next != s_chat_scroll) {
        s_chat_scroll = (uint16_t)next;
        ak_render_status();
    }
}

static void ak_chat_page_draw(void)
{
    const char *state = ak_tr(AK_STR_CHAT_IDLE);
    uint16_t    color = AK_COL_GRAY;
    if (s_voice_ui_state == AK_VOICE_UI_LISTENING) {
        state = ak_tr(AK_STR_ST_LISTENING);
        color = AK_COL_RED;
    } else if (s_voice_ui_state == AK_VOICE_UI_RECOGNIZING) {
        state = ak_tr(AK_STR_CHAT_THINKING);
        color = AK_COL_YELLOW;
    } else if (s_chat_view.replying || ak_asr_audio_busy()) {
        state = ak_tr(AK_STR_CHAT_REPLYING);
        color = AK_COL_GREEN;
    }
    ak_gfx_text_thin(LCD_CHAT_X, 29U, ak_tr(AK_STR_CHAT_TITLE), AK_COL_CYAN, AK_COL_BLACK, 1);
    ak_gfx_text_thin(LCD_CHAT_X, 43U, state, color, AK_COL_BLACK, 1);

    uint16_t scroll = s_chat_scroll > ak_chat_max_scroll() ? ak_chat_max_scroll() : s_chat_scroll;
    uint8_t  end    = (uint8_t)(s_chat_line_count - scroll);
    uint8_t  first  = ak_chat_first_fitting(end);
    uint16_t y      = LCD_HISTORY_Y;
    for (uint8_t i = first; i < end; i++) {
        uint16_t bg = s_chat_lines[i].from_agent ? LCD_AGENT_RESPONSE_BG : AK_COL_BLACK;
        ak_gfx_text_thin(LCD_CHAT_X, y, s_chat_lines[i].text, AK_COL_WHITE, bg, 1);
        y = (uint16_t)(y + ak_history_line_advance(&s_chat_lines[i]));
    }
}

#define LCD_SETTINGS_ROW_X 12U
#define LCD_SETTINGS_ROW_W 296U

static void ak_settings_outline(uint16_t x, uint16_t y, uint16_t w,
                                uint16_t h, uint16_t color)
{
    ak_gfx_fill_rect(x, y, w, 1U, color);
    ak_gfx_fill_rect(x, (uint16_t)(y + h - 1U), w, 1U, color);
    ak_gfx_fill_rect(x, y, 1U, h, color);
    ak_gfx_fill_rect((uint16_t)(x + w - 1U), y, 1U, h, color);
}

#define LCD_SETTINGS_ROW_Y0 28U
/* 10 rows must end above the footer at y=207: 28 + 10 * 17 = 198. */
#define LCD_SETTINGS_ROW_H  17U
#define LCD_SETTINGS_TEXT_DY 4U /* 8 px font centred in the 15 px row (12 px CJK: 2 px higher) */

static bool item_value_row(ak_settings_item_t item)
{
    return item == AK_SETTINGS_ITEM_VOLUME || item == AK_SETTINGS_ITEM_BRIGHTNESS;
}

/* One settings row: label on the left, value / state on the right. */
static void ak_settings_row(ak_settings_item_t item, const char *label, const char *value, uint16_t value_color)
{
    uint16_t y        = (uint16_t)(LCD_SETTINGS_ROW_Y0 + (uint16_t)item * LCD_SETTINGS_ROW_H);
    bool     selected = s_settings.selected == item;
    bool     editing  = selected && s_settings.editing;
    uint16_t bg       = selected ? LCD_SESSION_SELECTED_BG : AK_COL_BLACK;

    ak_gfx_fill_rect(LCD_SETTINGS_ROW_X, y, LCD_SETTINGS_ROW_W, (uint16_t)(LCD_SETTINGS_ROW_H - 2U), bg);
    if (editing) {
        /* A bright outline signals "rotate now adjusts this value". */
        ak_settings_outline(LCD_SETTINGS_ROW_X, y, LCD_SETTINGS_ROW_W, (uint16_t)(LCD_SETTINGS_ROW_H - 2U),
                            AK_COL_YELLOW);
    } else if (!selected) {
        ak_settings_outline(LCD_SETTINGS_ROW_X, y, LCD_SETTINGS_ROW_W, (uint16_t)(LCD_SETTINGS_ROW_H - 2U),
                            AK_COL_GRAY);
    }
    ak_gfx_text_thin(24U, (uint16_t)(y + LCD_SETTINGS_TEXT_DY), label, selected ? AK_COL_WHITE : AK_COL_GRAY, bg, 1);
    if (value != NULL && value[0] != '\0') {
        uint16_t fg = editing ? AK_COL_YELLOW : (selected ? AK_COL_WHITE : value_color);
        ak_gfx_text_thin(184U, (uint16_t)(y + LCD_SETTINGS_TEXT_DY), value, fg, bg, 1);
    }
}

/* Pairing mode row: "OPEN m:ss" while the window is open. Also redrawn alone
 * once a second, so the countdown never costs a full-screen repaint. */
static void ak_settings_draw_pairing_row(uint32_t now_ms)
{
    if (ak_provision_is_open(&s_prov, now_ms)) {
        char     value[24];
        uint32_t left = ak_provision_remaining_s(&s_prov, now_ms);
        (void)snprintf(value, sizeof(value), "%s %u:%02u", ak_tr(AK_STR_SET_OPEN), (unsigned)(left / 60U),
                       (unsigned)(left % 60U));
        ak_settings_row(AK_SETTINGS_ITEM_PAIRING, ak_tr(AK_STR_SET_PAIRING), value, AK_COL_ORANGE);
    } else {
        ak_settings_row(AK_SETTINGS_ITEM_PAIRING, ak_tr(AK_STR_SET_PAIRING), ">", AK_COL_GRAY);
    }
}

/* "Reset network?" — a Companion reset_netcfg waits for K6 (FW-12). Leaves
 * the status bar (window countdown) visible. */
static void ak_draw_reset_confirm(void)
{
    ak_gfx_fill_rect(0U, 0U, AK_STATUSLCD_W, LCD_STATUS_BAR_Y, AK_COL_BLACK);
    ak_gfx_fill_rect(0U, 0U, AK_STATUSLCD_W, 22U, AK_COL_RED);
    ak_gfx_text(6U, 7U, ak_tr(AK_STR_RST_TITLE), AK_COL_WHITE, AK_COL_RED, 1);
    ak_gfx_text_thin(16U, 44U, ak_tr(AK_STR_RST_L1), AK_COL_WHITE, AK_COL_BLACK, 1);
    ak_gfx_text_thin(16U, 60U, ak_tr(AK_STR_RST_L2), AK_COL_WHITE, AK_COL_BLACK, 1);
    ak_gfx_text_thin(16U, 76U, ak_tr(AK_STR_RST_L3), AK_COL_WHITE, AK_COL_BLACK, 1);
    ak_gfx_text_thin(16U, 100U, ak_tr(AK_STR_RST_L4), AK_COL_GRAY, AK_COL_BLACK, 1);
    ak_gfx_text_thin(16U, 140U, ak_tr(AK_STR_RST_KEYS), AK_COL_ORANGE, AK_COL_BLACK, 1);
    ak_gfx_text_thin(16U, 156U, ak_tr(AK_STR_RST_TIMEOUT), AK_COL_GRAY, AK_COL_BLACK, 1);
}

#if AK_TRANSPORT_HAS_UART
/* ⑦ firmware update (docs/ota.md): the K6 / K1 question, then progress.
 * Keys are ignored after K6 until the keyboard reboots or the update fails. */
static void ak_draw_ota(const ak_ota_view_t *v)
{
    char line[64];
    ak_gfx_fill_rect(0U, 0U, AK_STATUSLCD_W, LCD_STATUS_BAR_Y, AK_COL_BLACK);
    ak_gfx_fill_rect(0U, 0U, AK_STATUSLCD_W, 22U, AK_COL_ORANGE);
    ak_gfx_text(6U, 7U, ak_tr(AK_STR_OTA_TITLE), AK_COL_BLACK, AK_COL_ORANGE, 1);
    if (v->state == AK_OTA_CONFIRM) {
        (void)snprintf(line, sizeof(line), ak_tr(AK_STR_OTA_INSTALL), v->version);
        ak_gfx_text_thin(16U, 44U, line, AK_COL_WHITE, AK_COL_BLACK, 1);
        (void)snprintf(line, sizeof(line), ak_tr(AK_STR_OTA_RUNNING), APP_FW_VERSION);
        ak_gfx_text_thin(16U, 60U, line, AK_COL_WHITE, AK_COL_BLACK, 1);
        ak_gfx_text_thin(16U, 84U, ak_tr(AK_STR_OTA_KEEP_USB), AK_COL_WHITE, AK_COL_BLACK, 1);
        ak_gfx_text_thin(16U, 100U, ak_tr(AK_STR_OTA_KEPT), AK_COL_GRAY, AK_COL_BLACK, 1);
        ak_gfx_text_thin(16U, 140U, ak_tr(AK_STR_OTA_KEYS), AK_COL_ORANGE, AK_COL_BLACK, 1);
        ak_gfx_text_thin(16U, 156U, ak_tr(AK_STR_RST_TIMEOUT), AK_COL_GRAY, AK_COL_BLACK, 1);
        return;
    }
    if (v->state == AK_OTA_VERIFIED) {
        (void)snprintf(line, sizeof(line), ak_tr(AK_STR_OTA_INSTALLING), v->version);
    } else if (v->state == AK_OTA_ERASING) {
        (void)snprintf(line, sizeof(line), ak_tr(AK_STR_OTA_PREPARING), v->version);
    } else {
        (void)snprintf(line, sizeof(line), ak_tr(AK_STR_OTA_RECEIVING), v->version, (unsigned)v->percent);
    }
    ak_gfx_text_thin(16U, 52U, line, AK_COL_WHITE, AK_COL_BLACK, 1);
    ak_gfx_text_thin(16U, 84U, ak_tr(AK_STR_OTA_NO_UNPLUG), AK_COL_ORANGE, AK_COL_BLACK, 1);
    ak_gfx_text_thin(16U, 100U, ak_tr(AK_STR_OTA_RESTART), AK_COL_GRAY, AK_COL_BLACK, 1);
}
#endif

/* Settings > Credentials: SET / NOT SET only, re-read on every draw. */
static void ak_settings_draw_credentials(uint32_t now_ms)
{
    ak_cred_status_t st = ak_cred_read_status();
    const char *const labels[3] = {ak_tr(AK_STR_CRED_PID), "UUID", "AuthKey"};
    bool                     set[3]    = {st.pid, st.uuid, st.authkey};

    ak_gfx_fill_rect(0U, 0U, AK_STATUSLCD_W, 22U, LCD_SESSION_SELECTED_BG);
    ak_gfx_text(6U, 7U, ak_tr(AK_STR_SET_CREDENTIALS), AK_COL_BLACK, LCD_SESSION_SELECTED_BG, 1);
    for (uint16_t i = 0U; i < 3U; i++) {
        uint16_t y = (uint16_t)(40U + i * 26U);
        ak_gfx_text_thin(24U, y, labels[i], AK_COL_WHITE, AK_COL_BLACK, 1);
        ak_gfx_text_thin(184U, y, set[i] ? ak_tr(AK_STR_CRED_SET) : ak_tr(AK_STR_CRED_NOT_SET), set[i] ? AK_COL_GREEN : AK_COL_ORANGE, AK_COL_BLACK,
                         1);
    }
#if AK_ENABLE_LICENSE_DEBUG_READ
    {
        ak_license_debug_info_t info;
        (void)ak_license_debug_read(&info);
        memset(info.uuid, 0, sizeof(info.uuid));
        memset(info.authkey_tail, 0, sizeof(info.authkey_tail));
        ak_gfx_text_thin(24U, 118U, ak_tr(AK_STR_CRED_SOURCE), AK_COL_GRAY, AK_COL_BLACK, 1);
        ak_gfx_text_thin(184U, 118U, info.source != NULL ? info.source : "none", AK_COL_GRAY, AK_COL_BLACK, 1);
    }
#endif
    bool license = st.uuid && st.authkey;
    if (ak_provision_overwrite_armed(&s_prov, now_ms)) {
        ak_gfx_text_thin(16U, 150U, ak_tr(AK_STR_CRED_OVERWRITE_ON), AK_COL_YELLOW, AK_COL_BLACK, 1);
    } else if (license && ak_provision_is_open(&s_prov, now_ms)) {
        ak_gfx_text_thin(16U, 150U, ak_tr(AK_STR_CRED_OVERWRITE_K6), AK_COL_CYAN, AK_COL_BLACK, 1);
    } else if (!license) {
        ak_gfx_text_thin(16U, 150U, ak_tr(AK_STR_CRED_NEED_USB), AK_COL_ORANGE, AK_COL_BLACK, 1);
        ak_gfx_text_thin(16U, 166U, ak_tr(AK_STR_CRED_NEED_USB2), AK_COL_ORANGE, AK_COL_BLACK, 1);
    }
#if AK_TRANSPORT_DUAL || AK_TRANSPORT_BLE_GATT
    ak_gfx_text_thin(24U, 186U, ak_tr(AK_STR_CRED_BLE), AK_COL_WHITE, AK_COL_BLACK, 1);
    ak_gfx_text_thin(184U, 186U, s_ble_key_set ? ak_tr(AK_STR_CRED_SET) : ak_tr(AK_STR_CRED_NOT_SET), s_ble_key_set ? AK_COL_GREEN : AK_COL_ORANGE,
                     AK_COL_BLACK, 1);
#endif
    ak_gfx_fill_rect(0U, 207U, AK_STATUSLCD_W, 1U, LCD_SESSION_SELECTED_BG);
    ak_gfx_text_thin(4U, 213U, ak_tr(AK_STR_CRED_BACK), AK_COL_CYAN, AK_COL_BLACK, 1);
#if AK_TRANSPORT_DUAL || AK_TRANSPORT_BLE_GATT
    if (s_ble_key_set) {
        ak_gfx_text_thin(4U, 227U, s_cred_unpair_armed ? ak_tr(AK_STR_CRED_UNPAIR_ARMED) : ak_tr(AK_STR_CRED_UNPAIR),
                         s_cred_unpair_armed ? AK_COL_YELLOW : AK_COL_GRAY, AK_COL_BLACK, 1);
    }
#endif
}

/* "Enter pairing mode?" confirm dialog (Cancel preselected). */
static void ak_settings_draw_pair_confirm(uint32_t now_ms)
{
    char line[64];
    ak_gfx_fill_rect(0U, 0U, AK_STATUSLCD_W, 22U, AK_COL_ORANGE);
    ak_gfx_text(6U, 7U, ak_tr(AK_STR_PAIR_TITLE), AK_COL_BLACK, AK_COL_ORANGE, 1);
    ak_gfx_text_thin(16U, 44U, ak_tr(AK_STR_PAIR_L1), AK_COL_WHITE, AK_COL_BLACK, 1);
    (void)snprintf(line, sizeof(line), ak_tr(AK_STR_PAIR_L2), (unsigned)(AK_PROV_WINDOW_MS / 1000U));
    ak_gfx_text_thin(16U, 60U, line, AK_COL_WHITE, AK_COL_BLACK, 1);
    ak_gfx_text_thin(16U, 84U, ak_tr(AK_STR_PAIR_L3), AK_COL_GRAY, AK_COL_BLACK, 1);
    ak_gfx_text_thin(16U, 100U, ak_tr(AK_STR_PAIR_L4), AK_COL_GRAY, AK_COL_BLACK, 1);

    bool     confirm = s_prov.choice_confirm;
    uint16_t cbg     = confirm ? AK_COL_BLACK : LCD_SESSION_SELECTED_BG;
    uint16_t obg     = confirm ? LCD_SESSION_SELECTED_BG : AK_COL_BLACK;
    ak_gfx_fill_rect(40U, 132U, 110U, 28U, cbg);
    ak_settings_outline(40U, 132U, 110U, 28U, AK_COL_GRAY);
    ak_gfx_text_thin(72U, 142U, ak_tr(AK_STR_PAIR_CANCEL), AK_COL_WHITE, cbg, 1);
    ak_gfx_fill_rect(170U, 132U, 110U, 28U, obg);
    ak_settings_outline(170U, 132U, 110U, 28U, AK_COL_GRAY);
    ak_gfx_text_thin(196U, 142U, ak_tr(AK_STR_PAIR_CONFIRM), AK_COL_WHITE, obg, 1);

    (void)snprintf(line, sizeof(line), ak_tr(AK_STR_PAIR_AUTO_CANCEL), (unsigned)ak_provision_remaining_s(&s_prov, now_ms));
    ak_gfx_text_thin(16U, 180U, line, AK_COL_GRAY, AK_COL_BLACK, 1);

    ak_gfx_fill_rect(0U, 207U, AK_STATUSLCD_W, 1U, AK_COL_ORANGE);
    ak_gfx_text_thin(4U, 213U, ak_tr(AK_STR_PAIR_KEYS), AK_COL_ORANGE, AK_COL_BLACK, 1);
    ak_gfx_text_thin(4U, 227U, ak_tr(AK_STR_PAIR_HINT), AK_COL_GRAY, AK_COL_BLACK, 1);
}

/* Drawing writes straight into the PSRAM framebuffer, and ak_statuslcd_flush()
 * returns while SPI DMA may still be reading it. Call this before ANY draw:
 * it waits for an in-flight flush (normally already done) and returns false
 * when the panel is unusable. */
static bool ak_lcd_draw_begin(void)
{
    if (ak_statuslcd_flush_busy()) {
        (void)ak_statuslcd_flush_wait();
    }
    return ak_statuslcd_ready();
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
    /* Do not draw over a framebuffer the panel's DMA is still reading. */
    if (!ak_lcd_draw_begin()) {
        return;
    }

    ak_gfx_clear(AK_COL_BLACK);
#if AK_TRANSPORT_HAS_UART
    {
        ak_ota_view_t ota;
        if (ak_ota_link_view(&ota)) {
            ak_draw_ota(&ota);
            ak_status_bar_draw();
            ak_statuslcd_flush();
            s_lcd_redraw_pending = false;
            return;
        }
    }
#endif
    if (ak_provision_reset_pending(&s_prov, now_ms)) {
        ak_draw_reset_confirm();
        ak_status_bar_draw();
        ak_statuslcd_flush();
        s_lcd_redraw_pending = false;
        return;
    }
    if (s_cred_page) {
        ak_settings_draw_credentials(now_ms);
        ak_statuslcd_flush();
        s_lcd_redraw_pending = false;
        return;
    }
    if (s_prov.state == AK_PROV_CONFIRMING) {
        ak_settings_draw_pair_confirm(now_ms);
        ak_statuslcd_flush();
        s_lcd_redraw_pending = false;
        return;
    }
    ak_gfx_fill_rect(0U, 0U, AK_STATUSLCD_W, 22U, LCD_SESSION_SELECTED_BG);
    ak_gfx_text(6U, 7U, ak_tr(AK_STR_SET_TITLE), AK_COL_BLACK,
                LCD_SESSION_SELECTED_BG, 1);

    static const ak_str_id_t led_mode_str[AK_LED_MODE_COUNT] = {AK_STR_LED_OFF, AK_STR_LED_ON, AK_STR_LED_AUTO};
    static const ak_str_id_t led_time_str[AK_LED_TIMEOUT_COUNT] = {AK_STR_TIME_30S, AK_STR_TIME_60S,
                                                                   AK_STR_TIME_5MIN, AK_STR_TIME_15MIN};
    char value[32];
    (void)snprintf(value, sizeof(value), "< %3u%% >", (unsigned)s_settings.volume);
    ak_settings_row(AK_SETTINGS_ITEM_VOLUME, ak_tr(AK_STR_SET_VOLUME), value, AK_COL_CYAN);
    (void)snprintf(value, sizeof(value), "< %3u%% >", (unsigned)s_settings.brightness);
    ak_settings_row(AK_SETTINGS_ITEM_BRIGHTNESS, ak_tr(AK_STR_SET_BRIGHTNESS), value, AK_COL_CYAN);
    (void)snprintf(value, sizeof(value), "< %s >",
                   ak_tr(s_settings.voice_chat ? AK_STR_VOICE_CHAT : AK_STR_VOICE_ASR));
    ak_settings_row(AK_SETTINGS_ITEM_VOICE_MODE, ak_tr(AK_STR_SET_VOICE_MODE), value, AK_COL_CYAN);
    (void)snprintf(value, sizeof(value), "< %s >",
                   ak_tr(led_mode_str[s_settings.led_mode < AK_LED_MODE_COUNT ? s_settings.led_mode : 0U]));
    ak_settings_row(AK_SETTINGS_ITEM_LED_POWER, ak_tr(AK_STR_SET_LED_POWER), value, AK_COL_CYAN);
    (void)snprintf(value, sizeof(value), "< %s >",
                   ak_tr(led_time_str[s_settings.led_timeout_idx < AK_LED_TIMEOUT_COUNT ? s_settings.led_timeout_idx
                                                                                         : 0U]));
    ak_settings_row(AK_SETTINGS_ITEM_LED_TIMEOUT, ak_tr(AK_STR_SET_LED_TIMEOUT), value,
                    s_settings.led_mode == (uint8_t)AK_LED_MODE_AUTO ? AK_COL_CYAN : AK_COL_GRAY);
    /* Shown in its own language either way, so it can always be found again. */
    (void)snprintf(value, sizeof(value), "< %s >", ak_lang_name((ak_lang_t)s_settings.lang));
    ak_settings_row(AK_SETTINGS_ITEM_LANGUAGE, ak_tr(AK_STR_SET_LANGUAGE), value, AK_COL_CYAN);
    ak_settings_row(AK_SETTINGS_ITEM_CREDENTIALS, ak_tr(AK_STR_SET_CREDENTIALS), ">", AK_COL_GRAY);
    ak_settings_draw_pairing_row(now_ms);
    ak_settings_row(AK_SETTINGS_ITEM_TEST, ak_tr(AK_STR_SET_TEST), ">", AK_COL_GRAY);
    ak_settings_row(AK_SETTINGS_ITEM_EXIT, ak_tr(AK_STR_SET_EXIT), "", AK_COL_GRAY);
    if (s_settings_confirmed && item_value_row(s_settings.selected)) {
        uint16_t y = (uint16_t)(LCD_SETTINGS_ROW_Y0 + (uint16_t)s_settings.selected * LCD_SETTINGS_ROW_H);
        ak_gfx_text_thin(128U, (uint16_t)(y + LCD_SETTINGS_TEXT_DY), ak_tr(AK_STR_SET_APPLIED), AK_COL_WHITE,
                         LCD_SESSION_SELECTED_BG, 1);
    }

    ak_gfx_fill_rect(0U, 207U, AK_STATUSLCD_W, 33U, AK_COL_BLACK);
    ak_gfx_fill_rect(0U, 207U, AK_STATUSLCD_W, 1U,
                     LCD_SESSION_SELECTED_BG);
    if (s_settings.editing) {
        ak_gfx_text_thin(4U, 213U, ak_tr(AK_STR_SET_HINT_ADJUST),
                         AK_COL_YELLOW, AK_COL_BLACK, 1);
        ak_gfx_text_thin(4U, 227U, ak_tr(AK_STR_SET_HINT_COMMIT), AK_COL_GRAY,
                         AK_COL_BLACK, 1);
    } else if (s_settings.selected == AK_SETTINGS_ITEM_PAIRING) {
        ak_gfx_text_thin(4U, 213U, ak_tr(AK_STR_SET_HINT_PAIRING),
                         AK_COL_CYAN, AK_COL_BLACK, 1);
        ak_gfx_text_thin(4U, 227U, ak_tr(AK_STR_SET_HINT_PAIRING2),
                         AK_COL_GRAY, AK_COL_BLACK, 1);
    } else {
        ak_gfx_text_thin(4U, 213U, ak_tr(AK_STR_SET_HINT_SELECT),
                         AK_COL_CYAN, AK_COL_BLACK, 1);
        ak_gfx_text_thin(4U, 227U, ak_tr(AK_STR_SET_HINT_CLICK),
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
        /* Main page: the EC11 push runs through the gesture classifier like
         * any NORMAL key; its long-press opens settings in
         * dispatch_gesture_for_slot(). */
        return false;
    }

    /* Settings is modal: swallow every key so K1-K6 / PTT never reach the
     * companion or change the hidden work screen. */
    if (kind != AK_INPUT_KIND_KEY) {
        return true;
    }
    /* Credentials page: K4 or a knob click goes back; K6 (window open, a
     * License stored) allows exactly one overwrite for 60 s (FW-02). */
    if (s_cred_page) {
#if AK_TRANSPORT_DUAL || AK_TRANSPORT_BLE_GATT
        /* K1 twice: clear the BLE pairing (device-local only). */
        if (event == AK_INPUT_EVENT_DOWN && strcmp(id, "k1") == 0 && s_ble_key_set) {
            if (s_cred_unpair_armed) {
                s_cred_unpair_armed = false;
                ble_key_clear();
                ak_status_bar_set(ak_tr(AK_STR_ST_BLE_CLEARED), AK_COL_ORANGE);
            } else {
                s_cred_unpair_armed = true;
            }
            ak_settings_draw();
            return true;
        }
        if (event == AK_INPUT_EVENT_DOWN) {
            s_cred_unpair_armed = false; /* any other key disarms */
        }
#endif
        if (event == AK_INPUT_EVENT_DOWN && strcmp(id, "k6") == 0) {
            uint32_t now_ms = tal_system_get_millisecond();
            if (ak_license_present() && ak_provision_arm_overwrite(&s_prov, now_ms)) {
                PR_NOTICE("license overwrite allowed once for %u s", (unsigned)(AK_PROV_OVERWRITE_MS / 1000U));
            }
            ak_settings_draw();
        } else if ((event == AK_INPUT_EVENT_DOWN && strcmp(id, "k4") == 0) ||
                   (event == AK_INPUT_EVENT_UP && strcmp(id, AK_ENC_SW_ID) == 0 && !s_center_long_consumed)) {
            s_cred_page = false;
            ak_settings_draw();
        } else if (strcmp(id, AK_ENC_SW_ID) == 0 && event == AK_INPUT_EVENT_DOWN) {
            s_center_long_consumed = false;
        } else if (strcmp(id, AK_ENC_SW_ID) == 0 && event == AK_INPUT_EVENT_LONG) {
            s_center_long_consumed = true;
        }
        return true;
    }
    /* Pairing confirm dialog (⑦): K6 confirms, K1 cancels (as every system
     * confirm). */
    if (s_prov.state == AK_PROV_CONFIRMING && event == AK_INPUT_EVENT_DOWN &&
        (strcmp(id, "k6") == 0 || strcmp(id, "k1") == 0)) {
        if (strcmp(id, "k6") == 0) {
            ak_prov_confirm_dialog();
        } else {
            ak_provision_close(&s_prov);
        }
        ak_prov_changed();
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
            if (s_prov.state == AK_PROV_CONFIRMING) {
                /* Click runs the highlighted dialog choice. */
                if (s_prov.choice_confirm) {
                    ak_prov_confirm_dialog();
                } else {
                    (void)ak_provision_click(&s_prov, tal_system_get_millisecond());
                }
                ak_prov_changed();
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
                s_lights_valid = false; /* the test page drove the LEDs itself */
                break;
            case AK_SETTINGS_RESULT_CREDENTIALS:
                s_cred_page = true;
                ak_settings_draw();
                break;
            case AK_SETTINGS_RESULT_PAIRING: {
                /* Closed: ask for confirmation. Open: this is "Exit pairing mode". */
                uint32_t now_ms = tal_system_get_millisecond();
                if (ak_provision_is_open(&s_prov, now_ms)) {
                    ak_provision_close(&s_prov);
                } else {
                    ak_provision_request(&s_prov, now_ms);
                }
                ak_prov_changed();
                break;
            }
            case AK_SETTINGS_RESULT_ENTER_EDIT:
            case AK_SETTINGS_RESULT_CONFIRMED:
            default:
                s_settings_confirmed =
                    (result == AK_SETTINGS_RESULT_CONFIRMED);
                if (result == AK_SETTINGS_RESULT_CONFIRMED &&
                    s_settings.selected == AK_SETTINGS_ITEM_VOICE_MODE) {
                    ak_voice_mode_apply(s_settings.voice_chat ? AK_VOICE_MODE_CHAT : AK_VOICE_MODE_ASR, true);
                    ak_status_bar_set(ak_tr(s_settings.voice_chat ? AK_STR_NOTE_CHAT : AK_STR_NOTE_ASR), AK_COL_CYAN);
                }
                if (result == AK_SETTINGS_RESULT_CONFIRMED && s_settings.selected == AK_SETTINGS_ITEM_LANGUAGE) {
                    ak_language_apply((ak_lang_t)s_settings.lang, true);
                }
                if (result == AK_SETTINGS_RESULT_CONFIRMED &&
                    (s_settings.selected == AK_SETTINGS_ITEM_LED_POWER ||
                     s_settings.selected == AK_SETTINGS_ITEM_LED_TIMEOUT)) {
                    ak_led_setting_commit();
                    send_device_settings(); /* expose the new LED power setting to the Companion */
                    ak_status_bar_set(ak_tr(s_settings.led_mode == (uint8_t)AK_LED_MODE_OFF  ? AK_STR_NOTE_LEDS_OFF
                                            : s_settings.led_mode == (uint8_t)AK_LED_MODE_ON ? AK_STR_NOTE_LEDS_ON
                                                                                             : AK_STR_NOTE_LEDS_AUTO),
                                      AK_COL_CYAN);
                }
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
    if (s_cred_page) {
        return true;
    }
    if (s_prov.state == AK_PROV_CONFIRMING) {
        ak_provision_select(&s_prov, detents > 0);
        ak_settings_draw();
        return true;
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
    if (!ak_lcd_draw_begin()) {
        return;
    }
    for (int i = 0; i < 7; i++) {
        ak_test_draw_key_cell(i, (s_test_key_mask & (1U << i)) != 0U);
    }
    ak_statuslcd_flush();
}

/* Repaint only the EC11 detent-count line. */
static void ak_test_draw_enc(void)
{
    char line[48];

    if (!ak_lcd_draw_begin()) {
        return;
    }
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

    if (!ak_lcd_draw_begin()) {
        return;
    }

    ak_gfx_clear(AK_COL_BLACK);
    ak_gfx_fill_rect(0U, 0U, AK_STATUSLCD_W, 22U, LCD_SESSION_SELECTED_BG);
    ak_gfx_text(6U, 7U, ak_tr(AK_STR_TEST_TITLE), AK_COL_BLACK, LCD_SESSION_SELECTED_BG, 1);

    ak_gfx_text_thin(6U, 30U, ak_tr(AK_STR_TEST_KEYS), AK_COL_GRAY, AK_COL_BLACK, 1);
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
    ak_gfx_text_thin(6U, AK_TEST_Y_BAT, ak_tr(AK_STR_TEST_BATTERY_WAIT), AK_COL_WHITE, AK_COL_BLACK, 1);
    (void)snprintf(line, sizeof(line), "UART0 (%s): rx %lu bytes", AK_TRANSPORT_NAME,
                   (unsigned long)s_test_uart_rx);
    ak_gfx_text_thin(6U, AK_TEST_Y_UART, line, AK_COL_WHITE, AK_COL_BLACK, 1);
    ak_gfx_text_thin(6U, AK_TEST_Y_WIFI, ak_tr(AK_STR_TEST_WIFI_WAIT), AK_COL_WHITE, AK_COL_BLACK, 1);
    ak_gfx_text_thin(6U, AK_TEST_Y_BLE, ak_tr(AK_STR_TEST_BLE_WAIT), AK_COL_WHITE, AK_COL_BLACK, 1);

    ak_gfx_fill_rect(0U, 207U, AK_STATUSLCD_W, 33U, AK_COL_BLACK);
    ak_gfx_fill_rect(0U, 207U, AK_STATUSLCD_W, 1U, LCD_SESSION_SELECTED_BG);
    ak_gfx_text_thin(4U, 213U, ak_tr(AK_STR_TEST_HINT), AK_COL_CYAN, AK_COL_BLACK, 1);
    ak_gfx_text_thin(4U, 227U, ak_tr(AK_STR_TEST_EXIT), AK_COL_GRAY, AK_COL_BLACK, 1);
    ak_statuslcd_flush();
}

/* Repaint the slow rows (battery/UART/WiFi/BLE) on the periodic tick. */
static void ak_test_draw_slow(void)
{
    char line[48];

    if (!ak_lcd_draw_begin()) {
        return;
    }

    ak_gfx_fill_rect(0U, AK_TEST_Y_BAT, AK_STATUSLCD_W, 14U, AK_COL_BLACK);
    if (ak_battery_available()) {
        unsigned          pct = (unsigned)ak_battery_percent();
        ak_battery_info_t bi;
        ak_battery_get_info(&bi);
        /* cell = what a multimeter on BAT should read; adc = tap voltage. */
        (void)snprintf(line, sizeof(line), "Bat %u%% cell %ldmV adc %ldmV%s%s", pct,
                       (long)bi.cell_mv, (long)bi.node_mv, bi.charging ? " CHG" : "",
                       bi.skipped ? " LED" : "");
    } else {
        (void)snprintf(line, sizeof(line), "%s", ak_tr(AK_STR_TEST_BATTERY_NA));
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
#if AK_TRANSPORT_HAS_BLE
    (void)snprintf(line, sizeof(line), "BLE: %s",
                   ak_ble_gatt_link_state_str(ak_ble_gatt_link_state()));
#else
    (void)snprintf(line, sizeof(line), "%s", ak_tr(AK_STR_TEST_BLE_NA));
#endif
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

/* --- Test-page "LED power" view ---------------------------------------------
 * Switches the chain power (LED_PWR_EN = P18) by hand through the normal
 * sequence in ak_led_power.h: on = P18 high, wait for VDD_LED to settle, then
 * the first frame (a fixed test pattern); off = an all-zero frame, DIN idle
 * low, then P18 low. The Settings LED power mode does not apply here. On lite
 * / v0.3.1 boards P18 is not connected, so the switch does nothing there and
 * the chain stays powered; the page only says so (no hardware detection). */
#define AK_TEST_LP_Y_PIN   40U
#define AK_TEST_LP_Y_CHAIN 58U

static void ak_test_ledpwr_set(bool on)
{
    if (!ak_leds_available()) {
        return;
    }
    ak_leds_clear();
    if (on) {
        static const uint8_t pattern[AK_LEDS_COUNT][3] = {
            {48, 0, 0}, {0, 48, 0}, {0, 0, 48}, {32, 32, 32}, {48, 0, 0}, {0, 48, 0}, {0, 0, 48},
        };
        for (uint32_t i = 0; i < AK_LEDS_COUNT; i++) {
            ak_leds_set(i, pattern[i][0], pattern[i][1], pattern[i][2]);
        }
    }
    (void)ak_leds_show();
}

/* Repaint only the two live rows (P18 level, chain state). */
static void ak_test_draw_ledpwr_rows(void)
{
    char line[48];
    if (!ak_lcd_draw_begin()) {
        return;
    }
    bool high = ak_leds_power_enabled();
    ak_gfx_fill_rect(0U, AK_TEST_LP_Y_PIN, AK_STATUSLCD_W, 14U, AK_COL_BLACK);
    (void)snprintf(line, sizeof(line), "P18 LED_PWR_EN: %s", high ? "HIGH" : "LOW");
    ak_gfx_text_thin(6U, AK_TEST_LP_Y_PIN, line, high ? AK_COL_GREEN : AK_COL_GRAY, AK_COL_BLACK, 1);
    ak_gfx_fill_rect(0U, AK_TEST_LP_Y_CHAIN, AK_STATUSLCD_W, 14U, AK_COL_BLACK);
    (void)snprintf(line, sizeof(line), "Chain: %s", ak_leds_available() ? ak_leds_power_state_str() : "unavailable");
    ak_gfx_text_thin(6U, AK_TEST_LP_Y_CHAIN, line, AK_COL_WHITE, AK_COL_BLACK, 1);
    ak_statuslcd_flush();
}

static void ak_test_draw_ledpwr(void)
{
    char line[48];
    if (!ak_lcd_draw_begin()) {
        return;
    }
    ak_gfx_clear(AK_COL_BLACK);
    ak_gfx_fill_rect(0U, 0U, AK_STATUSLCD_W, 22U, LCD_SESSION_SELECTED_BG);
    ak_gfx_text(6U, 7U, ak_tr(AK_STR_SET_LED_POWER), AK_COL_BLACK, LCD_SESSION_SELECTED_BG, 1);
    ak_gfx_text_thin(6U, 84U, "On : P18 high, settle 10 ms, colours", AK_COL_GRAY, AK_COL_BLACK, 1);
    ak_gfx_text_thin(6U, 98U, "Off: dark frame, DIN low, P18 low", AK_COL_GRAY, AK_COL_BLACK, 1);
    (void)snprintf(line, sizeof(line), "Setting %s / %s: not used here",
                   ak_led_mode_str((ak_led_mode_t)s_settings.led_mode),
                   ak_led_policy_timeout_str(s_settings.led_timeout_idx));
    ak_gfx_text_thin(6U, 120U, line, AK_COL_GRAY, AK_COL_BLACK, 1);
    ak_gfx_text_thin(6U, 150U, ak_tr(AK_STR_TEST_NO_SWITCH1), AK_COL_ORANGE, AK_COL_BLACK, 1);
    ak_gfx_text_thin(6U, 164U, ak_tr(AK_STR_TEST_NO_SWITCH2), AK_COL_ORANGE, AK_COL_BLACK, 1);
    ak_gfx_fill_rect(0U, 207U, AK_STATUSLCD_W, 1U, LCD_SESSION_SELECTED_BG);
    ak_gfx_text_thin(4U, 213U, ak_tr(AK_STR_TEST_LP_KEYS), AK_COL_CYAN, AK_COL_BLACK, 1);
    ak_gfx_text_thin(4U, 227U, ak_tr(AK_STR_TEST_LP_HINT), AK_COL_GRAY, AK_COL_BLACK, 1);
    ak_statuslcd_flush();
    ak_test_draw_ledpwr_rows();
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
        if (s_test_view == AK_TEST_VIEW_LEDPWR) {
            /* LED power view: K1 = power on with test colours, K2 = off. */
            if (bit == 0 || bit == 1) {
                ak_test_ledpwr_set(bit == 0);
            }
        } else if (bit >= 0 && bit < AK_TEST_FX_COUNT) {
            /* K1-K6 pick one of the six LED effects (bit 0..5). */
            s_test_fx = bit;
            ak_test_leds_render();
        }
        if (s_test_view == AK_TEST_VIEW_HW) {
            ak_test_draw_keys();
        }
    } else if (event == AK_INPUT_EVENT_UP) {
        s_test_key_mask &= (uint8_t)~(1U << bit);
        if (s_test_view == AK_TEST_VIEW_HW) {
            ak_test_draw_keys();
        }
    }

    /* Double-click of the EC11 push exits. Feeding every enc edge lets the
     * detector see DOWN/LONG/UP; a hold fires LONG which it excludes, so
     * hold-to-test never trips the exit. */
    if (bit == 6) {
        if (event == AK_INPUT_EVENT_DOWN) {
            s_test_enc_long = false;
        } else if (event == AK_INPUT_EVENT_LONG) {
            s_test_enc_long = true;
        }
        if (ak_dclick_feed(&s_test_enc_dclick, event,
                           tal_system_get_millisecond(), held_ms)) {
            s_test_exit_req = true;
        }
        /* A short click (not a hold, not the 2nd click of the exit double
         * click) steps HW -> Font -> LED power -> HW. */
        if (event == AK_INPUT_EVENT_UP && !s_test_enc_long && !s_test_exit_req) {
            s_test_view = (ak_test_view_t)((s_test_view + 1) % AK_TEST_VIEW_COUNT);
            if (s_test_view == AK_TEST_VIEW_FONT) {
                ak_fonttest_draw(s_test_font_idx);
            } else if (s_test_view == AK_TEST_VIEW_LEDPWR) {
                ak_test_ledpwr_set(false); /* start from a known state: chain off */
                ak_test_draw_ledpwr();
            } else {
                ak_test_draw_frame();
                ak_test_draw_slow();
                ak_test_leds_render();
            }
        }
    }
}

static void ak_test_enc_cb(int32_t detents)
{
    s_test_enc_count += detents;
    if (s_test_view == AK_TEST_VIEW_LEDPWR) {
        return; /* rotation has no job in the LED power view */
    }
    if (s_test_view == AK_TEST_VIEW_FONT) {
        /* Font view: each rotation steps through the candidate fonts. */
        s_test_font_idx += detents > 0 ? 1 : -1;
        s_test_font_idx = (int)(((s_test_font_idx % (int)ak_fonttest_count()) + (int)ak_fonttest_count()) %
                                (int)ak_fonttest_count());
        ak_fonttest_draw(s_test_font_idx);
        return;
    }
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
    s_test_view         = AK_TEST_VIEW_HW;
    s_test_font_idx     = 0;
    s_test_enc_long     = false;
    ak_dclick_init(&s_test_enc_dclick);
    ak_leds_set_test_override(true); /* the test page drives the LEDs in every LED power mode */

    ak_test_draw_frame();
    ak_test_draw_slow();
    ak_test_leds_render();

    uint32_t last_tick = tal_system_get_millisecond();
    uint32_t last_fx_ms = last_tick;

    while (!s_test_exit_req) {
        uint32_t now = tal_system_get_millisecond();

        /* Tight input polling -> keys feel immediate. */
        ak_input_poll(now, ak_test_input_cb, ak_test_enc_cb);

        /* Keep draining both links so their buffers never overflow; count the
         * bytes. Nothing is parsed while the test page runs. */
        uint8_t rx[64];
        int     got = 0;
#if AK_TRANSPORT_HAS_UART
        int got_uart = ak_transport_read_uart(rx, sizeof(rx));
        if (got_uart > 0) {
            got += got_uart;
            /* Companion pings keep arriving; refresh the USB liveness stamp so
             * the main loop does not falsely time the link out on exit. */
            s_usb_last_rx_ms = now;
            ak_rx_reader_reset(&s_rx_uart);
        }
#endif
#if AK_TRANSPORT_HAS_BLE
        int got_ble = ak_transport_read_ble(rx, sizeof(rx));
        if (got_ble > 0) {
            got += got_ble;
            ak_rx_reader_reset(&s_rx_ble);
                ble_sec_drop();
        }
#endif
        if (got > 0) {
            s_test_uart_rx += (uint32_t)got;
        }

        /* Periodic: repaint the slow status rows (120ms). */
        if ((uint32_t)(now - last_tick) >= AK_TEST_TICK_MS) {
            last_tick = now;
            if (s_test_view == AK_TEST_VIEW_HW) {
                ak_test_draw_slow();
            } else if (s_test_view == AK_TEST_VIEW_LEDPWR) {
                ak_test_draw_ledpwr_rows();
            }
        }

        /* Advance the LED effect animation at the effect's own frame rate
         * (not in the LED power view: there K1 / K2 own the chain). */
        if (s_test_view != AK_TEST_VIEW_LEDPWR && (uint32_t)(now - last_fx_ms) >= ak_test_fx_frame_ms()) {
            last_fx_ms = now;
            s_test_ws_phase++;
            ak_test_leds_render();
        }
        ak_leds_tick(now);

        tal_system_sleep(POLL_INTERVAL_MS);
    }

    /* Clean up and return to the settings page the user came from. */
    if (ak_leds_available()) {
        ak_leds_clear();
        (void)ak_leds_show();
    }
    ak_leds_set_test_override(false); /* back under the Settings LED power mode */
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

    char buf[LCD_SESSION_ALIAS_BYTES];
    size_t n = 0U;
    for (const char *p = alias; *p != '\0' && n + 1U < sizeof(buf); p++) {
        buf[n++] = (unsigned char)*p < 0x20U ? ' ' : *p;
    }
    buf[n] = '\0';
    ak_text_utf8_trim(buf);
    buf[ak_text_wrap(buf, LCD_SIDEBAR_W - 4U)] = '\0'; /* 7 ASCII or 4 CJK characters */
    memcpy(s_session_labels[slot], buf, strlen(buf) + 1U);
}

static void ak_draw_session_sidebar(void)
{
    for (uint16_t i = 0; i < LCD_SESSION_ROWS; i++) {
        bool selected = (i == s_selected_session);
        uint16_t y = (uint16_t)(22U + i * LCD_SESSION_ROW_H);
        uint16_t h = (i + 1U == LCD_SESSION_ROWS)
                         ? (uint16_t)(LCD_STATUS_BAR_Y - y)
                         : LCD_SESSION_ROW_H;
        /* Background now carries the slot's agent-status color; selection is
         * shown by a bright outline + white foreground instead of the bg. */
        uint16_t bg = ak_status_color(s_sessions[i].state);
        uint16_t fg = selected ? AK_COL_WHITE : AK_COL_GRAY;
        uint16_t label_width = ak_text_width(s_session_labels[i]);
        uint16_t label_x = label_width < LCD_SIDEBAR_W
                               ? (uint16_t)((LCD_SIDEBAR_W - label_width) / 2U)
                               : 0U;

        ak_gfx_fill_rect(0, y, LCD_SIDEBAR_W - 1U, h, bg);
        if (selected) {
            /* 2px white outline marks the selected slot over any status color. */
            ak_gfx_fill_rect(0, y, LCD_SIDEBAR_W - 1U, 2U, AK_COL_WHITE);
            ak_gfx_fill_rect(0, (uint16_t)(y + h - 2U), LCD_SIDEBAR_W - 1U, 2U, AK_COL_WHITE);
            ak_gfx_fill_rect(0, y, 2U, h, AK_COL_WHITE);
            ak_gfx_fill_rect((uint16_t)(LCD_SIDEBAR_W - 3U), y, 2U, h, AK_COL_WHITE);
        }
        ak_gfx_icon(16U, (uint16_t)(y + 8U), AK_SESSION_ICON_SESSION,
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

/* ── LCD menu screen ─────────────────────────────────────────────────────
 * Header: kind + title, queue and page/pages. Then up to 3 context lines,
 * the page's items (direct menus tag each with its key, K2..K5), one hint
 * line, and the status bar. UTF-8 text from the Companion is drawn as is
 * (ak_gfx_text: 8x8 for ASCII, the 12 px UI font for Chinese / mixed) and
 * wrapped / clipped by pixel width. */
#define LCD_MENU_COLS     38U
#define LCD_MENU_TEXT_W   (LCD_MENU_COLS * AK_TEXT_ASCII_W) /* 304 px */
#define LCD_MENU_CTX_Y    28U
#define LCD_MENU_ITEM_Y   66U
#define LCD_MENU_ITEM_H   20U
#define LCD_MENU_HINT_Y   194U

/* Copy UTF-8 for the LCD: control characters become spaces, a sequence cut
 * by `cap` is dropped. */
static void ak_lcd_text(const char *utf8, char *out, size_t cap)
{
    size_t n = 0U;
    for (; *utf8 != '\0' && n + 1U < cap; utf8++) {
        out[n++] = (unsigned char)*utf8 < 0x20U ? ' ' : *utf8;
    }
    out[n] = '\0';
    ak_text_utf8_trim(out);
}

/* Cut `s` to what fits in max_px (at a character / word boundary). */
static void ak_lcd_clip(char *s, uint16_t max_px)
{
    if (ak_text_width(s) > max_px) {
        s[ak_text_wrap(s, max_px)] = '\0';
    }
}

/* Draw up to `rows` wrapped lines of `text` from y with `pitch`; the last
 * line ends in "..." when text remains. Returns the number of lines drawn. */
static uint8_t ak_lcd_wrap_draw(const char *text, uint16_t y, uint16_t pitch, uint8_t rows, uint16_t color)
{
    char        line[LCD_MENU_COLS * 3U + 8U];
    const char *p = text;
    uint8_t     n = 0U;
    while (*p != '\0' && n < rows) {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        size_t len = ak_text_wrap(p, LCD_MENU_TEXT_W);
        bool   more = p[len] != '\0' && n + 1U == rows;
        if (more) {
            len = ak_text_wrap(p, LCD_MENU_TEXT_W - 3U * AK_TEXT_ASCII_W);
        }
        if (len >= sizeof(line) - 4U) {
            len = sizeof(line) - 4U;
            while (len > 0U && ((unsigned char)p[len] & 0xC0U) == 0x80U) {
                len--;
            }
        }
        memcpy(line, p, len);
        memcpy(line + len, more ? "..." : "", more ? 4U : 1U);
        ak_gfx_text_thin(4U, (uint16_t)(y + n * pitch), line, color, AK_COL_BLACK, 1);
        p += len > 0U ? len : 1U;
        n++;
    }
    return n;
}

static uint16_t ak_menu_role_lcd(uint8_t role)
{
    switch (role) {
    case AK_MENU_ROLE_CONFIRM:
        return AK_COL_GREEN;
    case AK_MENU_ROLE_CANCEL:
    case AK_MENU_ROLE_DANGER:
        return AK_COL_RED;
    case AK_MENU_ROLE_ATTENTION:
        return AK_COL_YELLOW;
    case AK_MENU_ROLE_VOICE:
        return AK_COL_PURPLE;
    default:
        return AK_RGB565(0x40U, 0x80U, 0xFFU); /* selectable option: blue */
    }
}

static void ak_menu_draw(void)
{
    static const ak_str_id_t kind_tag[] = {AK_STR_MENU_OPTIONS, AK_STR_MENU_APPROVE, AK_STR_MENU_QUESTION,
                                           AK_STR_MENU_MODEL, AK_STR_MENU_MODE};
    const ak_menu_t         *m          = &s_menu;
    const ak_menu_pg_t      *pg         = &m->pg[m->cur_page];
    char                     line[LCD_MENU_COLS * 2U + 8U];
    char                     text[AK_MENU_CTX_MAX + 1U];

    ak_gfx_clear(AK_COL_BLACK);
    const ak_menu_pg_t *head = pg->count > 0U ? pg : &m->pg[0]; /* page not here yet */
    uint16_t head_bg = head->kind == AK_MENU_KIND_APPROVAL ? AK_COL_ORANGE
                       : head->kind == AK_MENU_KIND_QUESTION ? AK_COL_CYAN
                                                             : LCD_SESSION_SELECTED_BG;
    ak_gfx_fill_rect(0U, 0U, AK_STATUSLCD_W, 22U, head_bg);
    ak_lcd_text(head->title, text, sizeof(text));
    (void)snprintf(line, sizeof(line), "%s %s", ak_tr(kind_tag[(unsigned)head->kind < 5U ? head->kind : 0]), text);
    ak_text_utf8_trim(line);
    ak_lcd_clip(line, 26U * AK_TEXT_ASCII_W);
    ak_gfx_text(4U, 7U, line, AK_COL_BLACK, head_bg, 1);
    if (m->pages > 1U || m->queue[0] != '\0') {
        char right[24];
        ak_lcd_text(m->queue, text, sizeof(text));
        if (m->pages > 1U) {
            (void)snprintf(right, sizeof(right), "%s %u/%u", text, (unsigned)(m->cur_page + 1U), (unsigned)m->pages);
        } else {
            (void)snprintf(right, sizeof(right), "%s", text);
        }
        ak_text_utf8_trim(right);
        ak_gfx_text_thin((uint16_t)(AK_STATUSLCD_W - 4U - ak_text_width(right)), 7U, right, AK_COL_BLACK, head_bg, 1);
    }

    if (m->state == AK_MENU_WAIT) {
        ak_gfx_text(144U, 110U, "...", AK_COL_WHITE, AK_COL_BLACK, 2);
        return;
    }

    /* Context: up to 3 lines, wrapped by width. Long content (diffs, code) is
     * never sent; `more` says it is on the PC screen. */
    ak_lcd_text(m->ctx, text, sizeof(text));
    uint8_t rows = ak_lcd_wrap_draw(text, LCD_MENU_CTX_Y, 12U, 3U, AK_COL_WHITE);
    if (m->more && rows < 3U) {
        ak_gfx_text_thin(4U, (uint16_t)(LCD_MENU_CTX_Y + rows * 12U), ak_tr(AK_STR_MENU_SEE_SCREEN), AK_COL_YELLOW, AK_COL_BLACK, 1);
    }

    if (pg->count == 0U) {
        ak_gfx_text(144U, (uint16_t)(LCD_MENU_ITEM_Y + 40U), "...", AK_COL_WHITE, AK_COL_BLACK, 2);
    }
    for (uint8_t i = 0U; i < pg->count; i++) {
        const ak_menu_item_t *it  = &pg->items[i];
        uint16_t              y   = (uint16_t)(LCD_MENU_ITEM_Y + i * LCD_MENU_ITEM_H);
        bool                  hi  = i == m->cur_idx;
        uint16_t              bg  = hi ? LCD_AGENT_RESPONSE_BG : AK_COL_BLACK;
        uint16_t              tag = ak_menu_role_lcd(it->role);
        char                  k[3] = {'K', (char)('2' + i), '\0'}; /* item i sits on K(i+2) */
        ak_gfx_fill_rect(0U, y, AK_STATUSLCD_W, (uint16_t)(LCD_MENU_ITEM_H - 2U), bg);
        ak_gfx_fill_rect(4U, (uint16_t)(y + 2U), 26U, 14U, tag);
        ak_gfx_text(9U, (uint16_t)(y + 5U), k, AK_COL_BLACK, tag, 1);
        const char *mark = m->multi ? (ak_menu_item_ticked(m, m->cur_page, i) ? "[x] " : "[ ] ")
                           : ak_menu_item_ticked(m, m->cur_page, i) ? "* "
                                                                    : "  ";
        ak_lcd_text(it->label, text, sizeof(text));
        (void)snprintf(line, sizeof(line), "%s%s", mark, text);
        ak_text_utf8_trim(line);
        ak_lcd_clip(line, (uint16_t)(AK_STATUSLCD_W - 38U - 4U));
        ak_gfx_text(38U, (uint16_t)(y + 5U), line, it->role == AK_MENU_ROLE_NORMAL ? AK_COL_WHITE : tag, bg, 1);
    }

    const char *hint;
    uint16_t    hint_color = AK_COL_GRAY;
    if (ak_menu_armed(m)) {
        hint       = ak_tr(AK_STR_MENU_ARMED);
        hint_color = AK_COL_YELLOW;
    } else if (!ak_menu_enter_allowed(m)) {
        hint = ak_tr(AK_STR_MENU_HINT_ANSWER);
    } else if (m->multi) {
        hint = ak_tr(AK_STR_MENU_HINT_MULTI);
    } else {
        hint = ak_tr(AK_STR_MENU_HINT_SINGLE);
    }
    ak_gfx_text_thin(4U, LCD_MENU_HINT_Y, hint, hint_color, AK_COL_BLACK, 1);
}

/* Header + wrapped text of a ③ screen (voice input or dictated answer). */
static void ak_confirm_text_draw(const char *title, uint16_t color, const char *utf8)
{
    char text[ASR_TRANSCRIPT_MAX];

    ak_gfx_clear(AK_COL_BLACK);
    ak_gfx_fill_rect(0U, 0U, AK_STATUSLCD_W, 22U, color);
    ak_gfx_text(4U, 7U, title, AK_COL_BLACK, color, 1);
    if (utf8 == NULL) {
        return;
    }
    ak_lcd_text(utf8, text, sizeof(text));
    (void)ak_lcd_wrap_draw(text, LCD_MENU_CTX_Y, 14U, 11U, AK_COL_WHITE);
}

/* ③ of a voice input: the text that K6 sends. */
static void ak_input_draw(void)
{
    ak_confirm_text_draw(ak_tr(AK_STR_INPUT_TITLE), AK_COL_GREEN, s_input_text);
    ak_gfx_text_thin(4U, LCD_MENU_HINT_Y, ak_tr(AK_STR_INPUT_HINT), AK_COL_GRAY, AK_COL_BLACK, 1);
}

/* Dictated answer (②/③ for a menu's voice item): the recognised text, or
 * how to speak. */
static void ak_dictation_draw(void)
{
    bool confirm = ak_ui_state(&s_ui) == AK_UI_CONFIRM;
    ak_confirm_text_draw(ak_tr(AK_STR_DICT_TITLE), AK_COL_PURPLE, confirm ? s_dictated : NULL);
    if (!confirm) {
        ak_gfx_text(4U, 80U, ak_tr(AK_STR_DICT_HOLD), AK_COL_WHITE, AK_COL_BLACK, 2);
        ak_gfx_text(4U, 104U, ak_tr(AK_STR_DICT_SPEAK), AK_COL_WHITE, AK_COL_BLACK, 2);
    }
    ak_gfx_text_thin(4U, LCD_MENU_HINT_Y, confirm ? ak_tr(AK_STR_DICT_CONFIRM_HINT) : ak_tr(AK_STR_DICT_BACK_HINT),
                     AK_COL_GRAY, AK_COL_BLACK, 1);
}

/* Render the selected A/B/C session and its independent ten-entry history. */
static void ak_render_status(void)
{
    if (!ak_statuslcd_ready()) {
        return;
    }
    /* Wait for any in-flight flush BEFORE drawing (see ak_lcd_draw_begin). */
    if (!ak_lcd_draw_begin()) {
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
#if AK_TRANSPORT_HAS_UART
    {
        ak_ota_view_t ota;
        if (ak_ota_link_view(&ota)) {
            ak_draw_ota(&ota);
            ak_status_bar_draw();
            ak_statuslcd_flush();
            s_lcd_redraw_pending = false;
            return;
        }
    }
#endif
    if (ak_provision_reset_pending(&s_prov, now_ms)) {
        ak_draw_reset_confirm();
        ak_status_bar_draw();
        ak_statuslcd_flush();
        s_lcd_redraw_pending = false;
        return;
    }
    if (s_menu.state == AK_MENU_OPEN && s_ui.dictating) {
        ak_dictation_draw();
        ak_status_bar_draw();
        ak_statuslcd_flush();
        s_lcd_redraw_pending = false;
        return;
    }
    if (ak_input_confirm()) {
        ak_input_draw();
        ak_status_bar_draw();
        ak_statuslcd_flush();
        s_lcd_redraw_pending = false;
        return;
    }
    if ((s_menu.state == AK_MENU_OPEN && !s_menu.hidden) || s_menu.state == AK_MENU_WAIT) {
        ak_menu_draw();
        ak_status_bar_draw();
        ak_statuslcd_flush();
        s_lcd_redraw_pending = false;
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

    if (s_voice_mode == AK_VOICE_MODE_CHAT) {
        ak_chat_page_draw();
        ak_status_bar_draw();
        ak_statuslcd_flush();
        s_lcd_redraw_pending = false;
        return;
    }
    (void)snprintf(line, sizeof(line), ak_tr(AK_STR_ST_SESSION),
                   s_session_labels[s_selected_session],
                   session->agent[0] ? session->agent : "-");
    ak_gfx_text_thin(LCD_CHAT_X, 29U, line, AK_COL_WHITE, AK_COL_BLACK, 1);
    (void)snprintf(line, sizeof(line), ak_tr(AK_STR_ST_STATUS),
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
    char feedback[48];
    (void)snprintf(feedback, sizeof(feedback), ak_tr(AK_STR_ST_SELECTED_SESSION),
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
        ak_status_bar_set(ak_tr(AK_STR_ST_LISTENING), AK_COL_RED);
        break;
    case AK_VOICE_UI_RECOGNIZING:
        ak_status_bar_set(ak_tr(AK_STR_ST_RECOGNIZING), AK_COL_YELLOW);
        break;
    case AK_VOICE_UI_IDLE:
    default:
        ak_status_bar_set(ak_tr(AK_STR_ST_READY), AK_COL_GREEN);
        break;
    }
    ak_render_status();
}

/* ---- Post-ASR input confirm (③, firmware-led) ---------------------------
 * Entered after a successful recognition; LED4 (K4=cancel) breathes red and
 * LED6 (K6=confirm) blinks green to cue the gestures (ak_session_lights,
 * painted by ak_menu_loop_tick). ak_ui_tick times the state out. */

static void ak_pending_confirm_enter(uint32_t now_ms)
{
    ak_leds_activity(); /* the K4/K6 hints must be visible in Auto */
    ak_ui_voice_end(&s_ui, AK_UI_VOICE_TEXT, s_voice_mode == AK_VOICE_MODE_CHAT, now_ms);
}

/* ② / ③ end without an answer (settings opened, voice mode changed). */
static void ak_pending_confirm_exit(void)
{
    ak_ui_voice_abort(&s_ui);
    ak_ui_sync(tal_system_get_millisecond());
}

static void ak_pending_confirm_tick(uint32_t now_ms)
{
    if (ak_ui_tick(&s_ui, now_ms)) {
        if (s_menu.state == AK_MENU_OPEN && ak_ui_state(&s_ui) == AK_UI_MENU) {
            ak_menu_dictate_cancel(&s_menu); /* an unsent dictated answer: back to the menu */
        }
        ak_ui_sync(now_ms);
        ak_render_status();
    }
}

/* Lights of a dictated answer (②/③ for a menu): K1 red (back to the menu),
 * the voice key purple, K6 green once there is text to send. */
static void ak_dictation_lights(ak_rgb_t frame[AK_LIGHT_COUNT])
{
    memset(frame, 0, sizeof(ak_rgb_t) * AK_LIGHT_COUNT);
    frame[0] = ak_light_rgb(AK_LIGHT_RED, AK_LIGHT_FULL_PCT);
    for (int i = 0; i < AK_KEYMAP_SLOTS - 1; i++) {
        if (s_keymap.entries[i].kind == AK_KEY_KIND_VOICE) {
            frame[i] = ak_light_rgb(AK_LIGHT_PURPLE, AK_LIGHT_FULL_PCT);
        }
    }
    if (ak_ui_state(&s_ui) == AK_UI_CONFIRM) {
        frame[5] = ak_light_rgb(AK_LIGHT_GREEN, AK_LIGHT_FULL_PCT);
    }
}

/* Main loop: menu timers, state overlays, status bar expiry, key lights. */
static void ak_menu_loop_tick(uint32_t now)
{
    ak_menu_out_t out;
    ak_menu_tick(&s_menu, now, &out);
    if (out.kind != AK_MENU_OUT_NONE || out.redraw) {
        ak_menu_handle(&out); /* also syncs */
    } else {
        ak_ui_sync(now);
    }

    if (s_status_tmp_on && (int32_t)(now - s_status_tmp_until_ms) >= 0) {
        s_status_tmp_on      = false;
        s_lcd_redraw_pending = true;
    }

    if (!ak_leds_available()) {
        return;
    }
    ak_rgb_t frame[AK_LIGHT_COUNT];
    if (ak_menu_owns_keys(&s_menu)) {
        ak_menu_lights(&s_menu, now, frame);
    } else if (s_ui.dictating) {
        ak_dictation_lights(frame);
    } else {
        uint8_t waiting = 0U;
        for (uint8_t i = 0U; i < LCD_SESSION_ROWS; i++) {
            if (strcmp(s_sessions[i].state, "waiting") == 0) {
                waiting |= (uint8_t)(1U << i);
            }
        }
        ak_session_lights(waiting, ak_ui_k4_waiting(&s_ui, s_menu_parked), ak_input_confirm(), now - s_ui.confirm_ms,
                          now, frame);
    }
    if (s_lights_valid && memcmp(frame, s_lights_shown, sizeof(frame)) == 0) {
        return;
    }
    for (uint32_t i = 0; i < AK_LIGHT_COUNT; i++) {
        ak_leds_set(i, frame[i].r, frame[i].g, frame[i].b);
    }
    (void)ak_leds_show();
    memcpy(s_lights_shown, frame, sizeof(frame));
    s_lights_valid = true;
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
        ak_leds_activity(); /* a voice result (or its warning) wakes Auto LED power */
        bool for_menu  = s_asr_for_menu;
        s_asr_for_menu = false;
        if (!is_error && for_menu) {
            /* Dictated menu answer: confirm it here (③), K6 sends it. */
            (void)snprintf(s_dictated, sizeof(s_dictated), "%s", s_asr_pending_text);
            ak_ui_voice_end(&s_ui, AK_UI_VOICE_TEXT, false, now_ms);
            ak_status_bar_set(ak_tr(AK_STR_ST_DICTATED_HINT), AK_COL_GREEN);
        } else if (!is_error) {
            ak_chat_history_add(&s_sessions[s_selected_session], s_asr_pending_text, false);
            (void)snprintf(s_input_text, sizeof(s_input_text), "%s", s_asr_pending_text);
            ak_status_bar_set(ak_tr(AK_STR_ST_CONFIRM_HINT), AK_COL_GREEN);
            /* ③: the text stays on the LCD; K4/K6/K1 edit, send, cancel and
             * the hint LEDs light. A new result while one is still pending
             * (re-dictated without confirming) simply replaces it. */
            ak_pending_confirm_enter(now_ms);
        } else {
            ak_ui_voice_end(&s_ui, AK_UI_VOICE_ERROR, s_voice_mode == AK_VOICE_MODE_CHAT, now_ms);
            if (strcmp(s_asr_pending_text, "asr_empty") == 0) {
                ak_status_bar_set(ak_tr(AK_STR_ST_NO_SPEECH), AK_COL_ORANGE);
            } else {
                ak_status_bar_set(ak_tr(AK_STR_ST_ASR_FAILED), AK_COL_RED);
            }
        }
        ak_ui_sync(now_ms);
        redraw = true;
        s_asr_result_pending = false;
        s_voice_ui_state = AK_VOICE_UI_IDLE;
        s_voice_ui_state_since_ms = now_ms;
    }

    if (s_voice_ui_state == AK_VOICE_UI_RECOGNIZING &&
        (uint32_t)(now_ms - s_voice_ui_state_since_ms) >= ASR_RECOGNIZE_TIMEOUT_MS) {
        s_voice_ui_state = AK_VOICE_UI_IDLE;
        s_voice_ui_state_since_ms = now_ms;
        s_asr_for_menu            = false;
        ak_ui_voice_end(&s_ui, AK_UI_VOICE_ERROR, s_voice_mode == AK_VOICE_MODE_CHAT, now_ms);
        ak_ui_sync(now_ms);
        ak_status_bar_set(ak_tr(AK_STR_ST_ASR_TIMEOUT), AK_COL_ORANGE);
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
static void ak_transport_feedback(ak_str_id_t text_id, uint16_t color)
{
    send_log_uart("transport", ak_tr_lang(text_id, AK_LANG_EN)); /* logs stay English */
#if AK_ENABLE_DISPLAY
    ak_status_bar_set(ak_tr(text_id), color);
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
            ak_status_bar_set(ak_tr(AK_STR_ST_ASR_FALLBACK), AK_COL_ORANGE);
#endif
        } else if (s_voice_route.effective == AK_VOICE_CLOUD) {
            PR_NOTICE("Tuya ASR %s; using board microphone", was_fallback ? "restored" : "ready");
#if AK_ENABLE_DISPLAY
            ak_status_bar_set(was_fallback ? ak_tr(AK_STR_ST_ASR_RESTORED) : ak_tr(AK_STR_ST_ASR_READY), AK_COL_GREEN);
#endif
        } else {
            PR_NOTICE("PC ASR selected explicitly");
#if AK_ENABLE_DISPLAY
            ak_status_bar_set(ak_tr(AK_STR_ST_PC_ASR), AK_COL_CYAN);
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

/* Call after every app-thread change of s_prov: publishes the new state to
 * the Companion and the LCD. */
static void ak_prov_changed(void)
{
    uint32_t now      = tal_system_get_millisecond();
    bool     open     = ak_provision_is_open(&s_prov, now);
    bool     was_open = s_prov_open;
    s_prov_open       = open;
    s_prov_shown_s    = ak_provision_remaining_s(&s_prov, now);
    send_prov_status_uart(NULL, NULL);
    if (open && !was_open) {
        PR_NOTICE("pairing window open for %u s", (unsigned)s_prov_shown_s);
        /* A device waiting for its QR scan can show the code now. */
        send_activation_status_uart(ak_cloud_activation_state(), ak_prov_qr_url(ak_cloud_binding_qr_url()));
    } else if (!open && was_open) {
        PR_NOTICE("pairing window closed");
        ak_cloud_netcfg_close(); /* Tuya BLE/SoftAP config closes with the window */
    }
#if AK_ENABLE_DISPLAY
    if (s_settings_active) {
        ak_settings_draw();
    } else {
        ak_render_status();
    }
#endif
}

/* Confirmed on the keyboard (K6 or knob click in the pairing dialog). With a
 * License, the Tuya BLE/SoftAP netcfg can only start in a boot that the marker
 * opens, so the keyboard stores the marker and restarts; the window then opens
 * at boot. Without a License the window opens now (no Tuya App pairing yet):
 * USB writes the License inside it. */
static void ak_prov_confirm_dialog(void)
{
    uint32_t now_ms = tal_system_get_millisecond();

    if (!ak_license_present()) {
        (void)ak_provision_confirm(&s_prov, now_ms);
        return;
    }
    if (ak_reset_netcfg_mark_reopen() != OPRT_OK) {
        ak_provision_close(&s_prov);
        ak_status_bar_set(ak_tr(AK_STR_ST_PAIRING_STORAGE), AK_COL_RED);
        return;
    }
    PR_NOTICE("pairing: restarting to open the window with Tuya BLE/SoftAP netcfg");
    ak_status_bar_set(ak_tr(AK_STR_ST_PAIRING_RESTART), AK_COL_ORANGE);
    tal_system_sleep(1500U); /* let the LCD show the message before the reset */
    tal_system_reset();
}

/* Once a second while the dialog or window is up: repaint only the countdown
 * (status bar, or the Pairing row in settings), never the whole screen, so
 * the countdown does not stall UART reads while the Companion provisions. */
static void ak_prov_refresh_countdown(uint32_t now)
{
#if AK_ENABLE_DISPLAY
    if (!ak_statuslcd_ready()) {
        return;
    }
    if ((int32_t)(s_lcd_audio_guard_until_ms - now) > 0 || ak_asr_audio_busy()) {
        s_lcd_redraw_pending = true;
        return;
    }
    if (s_settings_active && s_prov.state == AK_PROV_CONFIRMING) {
        ak_settings_draw();
        return;
    }
    if (!ak_lcd_draw_begin()) {
        return;
    }
    if (s_settings_active) {
        ak_settings_draw_pairing_row(now);
    } else {
        ak_status_bar_draw();
    }
    ak_statuslcd_flush();
#else
    (void)now;
#endif
}

/* Switch ASR <-> Chat. Only called from the device (settings, or the KV value
 * restored at boot); no PC command can change the mode. */
static void ak_voice_mode_apply(ak_voice_mode_t mode, bool persist)
{
    ak_voice_mode_t before = s_voice_mode;
    if (before == AK_VOICE_MODE_CHAT && mode != AK_VOICE_MODE_CHAT) {
        ak_asr_interrupt(); /* stop a reply still playing */
    }
    s_voice_mode = mode;
#if AK_ENABLE_DISPLAY
    s_settings.voice_chat = (mode == AK_VOICE_MODE_CHAT);
    if (mode == AK_VOICE_MODE_CHAT) {
        ak_pending_confirm_exit(); /* no post-ASR edit state in Chat */
    }
    memset(&s_chat_view, 0, sizeof(s_chat_view));
    s_chat_line_count = 0U;
    s_chat_scroll     = 0U;
    s_chat_sent_reply = false;
#endif
    ak_asr_set_chat_mode(mode == AK_VOICE_MODE_CHAT);
    if (persist && before != mode) {
        uint8_t b = ak_voice_mode_to_byte(mode);
        if (ak_kv_set(AK_VOICE_MODE_KV_KEY, &b, 1U) != OPRT_OK) {
            PR_WARN("voice mode: KV write failed (applied until reboot)");
        }
    }
    PR_NOTICE("voice mode -> %s", ak_voice_mode_str(mode));
    send_voice_source();
}

/* FR-01: one interface language for the LCD, the Companion and the Chat-mode
 * agent. Called from the Settings commit, a Companion device_settings frame
 * and the boot-time KV restore (persist=false). Every change is reported to
 * the Companion at once; the last change wins on both sides. */
static void ak_language_apply(ak_lang_t lang, bool persist)
{
    ak_lang_t before = ak_lang_get();
    ak_lang_set(lang);
    s_settings.lang = (uint8_t)ak_lang_get();
    if (persist && before != ak_lang_get()) {
        uint8_t b = ak_lang_to_byte(ak_lang_get());
        if (ak_kv_set(AK_LANG_KV_KEY, &b, 1U) != OPRT_OK) {
            PR_WARN("language: KV write failed (applied until reboot)");
        }
    }
    PR_NOTICE("language -> %s", ak_lang_wire_str(ak_lang_get()));
#if AK_ENABLE_DISPLAY
    if (persist) {
        send_device_settings(); /* also the echo for a Companion request */
    }
    if (persist && before != ak_lang_get()) {
        ak_status_bar_set(ak_tr(AK_STR_NOTE_LANGUAGE), AK_COL_CYAN);
        if (s_settings_active) {
            ak_settings_draw();
        } else {
            ak_render_status();
        }
    }
#endif
}

static void ak_language_load(void)
{
    uint8_t  *blob = NULL;
    size_t    len  = 0;
    ak_lang_t lang = AK_LANG_DEFAULT;
    if (ak_kv_get(AK_LANG_KV_KEY, &blob, &len) == OPRT_OK && !ak_lang_from_blob(blob, len, &lang)) {
        PR_WARN("language: invalid KV value; using %s", ak_lang_wire_str(AK_LANG_DEFAULT));
        lang = AK_LANG_DEFAULT;
    }
    if (blob != NULL) {
        ak_kv_free(blob);
    }
    ak_language_apply(lang, false);
}

/* Poll the Chat turn: refresh the page and mirror each finished utterance /
 * reply to the Companion as display-only tuya_chat frames. */
static void ak_chat_poll(void)
{
#if AK_ENABLE_DISPLAY
    if (s_voice_mode != AK_VOICE_MODE_CHAT || !ak_asr_chat_take(&s_chat_view)) {
        return;
    }
    if (s_chat_view.turn != s_chat_sent_turn) {
        ak_leds_activity(); /* a new Chat turn is a voice event */
        s_chat_sent_turn  = s_chat_view.turn;
        s_chat_sent_reply = false;
        s_chat_scroll     = 0U;
        ak_voice_ui_set_state(AK_VOICE_UI_IDLE); /* recognized: no ASR timeout */
        if (s_chat_view.user.len > 0U) {
            send_tuya_chat_uart("user", s_chat_view.user.text);
        }
    }
    if (s_chat_view.reply_done && !s_chat_sent_reply && s_chat_view.reply.len > 0U) {
        s_chat_sent_reply = true;
        send_tuya_chat_uart("agent", s_chat_view.reply.text);
    }
    ak_chat_rebuild_lines();
    if (!s_settings_active) {
        ak_render_status(); /* deferred by the audio guard while TTS plays */
    }
#endif
}

/* ble_pair (UART, pairing window open): create a fresh BLE pairing key with
 * the secure random generator, store it in KV and hand it to the USB
 * Companion once. Any BLE session under the old key ends. */
static void ble_pair_over_uart(uint32_t seq)
{
#if AK_TRANSPORT_DUAL
    uint8_t key[AK_BLE_KEY_LEN];
    uint8_t blob[AK_BLE_KEY_BLOB_LEN];
    char    key_hex[2U * AK_BLE_KEY_LEN + 1U];
    char    key_id[AK_BLE_KEY_ID_HEX];
    char    frame[FRAME_BUFFER_SIZE];

    if (ak_secure_random(NULL, key, sizeof(key)) != 0) {
        send_prov_status_uart("ble_pair", "no_random");
        return;
    }
    ak_ble_key_to_blob(key, blob);
    OPERATE_RET rt = ak_kv_set(AK_BLE_KEY_KV_KEY, blob, sizeof(blob));
    mbedtls_platform_zeroize(blob, sizeof(blob));
    if (rt != OPRT_OK) {
        mbedtls_platform_zeroize(key, sizeof(key));
        PR_WARN("ble pair: KV write failed: %d", rt);
        send_prov_status_uart("ble_pair", "storage");
        return;
    }
    if (transport_lock()) {
        memcpy(s_ble_key, key, sizeof(s_ble_key));
        s_ble_key_set = true;
        ak_ble_sec_reset(&s_ble_sec);
        transport_unlock();
    }
    ak_ble_hex(key, sizeof(key), key_hex);
    ak_ble_key_id(key, key_id);
    mbedtls_platform_zeroize(key, sizeof(key));
    if (transport_lock()) {
        uint32_t sequence = ++s_sequence;
        int      length   = ak_proto_ble_pair_key(frame, sizeof(frame), sequence, key_hex, key_id);
        if (length > 0 && length < (int)sizeof(frame)) {
            (void)ak_uart_write(frame, (uint32_t)length); /* UART0 only, never BLE */
        }
        transport_unlock();
    }
    mbedtls_platform_zeroize(key_hex, sizeof(key_hex));
    mbedtls_platform_zeroize(frame, sizeof(frame));
    PR_NOTICE("ble pair: new pairing key %s created", key_id);
    send_ack(seq);
#if AK_ENABLE_DISPLAY
    ak_status_bar_set(ak_tr(AK_STR_ST_BLE_PAIRED), AK_COL_GREEN);
    ak_prov_refresh_countdown(tal_system_get_millisecond());
#endif
#else
    (void)seq; /* BLE pairing needs the USB link of the DUAL profile */
    PR_WARN("ble pair: not available in this build");
#endif
}

/* K6 / K4 on the "Reset network?" dialog. */
static void ak_prov_reset_resolve(bool confirm)
{
    if (!ak_provision_reset_resolve(&s_prov, confirm, tal_system_get_millisecond())) {
        PR_NOTICE("reset_netcfg: cancelled on the device");
        send_prov_status_uart("reset_netcfg", "cancelled");
        ak_prov_changed();
        return;
    }
    /* ACK first so the companion sees the request was accepted before the
     * device reboots and the link drops. */
    send_ack(s_prov_reset_seq);
    /* The reset was confirmed on the device, so reopen the window after the
     * reboot for the follow-up Wi-Fi. */
    (void)ak_reset_netcfg_mark_reopen();
    OPERATE_RET rr = ak_cloud_enter_pairing();
    if (rr != OPRT_OK) {
        (void)ak_reset_netcfg_take_reopen(); /* no reboot follows: drop the marker */
        PR_WARN("reset_netcfg: enter pairing failed or unsupported: %d", rr);
    } else {
        PR_NOTICE("reset_netcfg: entering pairing mode (reboot follows)");
    }
    ak_prov_changed();
}

/* Report lines the assemblers dropped (oversized, or containing NUL). */
static void ak_rx_report_dropped(void)
{
    static uint32_t s_reported;
    uint32_t        total = s_rx_uart.dropped_lines + s_rx_ble.dropped_lines;
    if (total != s_reported) {
        PR_WARN("rx: dropped %u oversized line(s)", (unsigned)(total - s_reported));
        s_reported = total;
    }
}

static const char *ak_rx_source_str(ak_rx_source_t source)
{
    return source == AK_RX_SRC_UART ? "uart" : "ble";
}

static void send_wifi_status_frame(const char *state, const char *ssid)
{
    char     frame[FRAME_BUFFER_SIZE];
    uint32_t sequence;
    if (transport_begin_sequenced(&sequence)) {
        transport_end_sequenced(frame, ak_proto_wifi_status(frame, sizeof(frame), sequence, state, ssid));
    }
}

static void send_license_status_frame(const char *state)
{
    char     frame[FRAME_BUFFER_SIZE];
    uint32_t sequence;
    if (transport_begin_sequenced(&sequence)) {
        transport_end_sequenced(frame, ak_proto_license_status(frame, sizeof(frame), sequence, state));
    }
}

/* Apply a keymap frame to `map`. Returns false (and leaves the caller's table
 * untouched, since it works on a copy) when any field is malformed. */
static bool ak_keymap_apply_frame(ak_keymap_t *map, const char *payload, bool *changed)
{
    static const char *const slot_ids[AK_KEYMAP_SLOTS] = {
        "k1", "k2", "k3", "k4", "k5", "k6", "enc",
    };
    static const char *const gesture_fields[AK_GESTURE_SLOT_COUNT] = {
        "single", "double", "long",
    };
    char field[16];
    char value[16];
    for (int i = 0; i < AK_KEYMAP_SLOTS; i++) {
        /* <key>.kind = voice|normal */
        snprintf(field, sizeof(field), "%s.kind", slot_ids[i]);
        ak_json_rc_t rc = ak_json_get_str(payload, field, value, sizeof(value));
        if (rc == AK_JSON_INVALID) {
            PR_WARN("keymap rejected: malformed %s", field);
            return false;
        }
        if (rc == AK_JSON_OK) {
            if (strcmp(value, "voice") == 0 && strcmp(slot_ids[i], "enc") == 0) {
                /* The EC11 stays a normal key: its long press opens Settings. */
                PR_WARN("keymap: enc cannot be a voice key; ignored");
            } else if (strcmp(value, "voice") == 0) {
                if (ak_keymap_set_voice(map, slot_ids[i])) {
                    *changed = true;
                }
            } else if (strcmp(value, "normal") == 0) {
                /* Binding any gesture below also forces NORMAL; an explicit
                 * normal with no gesture fields just clears the voice flag. */
                (void)ak_keymap_bind(map, slot_ids[i], AK_GESTURE_SLOT_SINGLE,
                                     ak_keymap_action(map, slot_ids[i], AK_GESTURE_SLOT_SINGLE));
                *changed = true;
            } else {
                PR_WARN("keymap rejected: unknown kind for %s", slot_ids[i]);
                return false;
            }
        }
        /* <key>.single/.double/.long = <action wire> */
        for (int gslot = 0; gslot < AK_GESTURE_SLOT_COUNT; gslot++) {
            snprintf(field, sizeof(field), "%s.%s", slot_ids[i], gesture_fields[gslot]);
            rc = ak_json_get_str(payload, field, value, sizeof(value));
            if (rc == AK_JSON_INVALID) {
                PR_WARN("keymap rejected: malformed %s", field);
                return false;
            }
            if (rc != AK_JSON_OK) {
                continue;
            }
            if (strcmp(slot_ids[i], "enc") == 0 && gslot == AK_GESTURE_SLOT_LONG) {
                continue; /* reserved for Settings on the device */
            }
            ak_action_t action;
            if (ak_keymap_action_from_str(value, &action)) {
                if (ak_keymap_bind(map, slot_ids[i], (ak_gesture_slot_t)gslot, action)) {
                    *changed = true;
                }
            } else {
                /* Unknown action names are skipped so a newer Companion can
                 * add actions without breaking the rest of the table. */
                PR_WARN("keymap: ignoring unknown action '%s' for %s", value, field);
            }
        }
    }
    return true;
}

#if AK_TRANSPORT_HAS_UART
/* ── Local OTA glue (ak_ota_link.h, docs/ota.md) ─────────────────────────── */
static void ota_send_uart(const char *line, int length)
{
    if (length <= 0 || !transport_lock()) {
        return;
    }
    int written = ak_uart_write(line, (uint32_t)length);
    if (written != length) {
        PR_WARN("UART0 ota frame short write: %d/%d", written, length);
    }
    transport_unlock();
}

/* OTA frames only arrive over UART0: a live Companion link there means the
 * USB cable (and so VBUS) is present. */
static bool ota_usb_power(void)
{
    /* CHG_DET_N low (ak_battery, 94b9c8e) also proves VBUS: the charger only
     * charges from USB. It is high once the cell is full, so it can only add
     * to the link test, never veto it. */
    ak_battery_info_t bi;
    bool              charging = false;
    if (ak_battery_available()) {
        ak_battery_get_info(&bi);
        charging = bi.charging;
    }
    return s_pc_connected || charging;
}

/* shortcut: the battery gauge (94b9c8e) is filtered but uncalibrated
 * (AK_BAT_CAL_PERMILLE default, no "reading trusted" flag), so no battery
 * percent counts as trusted yet and an update needs USB power, which an OTA
 * over USB always has. Upgrade when ak_battery exports a trusted flag. */
static bool ota_battery_trusted(uint8_t *percent)
{
    *percent = ak_battery_available() ? ak_battery_percent() : 0U;
    return false;
}

static void ota_ui(ak_ota_ui_t what, const char *text)
{
#if AK_ENABLE_DISPLAY
    /* Confirm / progress / installing are the full-screen ⑦ dialog
     * (ak_draw_ota); the outcome stays on the status bar afterwards.
     * FAILED carries the reason word, UPDATED the version. */
    char line[64];
    if (what == AK_OTA_UI_FAILED) {
        (void)snprintf(line, sizeof(line), ak_tr(AK_STR_OTA_FAILED), text);
        ak_status_bar_set(line, AK_COL_RED);
    } else if (what == AK_OTA_UI_UPDATED) {
        (void)snprintf(line, sizeof(line), ak_tr(AK_STR_OTA_UPDATED), text);
        ak_status_bar_set(line, AK_COL_GREEN);
    }
    if (what == AK_OTA_UI_CONFIRM) {
        ak_leds_activity(); /* the question wakes Auto LED power */
    }
    ak_ui_sync(tal_system_get_millisecond());
    ak_render_status();
#else
    (void)what;
    (void)text;
#endif
}

static const ak_ota_link_cb_t s_ota_cb = {
    ota_send_uart, ota_usb_power, ota_battery_trusted, ota_ui,
};
#endif

/* Handle one inbound JSONL frame (PC -> board). `source` is the link the whole
 * line arrived on: every byte of it came through that one ak_rx reader (BLE
 * frames have already been decrypted and authenticated, see on_rx_line).
 * Malformed frames are dropped; unknown types are ignored. */
static void on_rx_frame(const char *line, ak_rx_source_t source)
{
    ak_rx_frame_t frame;
    if (!ak_rx_parse_frame(line, &frame)) {
        PR_WARN("rx(%s): malformed frame dropped", ak_rx_source_str(source));
        return;
    }
    const char    *type      = frame.type;
    const char    *payload   = frame.payload;
    const uint32_t seq       = frame.seq;
    const bool     from_uart = (source == AK_RX_SRC_UART);

    /* Provisioning commands (wifi/license/license_read/reset_netcfg) are only
     * accepted when the whole line came from the physically attached UART. */
    if (!ak_rx_source_allows(type, source)) {
        PR_WARN("%s rejected from %s: USB transport required", type, ak_rx_source_str(source));
        return;
    }
#if AK_TRANSPORT_HAS_UART
    /* Firmware update and debug probes: UART0 only (gate above). */
    if (from_uart && ak_ota_link_on_frame(type, payload, tal_system_get_millisecond())) {
        return;
    }
#endif
    /* ...and only while the user has opened the pairing window on the device. */
    if (ak_rx_type_is_sensitive(type)) {
        ak_prov_cmd_t cmd = strcmp(type, "wifi") == 0           ? AK_PROV_CMD_WIFI
                            : strcmp(type, "license") == 0      ? AK_PROV_CMD_LICENSE
                            : strcmp(type, "license_read") == 0 ? AK_PROV_CMD_LICENSE_READ
                            : strcmp(type, "ble_pair") == 0     ? AK_PROV_CMD_BLE_PAIR
                                                                : AK_PROV_CMD_RESET;
        ak_prov_verdict_t verdict =
            ak_provision_check(&s_prov, cmd, source, cmd == AK_PROV_CMD_LICENSE && ak_license_present(),
                               tal_system_get_millisecond());
        if (verdict == AK_PROV_DENY_EXISTS) {
            /* FW-02: keep the stored License unless the user allowed one
             * overwrite on the device (Settings > Credentials > K6). */
            PR_WARN("license write rejected: a License is stored and overwrite is not allowed");
            send_license_status_frame("rejected");
            send_prov_status_uart(type, ak_provision_verdict_str(verdict));
#if AK_ENABLE_DISPLAY
            ak_status_bar_set(ak_tr(AK_STR_ST_LICENSE_EXISTS), AK_COL_ORANGE);
            ak_prov_refresh_countdown(tal_system_get_millisecond());
#endif
            return;
        }
        if (verdict != AK_PROV_ALLOW && verdict != AK_PROV_NEEDS_DEVICE_CONFIRM) {
            PR_WARN("%s rejected: pairing window %s", type, ak_provision_verdict_str(verdict));
            send_prov_status_uart(type, ak_provision_verdict_str(verdict));
#if AK_ENABLE_DISPLAY
            ak_status_bar_set(ak_tr(AK_STR_ST_PAIRING_FIRST), AK_COL_ORANGE);
            ak_prov_refresh_countdown(tal_system_get_millisecond());
#endif
            return;
        }
    }

    if (strcmp(type, "hello") == 0) {
        /* Only a greeting received over UART proves the USB companion link is
         * up. In DUAL mode a BLE hello must not light the USB icon. */
        if (from_uart) {
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
                    ak_rx_reader_reset(&s_rx_ble);
                ble_sec_drop();
                }
                PR_NOTICE("dual: Companion selected USB; AK BLE disabled for this boot");
            }
#endif
            if (!s_pc_connected) {
                s_pc_connected = true;
                ak_transport_feedback(AK_STR_ST_USB_CONNECTED, AK_COL_GREEN);
                send_system_info_uart();
            }
        }
        /* Respond only after the selected transport is locked, so a USB hello
         * is never mirrored to BLE during the transition. */
        send_hello_ack();
#if AK_TRANSPORT_HAS_UART
        if (from_uart) {
            ak_ota_link_on_hello(); /* OTA availability, or "installed" once */
        }
#endif
    } else if (strcmp(type, "ping") == 0) {
        send_pong();
    } else if (strcmp(type, "bye") == 0) {
        /* The PC companion is disconnecting gracefully. Plain UART gives no
         * hardware disconnect edge, so this frame is how the board learns the
         * link is going down -> flip the header to "USB Disconnected". */
        if (from_uart && s_pc_connected) {
            s_pc_connected = false;
            ak_transport_feedback(AK_STR_ST_USB_DISCONNECTED, AK_COL_ORANGE);
            send_system_info_uart();
        }
    } else if (strcmp(type, "config") == 0) {
        /* The PC selects the voice path here: {"voice_engine":"cloud"|"system"}
         * (the companion mirrors its choice to the board via config). */
        char         engine[16] = "";
        ak_json_rc_t rc         = ak_json_get_str(payload, "voice_engine", engine, sizeof(engine));
        if (rc == AK_JSON_OK) {
            ak_voice_source_t want;
            if (strcmp(engine, "cloud") == 0) {
                want = AK_VOICE_CLOUD;
            } else if (strcmp(engine, "system") == 0) {
                want = AK_VOICE_SYSTEM;
            } else {
                PR_WARN("config rejected: unknown voice_engine");
                return;
            }
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
        } else if (rc == AK_JSON_INVALID) {
            PR_WARN("config rejected: malformed voice_engine");
            return;
        }
        send_ack(seq);
    } else if (strcmp(type, "keymap") == 0) {
        /* Plan A (stage 乙): Companion downlinks per-gesture bindings via flat
         * dotted fields — "<key>.kind"="voice" makes a key hold-to-talk;
         * "<key>.single"/".double"/".long"=<action wire> binds a gesture slot.
         * Omitted fields keep the board's current binding (partial update).
         * The frame applies atomically: a malformed field rejects all of it.
         * The resolved table is cached in TAL KV and acked. */
        ak_keymap_t next    = s_keymap;
        bool        changed = false;
        if (!ak_keymap_apply_frame(&next, payload, &changed)) {
            return;
        }
        s_keymap = next;
        if (changed) {
            /* A kind/binding change can move a slot between the voice-edge path
             * and the gesture classifier; re-init all classifiers so none keeps
             * stale half-state. */
            for (int gi = 0; gi < AK_KEYMAP_SLOTS; gi++) {
                ak_gesture_init(&s_key_gesture[gi]);
            }
            uint8_t  blob[AK_KEYMAP_BLOB_SIZE];
            unsigned n = ak_keymap_to_blob(&s_keymap, blob, sizeof(blob));
            if (n == AK_KEYMAP_BLOB_SIZE) {
                if (ak_kv_set(AK_KEYMAP_KV_KEY, blob, n) == OPRT_OK) {
                    PR_NOTICE("keymap: applied + cached %u-byte blob", n);
                } else {
                    PR_WARN("keymap: KV cache write failed (applied in RAM)");
                }
            }
        }
        send_ack(seq);
    } else if (strcmp(type, "led") == 0) {
        /* No Companion-driven LED effects exist yet; a led frame still counts
         * as something to show and wakes Auto LED power (FW-05 follow-up). */
        ak_leds_activity();
    } else if (strcmp(type, "device_settings") == 0) {
#if AK_ENABLE_DISPLAY
        uint32_t     volume     = 0U;
        uint32_t     brightness = 0U;
        ak_json_rc_t vol_rc     = ak_json_get_uint(payload, "volume", 100U, &volume);
        ak_json_rc_t bri_rc     = ak_json_get_uint(payload, "brightness", 100U, &brightness);
        char         lang_str[8];
        ak_json_rc_t lang_rc    = ak_json_get_str(payload, "language", lang_str, sizeof(lang_str));
        ak_lang_t    lang       = AK_LANG_DEFAULT;
        bool         changed    = false;

        if (vol_rc == AK_JSON_INVALID || bri_rc == AK_JSON_INVALID ||
            (lang_rc == AK_JSON_OK && !ak_lang_from_wire(lang_str, &lang)) || lang_rc == AK_JSON_INVALID) {
            PR_WARN("device settings rejected: volume/brightness 0..100, language zh/en");
            return;
        }
        if (lang_rc == AK_JSON_OK) {
            /* Last change wins; ak_language_apply reports the state and redraws. */
            ak_language_apply(lang, true);
        }
        if (vol_rc == AK_JSON_OK) {
            if (ak_asr_set_volume((uint8_t)volume) == OPRT_OK) {
                s_settings.volume = (uint8_t)volume;
                changed           = true;
            } else {
                PR_WARN("device settings: volume apply failed");
            }
        }
        if (bri_rc == AK_JSON_OK) {
            uint8_t level = (uint8_t)brightness;
            if (level < AK_SETTINGS_BRIGHTNESS_MIN) {
                level = AK_SETTINGS_BRIGHTNESS_MIN;
            }
            if (ak_statuslcd_set_brightness(level) == OPRT_OK) {
                s_settings.brightness = level;
                changed               = true;
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
        if (vol_rc == AK_JSON_OK || bri_rc == AK_JSON_OK) {
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
            /* Display-only label: clip to the LCD buffer at a UTF-8 boundary. */
            if (ak_json_get_str_clip(payload, fields[i], alias, sizeof(alias)) == AK_JSON_OK) {
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
        /* PC provisions Wi-Fi: {"type":"wifi","payload":{"ssid":"..","pass":".."}}.
         * Apply via ak_wifi (netmgr) so the board connects + cloud-activates;
         * the connect result comes back asynchronously as a wifi_status frame.
         * Fields that do not fit or fail the format check are rejected, never
         * truncated (FW-06/FW-12). */
        char ssid[AK_WIFI_SSID_MAX + 1U] = "";
        char pass[AK_WIFI_PASS_MAX + 1U] = "";
        if (ak_json_get_str(payload, "ssid", ssid, sizeof(ssid)) != AK_JSON_OK ||
            ak_json_get_str(payload, "pass", pass, sizeof(pass)) != AK_JSON_OK ||
            !ak_wifi_ssid_valid(ssid) || !ak_wifi_pass_valid(pass)) {
            PR_WARN("wifi config rejected: ssid must be 1..%u bytes, pass empty or %u..%u chars",
                    (unsigned)AK_WIFI_SSID_MAX, (unsigned)AK_WIFI_PASS_MIN, (unsigned)AK_WIFI_PASS_MAX);
            send_wifi_status_frame("failed", "");
            return;
        }
        PR_NOTICE("wifi config: ssid=%s (pass %u chars)", ssid, (unsigned)strlen(pass));
        ak_wifi_connect(ssid, pass);
        send_ack(seq);
    } else if (strcmp(type, "license") == 0) {
        /* PC provisions the device license: {"type":"license","payload":
         * {"uuid":"..","authkey":".."}}. Written to persistent storage; PID
         * stays compile-time. Never echo the secret back — only a status. */
        char uuid[AK_LICENSE_UUID_LEN + 1U]   = "";
        char authkey[AK_LICENSE_KEY_LEN + 1U] = "";
        if (ak_json_get_str(payload, "uuid", uuid, sizeof(uuid)) != AK_JSON_OK ||
            ak_json_get_str(payload, "authkey", authkey, sizeof(authkey)) != AK_JSON_OK ||
            !ak_license_uuid_valid(uuid) || !ak_license_authkey_valid(authkey)) {
            PR_WARN("license write rejected: uuid must be %u/%u and authkey %u chars of [A-Za-z0-9]",
                    (unsigned)AK_LICENSE_UUID_LEN, (unsigned)AK_LICENSE_UUID_LEN_SHORT,
                    (unsigned)AK_LICENSE_KEY_LEN);
            send_license_status_frame("failed");
            return;
        }
        PR_NOTICE("license write: uuid len %u, authkey len %u", (unsigned)strlen(uuid),
                  (unsigned)strlen(authkey));
        ak_license_result_t lr = ak_license_write(uuid, authkey);
        memset(authkey, 0, sizeof(authkey));
        ak_provision_consume_overwrite(&s_prov);
        send_license_status_frame(ak_license_result_str(lr));
        send_ack(seq);
        send_system_info_uart();
#if AK_ENABLE_DISPLAY
        if (lr == AK_LICENSE_WRITTEN_REBOOT_REQUIRED) {
            /* The running IoT client keeps the identity it booted with. */
            ak_status_bar_set(ak_tr(AK_STR_ST_LICENSE_SAVED), AK_COL_GREEN);
        } else {
            ak_status_bar_set(ak_tr(AK_STR_ST_LICENSE_WRITE_FAILED), AK_COL_RED);
        }
        ak_prov_refresh_countdown(tal_system_get_millisecond());
#endif
    } else if (strcmp(type, "ble_pair") == 0) {
        ble_pair_over_uart(seq);
    } else if (strcmp(type, "license_read") == 0) {
#if AK_ENABLE_LICENSE_DEBUG_READ
        send_license_info_uart();
        send_ack(seq);
#else
        PR_WARN("license_read is not available in this build");
#endif
    } else if (strcmp(type, "reset_netcfg") == 0) {
        /* Companion "enter pairing" — clear Wi-Fi/cloud binding and reboot into
         * provisioning. Destructive, so it may only enter through the physically
         * trusted USB/UART transport (same gate as wifi/license). The License
         * (UUID/AuthKey) is preserved; only Wi-Fi + cloud binding are cleared. */
        /* FW-12: even inside the window, the device asks once more (K6 /
         * K1, 10 s). ak_prov_reset_resolve() runs it. */
        ak_provision_reset_request(&s_prov, tal_system_get_millisecond());
        s_prov_reset_seq = seq;
        send_prov_status_uart(type, ak_provision_verdict_str(AK_PROV_NEEDS_DEVICE_CONFIRM));
        ak_prov_changed();
    } else if (strcmp(type, "menu") == 0 || strcmp(type, "menu_close") == 0 || strcmp(type, "notice") == 0 ||
               strcmp(type, "menu_parked") == 0) {
        /* LCD menu frames: USB, or BLE after the secure channel (on_rx_line
         * only delivers authenticated BLE plaintext, <= 511 bytes). */
#if AK_ENABLE_DISPLAY
        ak_menu_on_frame(type, payload);
#endif
    } else if (strcmp(type, "input_restore") == 0) {
        /* A direct send of the ③ text failed on the PC: back to ③ with it so
         * it can be sent again or cancelled. Only from standby (①). */
#if AK_ENABLE_DISPLAY
        char     text[ASR_TRANSCRIPT_MAX];
        char     msg[AK_MENU_NOTICE_MAX + 1U] = "";
        uint32_t now = tal_system_get_millisecond();
        if (ak_json_get_str_clip(payload, "text", text, sizeof(text)) != AK_JSON_OK || text[0] == '\0') {
            PR_WARN("input_restore rejected: no text");
            return;
        }
        (void)ak_json_get_str_clip(payload, "msg", msg, sizeof(msg));
        if (!ak_ui_input_restore(&s_ui, now)) {
            PR_WARN("input_restore ignored: not in standby");
            return;
        }
        (void)snprintf(s_input_text, sizeof(s_input_text), "%s", text);
        ak_leds_activity();
        ak_status_bar_set(msg[0] != '\0' ? msg : ak_tr(AK_STR_ST_INPUT_NOT_SENT), AK_COL_RED);
        ak_ui_sync(now);
        ak_render_status();
#endif
    } else if (strcmp(type, "display") == 0) {
        uint32_t chat_slot = 0U;
        char     agent[32] = "";
        char     state[16] = "";
        char     text[128] = "";
        if (ak_json_get_uint(payload, "chat_slot", AK_CHAT_SLOT_COUNT, &chat_slot) != AK_JSON_OK ||
            chat_slot < 1U) {
            PR_WARN("display rejected: chat_slot must be 1..%u", (unsigned)AK_CHAT_SLOT_COUNT);
            return;
        }
        /* Display-only text: clip to the LCD buffers at a UTF-8 boundary. */
        (void)ak_json_get_str_clip(payload, "agent_name", agent, sizeof(agent));
        (void)ak_json_get_str_clip(payload, "state", state, sizeof(state));
        (void)ak_json_get_str_clip(payload, "text", text, sizeof(text));
        uint8_t session_index = (uint8_t)(chat_slot - 1U);
        PR_NOTICE("display: chat=%c agent=%s state=%s", (char)('A' + session_index), agent, state);
#if AK_ENABLE_DISPLAY
        ak_session_ui_t *session = &s_sessions[session_index];
        /* A slot state change (working -> waiting_for_user, an error, ...) is
         * something to show: wake Auto LED power. Text-only updates are not. */
        if (strcmp(session->state, state) != 0) {
            ak_leds_activity();
        }
        snprintf(session->agent, sizeof(session->agent), "%s", agent);
        snprintf(session->state, sizeof(session->state), "%s", state);
        ak_chat_history_add(session, text, true);
        if (session_index == s_selected_session) {
            ak_render_status();
        } else {
            /* The sidebar shows all slots, so repaint non-selected status changes too. */
            s_lcd_redraw_pending = true;
        }
#endif
    }
    /* ack / pong / unknown: ignore per protocol */
}

/* Every assembled line enters here. UART lines are processed directly; BLE
 * lines must pass the secure channel first: unpaired or unauthenticated BLE
 * links get nothing processed and nothing sent back except the handshake. */
static void on_rx_line(const char *line, ak_rx_source_t source)
{
#if AK_TRANSPORT_DUAL || AK_TRANSPORT_BLE_GATT
    if (source == AK_RX_SRC_BLE) {
        static char         plain[AK_RX_LINE_MAX];
        static char         reply[160];
        ak_ble_sec_action_t act = AK_BLE_SEC_DROP;
        if (!transport_lock()) {
            return;
        }
        act = ak_ble_sec_on_line(&s_ble_sec, s_ble_key_set ? s_ble_key : NULL, line, ak_secure_random, NULL, plain,
                                 sizeof(plain), reply, sizeof(reply));
        if (act == AK_BLE_SEC_REPLY) {
            (void)ak_ble_gatt_write(reply, (uint32_t)strlen(reply)); /* handshake only, carries a nonce */
        }
        transport_unlock();
        if (act == AK_BLE_SEC_AUTH_OK) {
            PR_NOTICE("ble sec: Companion authenticated");
        } else if (act == AK_BLE_SEC_DROP && s_ble_sec.state == AK_BLE_SEC_FAILED) {
            PR_WARN("ble sec: rejected BLE input; link ignored until disconnect");
        }
        if (act == AK_BLE_SEC_DELIVER || act == AK_BLE_SEC_AUTH_OK) {
            on_rx_frame(plain, AK_RX_SRC_BLE);
        }
        return;
    }
#endif
    on_rx_frame(line, source);
}

static void user_main(void)
{
    OPERATE_RET rt;
    uint32_t    now;
    uint32_t    last_heartbeat;

    tal_log_init(TAL_LOG_LEVEL_DEBUG, 2048, (TAL_LOG_OUTPUT_CB)tkl_log_output);
    /* Mount KV before ANY KV access, in every profile: littlefs faults on
     * use before mount (boot loop seen on hardware). Failure is not fatal;
     * ak_kv_* then fails cleanly and settings use their defaults. */
    (void)ak_kv_init();
    ak_language_load(); /* FR-01: before the first LCD draw; needs ak_kv_init */

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
#if AK_ENABLE_DISPLAY
    ak_menu_init(&s_menu); /* RAM only */
    ak_ui_init(&s_ui);
#endif
    ak_keymap_init_defaults(&s_keymap);
    /* Plan A: a Companion-provided keymap cached in TAL KV overrides the
     * defaults so a disconnected board keeps the last map. A missing/corrupt
     * blob leaves the v0.3 defaults in place. */
    {
        uint8_t *kv_buf = NULL;
        size_t   kv_len = 0;
        if (ak_kv_get(AK_KEYMAP_KV_KEY, &kv_buf, &kv_len) == OPRT_OK && kv_buf != NULL) {
            if (ak_keymap_from_blob(&s_keymap, kv_buf, (unsigned)kv_len)) {
                PR_NOTICE("keymap: loaded Companion override from KV");
            } else {
                PR_WARN("keymap: KV blob invalid, using v0.3 defaults");
            }
            ak_kv_free(kv_buf);
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
        ak_status_bar_set(ak_tr(AK_STR_ST_LCD_READY), AK_COL_GREEN);
        ak_render_status();
    }
    /* Battery fuel gauge (ADC). Optional: on failure the header omits the
     * gauge and everything else keeps working. */
    if (ak_battery_init() != OPRT_OK) {
        PR_WARN("battery ADC init failed; header gauge disabled");
    }
    /* WS2812 chain (test page, pending-confirm hints). Optional. */
    if (ak_leds_init() != OPRT_OK) {
        PR_WARN("WS2812 init failed; test page LED block disabled");
    }
    ak_led_setting_load();
#endif

#if AK_ENABLE_DISPLAY
    ak_status_bar_set(ak_tr(AK_STR_ST_CLOUD_STARTING), AK_COL_CYAN);
    ak_render_status();
#endif

    /* Tuya IoT / cloud bring-up (stub unless AK_ASR_BOARD_CLOUD is set). MUST run
     * BEFORE ak_asr_init: ai_chat_init needs an initialized IoT / AI-service
     * context (tuya_iot_init + netmgr), or it dereferences a NULL handle and
     * hard-faults at boot (the black-screen crash). No-op on the v1/BLE profiles. */
    OPERATE_RET cloud_rt = ak_cloud_init();
#if AK_ENABLE_DISPLAY
    ak_status_bar_set(cloud_rt == OPRT_OK ? ak_tr(AK_STR_ST_CLOUD_OK)
                                          : ak_tr(AK_STR_ST_CLOUD_FAILED),
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
    /* The T5 exposes one global BLE callback set. While the Tuya device is
     * unactivated (the SDK's provisioning state), leave BLE with Tuya for the
     * phone app; UART0 remains available to Companion. Once provisioning has
     * completed, start AK BLE so Companion can choose BLE. If Companion chooses
     * USB, its hello frame stops AK advertising and locks this boot to USB.
     * Wi-Fi/MQTT availability is deliberately not part of this decision. */
    OPERATE_RET ble_rt = OPRT_OK;
#if AK_ASR_BOARD_CLOUD
    if (cloud_rt != OPRT_OK || !ak_cloud_network_ready() || !ak_cloud_activated()) {
        /* Unactivated, or the cloud stack did not come up: Tuya BLE (or nothing)
         * owns the radio. Taking the callbacks here would silently block the
         * phone app. The runtime path below takes over once activation completes. */
        PR_NOTICE("dual: AK BLE deferred until activation; Tuya BLE/SoftAP provisioning owns the radio (UART0 available)");
    } else {
        ble_rt = ak_ble_gatt_takeover();
        s_ak_ble_active = (ble_rt == OPRT_OK);
    }
#else
    ble_rt = ak_ble_gatt_init();
    s_ak_ble_active = (ble_rt == OPRT_OK);
#endif
    if (ble_rt != OPRT_OK) {
        PR_ERR("dual: delayed AK BLE setup failed: %d (UART remains available)", ble_rt);
        ak_transport_feedback(AK_STR_ST_BLE_INIT_FAILED, AK_COL_ORANGE);
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
        ak_status_bar_set(ak_tr(AK_STR_ST_ASR_STARTING), AK_COL_CYAN);
        ak_render_status();
#endif
        OPERATE_RET asr_rt = ak_asr_init(on_asr_text, on_asr_error);
        (void)asr_rt;
#if AK_ENABLE_DISPLAY
        if (asr_rt == OPRT_OK) {
            s_settings.volume = ak_asr_get_volume();
        }
        ak_status_bar_set(asr_rt == OPRT_OK ? ak_tr(AK_STR_ST_ASR_OK)
                                            : ak_tr(AK_STR_ST_ASR_INIT_FAILED),
                          asr_rt == OPRT_OK ? AK_COL_GREEN : AK_COL_RED);
        ak_render_status();
#endif
    }
    PR_NOTICE("Voice preference: %s (cloud backend %s)", ak_voice_source_str(s_voice_route.preferred),
              ak_asr_available() ? "ready" : "unavailable");

    /* Wi-Fi provisioning (netmgr in the cloud profile, stub otherwise). The PC
     * pushes credentials over a `wifi` frame; results return as wifi_status. */
#if AK_ENABLE_DISPLAY
    ak_status_bar_set(ak_tr(AK_STR_ST_WIFI_STARTING), AK_COL_CYAN);
    ak_render_status();
#endif
    OPERATE_RET wifi_rt = ak_wifi_init(on_wifi_status);
    (void)wifi_rt;
    PR_NOTICE("Wi-Fi provisioning: %s", ak_wifi_available() ? "ready" : "stub/off");
#if AK_ENABLE_DISPLAY
    if (wifi_rt != OPRT_OK) {
        ak_status_bar_set(ak_tr(AK_STR_ST_WIFI_INIT_FAILED), AK_COL_RED);
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

    /* A reset the user confirmed physically (three fast power cycles, or
     * reset_netcfg inside an open window) asked for the pairing window. */
    ak_provision_init(&s_prov);
#if AK_TRANSPORT_DUAL || AK_TRANSPORT_BLE_GATT
    ak_ble_sec_reset(&s_ble_sec);
    ble_key_load();
#endif
    if (cloud_rt == OPRT_OK && ak_cloud_boot_window()) {
        ak_provision_open(&s_prov, tal_system_get_millisecond());
        ak_prov_changed();
    }

    /* Q6: restore the last voice mode and say once which one is active. */
    {
        uint8_t        *blob = NULL;
        size_t          len  = 0;
        ak_voice_mode_t mode = AK_VOICE_MODE_ASR;
        if (ak_kv_get(AK_VOICE_MODE_KV_KEY, &blob, &len) == OPRT_OK) {
            if (!ak_voice_mode_from_blob(blob, len, &mode)) {
                PR_WARN("voice mode: invalid KV value; using ASR");
                mode = AK_VOICE_MODE_ASR;
            }
        }
        if (blob != NULL) {
            ak_kv_free(blob);
        }
        ak_voice_mode_apply(mode, false);
#if AK_ENABLE_DISPLAY
        ak_status_bar_set(mode == AK_VOICE_MODE_CHAT ? ak_tr(AK_STR_ST_BOOT_CHAT)
                                                     : ak_tr(AK_STR_ST_BOOT_ASR),
                          AK_COL_CYAN);
        ak_render_status();
#endif
    }

#if AK_TRANSPORT_HAS_UART
    /* Install markers + "Updated to x.y.z" after the voice-mode line, so the
     * notice is the one left on the status bar. */
    ak_ota_link_init(&s_ota_cb);
#endif

    /* Default preference is Tuya ASR. Resolve it now: if the cloud/AI agent is
     * not ready yet, use PC ASR immediately and monitor for recovery below. */
    ak_voice_route_update(true);

    now                         = tal_system_get_millisecond();
    last_heartbeat              = now;
    s_voice_route_last_check_ms = now;

    ak_rx_reader_init(&s_rx_uart, AK_RX_SRC_UART);
    ak_rx_reader_init(&s_rx_ble, AK_RX_SRC_BLE);
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
        /* Reclaim a finished background flush without ever sleeping, so the
         * next redraw rarely has to wait and the loop keeps reading UART. */
        ak_statuslcd_flush_poll();
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
                ak_status_bar_set(ak_tr(AK_STR_ST_SCAN_QR), AK_COL_CYAN);
            } else if (strcmp(state, "token_received") == 0) {
                ak_status_bar_set(ak_tr(AK_STR_ST_ACTIVATING), AK_COL_CYAN);
            } else if (strcmp(state, "activated") == 0) {
                ak_status_bar_set(ak_tr(AK_STR_ST_ACTIVATED), AK_COL_GREEN);
            } else if (strcmp(state, "cloud_ready") == 0) {
                ak_status_bar_set(ak_tr(AK_STR_ST_CLOUD_READY), AK_COL_GREEN);
            } else if (strcmp(state, "cloud_disconnected") == 0) {
                ak_status_bar_set(ak_tr(AK_STR_ST_CLOUD_DISCONNECTED), AK_COL_ORANGE);
            } else if (strcmp(state, "resetting") == 0) {
                ak_status_bar_set(ak_tr(AK_STR_ST_RESETTING), AK_COL_ORANGE);
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
                    provisioning ? AK_STR_ST_PAIRING_TUYA_APP
                                 : AK_STR_ST_PROVISIONED,
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
        /* Before activation, Tuya BLE may advertise only inside a pairing window.
         * Its stack starts at boot and its advertising is not gated by the SDK,
         * so stop it whenever the window is closed (checked every loop pass, a
         * cheap host-state read; shortcut: upgrade to an SDK gate if one exists). */
        if (!ak_cloud_activated() && !ak_provision_is_open(&s_prov, now)) {
            ak_ble_gatt_stop_adv();
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
                        ak_transport_feedback(AK_STR_ST_BLE_CONNECTED, AK_COL_GREEN);
                        break;
                    case AK_BLE_LINK_CONNECTED:
                        ak_transport_feedback(AK_STR_ST_BLE_SUBSCRIBING, AK_COL_DKBLUE);
                        break;
                    case AK_BLE_LINK_ADVERTISING:
                    default:
                        ak_transport_feedback(
                            previous == (ak_ble_link_state_t)-1
                                ? AK_STR_ST_BLE_ADVERTISING
                                : AK_STR_ST_BLE_DISCONNECTED,
                            AK_COL_ORANGE);
                        break;
                    }
                    send_system_info_uart();
                }
            }
        }
#endif

        ak_input_poll(now, on_input_event, on_encoder_event);

        /* Resolve a pending single-click once its double-click window lapses with
         * no second press (scheme X). Every NORMAL key runs the classifier; the
         * VOICE key does not. */
        for (int gi = 0; gi < AK_KEYMAP_SLOTS; gi++) {
            if (s_keymap.entries[gi].kind != AK_KEY_KIND_NORMAL) {
                continue;
            }
            ak_gesture_t g = ak_gesture_tick(&s_key_gesture[gi], now);
            if (g != AK_GESTURE_NONE) {
                const char *id = ak_keymap_id_for_slot(gi);
                dispatch_gesture_for_slot(id, g);
            }
        }

#if AK_ENABLE_DISPLAY
        /* Post-ASR pending-confirm timeout; LCD menu timers + key lights. */
        ak_pending_confirm_tick(now);
        ak_menu_loop_tick(now);
#endif
        ak_leds_tick(now);

        ak_wifi_tick(now);
        ak_chat_poll();

        /* Pairing window: cloud_ready shortens it, then it expires on time. */
        if (s_prov_cloud_ready_pending) {
            s_prov_cloud_ready_pending = false;
            ak_provision_on_cloud_ready(&s_prov, now);
        }
        if (ak_provision_tick(&s_prov, now)) {
            ak_prov_changed();
        } else if (s_prov.state != AK_PROV_CLOSED &&
                   ak_provision_remaining_s(&s_prov, now) != s_prov_shown_s) {
            s_prov_shown_s = ak_provision_remaining_s(&s_prov, now);
            ak_prov_refresh_countdown(now);
        }

        uint8_t rx[64];
#if AK_TRANSPORT_HAS_UART
        ak_ota_link_tick(now);
        /* An OTA transfer streams ~11 KB/s: drain UART0 fully while it runs
         * (one 64-byte read per 5 ms tick would fall behind). */
        for (int burst = 0; burst < (ak_ota_link_busy() ? 64 : 1); burst++) {
            int got = ak_transport_read_uart(rx, sizeof(rx));
            if (got <= 0) {
                break;
            }
            /* The companion answers the board's 2 s ping with pong, so any
             * UART traffic proves the USB protocol link is still alive. */
            s_usb_last_rx_ms = now;
            ak_rx_reader_feed(&s_rx_uart, rx, (size_t)got, on_rx_line);
        }
#endif
#if AK_TRANSPORT_HAS_BLE
        {
            /* A half line left by one central must never be completed by the
             * next one: drop BLE partial lines whenever the link is not live. */
            bool ble_live = AK_BLE_RUNTIME_ACTIVE() && ak_ble_gatt_link_state() != AK_BLE_LINK_ADVERTISING;
            if (!ble_live) {
                ak_rx_reader_reset(&s_rx_ble);
                ble_sec_drop();
            }
            int got_ble = ak_transport_read_ble(rx, sizeof(rx));
            if (ak_ble_gatt_take_link_reset()) {
                ak_rx_reader_reset(&s_rx_ble);
                ble_sec_drop(); /* the central changed under us */
            }
            if (got_ble > 0) {
                ak_rx_reader_feed(&s_rx_ble, rx, (size_t)got_ble, on_rx_line);
            }
        }
#endif
        ak_rx_report_dropped();

#if AK_TRANSPORT_UART || AK_TRANSPORT_DUAL
        if (s_pc_connected &&
            (uint32_t)(now - s_usb_last_rx_ms) >= USB_LINK_TIMEOUT_MS) {
            s_pc_connected = false;
            PR_NOTICE("USB companion timed out; marking link disconnected");
            ak_transport_feedback(AK_STR_ST_USB_TIMEOUT, AK_COL_ORANGE);
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
