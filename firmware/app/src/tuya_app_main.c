/**
 * @file tuya_app_main.c
 * @brief Agentic Keyboard for Kiro Crew -- product firmware entry.
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
#include "ak_i18n.h"
#include "ak_text.h"
#include "ak_layout.h"
#include "ak_palette.h"
#include "ak_asr.h"
#include "ak_cloud.h"
#include "ak_combo.h"
#include "ak_cred.h"
#include "ak_pid.h"
#include "ak_wifi.h"
#include "ak_input.h"
#include "ak_keymap.h"
#include "ak_menu.h"
#include "ak_bind.h"
#include "ak_inputbar.h"
#include "ak_inputbar_draw.h"
#include "ak_ui.h"
#include "ak_icons16.h"
#include "ak_session_icons.h"
#include "ak_ghost.h"
#include "ak_ghost_frames.h"
#include "ak_protocol.h"
#include "ak_provision.h"
#include "ak_factory_reset.h"
#include "ak_reset_netcfg.h"
#include "ak_rx.h"
#include "ak_validate.h"
#include "ak_scroll.h"
#include "ak_settings.h"
#include "ak_statuslcd.h"
#include "ak_voice_route.h"
#include "ak_kv.h"
#include "ak_splash.h"
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
#define FRAME_BUFFER_SIZE         AK_FRAME_BUFFER_SIZE  /* ak_protocol.h: sized by test_frame_budgets */
#define SYSTEM_INFO_FRAME_BUFFER_SIZE AK_FRAME_BUFFER_LARGE
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
/* The Companion's hello said it returns PC ASR text (`pc_asr`): in the system
 * route the keyboard then waits in ② after the voice key (docs/pc-asr.md). */
static bool s_pc_asr_results = false;
/* A Companion link exists, so K6 / K4 frames reach somebody. */
static bool ak_companion_linked(void)
{
    bool linked = false;
#if AK_TRANSPORT_UART || AK_TRANSPORT_DUAL
    linked = s_pc_connected;
#endif
#if AK_TRANSPORT_BLE_GATT || AK_TRANSPORT_DUAL
#if AK_TRANSPORT_DUAL
    linked = linked || (s_ak_ble_active && ak_ble_gatt_link_state() != AK_BLE_LINK_ADVERTISING);
#else
    linked = linked || ak_ble_gatt_link_state() != AK_BLE_LINK_ADVERTISING;
#endif
#endif
    return linked;
}

#if AK_TRANSPORT_UART || AK_TRANSPORT_DUAL
static uint32_t s_usb_last_rx_ms;
#endif

#if AK_ENABLE_DISPLAY
/* netmgr callbacks can run outside the app loop. Cache WiFi state there and
 * let the app loop perform the frame-buffer redraw. */
static volatile bool            s_wifi_connected    = false;
static volatile bool            s_wifi_status_dirty = false;
#if AK_TRANSPORT_HAS_BLE
static bool s_wifi_status_held = false; /* app loop only: shown when the BLE pairing window closes */
#endif
static volatile ak_wifi_state_t s_wifi_lcd_state    = AK_WIFI_DISCONNECTED;
#endif

/* ASR routing keeps a user preference separate from the effective K5 route.
 * Default preference is Tuya cloud ASR. If cloud readiness drops, K5 falls
 * back to PC ASR immediately; it returns to cloud when readiness recovers,
 * unless the user explicitly selected PC ASR. */
#define AK_VOICE_ROUTE_CHECK_MS 500U
static ak_voice_route_t s_voice_route;
/* Voice mode (ASR / Chat), changed only on the device and kept in KV (Q6). */
#define AK_VOICE_MODE_KV_KEY "ak_vmode"
/* Interface language (FR-01), one byte (ak_lang_to_byte), kept in KV. */
#define AK_LANG_KV_KEY "ak_lang"
/* LCD font size, one byte: its px (12 / 14 / 16, ak_text_size_px), kept in KV. */
#define AK_FONT_SIZE_KV_KEY "ak_fontsz"
/* Chat ghost animation (FR-14), one byte 0 / 1, kept in KV. */
#define AK_GHOST_KV_KEY "ak_ghost"
static ak_voice_mode_t s_voice_mode = AK_VOICE_MODE_ASR;
/* K5 pressed with nothing to answer it (Chat without cloud, or no Companion
 * for PC speech input): swallow its LONG/UP. */
static bool s_ptt_chat_blocked;
/* Voice status messages own the status bar for a few seconds, ahead of the
 * standing "pairing mode" / "PC ASR" lines (which would otherwise hide them). */
#define AK_VOICE_STATUS_MS 4000U
static bool     s_voice_status_on;
static uint32_t s_voice_status_until_ms;
/* Chat mode fell back to speech input because the Tuya cloud is down; Chat
 * comes back by itself when the cloud does (the saved mode is untouched). */
static bool s_chat_degraded;
static bool             s_voice_route_initialized;
static uint32_t         s_voice_route_last_check_ms;
/* True only for automatic cloud -> PC degradation, not a manual PC choice. */
static bool s_voice_fallback_active;
/* Route is latched on K4 DOWN. Cloud waits for the LONG event (matching FP-3
 * HOLD semantics); system emits start immediately and always receives its UP. */
static bool s_ptt_cloud_route   = false;
static bool s_ptt_cloud_started = false;
/* Double-click detector for K5: a double-click in the cloud ASR route sounds
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
/* FR-03 scene slots (ak_scene.h; KV handling near the keymap frame). */
static ak_scene_t s_scenes[AK_SCENE_SLOTS];
static bool       s_scene_used[AK_SCENE_SLOTS];
static int        s_scene_active = -1; /* -1 = no scene applied yet */
static uint8_t    s_scene_pick_slot[AK_SCENE_SLOTS]; /* picker item -> slot */
static void       ak_scene_picker_open(void);
static bool       ak_scene_dirty(void);
static const char *ak_scene_switch(uint8_t slot);
static void       send_scene_state(const char *error, const char *req);
#endif

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
/* The voice input text stays on the LCD (s_input_text, the strip on the home
 * page): K4 edits it locally, K6 sends it in the confirm frame. K1-K3 keep
 * selecting chats and leave the text alone. */
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
/* Settings > Bind session (ak_bind.h): the slot page is drawn inside the
 * settings modal; the instance / session menus run with settings closed. */
static ak_bind_t           s_bind;

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
/* Voice input strip (keyboard mode, ak_inputbar.h): the words K6 sends. It lives on the
 * home page in ①; there is no page of its own and no time-out. */
static char          s_input_text[AK_INPUT_TEXT_MAX + 1U];
static ak_scroll_t   s_input_scroll;   /* FR-17: the knob scrolls a text longer than the strip */
static uint32_t      s_input_since_ms; /* the strip or the PC fill began (light blink phase) */
/* Per slot A/B/C: the Companion fills the PC input box instead (`bindings` mode_a..c). */
static bool          s_slot_pc[3];
/* PC mode: after a fill K6 presses Enter and K4 edits / clears it, for AK_PC_FILL_MS. */
#define AK_PC_FILL_MS 20000U
static bool          s_pc_fill;
static uint32_t      s_pc_fill_until_ms;
/* The session whose history got the last sent text, to take it back when the send fails. */
static uint8_t       s_sent_session = 0xFFU;
/* Key lights last sent (ak_menu_lights / ak_session_lights). */
static ak_rgb_t  s_lights_shown[AK_LIGHT_COUNT];
static bool      s_lights_valid;
/* K1-K3 long press just switched Crew instance: that key's light turns blue for
 * AK_INSTANCE_FLASH_MS (-1 = none). */
#define AK_INSTANCE_FLASH_MS 1200U
static int       s_inst_flash_slot = -1;
static uint32_t  s_inst_flash_until_ms;
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
/* Views inside the test page; a short EC11 click steps HW -> LED power -> HW.
 * LED power view: K1 / K2 switch the chain (P18) on / off by hand. */
typedef enum {
    AK_TEST_VIEW_HW = 0,
    AK_TEST_VIEW_LEDPWR,
    AK_TEST_VIEW_COUNT,
} ak_test_view_t;
static ak_test_view_t s_test_view;
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
#include "ak_ble_pair.h"

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

/* True only for a subscribed central that passed the secure-channel handshake
 * (hello + first sealed frame). The GAP link and the notify subscription alone
 * prove nothing: Tuya's phone app uses the same GATT service and subscribes to
 * it too, so "link live" must never be shown as "Companion connected". */
static bool ak_ble_companion_authed(void)
{
    return s_ble_sec.state == AK_BLE_SEC_AUTHED && ak_ble_gatt_link_state() == AK_BLE_LINK_LIVE;
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

/* Store a new pairing key (USB ble_pair or over-the-air pairing): KV first,
 * then the live copy; any BLE session under the old key ends. */
static bool ble_key_store(const uint8_t key[AK_BLE_KEY_LEN])
{
    uint8_t blob[AK_BLE_KEY_BLOB_LEN];
    ak_ble_key_to_blob(key, blob);
    OPERATE_RET rt = ak_kv_set(AK_BLE_KEY_KV_KEY, blob, sizeof(blob));
    mbedtls_platform_zeroize(blob, sizeof(blob));
    if (rt != OPRT_OK) {
        PR_WARN("ble pair: KV write failed: %d", rt);
        return false;
    }
    if (transport_lock()) {
        memcpy(s_ble_key, key, sizeof(s_ble_key));
        s_ble_key_set = true;
        ak_ble_sec_reset(&s_ble_sec);
        transport_unlock();
    }
    return true;
}

/* Over-the-air pairing window (Settings > BLE pairing, ak_ble_pair.h). */
static ak_ble_pair_t s_ble_pair;
static char          s_ble_pair_reply[320];
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
static bool           s_devinfo_page;      /* Settings > Credentials is showing */
#if AK_ENABLE_DISPLAY
/* Settings > Saved Wi-Fi (FR-15): knob selects, K6 deletes (twice), K1 back. */
static bool           s_wifi_page;
static bool           s_wifi_help; /* the "changing Wi-Fi" help page, over Saved Wi-Fi or Settings */
static uint8_t        s_wifi_sel;
static bool           s_wifi_del_armed;
static char           s_wifi_del_ssid[AK_WIFI_STORE_SSID_MAX + 1U];
static uint32_t       s_wifi_del_deadline_ms;
#define AK_WIFI_DEL_CONFIRM_MS 3000U /* same window as the menu double confirm */
#endif
static uint32_t       s_wifi_sent_rev = 0xFFFFFFFFU; /* last wifi_list_state pushed */
static volatile bool  s_wifi_list_dirty;             /* link state changed: "current" may differ */
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
static void on_ptt_event(ak_input_event_t event, uint32_t held_ms);
static void send_voice(const char *state);
static uint8_t ak_input_slot(void);
static void send_voice_source(void);
static void ak_language_apply(ak_lang_t lang, bool persist);
static void ak_font_size_apply(ak_text_size_t size, bool persist);
#if AK_ENABLE_DISPLAY
static void ak_ghost_setting_apply(bool on, bool persist);
static void send_device_settings(void);
static void ak_voice_ui_set_state(ak_voice_ui_state_t state);
static void ak_voice_ui_queue_result(const char *text, bool is_error);
static void ak_voice_ui_tick(uint32_t now_ms);
static void ak_pending_confirm_exit(void);
static void ak_pending_confirm_tick(uint32_t now_ms);
static void ak_status_bar_set(const char *text, uint16_t color);
static void ak_voice_status(const char *text, uint16_t color);
static void dispatch_gesture_for_slot(const char *id, ak_gesture_t g);
static void ak_instance_switch_feedback(const char *id, uint8_t n);
static void ak_select_session_key(const char *id);
static void ak_render_status(void);
static void ak_settings_draw(void);
static void ak_bind_draw(uint32_t now_ms);
static void ak_bind_apply(ak_bind_do_t act, const char *msg, uint16_t color);
static void ak_lcd_text(const char *utf8, char *out, size_t cap);
static void ak_chat_scroll(int32_t detents);
static void ak_voice_mode_apply(ak_voice_mode_t mode, bool persist);
static void ak_prov_changed(void);
static void ak_prov_confirm_dialog(void);
static void ak_prov_reset_resolve(bool confirm);
static void ak_prov_unpair_resolve(bool confirm);
#if AK_TRANSPORT_HAS_BLE
static void ble_pair_apply(ak_ble_pair_act_t act);
#endif
static void ak_prov_rebind_resolve(bool confirm);
/* Settings > Reset system (ak_factory_reset.h): two ⑦ confirms. */
static ak_freset_t s_freset;
static uint32_t    s_freset_reboot_at_ms; /* fallback reboot after the SDK reset; 0 = none */
static void        ak_freset_resolve_key(bool confirm);
static bool ak_freset_up(uint32_t now_ms)
{
    return ak_freset_stage(&s_freset, now_ms) != AK_FRESET_IDLE;
}
#if AK_ENABLE_DISPLAY
static void ak_rebind_clicked(void);
#endif
static bool ak_settings_consume_input(ak_input_kind_t kind, const char *id,
                                      ak_input_event_t event, uint32_t held_ms);
/* Returns true when the encoder rotation was consumed by the modal settings UI
 * (so it must not be forwarded to the companion as an encoder frame). */
static bool ak_settings_consume_encoder(int32_t detents);
/* FR-17: ③ text longer than one screen scrolls with the knob. */
static bool ak_confirm_scroll(int32_t detents);
static bool ak_input_confirm(void);
static void ak_input_changed(void);
static void ak_input_sent_history(const char *text);
static void ak_input_send(const char *id, ak_gesture_t g);
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
    uint32_t held_ms = 0U;
    uint32_t wait_ms = 0U;
    int      gslot   = ak_keymap_slot_for_id(id);
    if (gslot >= 0) {
        ak_gesture_last(&s_key_gesture[gslot], &held_ms, &wait_ms);
    }
    transport_end_sequenced(frame, ak_proto_key_gesture_timed(frame, sizeof(frame), sequence, id,
                                                              ak_gesture_str(g), bind, held_ms, wait_ms));
    PR_DEBUG("gesture %s %s bind=%s held=%u wait=%u", id, ak_gesture_str(g), bind != NULL ? bind : "-",
             (unsigned)held_ms, (unsigned)wait_ms);
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

/* Voice input waits for a decision: the strip holds text, or the PC input box
 * was just filled (K6 = Enter, K4 = Backspace / clear). A dictated menu answer is not this. */
static bool ak_input_confirm(void)
{
    return s_input_text[0] != '\0' || s_pc_fill;
}

/* The strip text or the PC fill changed: keep the scroll at the end, tell ak_ui. */
static void ak_input_changed(void)
{
    ak_scroll_update(&s_input_scroll, ak_inputbar_max_first(s_input_text));
    ak_ui_set_bar(&s_ui, ak_input_confirm());
}

static void ak_input_begin(uint32_t now_ms)
{
    if (!ak_input_confirm()) {
        s_input_since_ms = now_ms;
    }
}

/* Give up the strip text and the PC fill (K4 long, voice mode change). */
static void ak_input_clear(void)
{
    s_input_text[0] = '\0';
    s_pc_fill       = false;
    ak_scroll_reset(&s_input_scroll, 0U, true);
    ak_ui_set_bar(&s_ui, false);
}

static void ak_pc_fill_start(uint32_t now_ms)
{
    ak_input_begin(now_ms);
    s_pc_fill          = true;
    s_pc_fill_until_ms = now_ms + AK_PC_FILL_MS;
    ak_ui_set_bar(&s_ui, true);
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
    bool system   = ak_provision_reset_pending(&s_prov, now) || ak_provision_unpair_pending(&s_prov, now) ||
                  ak_provision_rebind_pending(&s_prov, now) || s_prov.state == AK_PROV_CONFIRMING ||
                  ak_freset_up(now);
#if AK_TRANSPORT_HAS_BLE
    system = system || s_ble_pair.state == AK_BLE_PAIR_COMPARE; /* ⑦ BLE pairing code */
#endif
#if AK_TRANSPORT_HAS_UART
    system = system || ak_ota_link_busy(); /* ⑦ firmware update: confirm, then progress */
#endif
    ak_ui_set_bar(&s_ui, ak_input_confirm());
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

    if ((out->kind == AK_MENU_OUT_RESULT || out->kind == AK_MENU_OUT_DISMISS) && ak_menu_is_scene(&s_menu)) {
        /* FR-03 picker: local, nothing goes to the PC. Close it first so the
         * switch runs from ① standby. */
        bool    pick = out->kind == AK_MENU_OUT_RESULT && out->n == 1U && out->selected[0] < AK_SCENE_SLOTS;
        uint8_t slot = pick ? s_scene_pick_slot[out->selected[0]] : 0U;
        (void)ak_menu_on_close(&s_menu, AK_SCENE_PICKER_ID);
        ak_ui_menu_gone(&s_ui);
        ak_ui_sync(now);
        if (pick) {
            const char *err = ak_scene_switch(slot);
            if (err != NULL) {
                ak_status_tmp_set(ak_tr(AK_STR_ST_SCENE_BUSY), AK_UI_ST_WARN, AK_MENU_NOTICE_MS);
            }
            send_scene_state(err, err != NULL ? "scene_switch" : NULL);
        }
        ak_render_status();
        return;
    }
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
        if (ak_menu_is_bind(&s_menu)) {
            ak_bind_menu_answered(&s_bind, now);
        }
        ak_ui_menu_gone(&s_ui);
        break;
    case AK_MENU_OUT_DICTATE:
        /* The answer is the board's own cloud ASR text (ASR mode); in any
         * other route there is no text to send. */
        if (s_voice_route.effective == AK_VOICE_CLOUD && s_voice_mode == AK_VOICE_MODE_ASR && ak_asr_available()) {
            s_dictated[0] = '\0';
            ak_ui_dictate(&s_ui);
            ak_status_bar_set(ak_tr(AK_STR_ST_HOLD_TO_SPEAK), AK_UI_ST_ACTIVE);
        } else {
            ak_menu_dictate_cancel(&s_menu);
            ak_status_tmp_set(ak_tr(AK_STR_ST_DICTATION_NEEDS_CLOUD), AK_UI_ST_WARN, AK_MENU_NOTICE_MS);
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
static bool ak_answer_voice_route_key(const char *id, ak_input_event_t event)
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
        ak_status_bar_set(ak_tr(AK_STR_ST_READY), AK_UI_ST_OK);
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
            if (ak_menu_is_bind(&s_menu) && ak_bind_menu_opened(&s_bind, now)) {
                s_settings_active = false; /* the flow's menu shows as ⑤ */
            } else if (ak_bind_active(&s_bind) && !s_settings_active) {
                ak_bind_init(&s_bind); /* another menu took over between the flow's steps */
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
        bool was_bind = ak_menu_is_bind(&s_menu);
        if (!ak_menu_on_close(&s_menu, id)) {
            return; /* not the current menu */
        }
        if (was_bind) {
            ak_bind_menu_answered(&s_bind, now); /* bind_done or the 5 s reply timeout follows */
        }
        if (was_open) {
            if (s_ui.dictating) {
                s_asr_for_menu = false;
            }
            ak_ui_menu_gone(&s_ui);
        }
        if (msg[0] != '\0') {
            ak_status_tmp_set(msg, AK_UI_ST_INFO, AK_MENU_CLOSE_MSG_MS);
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
                          level == AK_NOTICE_ERROR  ? AK_UI_ST_ERROR
                          : level == AK_NOTICE_WARN ? AK_UI_ST_WARN
                                                    : AK_UI_ST_INFO,
                          AK_MENU_NOTICE_MS);
        ak_render_status();
    }
}

/* ───────────── Settings > Bind session glue (ak_bind.h) ─────────────
 * The slot page lives inside the settings modal; while the Companion's
 * instance / session menu is up, settings is closed so the menu shows as ⑤,
 * and it reopens when bind_done (or a timeout) ends that step. */

static void ak_bind_apply(ak_bind_do_t act, const char *msg, uint16_t color)
{
    switch (act) {
    case AK_BIND_DO_REQUEST: {
        char     frame[64];
        uint32_t sequence;
        if (transport_begin_sequenced(&sequence)) {
            transport_end_sequenced(frame, ak_proto_bind_session(frame, sizeof(frame), sequence, s_bind.slot));
        }
        PR_NOTICE("bind session: slot %u requested", (unsigned)s_bind.slot);
        break;
    }
    case AK_BIND_DO_SETTINGS:
    case AK_BIND_DO_PICK:
    case AK_BIND_DO_NO_REPLY:
        /* A flow menu still on screen (bind_done arrived early) goes away. */
        if (s_menu.state == AK_MENU_OPEN && ak_menu_is_bind(&s_menu) && ak_menu_on_close(&s_menu, s_menu.id)) {
            ak_ui_menu_gone(&s_ui);
        }
        s_settings_active    = true;
        s_settings_confirmed = false;
        if (act == AK_BIND_DO_NO_REPLY) {
            msg   = ak_tr(AK_STR_BIND_NO_REPLY);
            color = AK_UI_ST_ERROR;
        }
        break;
    default:
        break;
    }
    if (msg != NULL && msg[0] != '\0') {
        ak_status_tmp_set(msg, color, AK_MENU_NOTICE_MS); /* the settings / slot page shows it */
    }
    if (act != AK_BIND_DO_NONE) {
        ak_ui_sync(tal_system_get_millisecond());
        ak_render_status();
    }
}

/* bind_done {slot, result, msg}: the Companion finished (or left) a step. */
static void ak_bind_on_done(const char *payload)
{
    char     result[12] = "";
    char     msg[AK_MENU_NOTICE_MAX + 1U] = "";
    uint32_t slot = 0U;
    if (ak_json_get_uint(payload, "slot", AK_BIND_SLOTS, &slot) != AK_JSON_OK || slot == 0U ||
        ak_json_get_str(payload, "result", result, sizeof(result)) != AK_JSON_OK ||
        ak_json_get_str_clip(payload, "msg", msg, sizeof(msg)) == AK_JSON_INVALID) {
        PR_WARN("bind_done rejected: malformed");
        return;
    }
    ak_bind_result_t r = ak_bind_result_from(result);
    if (r == AK_BIND_RESULT_INVALID) {
        PR_WARN("bind_done rejected: result");
        return;
    }
    char text[AK_MENU_NOTICE_MAX + 1U];
    ak_lcd_text(msg, text, sizeof(text));
    if (text[0] == '\0' && (r == AK_BIND_RESULT_BOUND || r == AK_BIND_RESULT_UNBOUND)) {
        (void)snprintf(text, sizeof(text), "%s", ak_tr(r == AK_BIND_RESULT_BOUND ? AK_STR_BIND_BOUND : AK_STR_BIND_CLEARED));
    }
    ak_bind_do_t act = ak_bind_done(&s_bind, (uint8_t)slot, r, tal_system_get_millisecond());
    PR_NOTICE("bind_done slot %u %s -> %d", (unsigned)slot, result, (int)act);
    ak_bind_apply(act, text, r == AK_BIND_RESULT_ERROR ? AK_UI_ST_ERROR : AK_UI_ST_OK);
}
#endif /* AK_ENABLE_DISPLAY */

/* Input event -> local settings UI or JSONL frame on the active transport.
 * v0.3 stage 乙: VOICE keys run the hold-to-talk edge path; every other key is
 * NORMAL and runs the single/double/long classifier, whose resolved gesture
 * selects the bound action from the keymap. */
/* K6 confirms a system dialog on release, and only after a press that began
 * while the dialog was up; K1 cancels on press. So K6 going down a moment
 * before K1 (the K1 + K6 combo) never confirms anything. True when the
 * dialog is decided; *confirm says how. */
static bool s_confirm_k6_down;

static bool ak_confirm_key(const char *id, ak_input_event_t event, bool *confirm)
{
    if (strcmp(id, "k1") == 0 && event == AK_INPUT_EVENT_DOWN) {
        s_confirm_k6_down = false;
        *confirm          = false;
        return true;
    }
    if (strcmp(id, "k6") == 0) {
        if (event == AK_INPUT_EVENT_DOWN) {
            s_confirm_k6_down = true;
        } else if (event == AK_INPUT_EVENT_UP && s_confirm_k6_down) {
            s_confirm_k6_down = false;
            *confirm          = true;
            return true;
        }
    }
    return false;
}

/* Recovery combo (K1 + K6, 5 s; ak_combo.h). */
static ak_combo_t s_combo;

/* The combo began: drop every half-finished use of K1 and K6 so neither key
 * resolves a gesture, a menu item or a dialog confirm when it is released. */
static void ak_combo_drop_key_state(void)
{
    static const char *const ids[2] = {"k1", "k6"};
    uint8_t                  mask   = 0U;
    for (int i = 0; i < 2; i++) {
        int slot = ak_keymap_slot_for_id(ids[i]);
        if (slot >= 0) {
            ak_gesture_init(&s_key_gesture[slot]);
            mask |= (uint8_t)(1U << slot);
        }
    }
    s_confirm_k6_down = false;
#if AK_ENABLE_DISPLAY
    s_menu_swallow &= (uint8_t)~mask;
    ak_menu_drop_keys(&s_menu, mask);
#else
    (void)mask;
#endif
    PR_NOTICE("recovery combo: K1 + K6 held; unbind confirm in %u s", (unsigned)(AK_COMBO_HOLD_MS / 1000U));
}

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
        bool confirm = false;
        if (ak_ota_link_confirming() && kind == AK_INPUT_KIND_KEY && ak_confirm_key(id, event, &confirm)) {
            ak_ota_link_confirm(confirm, tal_system_get_millisecond());
        }
        return;
    }
#endif
    /* Live key view (Companion Input page): every NON-voice key also reports
     * its raw down/up edge the moment it settles (after the 20 ms debounce),
     * ahead of the resolved gesture, which can only be known on release (and
     * after the double-click window when a double is bound). The Companion
     * ignores these edges for control; the voice key reports its own edges
     * below. Skipped while a firmware update owns the keys (above). */
    if (kind == AK_INPUT_KIND_KEY && (event == AK_INPUT_EVENT_DOWN || event == AK_INPUT_EVENT_UP) &&
        !ak_keymap_is_voice(&s_keymap, id)) {
        ak_emit_input_frame(kind, id, event, held_ms);
    }
    /* Recovery combo: K1 + K6 held together. Once both are down, neither key
     * does anything else until both are up (ak_combo.h); the main loop opens
     * the unbind confirm after 5 s. */
    if (kind == AK_INPUT_KIND_KEY) {
        ak_combo_verdict_t cv = ak_combo_key(&s_combo, id, event, tal_system_get_millisecond());
        if (cv == AK_COMBO_START) {
            ak_combo_drop_key_state();
        }
        if (cv != AK_COMBO_PASS) {
            return;
        }
    }
    /* The reset confirm dialog is modal (⑦): only K6 (confirm, on release) /
     * K1 (cancel) act, nothing else reaches the settings page or the Companion. */
    if (ak_provision_reset_pending(&s_prov, tal_system_get_millisecond())) {
        bool confirm = false;
        if (kind == AK_INPUT_KIND_KEY && ak_confirm_key(id, event, &confirm)) {
            ak_prov_reset_resolve(confirm);
        }
        return;
    }
    /* Same for a Companion ble_unpair (FR-02). */
    if (ak_provision_unpair_pending(&s_prov, tal_system_get_millisecond())) {
        bool confirm = false;
        if (kind == AK_INPUT_KIND_KEY && ak_confirm_key(id, event, &confirm)) {
            ak_prov_unpair_resolve(confirm);
        }
        return;
    }
    /* Same for Unbind & re-pair (Settings, or the K1 + K6 combo). */
    if (ak_provision_rebind_pending(&s_prov, tal_system_get_millisecond())) {
        bool confirm = false;
        if (kind == AK_INPUT_KIND_KEY && ak_confirm_key(id, event, &confirm)) {
            ak_prov_rebind_resolve(confirm);
        }
        return;
    }
    /* Same for the two Reset system confirms (K6 next / reset, K1 cancels). */
    if (ak_freset_up(tal_system_get_millisecond())) {
        bool confirm = false;
        if (kind == AK_INPUT_KIND_KEY && ak_confirm_key(id, event, &confirm)) {
            ak_freset_resolve_key(confirm);
        }
        return;
    }
#if AK_TRANSPORT_HAS_BLE
    /* Same for the over-the-air BLE pairing code (K6 pairs, K1 cancels). */
    if (s_ble_pair.state == AK_BLE_PAIR_COMPARE) {
        bool confirm = false;
        if (kind == AK_INPUT_KIND_KEY && ak_confirm_key(id, event, &confirm)) {
            ble_pair_apply(ak_ble_pair_user(&s_ble_pair, confirm, tal_system_get_millisecond(), s_ble_pair_reply,
                                            sizeof(s_ble_pair_reply)));
        }
        return;
    }
#endif
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
    if (ak_menu_route_key(id, event) || ak_answer_voice_route_key(id, event)) {
        return;
    }
    /* Bind session between two steps (menu answered, Companion not done
     * yet): no key reaches the PC or the work screen. */
    if (s_bind.state == AK_BIND_WAIT) {
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
     * consumes DOWN/LONG/UP as one latched HOLD; system emits DOWN/UP voice frames.
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
            on_ptt_event(event, held_ms);
        }
        return;
    }

    /* NORMAL key: feed the single/double/long classifier. The resolved gesture
     * picks the bound action; raw down/up are NOT forwarded. */
    int slot = ak_keymap_slot_for_id(id);
    if (slot < 0) {
        return;
    }
    /* No double binding on this key: its single goes out on release, with no
     * wait for the double-click window (read from the live keymap each time). */
    ak_gesture_set_double_bound(&s_key_gesture[slot],
                                ak_keymap_action(&s_keymap, id, AK_GESTURE_SLOT_DOUBLE) != AK_ACTION_NONE);
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
    if (s_settings_active || ak_menu_owns_keys(&s_menu) || s_ui.dictating || s_bind.state == AK_BIND_WAIT) {
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
    /* ② waiting for the PC ASR text: K1 cancels it (the Companion drops the
     * utterance on voice:cancel); its double / long do nothing. */
    if (s_ui.pc_wait && strcmp(id, "k1") == 0) {
        if (g == AK_GESTURE_SINGLE) {
            send_voice("cancel");
            s_voice_ui_state          = AK_VOICE_UI_IDLE;
            s_voice_ui_state_since_ms = tal_system_get_millisecond();
            ak_pending_confirm_exit();
            ak_voice_status(ak_tr(AK_STR_ST_INPUT_CANCELLED), AK_UI_ST_NOTE);
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
#if AK_ENABLE_DISPLAY
        if (g == AK_GESTURE_LONG) {
            ak_instance_switch_feedback(id, n); /* at the 500 ms threshold, before release */
        }
#endif
        dispatch_gesture_frame(id, g, bind);
        return;
    }
    /* Companion-run actions (K6 long / double by default). */
    if (action == AK_ACTION_MODEL_MENU || action == AK_ACTION_MODEL_TOGGLE) {
        dispatch_gesture_frame(id, g, ak_keymap_action_str(action));
        return;
    }
    /* FR-03: the board-local scene picker (reported too; the PC ignores it). */
    if (action == AK_ACTION_SCENE_MENU) {
        dispatch_gesture_frame(id, g, "scene_menu");
#if AK_ENABLE_DISPLAY
        ak_scene_picker_open();
#endif
        return;
    }
    /* FR-03 limited shortcuts: the Companion types them into Kiro Crew, so
     * never while ③ shows text waiting to be confirmed. */
    if (ak_action_shortcut_n(action) != 0u) {
#if AK_ENABLE_DISPLAY
        if (ak_input_confirm()) {
            return;
        }
#endif
        dispatch_gesture_frame(id, g, ak_keymap_action_str(action));
        return;
    }

#if AK_ENABLE_DISPLAY
    /* Voice input edits are firmware-owned: they only run, and only carry a
     * bind, while the strip holds text or a PC fill waits. K4 single deletes the
     * last character, K4 long everything; K6 sends. */
    if (ak_input_confirm() &&
        (action == AK_ACTION_CANCEL_CHAR || action == AK_ACTION_CANCEL_ALL || action == AK_ACTION_CONFIRM)) {
        uint32_t now = tal_system_get_millisecond();
        if (action == AK_ACTION_CANCEL_CHAR) {
            dispatch_gesture_frame(id, g, "cancel_char");
            (void)ak_input_pop(s_input_text);
            if (s_pc_fill) {
                s_pc_fill_until_ms = now + AK_PC_FILL_MS; /* editing the PC box keeps the window */
            }
            if (s_input_text[0] == '\0' && !s_pc_fill) {
                ak_input_clear();
                ak_voice_status(ak_tr(AK_STR_ST_INPUT_CLEARED), AK_UI_ST_NOTE);
            }
            ak_input_changed();
        } else if (action == AK_ACTION_CANCEL_ALL) {
            dispatch_gesture_frame(id, g, "cancel_all");
            ak_input_clear();
            ak_voice_status(ak_tr(AK_STR_ST_INPUT_CANCELLED), AK_UI_ST_NOTE);
        } else {
            ak_input_send(id, g);
        }
        ak_ui_sync(now);
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
    if (ak_provision_reset_pending(&s_prov, tal_system_get_millisecond()) ||
        ak_provision_unpair_pending(&s_prov, tal_system_get_millisecond()) ||
        ak_provision_rebind_pending(&s_prov, tal_system_get_millisecond()) ||
        ak_freset_up(tal_system_get_millisecond())) {
        return;
    }
#if AK_TRANSPORT_HAS_BLE
    if (s_ble_pair.state == AK_BLE_PAIR_COMPARE) {
        return;
    }
#endif
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
    if (ak_confirm_scroll(detents)) {
        return; /* FR-17: ③ text longer than one screen scrolls; shorter keeps the old meaning */
    }
    if (s_ui.dictating || s_bind.state == AK_BIND_WAIT) {
        return; /* a dictated answer: nothing reaches the PC */
    }
    if (s_voice_mode == AK_VOICE_MODE_CHAT) {
        ak_chat_scroll(detents); /* scroll the Chat page; nothing goes to the PC */
        return;
    }
#endif
    ak_emit_encoder_frame(detents);
}

/* Settings > Device info and the `device_info` frame read the same snapshot
 * (ak_devinfo.h). Only "set / not set" of anything secret is ever read here. */
static ak_devinfo_t s_devinfo;     /* last snapshot (also what the page shows) */
static ak_devinfo_t s_devinfo_sig; /* ...as it was last compared (signal strength zeroed) */
static bool         s_hello_ble;   /* the Companion's hello arrived over BLE */
static uint32_t     s_devinfo_poll_ms;
#define AK_DEVINFO_POLL_MS 1000U

/* `creds` re-reads PID / UUID / AuthKey status (page open, hello); the 1 s
 * poll keeps the last answer so the licence storage is not read every second. */
static void ak_devinfo_snapshot(ak_devinfo_t *d, bool creds)
{
    ak_devinfo_t prev = *d;
    memset(d, 0, sizeof(*d));
    /* Connection */
#if AK_TRANSPORT_UART || AK_TRANSPORT_DUAL
    if (s_pc_connected) {
        d->companion = AK_DI_LINK_USB;
        d->handshake = true; /* USB is marked connected by the Companion's hello */
    }
#endif
#if AK_TRANSPORT_BLE_GATT || AK_TRANSPORT_DUAL
    bool ble_up = ak_ble_gatt_link_state() != AK_BLE_LINK_ADVERTISING;
#if AK_TRANSPORT_DUAL
    ble_up = ble_up && s_ak_ble_active;
#endif
    if (!ble_up) {
        s_hello_ble = false;
    }
    if (d->companion == AK_DI_LINK_NONE && ble_up) {
        d->companion = AK_DI_LINK_BLE;
        d->handshake = s_hello_ble;
    }
#endif
    if (!ak_cloud_activated()) {
        d->cloud = AK_DI_CLOUD_INACTIVE;
    } else {
        d->cloud = strcmp(ak_cloud_activation_state(), "cloud_ready") == 0 ? AK_DI_CLOUD_ACTIVE : AK_DI_CLOUD_OFFLINE;
    }
    /* Wi-Fi */
    ak_wifi_net_info_t net;
    ak_wifi_net_info(&net);
    if (ak_wifi_current_state() == AK_WIFI_CONNECTED) {
        ak_devinfo_set_ssid(d, ak_wifi_current_ssid());
    }
    if (net.ip_valid) {
        (void)snprintf(d->ip, sizeof(d->ip), "%s", net.ip);
    }
    d->rssi_valid     = net.rssi_valid;
    d->rssi           = net.rssi;
    d->wifi_mac_valid = net.mac_valid;
    memcpy(d->wifi_mac, net.mac, sizeof(d->wifi_mac));
    /* Bluetooth */
#if AK_TRANSPORT_BLE_GATT || AK_TRANSPORT_DUAL
    d->ble_available = true;
    d->ble_paired    = s_ble_key_set;
    (void)snprintf(d->ble_name, sizeof(d->ble_name), "%s", ak_ble_gatt_device_name());
    d->ble_mac_valid = ak_ble_gatt_address(d->ble_mac);
#endif
    /* Device */
    (void)snprintf(d->fw, sizeof(d->fw), "%s", APP_FW_VERSION);
    if (creds) {
        ak_cred_status_t st = ak_cred_read_status();
        d->pid              = st.pid;
        d->uuid             = st.uuid;
        d->authkey          = st.authkey;
    } else {
        d->pid     = prev.pid;
        d->uuid    = prev.uuid;
        d->authkey = prev.authkey;
    }
}

/* Read-only `device_info` frame for the Companion (same facts as the page). */
static void send_device_info(void)
{
    static char frame[AK_FRAME_BUFFER_DEVINFO];
    uint32_t    sequence;
    if (!transport_begin_sequenced(&sequence)) {
        return;
    }
    transport_end_sequenced(frame, ak_proto_device_info(frame, sizeof(frame), sequence, &s_devinfo));
}

#if AK_ENABLE_DISPLAY
static void ak_devinfo_page_changed(void); /* redraws the page when it is showing */
#endif

/* App loop, once a second: a change in what the snapshot shows (a link coming
 * or going, an address, pairing, cloud state; not the signal strength) refreshes
 * the page and tells the Companion. */
static void ak_devinfo_poll(uint32_t now_ms)
{
    if ((uint32_t)(now_ms - s_devinfo_poll_ms) < AK_DEVINFO_POLL_MS) {
        return;
    }
    s_devinfo_poll_ms = now_ms;
    ak_devinfo_t cur = s_devinfo; /* keeps the credentials answer */
    ak_devinfo_snapshot(&cur, false);
    ak_devinfo_t sig = cur;
    sig.rssi         = 0;
    if (memcmp(&sig, &s_devinfo_sig, sizeof(sig)) == 0) {
        return;
    }
    s_devinfo_sig = sig;
    s_devinfo     = cur;
    if (cur.companion != AK_DI_LINK_NONE) {
        send_device_info();
    }
#if AK_ENABLE_DISPLAY
    ak_devinfo_page_changed();
#endif
}

static void send_hello_ack(void)
{
    char frame[AK_HELLO_ACK_BUFFER_SIZE(APP_FW_VERSION, AK_TRANSPORT_NAME)]; /* grows with the capability list */
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
    /* A fresh snapshot (credentials re-read) for the Companion's device settings. */
    ak_devinfo_snapshot(&s_devinfo, true);
    s_devinfo_sig      = s_devinfo;
    s_devinfo_sig.rssi = 0;
    send_device_info();
}

/* The chat slot (1..3) voice input is for: the one K1-K3 selected; 0 = no LCD. */
#if AK_ENABLE_DISPLAY
static uint8_t s_selected_session;
static uint8_t ak_input_slot(void)
{
    /* A strip that holds text keeps taking words (0 = keyboard mode whatever the slot). */
    return s_input_text[0] != '\0' ? 0U : (uint8_t)(s_selected_session + 1U);
}
#else
static uint8_t ak_input_slot(void)
{
    return 0U;
}
#endif

/* Emit a voice.start / voice.stop edge (PC companion runs its own ASR). */
static void send_voice(const char *state)
{
    char     frame[FRAME_BUFFER_SIZE];
    uint32_t sequence;
    if (!transport_begin_sequenced(&sequence)) {
        return;
    }
    transport_end_sequenced(frame, ak_proto_voice(frame, sizeof(frame), sequence, state, ak_input_slot()));
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
    char     frame[SYSTEM_INFO_FRAME_BUFFER_SIZE]; /* > 192: about 240 bytes with every field */
    uint32_t sequence;
    if (!transport_begin_sequenced(&sequence)) {
        return;
    }
    ak_proto_settings_t st = {
        .volume          = s_settings.volume,
        .brightness      = s_settings.brightness,
        .led_power       = ak_led_mode_wire_str((ak_led_mode_t)s_settings.led_mode),
        .led_auto_off_s  = ak_led_policy_timeout_ms(s_settings.led_timeout_idx) / 1000U,
        .language        = ak_lang_wire_str(ak_lang_get()),
        .voice_mode      = ak_voice_mode_str(s_voice_mode),
        .font_size       = ak_text_size_px(ak_text_size_get()),
        .animation       = s_settings.animation,
        .license_present = ak_license_present(), /* cached after the first read (ak_wifi.c) */
        .pid             = ak_pid_pending_restart() ? "pending" : (ak_pid_known() ? "present" : "missing"),
#if AK_TRANSPORT_DUAL || AK_TRANSPORT_BLE_GATT
        .ble_paired = s_ble_key_set,
#else
        .ble_paired = false,
#endif
    };
    transport_end_sequenced(frame, ak_proto_device_settings(frame, sizeof(frame), sequence, &st));
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
        ak_proto_asr(s_asr_tx_frame, sizeof(s_asr_tx_frame), sequence, text, ak_input_slot()));
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
    char frame[AK_FRAME_BUFFER_LARGE]; /* the SSID may need JSON escapes */
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
    s_wifi_list_dirty = true;
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

/* K5 push-to-talk routing. The route is decided on DOWN and held until UP:
 * - system: emit voice.start immediately, then voice.stop on UP;
 * - cloud: wait for LONG, inject FP-3 HOLD long-press, then release on UP.
 * A short cloud press intentionally starts no ASR session, matching FP-3. */
static void on_ptt_event(ak_input_event_t event, uint32_t held_ms)
{
    (void)held_ms; /* read only when the optional single-click feature is compiled in */
    if (event == AK_INPUT_EVENT_DOWN) {
        /* Cloud ASR when the Tuya cloud answers; otherwise PC speech input through
         * the Companion (no License, not provisioned, pairing window open, cloud
         * down). Chat has no PC fallback of its own: this press becomes speech input. */
        ak_ptt_plan_t plan = ak_voice_ptt_plan(s_voice_mode, ak_asr_available(),
                                               s_voice_route.effective == AK_VOICE_CLOUD, ak_companion_linked());
#if !AK_ENABLE_DISPLAY
        if (plan == AK_PTT_PLAN_CHAT_AS_PC) {
            plan = AK_PTT_PLAN_NEED_LINK; /* no strip to show the text in */
        }
#endif
        s_ptt_chat_blocked  = (plan == AK_PTT_PLAN_NEED_LINK);
        s_ptt_cloud_route   = (plan == AK_PTT_PLAN_CLOUD);
        s_ptt_cloud_started = false;
#if AK_ENABLE_DISPLAY
        if (plan == AK_PTT_PLAN_CHAT_AS_PC && !s_ui.dictating) {
            s_chat_degraded = true;
            ak_voice_mode_apply(AK_VOICE_MODE_ASR, false); /* the saved mode stays Chat */
            ak_status_tmp_set(ak_tr(AK_STR_ST_CHAT_AS_ASR), AK_UI_ST_NOTE, AK_MENU_NOTICE_MS);
        }
#endif
#if AK_ENABLE_DISPLAY
        if (s_ui.dictating && !(s_ptt_cloud_route && s_voice_mode == AK_VOICE_MODE_ASR)) {
            /* A dictated answer needs the board's own text; PC speech input would
             * type into whatever window is in front. Ignore this press. */
            s_ptt_chat_blocked = true;
            ak_status_tmp_set(ak_tr(AK_STR_ST_DICTATION_NEEDS_CLOUD), AK_UI_ST_WARN, AK_MENU_NOTICE_MS);
            ak_render_status();
            return;
        }
        if (!s_ptt_chat_blocked) {
            s_asr_for_menu = s_ui.dictating;
            (void)ak_ui_voice_start(&s_ui); /* ① / ③ / dictating -> ② */
        }
#endif
        if (s_ptt_chat_blocked) {
            /* Neither the cloud nor the Companion can take the speech: say what is missing. */
#if AK_ENABLE_DISPLAY
            ak_voice_status(ak_tr(AK_STR_ST_VOICE_NEEDS_LINK), AK_UI_ST_WARN);
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
#if AK_ENABLE_DISPLAY
            if (s_pc_asr_results) {
                ak_voice_ui_set_state(AK_VOICE_UI_LISTENING);
            }
#endif
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
            if (s_pc_asr_results && ak_ui_pc_wait(&s_ui)) {
                /* PC ASR: stay in ② until input_restore brings the text;
                 * ASR_RECOGNIZE_TIMEOUT_MS or K1 end the wait. */
                ak_voice_ui_set_state(AK_VOICE_UI_RECOGNIZING);
            } else {
                /* Older Companion: nothing comes back to the keyboard. */
                ak_voice_ui_set_state(AK_VOICE_UI_IDLE);
                ak_ui_voice_end(&s_ui, AK_UI_VOICE_NONE, s_voice_mode == AK_VOICE_MODE_CHAT,
                                tal_system_get_millisecond());
            }
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

/* wifi_list_state (FR-15): SSIDs only, most recent first. Never a password. */
static char s_wifi_list_frame[1536];

static void send_wifi_list(void)
{
    ak_wifi_saved_t       saved[AK_WIFI_STORE_MAX];
    ak_proto_wifi_entry_t entry[AK_WIFI_STORE_MAX];
    size_t                n = ak_wifi_saved_list(saved);
    for (size_t i = 0U; i < n; i++) {
        entry[i].ssid    = saved[i].ssid;
        entry[i].sec     = saved[i].sec;
        entry[i].current = saved[i].current;
    }
    uint32_t sequence = 0U;
    if (!transport_begin_sequenced(&sequence)) {
        return;
    }
    transport_end_sequenced_sized(
        s_wifi_list_frame, sizeof(s_wifi_list_frame),
        ak_proto_wifi_list(s_wifi_list_frame, sizeof(s_wifi_list_frame), sequence, entry, n, AK_WIFI_STORE_MAX));
    s_wifi_sent_rev = ak_wifi_saved_revision();
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
        return AK_STATE_UNBOUND;
    } else if (strcmp(state, "idle") == 0) {
        return AK_STATE_IDLE;
    } else if (strcmp(state, "running") == 0) {
        return AK_STATE_RUNNING;
    } else if (strcmp(state, "waiting") == 0) {
        return AK_STATE_WAITING;
    } else if (strcmp(state, "info") == 0) {
        return AK_STATE_INFO;
    }
    return AK_STATE_UNBOUND;
}

#define LCD_SESSION_ROWS          AK_CHAT_SLOT_COUNT
#define LCD_SESSION_ALIAS_BYTES                                                                                        \
    16U /* UTF-8; the 58 px label (LCD_CARD_LABEL_W) shows 9 / 8 / 7 ASCII or 4 / 4 / 3 CJK */
/* Sidebar cards, chat column, status bar and footer: common/include/ak_layout.h. */
#define LCD_HISTORY_CAPACITY      10U
#define LCD_HISTORY_TEXT_BYTES    128U
#define LCD_HISTORY_VISIBLE_LINES 16U
#define LCD_STATUS_TEXT_BYTES     96U

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
static char     s_session_aliases[LCD_SESSION_ROWS][LCD_SESSION_ALIAS_BYTES]; /* as received; "" = default */
static char     s_session_labels[LCD_SESSION_ROWS][LCD_SESSION_ALIAS_BYTES] = {"A", "B", "C"}; /* fitted */
/* Name of the Crew session bound to each slot (`bindings` name_a..c), shown
 * on the Bind session page; empty = not bound. */
#define LCD_SESSION_NAME_BYTES 49U /* 48 bytes UTF-8 */
static char     s_session_names[LCD_SESSION_ROWS][LCD_SESSION_NAME_BYTES];
/* s_selected_session: declared above send_voice */
static char     s_status_bar_text[LCD_STATUS_TEXT_BYTES] = "Starting system...";
static uint16_t s_status_bar_color                       = AK_UI_ST_INFO;

/* ── LCD style helpers (ak_layout.h) ────────────────────────────────────────
 * One style at every font size: rounded boxes, one selection colour, text
 * placed by box (ak_gfx_text_box / _center / _right). */

/* `s` cut to max_px at a character boundary. With `ellipsis` (user data:
 * SSIDs, session names) the cut ends in "..."; interface strings fit by
 * design (host tests). Returns s itself when it fits, else buf. */
static const char *ak_lcd_fit(const char *s, uint16_t max_px, bool ellipsis, char *buf, size_t cap)
{
    if (s == NULL || ak_text_width(s) <= max_px || cap < 5U) {
        return s == NULL ? "" : s;
    }
    uint16_t dots = ellipsis ? ak_text_width("...") : 0U;
    size_t   n    = ak_text_clip(s, max_px > dots ? (uint16_t)(max_px - dots) : 0U);
    if (n > cap - 4U) {
        n = cap - 4U;
        while (n > 0U && ((unsigned char)s[n] & 0xC0U) == 0x80U) {
            n--;
        }
    }
    memcpy(buf, s, n);
    memcpy(buf + n, ellipsis ? "..." : "", ellipsis ? 4U : 1U);
    return buf;
}

/* Title bar: 22 px, the title left at the margin, `right` (page / queue)
 * right-aligned; both centred on the bar. Purple with White text; with `warn`
 * (AK_UI_WARN_*, not AK_UI_NO_WARN) a black bar with a 2 px rule in that
 * functional colour along its bottom. */
static void ak_title_bar(const char *title, uint16_t warn, const char *right)
{
    char     fit[96];
    uint16_t rw  = right != NULL && right[0] != '\0' ? (uint16_t)(ak_text_width(right) + 8U) : 0U;
    uint16_t bar = warn != AK_UI_NO_WARN ? AK_UI_WARN_BG : AK_UI_TITLE_BG;
    uint16_t fg  = AK_UI_TITLE_FG;
    ak_gfx_fill_rect(0U, 0U, AK_STATUSLCD_W, LCD_TITLE_H, bar);
    if (warn != AK_UI_NO_WARN) {
        ak_gfx_fill_rect(0U, (uint16_t)(LCD_TITLE_H - 2U), AK_STATUSLCD_W, 2U, warn);
    }
    (void)ak_gfx_text_box(LCD_M, 0U, LCD_TITLE_H,
                          ak_lcd_fit(title, (uint16_t)(AK_STATUSLCD_W - 2U * LCD_M - rw), false, fit, sizeof(fit)), fg,
                          bar);
    if (rw > 0U) {
        (void)ak_gfx_text_right((uint16_t)(AK_STATUSLCD_W - LCD_M), 0U, LCD_TITLE_H, right, fg, bar);
    }
}

/* Footer of a full-screen page: the rule, then up to two hint lines. */
static void ak_footer_draw(const char *h1, uint16_t c1, const char *h2, uint16_t c2)
{
    char        fit[96];
    const char *hint[2] = {h1, h2};
    uint16_t    col[2]  = {c1, c2};
    ak_gfx_fill_rect(0U, LCD_FOOTER_Y, AK_STATUSLCD_W, LCD_FOOTER_H, AK_UI_BG);
    ak_gfx_fill_rect(0U, LCD_FOOTER_Y, AK_STATUSLCD_W, 1U, AK_UI_RULE);
    for (uint16_t i = 0U; i < 2U; i++) {
        if (hint[i] != NULL && hint[i][0] != '\0') {
            (void)ak_gfx_text_box(LCD_M, LCD_HINT_Y(i), LCD_L,
                                  ak_lcd_fit(hint[i], AK_STATUSLCD_W - 2U * LCD_M, false, fit, sizeof(fit)), col[i],
                                  AK_UI_BG);
        }
    }
}

/* One list row at y (settings, Saved Wi-Fi, credentials): a dark grey frame
 * when idle, Purple when selected, inverted (Black on White) while editing.
 * The label is cut with "..." when it would reach the value. */
static void ak_list_row(uint16_t y, const char *label, const char *value, uint16_t value_color, bool selected,
                        bool editing)
{
    char     fit[96];
    bool     edit      = selected && editing;
    uint16_t bg        = edit ? AK_UI_EDIT_BG : (selected ? AK_UI_SEL_BG : AK_UI_BG);
    uint16_t fg        = edit ? AK_UI_EDIT_FG : AK_UI_SEL_FG;
    bool     has_value = value != NULL && value[0] != '\0';
    uint16_t vw        = has_value ? ak_text_width(value) : 0U;
    if (selected) {
        ak_gfx_round_rect(LCD_LIST_X, y, LCD_LIST_W, LCD_B, LCD_R_ROW, bg);
    } else {
        ak_gfx_round_frame(LCD_LIST_X, y, LCD_LIST_W, LCD_B, LCD_R_ROW, 1U, AK_UI_FRAME, bg);
    }
    uint16_t max_px = (uint16_t)(LCD_LIST_W - 2U * LCD_ROW_PAD - vw - (has_value ? 8U : 0U));
    (void)ak_gfx_text_box((uint16_t)(LCD_LIST_X + LCD_ROW_PAD), y, LCD_B,
                          ak_lcd_fit(label, max_px, true, fit, sizeof(fit)), selected ? fg : AK_UI_TEXT_DIM, bg);
    if (has_value) {
        (void)ak_gfx_text_right((uint16_t)(LCD_LIST_X + LCD_LIST_W - LCD_ROW_PAD), y, LCD_B, value,
                                selected ? fg : value_color, bg);
    }
}

/* Running text line at the page margin, centred in its L-high line box. */
static void ak_lcd_line(uint16_t y, const char *s, uint16_t color)
{
    char fit[96];
    (void)ak_gfx_text_box(LCD_M, y, LCD_L, ak_lcd_fit(s, AK_STATUSLCD_W - 2U * LCD_M, false, fit, sizeof(fit)), color,
                          AK_UI_BG);
}

static void ak_status_bar_set(const char *text, uint16_t color)
{
    (void)snprintf(s_status_bar_text, sizeof(s_status_bar_text), "%s",
                   text == NULL ? "" : text);
    ak_text_utf8_trim(s_status_bar_text);
    s_status_bar_color = color;
}

/* A voice status message: shown ahead of the standing hints for a few seconds. */
static void ak_voice_status(const char *text, uint16_t color)
{
    ak_status_bar_set(text, color);
    s_voice_status_on       = true;
    s_voice_status_until_ms = tal_system_get_millisecond() + AK_VOICE_STATUS_MS;
}

static void ak_status_bar_set_wifi(ak_wifi_state_t state)
{
    switch (state) {
    case AK_WIFI_CONNECTING:
        ak_status_bar_set(ak_tr(AK_STR_ST_WIFI_CONNECTING), AK_UI_ST_INFO);
        break;
    case AK_WIFI_CONNECTED:
        ak_status_bar_set(ak_tr(AK_STR_ST_WIFI_CONNECTED), AK_UI_ST_OK);
        break;
    case AK_WIFI_FAILED:
        ak_status_bar_set(ak_tr(AK_STR_ST_WIFI_FAILED), AK_UI_ST_ERROR);
        break;
    case AK_WIFI_RESTORED:
        ak_status_bar_set(ak_tr(AK_STR_ST_WIFI_RESTORED), AK_UI_ST_NOTE);
        break;
    case AK_WIFI_DISCONNECTED:
    default:
        ak_status_bar_set(ak_tr(AK_STR_ST_WIFI_DISCONNECTED), AK_UI_ST_NOTE);
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
    if ((s_voice_status_on || s_voice_ui_state != AK_VOICE_UI_IDLE) && !s_status_tmp_on) {
        text  = s_status_bar_text; /* listening / recognising / the result, for a few seconds */
        color = s_status_bar_color;
    } else if (ak_provision_is_open(&s_prov, now_ms)) {
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
        color = AK_UI_ST_NOTE;
    } else if (s_status_tmp_on) {
        text  = s_status_tmp_text; /* menu_close msg / notice, 2-3 s */
        color = s_status_tmp_color;
    } else if (ak_cloud_provisioning()) {
        text  = ak_tr(AK_STR_ST_PAIRING_TUYA_APP);
        color = AK_UI_ST_NOTE;
    } else if (s_voice_fallback_active) {
        bool linked = ak_companion_linked();
        text        = ak_tr(linked ? AK_STR_ST_ASR_FALLBACK : AK_STR_ST_VOICE_NEEDS_LINK);
        color       = linked ? AK_UI_ST_NOTE : AK_UI_ST_WARN;
    } else {
        text  = s_status_bar_text;
        color = s_status_bar_color;
    }

    char fit[LCD_STATUS_TEXT_BYTES + 4U];
    ak_gfx_fill_rect(0U, LCD_STATUS_BAR_Y, AK_STATUSLCD_W, LCD_STATUS_BAR_H, AK_UI_BG);
    ak_gfx_fill_rect(0U, LCD_STATUS_BAR_Y, AK_STATUSLCD_W, 1U, AK_UI_RULE);
    (void)ak_gfx_text_box(LCD_M, (uint16_t)(LCD_STATUS_BAR_Y + 1U), (uint16_t)(LCD_STATUS_BAR_H - 1U),
                          ak_lcd_fit(text, AK_STATUSLCD_W - 2U * LCD_M, false, fit, sizeof(fit)), color, AK_UI_BG);
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

/* A sent text is the user's message in the chat history; a failed send takes it
 * back so the retry does not show it twice. */
static void ak_chat_history_drop_sent(void)
{
    if (s_sent_session >= AK_CHAT_SLOT_COUNT) {
        return;
    }
    ak_session_ui_t *session = &s_sessions[s_sent_session];
    if (session->history_count > 0U && !session->history_from_agent[session->history_count - 1U]) {
        session->history_count--;
    }
    s_sent_session = 0xFFU;
}

/* K6 sent the text: it is the user's message in the selected chat. */
static void ak_input_sent_history(const char *text)
{
    ak_chat_history_add(&s_sessions[s_selected_session], text, false);
    s_sent_session = s_selected_session;
}

/* K6 with the strip (or a PC fill) waiting. The Companion sends the strip text
 * to the chat of `slot` through the Crew API, or presses Enter in the PC input
 * box it filled. Without a link the text stays and the status says why. */
static void ak_input_send(const char *id, ak_gesture_t g)
{
    uint32_t sequence;

    if (!ak_companion_linked()) {
        ak_voice_status(ak_tr(AK_STR_ST_INPUT_NO_LINK), AK_UI_ST_ERROR);
        return;
    }
    if (s_input_text[0] == '\0') { /* PC fill: Enter on the PC */
        dispatch_gesture_frame(id, g, "confirm");
    } else {
        if (!transport_begin_sequenced(&sequence)) { /* the link is busy: keep the text */
            ak_voice_status(ak_tr(AK_STR_ST_INPUT_NO_LINK), AK_UI_ST_ERROR);
            return;
        }
        transport_end_sequenced(s_asr_tx_frame,
                                ak_proto_key_gesture_text(s_asr_tx_frame, sizeof(s_asr_tx_frame), sequence, id,
                                                          ak_gesture_str(g), "confirm", s_input_text,
                                                          (uint8_t)(s_selected_session + 1U)));
        ak_input_sent_history(s_input_text);
    }
    ak_input_clear();
    ak_voice_status(ak_tr(AK_STR_ST_INPUT_CONFIRMED), AK_UI_ST_OK);
}

/* One wrapped LCD line (LCD_CHAT_TEXT_W, ak_layout.h). UTF-8: 230 px holds at
 * most 19 (12 px) / 16 (14 px) / 14 (16 px) CJK characters (57 bytes); longer
 * byte runs (4-byte code points drawn as boxes) are cut. */
#define LCD_HISTORY_LINE_BYTES 64U
typedef struct {
    char text[LCD_HISTORY_LINE_BYTES + 1U];
    bool paragraph_start;
    bool from_agent;
} ak_history_line_t;

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

/* Chat column (home history and the Chat page): lines L apart, LCD_MSG_GAP
 * between messages, every agent message in one rounded bubble. */
static uint16_t ak_chat_span_h(const ak_history_line_t *lines, uint8_t a, uint8_t b)
{
    uint16_t h = 0U;
    for (uint8_t k = a; k < b; k++) {
        if (k > a && lines[k].paragraph_start) {
            h = (uint16_t)(h + LCD_MSG_GAP);
        }
        h = (uint16_t)(h + LCD_L);
    }
    return (uint16_t)(h + 2U);
}

/* First line of the newest run of lines[0..count) that fits in avail px. */
static uint8_t ak_chat_first_fit(const ak_history_line_t *lines, uint8_t count, uint16_t avail)
{
    if (count == 0U) {
        return 0U;
    }
    uint8_t first = (uint8_t)(count - 1U);
    while (first > 0U && ak_chat_span_h(lines, (uint8_t)(first - 1U), count) <= avail) {
        first--;
    }
    return first;
}

/* One past the last line from `first` that fits in avail px. */
static uint8_t ak_chat_end_fit(const ak_history_line_t *lines, uint8_t count, uint8_t first, uint16_t avail)
{
    uint8_t end = first;
    while (end < count && ak_chat_span_h(lines, first, (uint8_t)(end + 1U)) <= avail) {
        end++;
    }
    return end;
}

static void ak_chat_column_draw(const ak_history_line_t *lines, uint8_t first, uint8_t end, uint16_t top)
{
    uint16_t y = (uint16_t)(top + 1U);
    uint8_t  k = first;
    while (k < end) {
        if (k > first && lines[k].paragraph_start) {
            y = (uint16_t)(y + LCD_MSG_GAP);
        }
        uint8_t n = 1U;
        while ((uint8_t)(k + n) < end && !lines[k + n].paragraph_start) {
            n++;
        }
        bool agent = lines[k].from_agent;
        if (agent) {
            uint16_t bw = 0U;
            for (uint8_t j = 0U; j < n; j++) {
                uint16_t w = ak_text_width(lines[k + j].text);
                bw         = w > bw ? w : bw;
            }
            ak_gfx_round_frame((uint16_t)(LCD_CHAT_X - LCD_BUBBLE_PAD), (uint16_t)(y - 1U),
                               (uint16_t)(bw + 2U * LCD_BUBBLE_PAD), (uint16_t)(n * LCD_L + 2U), LCD_R_PANEL, 1U,
                               AK_UI_BUBBLE_FRAME, AK_UI_BG);
        }
        for (uint8_t j = 0U; j < n; j++) {
            (void)ak_gfx_text_box(LCD_CHAT_X, (uint16_t)(y + j * LCD_L), LCD_L, lines[k + j].text, AK_UI_TEXT,
                                  AK_UI_BG);
        }
        y = (uint16_t)(y + n * LCD_L);
        k = (uint8_t)(k + n);
    }
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
    /* The newest messages that fit above the status bar, or above the input strip. */
    uint16_t bottom = s_input_text[0] != '\0' ? (uint16_t)(ak_inputbar_top(s_input_text) - 2U)
                                                : (uint16_t)(LCD_STATUS_BAR_Y - 2U);
    uint16_t avail  = (uint16_t)(bottom - LCD_HISTORY_TOP);
    uint8_t  first = ak_chat_first_fit(lines, line_count, avail);
    ak_chat_column_draw(lines, first, ak_chat_end_fit(lines, line_count, first, avail), LCD_HISTORY_TOP);
}

/* FR-03 D2 `keys` home page: one row per key below the session rows, the
 * single-click action in body text and the double / long bindings dimmed
 * after it. Uses the live keymap, so a local change shows at once.
 * shortcut: rows that do not fit above the status bar are left out; all
 * seven fit at 12/14/16 px on the 240 px panel. */
static void ak_keys_page_draw(void)
{
    static const char *const ids[AK_KEYMAP_SLOTS] = {"K1", "K2", "K3", "K4", "K5", "K6", NULL};
    uint16_t row_w   = (uint16_t)(AK_STATUSLCD_W - LCD_CHAT_X - LCD_M);
    uint16_t label_w = (uint16_t)(ak_text_width(ak_tr(AK_STR_KEYS_KNOB)) + 6U);
    uint16_t y       = LCD_HISTORY_TOP;
    for (int i = 0; i < AK_KEYMAP_SLOTS && (uint16_t)(y + LCD_L) <= (uint16_t)(LCD_STATUS_BAR_Y - 2U); i++) {
        const ak_key_entry_t *e = &s_keymap.entries[i];
        char main_txt[24];
        char extra[64];
        char a[24];
        char b[24];
        char fit[96];
        extra[0] = '\0';
        if (e->kind == AK_KEY_KIND_VOICE) {
            (void)snprintf(main_txt, sizeof(main_txt), "%s", ak_tr(AK_STR_KEYS_VOICE));
        } else {
            (void)ak_scene_action_label(e->actions[AK_GESTURE_SLOT_SINGLE], main_txt, sizeof(main_txt));
            if (e->actions[AK_GESTURE_SLOT_DOUBLE] != AK_ACTION_NONE) {
                (void)snprintf(extra, sizeof(extra), "%s %s", ak_tr(AK_STR_KEYS_DOUBLE),
                               ak_scene_action_label(e->actions[AK_GESTURE_SLOT_DOUBLE], a, sizeof(a)));
            }
            if (e->actions[AK_GESTURE_SLOT_LONG] != AK_ACTION_NONE) {
                size_t used = strlen(extra);
                (void)snprintf(extra + used, sizeof(extra) - used, "%s%s %s", used > 0U ? "  " : "",
                               ak_tr(AK_STR_KEYS_LONG),
                               ak_scene_action_label(e->actions[AK_GESTURE_SLOT_LONG], b, sizeof(b)));
            }
        }
        (void)ak_gfx_text_box(LCD_CHAT_X, y, LCD_L, ids[i] != NULL ? ids[i] : ak_tr(AK_STR_KEYS_KNOB),
                              AK_UI_TEXT_ACCENT, AK_UI_BG);
        uint16_t x  = (uint16_t)(LCD_CHAT_X + label_w);
        uint16_t mw = (uint16_t)(ak_gfx_text_box(x, y, LCD_L,
                                                 ak_lcd_fit(main_txt, (uint16_t)(row_w - label_w), true, fit,
                                                            sizeof(fit)),
                                                 AK_UI_TEXT, AK_UI_BG) -
                                 x); /* returns the end x */
        if (extra[0] != '\0' && (uint16_t)(label_w + mw + 8U) < row_w) {
            uint16_t left = (uint16_t)(row_w - label_w - mw - 8U);
            (void)ak_gfx_text_box((uint16_t)(x + mw + 8U), y, LCD_L, ak_lcd_fit(extra, left, true, fit, sizeof(fit)),
                                  AK_UI_TEXT_DIM, AK_UI_BG);
        }
        y = (uint16_t)(y + LCD_L);
    }
}

/* FR-17: a 2 px scroll bar at the right edge of a scrolled text area
 * (x 314..315, clear of 304 px text from x=4 and the Chat column ending at
 * 302). Nothing is drawn when the text fits. */
#define LCD_SCROLL_BAR_X 314U
#define LCD_SCROLL_BAR_W 2U

static void ak_scroll_bar_draw(uint16_t y, uint16_t track, uint16_t total, uint16_t shown, uint16_t first)
{
    uint16_t top = 0U;
    uint16_t h   = 0U;
    if (!ak_scroll_bar(total, shown, first, track, LCD_BAR_MIN, &top, &h)) {
        return; /* everything shows: no bar */
    }
    ak_gfx_fill_rect(LCD_BAR_X, y, LCD_BAR_W, track, AK_UI_BG);
    ak_gfx_fill_rect((uint16_t)(LCD_BAR_X + 1U), y, 1U, track, AK_UI_BAR_TRACK);
    ak_gfx_round_rect(LCD_BAR_X, (uint16_t)(y + top), LCD_BAR_W, h, 2U, AK_UI_BAR_THUMB);
}

/* Turning past the first / last line: a short status bar note (the board has
 * no vibration motor). Returns true when the view moved and needs a redraw. */
static bool ak_scroll_feedback(ak_scroll_result_t r)
{
    if (r == AK_SCROLL_AT_TOP || r == AK_SCROLL_AT_BOTTOM) {
        ak_status_tmp_set(ak_tr(r == AK_SCROLL_AT_TOP ? AK_STR_SCROLL_TOP : AK_STR_SCROLL_BOTTOM), AK_UI_ST_INFO,
                          1500U);
        return true; /* the status bar shows the note */
    }
    return r == AK_SCROLL_MOVED;
}

/* ── Chat page (voice mode Chat, config-ui-design §5.2) ─────────────────── */
#define AK_CHAT_LINES_MAX 64U

static ak_asr_chat_t     s_chat_view;  /* last copy taken from ak_asr */
static ak_history_line_t s_chat_lines[AK_CHAT_LINES_MAX];
static uint8_t           s_chat_line_count;
static ak_scroll_t       s_chat_scroll; /* FR-17: first line shown; follows the bottom unless scrolled up */
static uint32_t          s_chat_sent_turn;
static bool              s_chat_sent_reply;

/* Chat page history: from LCD_HISTORY_TOP to 2 px above the status bar. */
#define LCD_CHAT_AREA_H ((uint16_t)(LCD_STATUS_BAR_Y - 2U - LCD_HISTORY_TOP))

/* First line of the newest run that fits the Chat area (the scroll maximum). */
static uint8_t ak_chat_first_fitting(uint8_t end)
{
    return ak_chat_first_fit(s_chat_lines, end, LCD_CHAT_AREA_H);
}

/* One past the last line that fits the Chat area when the view starts at `first`. */
static uint8_t ak_chat_end_fitting(uint8_t first, uint16_t avail)
{
    return ak_chat_end_fit(s_chat_lines, s_chat_line_count, first, avail);
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

/* The Chat text was rebuilt: a new turn starts at the bottom; the same turn
 * growing (streamed reply) follows at the bottom or keeps a scrolled-up view. */
static void ak_chat_scroll_sync(bool new_turn)
{
    uint16_t max = ak_chat_first_fitting(s_chat_line_count);
    if (new_turn) {
        ak_scroll_reset(&s_chat_scroll, max, true);
    } else {
        ak_scroll_update(&s_chat_scroll, max);
    }
}

/* EC11 rotation on the Chat page: clockwise towards the newest line. */
static void ak_chat_scroll(int32_t detents)
{
    if (ak_scroll_feedback(ak_scroll_step(&s_chat_scroll, detents))) {
        ak_render_status();
    }
}

static void ak_chat_page_draw(void)
{
    const char *state = ak_tr(AK_STR_CHAT_IDLE);
    char        fit[64];
    if (s_voice_ui_state == AK_VOICE_UI_LISTENING) {
        state = ak_tr(AK_STR_ST_LISTENING);
    } else if (s_voice_ui_state == AK_VOICE_UI_RECOGNIZING) {
        state = ak_tr(AK_STR_CHAT_THINKING);
    } else if (s_chat_view.replying || ak_asr_audio_busy()) {
        state = ak_tr(AK_STR_CHAT_REPLYING);
    }
    uint16_t row_w = (uint16_t)(AK_STATUSLCD_W - LCD_CHAT_X - LCD_M);
    (void)ak_gfx_text_box(LCD_CHAT_X, LCD_CHAT_ROW1_Y, LCD_L,
                          ak_lcd_fit(ak_tr(AK_STR_CHAT_TITLE), row_w, false, fit, sizeof(fit)), AK_UI_TEXT_ACCENT,
                          AK_UI_BG);
    (void)ak_gfx_text_box(LCD_CHAT_X, LCD_CHAT_ROW2_Y, LCD_L, ak_lcd_fit(state, row_w, false, fit, sizeof(fit)),
                          AK_UI_TEXT_DIM, AK_UI_BG);

    uint16_t bottom = (uint16_t)(LCD_STATUS_BAR_Y - 2U);
    if (s_chat_scroll.news) {
        /* Scrolled up while the reply grew: a Purple pill on its own row above the status bar. */
        const char *news = ak_tr(AK_STR_SCROLL_NEW);
        uint16_t    bw   = (uint16_t)(ak_text_width(news) + 2U * LCD_PAD);
        uint16_t    xr   = (uint16_t)(AK_STATUSLCD_W - LCD_M - LCD_GUTTER + 2U);
        uint16_t    py   = (uint16_t)(LCD_STATUS_BAR_Y - 3U - LCD_L);
        bottom           = (uint16_t)(LCD_STATUS_BAR_Y - 4U - LCD_L - 2U);
        ak_gfx_round_rect((uint16_t)(xr - bw), py, bw, LCD_L, LCD_R_PANEL, AK_UI_PILL_BG);
        (void)ak_gfx_text_center((uint16_t)(xr - bw), py, bw, LCD_L, news, AK_UI_PILL_FG, AK_UI_PILL_BG);
    }
    uint16_t avail = (uint16_t)(bottom - LCD_HISTORY_TOP);
    uint8_t  first = s_chat_scroll.first < s_chat_line_count ? (uint8_t)s_chat_scroll.first : 0U;
    uint8_t  end   = ak_chat_end_fitting(first, avail);
    ak_chat_column_draw(s_chat_lines, first, end, LCD_HISTORY_TOP);
    ak_scroll_bar_draw(LCD_HISTORY_TOP, avail, s_chat_line_count, (uint16_t)(end - first), first);
}

/* Settings list geometry: lcd_list_y0() / lcd_list_rows() (ak_layout.h). */

static bool item_value_row(ak_settings_item_t item)
{
    return item == AK_SETTINGS_ITEM_VOLUME || item == AK_SETTINGS_ITEM_BRIGHTNESS;
}

/* "Unbind device" is listed only on a device that is activated on Tuya: with no
 * binding there is nothing to unbind, and next to "Pairing mode" it looks like a
 * duplicate. Called before every draw and every key, and from the app loop when
 * the activation state changes, so the list and the cursor follow at once. */
static bool ak_settings_sync_rebind(void)
{
    return ak_settings_set_rebind_hidden(&s_settings, !ak_cloud_activated());
}

/* Top of a visible settings row, or 0 when the item is not listed or the list is scrolled past it. */
static uint16_t ak_settings_row_y(ak_settings_item_t item)
{
    if (!ak_settings_item_visible(&s_settings, item)) {
        return 0U;
    }
    unsigned rows  = lcd_list_rows();
    unsigned first = ak_settings_first_row(ak_settings_visible_pos(&s_settings, s_settings.selected), rows);
    unsigned pos   = ak_settings_visible_pos(&s_settings, item);
    if (pos < first || pos >= first + rows) {
        return 0U;
    }
    return (uint16_t)(lcd_list_y0() + (uint16_t)(pos - first) * LCD_R);
}

/* One settings row: framed when idle, filled when
 * selected, inverted while editing; label left, value right. */
static void ak_settings_row(ak_settings_item_t item, const char *label, const char *value, uint16_t value_color)
{
    uint16_t y        = ak_settings_row_y(item);
    bool     selected = s_settings.selected == item;
    bool     editing  = selected && s_settings.editing;

    if (y == 0U) {
        return;
    }
    ak_list_row(y, label, value, value_color, selected, editing);
}

/* Pairing mode row: "OPEN m:ss" while the window is open. Also redrawn alone
 * once a second, so the countdown never costs a full-screen repaint. */
static void ak_settings_draw_pairing_row(uint32_t now_ms)
{
    /* Activated: the row only changes Wi-Fi; Unbind device is the next row. */
    const char *label = ak_tr(ak_cloud_activated() ? AK_STR_SET_CHANGE_WIFI : AK_STR_SET_PAIRING);
    if (ak_provision_is_open(&s_prov, now_ms)) {
        char     value[24];
        uint32_t left = ak_provision_remaining_s(&s_prov, now_ms);
        (void)snprintf(value, sizeof(value), "%s %u:%02u", ak_tr(AK_STR_SET_OPEN), (unsigned)(left / 60U),
                       (unsigned)(left % 60U));
        ak_settings_row(AK_SETTINGS_ITEM_PAIRING, label, value, AK_UI_TEXT_ACCENT);
    } else {
        ak_settings_row(AK_SETTINGS_ITEM_PAIRING, label, ">", AK_UI_TEXT_DIM);
    }
}

/* BLE pairing row: "OPEN m:ss" while the over-the-air pairing window is open. */
static void ak_settings_draw_ble_pair_row(uint32_t now_ms)
{
#if AK_TRANSPORT_HAS_BLE
    if (ak_ble_pair_is_open(&s_ble_pair, now_ms)) {
        char     value[24];
        uint32_t left = ak_ble_pair_remaining_s(&s_ble_pair, now_ms);
        (void)snprintf(value, sizeof(value), "%s %u:%02u", ak_tr(AK_STR_SET_OPEN), (unsigned)(left / 60U),
                       (unsigned)(left % 60U));
        ak_settings_row(AK_SETTINGS_ITEM_BLE_PAIR, ak_tr(AK_STR_SET_BLE_PAIR), value, AK_UI_TEXT_ACCENT);
        return;
    }
#else
    (void)now_ms;
#endif
    ak_settings_row(AK_SETTINGS_ITEM_BLE_PAIR, ak_tr(AK_STR_SET_BLE_PAIR), ">", AK_UI_TEXT_DIM);
}

/* ⑦ confirm dialog body: title bar, lines from LCD_DLG_TOP
 * L apart (a half line before `gap` lines), the key prompt centred in a
 * rounded box, the time-out centred above the status bar (left visible). */
typedef struct {
    const char *text;
    uint16_t    color;
    bool        gap;
} ak_dlg_line_t;

static void ak_draw_dialog(const char *title, uint16_t warn, const ak_dlg_line_t *lines, size_t n, const char *keys)
{
    uint16_t keys_frame = warn != AK_UI_NO_WARN ? warn : AK_UI_KEYS_FRAME;
    ak_gfx_fill_rect(0U, 0U, AK_STATUSLCD_W, LCD_STATUS_BAR_Y, AK_UI_BG);
    ak_title_bar(title, warn, NULL);
    uint16_t y = LCD_DLG_TOP;
    for (size_t i = 0U; i < n; i++) {
        if (lines[i].gap) {
            y = (uint16_t)(y + LCD_L / 2U);
        }
        ak_lcd_line(y, lines[i].text, lines[i].color);
        y = (uint16_t)(y + LCD_L);
    }
    if (keys != NULL) {
        ak_gfx_round_frame(LCD_M, LCD_DLG_KEYS_Y, AK_STATUSLCD_W - 2U * LCD_M, LCD_DLG_KEYS_H, LCD_R_PANEL, 1U,
                           keys_frame, AK_UI_BG);
        (void)ak_gfx_text_center(LCD_M, LCD_DLG_KEYS_Y, AK_STATUSLCD_W - 2U * LCD_M, LCD_DLG_KEYS_H, keys, AK_UI_TEXT,
                                 AK_UI_BG);
        (void)ak_gfx_text_center(LCD_M, LCD_DLG_TIMEOUT_Y, AK_STATUSLCD_W - 2U * LCD_M, LCD_L,
                                 ak_tr(AK_STR_RST_TIMEOUT), AK_UI_TEXT_DIM, AK_UI_BG);
    }
}

/* "Reset network?" — a Companion reset_netcfg waits for K6 (FW-12). Leaves
 * the status bar (window countdown) visible. */
static void ak_draw_reset_confirm(void)
{
    const ak_dlg_line_t lines[] = {{ak_tr(AK_STR_RST_L1), AK_UI_TEXT, false},
                                   {ak_tr(AK_STR_RST_L2), AK_UI_TEXT, false},
                                   {ak_tr(AK_STR_RST_L3), AK_UI_TEXT, false},
                                   {ak_tr(AK_STR_RST_L4), AK_UI_TEXT_DIM, true}};
    ak_draw_dialog(ak_tr(AK_STR_RST_TITLE), AK_UI_WARN_DANGER, lines, 4U, ak_tr(AK_STR_RST_KEYS));
}

/* "Clear BLE pairing?" — a Companion ble_unpair waits for K6 (FR-02). */
static void ak_draw_unpair_confirm(void)
{
    const ak_dlg_line_t lines[] = {{ak_tr(AK_STR_UNPAIR_L1), AK_UI_TEXT, false},
                                   {ak_tr(AK_STR_UNPAIR_L2), AK_UI_TEXT, false}};
    ak_draw_dialog(ak_tr(AK_STR_UNPAIR_TITLE), AK_UI_WARN_DANGER, lines, 2U, ak_tr(AK_STR_UNPAIR_KEYS));
}

/* "Unbind and re-pair?" — Settings > Unbind & re-pair on an activated device.
 * A yellow rule like every double confirm; K6 also flashes yellow. */
static void ak_draw_rebind_confirm(void)
{
    const ak_dlg_line_t lines[] = {{ak_tr(AK_STR_REBIND_L1), AK_UI_TEXT, false},
                                   {ak_tr(AK_STR_REBIND_L2), AK_UI_TEXT, false},
                                   {ak_tr(AK_STR_REBIND_L3), AK_UI_TEXT, false},
                                   {ak_tr(AK_STR_REBIND_L4), AK_UI_TEXT_DIM, true}};
    ak_draw_dialog(ak_tr(AK_STR_REBIND_TITLE), AK_UI_WARN_DOUBLE, lines, 4U, ak_tr(AK_STR_REBIND_KEYS));
}

/* Settings > Reset system: both dialogs list what is cleared (I1-I6); dialog
 * 1 says a second confirm follows, dialog 2 that the License must be written
 * again. Both red (danger), K6 / K1, 10 s each. */
static void ak_draw_freset_confirm(ak_freset_stage_t stage)
{
    bool                last  = stage == AK_FRESET_ASK2;
    const ak_dlg_line_t lines[] = {{ak_tr(AK_STR_FRESET_I1), AK_UI_TEXT, false},
                                   {ak_tr(AK_STR_FRESET_I2), AK_UI_TEXT, false},
                                   {ak_tr(AK_STR_FRESET_I3), AK_UI_TEXT, false},
                                   {ak_tr(AK_STR_FRESET_I4), AK_UI_TEXT, false},
                                   {ak_tr(AK_STR_FRESET_I5), AK_UI_TEXT, false},
                                   {ak_tr(AK_STR_FRESET_I6), AK_UI_TEXT, false},
                                   {ak_tr(last ? AK_STR_FRESET_NOTE2 : AK_STR_FRESET_NOTE1),
                                    last ? AK_UI_ST_WARN : AK_UI_TEXT_DIM, true}};
    ak_draw_dialog(ak_tr(last ? AK_STR_FRESET_TITLE2 : AK_STR_FRESET_TITLE1), AK_UI_WARN_DANGER, lines, 7U,
                   ak_tr(last ? AK_STR_FRESET_KEYS2 : AK_STR_FRESET_KEYS1));
}

#if AK_TRANSPORT_HAS_BLE
/* "Pair over BLE?" — the 6-digit code of an over-the-air pairing; K6 pairs
 * only when the Companion shows the same digits (ak_ble_pair.h). */
static void ak_draw_ble_pair_confirm(void)
{
    char     code[12];
    uint32_t v = ak_ble_pair_code(&s_ble_pair);
    (void)snprintf(code, sizeof(code), "%03u %03u", (unsigned)(v / 1000U), (unsigned)(v % 1000U));
    const ak_dlg_line_t lines[] = {{ak_tr(AK_STR_BLE_PAIR_L1), AK_UI_TEXT, false},
                                   {code, AK_UI_TEXT_ACCENT, true},
                                   {ak_tr(AK_STR_BLE_PAIR_L3), AK_UI_TEXT_DIM, true}};
    ak_draw_dialog(ak_tr(AK_STR_BLE_PAIR_TITLE), AK_UI_NO_WARN, lines, 3U, ak_tr(AK_STR_BLE_PAIR_KEYS));
    /* This dialog times out after 30 s, not the 10 s ak_draw_dialog prints. */
    ak_gfx_fill_rect(LCD_M, LCD_DLG_TIMEOUT_Y, AK_STATUSLCD_W - 2U * LCD_M, LCD_L, AK_UI_BG);
    (void)ak_gfx_text_center(LCD_M, LCD_DLG_TIMEOUT_Y, AK_STATUSLCD_W - 2U * LCD_M, LCD_L,
                             ak_tr(AK_STR_BLE_PAIR_TIMEOUT), AK_UI_TEXT_DIM, AK_UI_BG);
}
#endif

#if AK_TRANSPORT_HAS_UART
/* ⑦ firmware update: the K6 / K1 question, then progress.
 * Keys are ignored after K6 until the keyboard reboots or the update fails. */
/* ⑦ firmware update: the K6 / K1 question, then progress.
 * Keys are ignored after K6 until the keyboard reboots or the update fails. */
static void ak_draw_ota(const ak_ota_view_t *v)
{
    char line[64];
    char run[64];
    if (v->state == AK_OTA_CONFIRM) {
        (void)snprintf(line, sizeof(line), ak_tr(AK_STR_OTA_INSTALL), v->version);
        (void)snprintf(run, sizeof(run), ak_tr(AK_STR_OTA_RUNNING), APP_FW_VERSION);
        const ak_dlg_line_t lines[] = {{line, AK_UI_TEXT, false},
                                       {run, AK_UI_TEXT, false},
                                       {ak_tr(AK_STR_OTA_KEEP_USB), AK_UI_TEXT, true},
                                       {ak_tr(AK_STR_OTA_KEPT), AK_UI_TEXT_DIM, false}};
        ak_draw_dialog(ak_tr(AK_STR_OTA_TITLE), AK_UI_NO_WARN, lines, 4U, ak_tr(AK_STR_OTA_KEYS));
        return;
    }
    ak_gfx_fill_rect(0U, 0U, AK_STATUSLCD_W, LCD_STATUS_BAR_Y, AK_UI_BG);
    ak_title_bar(ak_tr(AK_STR_OTA_TITLE), AK_UI_NO_WARN, NULL);
    unsigned pct = v->state == AK_OTA_RECEIVING ? (unsigned)v->percent : 100U;
    if (v->state == AK_OTA_VERIFIED) {
        (void)snprintf(line, sizeof(line), ak_tr(AK_STR_OTA_INSTALLING), v->version);
    } else if (v->state == AK_OTA_ERASING) {
        (void)snprintf(line, sizeof(line), ak_tr(AK_STR_OTA_PREPARING), v->version);
        pct = 0U;
    } else {
        (void)snprintf(line, sizeof(line), ak_tr(AK_STR_OTA_RECEIVING), v->version, (unsigned)v->percent);
    }
    uint16_t y = LCD_DLG_TOP;
    ak_lcd_line(y, line, AK_UI_TEXT);
    y = (uint16_t)(y + LCD_L + 6U);
    /* Progress bar: a rounded frame, the filled part 2 px inside. */
    uint16_t bw = (uint16_t)(AK_STATUSLCD_W - 2U * LCD_M), bh = (uint16_t)(LCD_B - 6U);
    ak_gfx_round_frame(LCD_M, y, bw, bh, LCD_R_ROW, 1U, AK_UI_FRAME, AK_UI_BG);
    ak_gfx_round_rect((uint16_t)(LCD_M + 2U), (uint16_t)(y + 2U),
                      (uint16_t)((bw - 4U) * (pct > 100U ? 100U : pct) / 100U), (uint16_t)(bh - 4U), LCD_R_ROW - 2U,
                      AK_UI_PROGRESS);
    y = (uint16_t)(y + bh + LCD_L / 2U);
    ak_lcd_line(y, ak_tr(AK_STR_OTA_NO_UNPLUG), AK_UI_TEXT_ACCENT);
    ak_lcd_line((uint16_t)(y + LCD_L), ak_tr(AK_STR_OTA_RESTART), AK_UI_TEXT_DIM);
}
#endif

/* Settings > Device info (read-only): connection, Wi-Fi, Bluetooth and device
 * lines from one snapshot (ak_devinfo.h), taken when the page opens and again
 * when a link, address or pairing changes (ak_devinfo_poll). The credentials
 * only ever say SET / NOT SET. Longer than the screen: the knob scrolls. The
 * two actions the old Credentials page had stay on their keys (K1 twice clears
 * the BLE pairing, K6 allows one License overwrite) and are named in the footer. */
static ak_devinfo_line_t s_devinfo_lines[AK_DEVINFO_MAX_LINES];
static size_t            s_devinfo_n;
static ak_scroll_t       s_devinfo_scroll;

static void ak_devinfo_page_open(void)
{
    ak_devinfo_snapshot(&s_devinfo, true);
    s_devinfo_sig      = s_devinfo;
    s_devinfo_sig.rssi = 0;
    s_devinfo_n        = ak_devinfo_build(&s_devinfo, s_devinfo_lines);
    ak_scroll_reset(&s_devinfo_scroll, s_devinfo_n > lcd_list_rows() ? (uint16_t)(s_devinfo_n - lcd_list_rows()) : 0U,
                    false);
}

static void ak_devinfo_page_changed(void)
{
    if (s_settings_active && s_devinfo_page) {
        s_devinfo_n = ak_devinfo_build(&s_devinfo, s_devinfo_lines);
        ak_settings_draw();
    }
}

static void ak_settings_draw_devinfo(uint32_t now_ms)
{
    uint16_t rows  = lcd_list_rows();
    uint16_t total = (uint16_t)s_devinfo_n;
    ak_scroll_update(&s_devinfo_scroll, total > rows ? (uint16_t)(total - rows) : 0U);
    uint16_t first = s_devinfo_scroll.first;

    ak_title_bar(ak_tr(AK_STR_SET_DEVINFO), AK_UI_NO_WARN, NULL);
    uint16_t y0 = lcd_list_y0();
    for (uint16_t r = 0U; r < rows && (uint16_t)(first + r) < total; r++) {
        const ak_devinfo_line_t *l = &s_devinfo_lines[first + r];
        uint16_t                 y = (uint16_t)(y0 + r * LCD_R);
        if (l->header) {
            (void)ak_gfx_text_box((uint16_t)(LCD_LIST_X + LCD_ROW_PAD), y, LCD_B, l->label, AK_UI_TEXT_ACCENT, AK_UI_BG);
            ak_gfx_fill_rect(LCD_LIST_X, (uint16_t)(y + LCD_B - 1U), LCD_LIST_W, 1U, AK_UI_RULE);
        } else {
            ak_list_row(y, l->label, l->value,
                        l->tone == AK_DI_TONE_DIM ? AK_UI_TEXT_DIM
                                                  : (l->tone == AK_DI_TONE_ATTENTION ? AK_UI_TEXT_ACCENT : AK_UI_TEXT),
                        false, false);
        }
    }
    ak_scroll_bar_draw(y0, (uint16_t)(rows * LCD_R - 2U), total, rows, first);

    /* Footer: the License action that applies now, the BLE pairing action, the keys. */
    bool        license = s_devinfo.uuid && s_devinfo.authkey;
    const char *lic     = NULL;
    if (ak_provision_overwrite_armed(&s_prov, now_ms)) {
        lic = ak_tr(AK_STR_CRED_OVERWRITE_ON);
    } else if (license && ak_provision_is_open(&s_prov, now_ms)) {
        lic = ak_tr(AK_STR_CRED_OVERWRITE_K6);
    } else if (!license) {
        lic = ak_tr(AK_STR_CRED_NEED_USB);
    }
    const char *unpair = NULL;
    uint16_t    uc     = AK_UI_TEXT_DIM;
#if AK_TRANSPORT_DUAL || AK_TRANSPORT_BLE_GATT
    if (s_ble_key_set) {
        unpair = s_cred_unpair_armed ? ak_tr(AK_STR_CRED_UNPAIR_ARMED) : ak_tr(AK_STR_CRED_UNPAIR);
        uc     = s_cred_unpair_armed ? AK_UI_TEXT_ACCENT : AK_UI_TEXT_DIM;
    }
#endif
    if (lic != NULL) {
        ak_footer_draw(lic, AK_UI_TEXT_ACCENT, unpair != NULL ? unpair : ak_tr(AK_STR_CRED_BACK),
                       unpair != NULL ? uc : AK_UI_TEXT_DIM);
    } else {
        ak_footer_draw(ak_tr(AK_STR_CRED_BACK), AK_UI_TEXT_ACCENT, unpair, uc);
    }
}

/* Settings > Saved Wi-Fi (FR-15): the remembered networks, most recently
 * connected first, in list rows like the settings page. SSID only; there is no password anywhere. */
static uint8_t ak_wifi_page_count(ak_wifi_saved_t *list)
{
    return (uint8_t)ak_wifi_saved_list(list);
}

static void ak_wifi_page_disarm(void)
{
    s_wifi_del_armed = false;
}

static void ak_settings_draw_wifi(uint32_t now_ms)
{
    ak_wifi_saved_t list[AK_WIFI_STORE_MAX];
    uint8_t         n = ak_wifi_page_count(list);
    char            title[48];

    if (s_wifi_sel >= n) {
        s_wifi_sel = n > 0U ? (uint8_t)(n - 1U) : 0U;
    }
    (void)snprintf(title, sizeof(title), "%s  %u/%u", ak_tr(AK_STR_SET_WIFI), (unsigned)n,
                   (unsigned)AK_WIFI_STORE_MAX);
    ak_title_bar(title, AK_UI_NO_WARN, NULL);

    uint16_t y0   = lcd_list_y0();
    uint16_t rows = lcd_list_rows();
    if (n == 0U) {
        (void)ak_gfx_text_box((uint16_t)(LCD_LIST_X + LCD_ROW_PAD), y0, LCD_B, ak_tr(AK_STR_WIFI_EMPTY), AK_UI_TEXT_DIM,
                              AK_UI_BG);
        (void)ak_gfx_text_box((uint16_t)(LCD_LIST_X + LCD_ROW_PAD), (uint16_t)(y0 + LCD_R), LCD_B,
                              ak_tr(AK_STR_WIFI_ADD_USB), AK_UI_TEXT_DIM, AK_UI_BG);
    }
    uint8_t first = (uint8_t)ak_settings_first_row(s_wifi_sel, rows);
    for (uint8_t i = first; i < n && i < first + rows; i++) {
        ak_list_row((uint16_t)(y0 + (uint16_t)(i - first) * LCD_R), list[i].ssid,
                    list[i].current ? ak_tr(AK_STR_WIFI_CURRENT) : NULL, AK_UI_TEXT, i == s_wifi_sel, false);
    }
    ak_scroll_bar_draw(y0, (uint16_t)(rows * LCD_R - 2U), n, rows, first);

    bool armed = s_wifi_del_armed && (int32_t)(s_wifi_del_deadline_ms - now_ms) > 0 && n > 0U;
    ak_footer_draw(armed ? ak_tr(list[s_wifi_sel].current ? AK_STR_WIFI_CONFIRM_CUR : AK_STR_WIFI_CONFIRM)
                         : ak_tr(AK_STR_WIFI_HINT1),
                   AK_UI_TEXT_ACCENT, ak_tr(AK_STR_WIFI_HINT2), AK_UI_TEXT_DIM);
}

/* Help page: the three ways to change the Wi-Fi network and what each keeps. */
static void ak_settings_draw_wifi_help(void)
{
    char saved[64];
    ak_title_bar(ak_tr(AK_STR_WIFI_HELP_TITLE), AK_UI_NO_WARN, NULL);
    (void)snprintf(saved, sizeof(saved), ak_tr(AK_STR_WIFI_HELP_D1), (unsigned)AK_WIFI_STORE_MAX);
    const ak_dlg_line_t lines[] = {{ak_tr(AK_STR_WIFI_HELP_H1), AK_UI_TEXT, false},
                                   {saved, AK_UI_TEXT_DIM, false},
                                   {ak_tr(AK_STR_WIFI_HELP_D4), AK_UI_TEXT_DIM, false},
                                   {ak_tr(AK_STR_WIFI_HELP_H2), AK_UI_TEXT, true},
                                   {ak_tr(AK_STR_WIFI_HELP_D2), AK_UI_TEXT_DIM, false},
                                   {ak_tr(AK_STR_WIFI_HELP_H3), AK_UI_TEXT, true},
                                   {ak_tr(AK_STR_WIFI_HELP_D3), AK_UI_TEXT_DIM, false}};
    uint16_t            y       = LCD_DLG_TOP;
    for (size_t i = 0U; i < sizeof(lines) / sizeof(lines[0]); i++) {
        if (lines[i].gap) {
            y = (uint16_t)(y + LCD_L / 2U);
        }
        ak_lcd_line(y, lines[i].text, lines[i].color);
        y = (uint16_t)(y + LCD_L);
    }
    ak_footer_draw(ak_tr(AK_STR_WIFI_HELP_KEYS), AK_UI_TEXT_ACCENT, ak_tr(AK_STR_WIFI_HELP_WHY), AK_UI_TEXT_DIM);
}

/* "Enter pairing mode?" confirm dialog (Cancel preselected): two rounded
 * buttons with centred labels above the auto-cancel line. */
static void ak_settings_draw_pair_confirm(uint32_t now_ms)
{
    char line[64];
    bool bound = ak_cloud_activated();
    ak_title_bar(ak_tr(bound ? AK_STR_CHWIFI_TITLE : AK_STR_PAIR_TITLE), AK_UI_NO_WARN, NULL);
    (void)snprintf(line, sizeof(line), ak_tr(bound ? AK_STR_CHWIFI_L2 : AK_STR_PAIR_L2),
                   (unsigned)(AK_PROV_WINDOW_MS / 1000U));
    const ak_dlg_line_t lines[] = {{ak_tr(bound ? AK_STR_CHWIFI_L1 : AK_STR_PAIR_L1), AK_UI_TEXT, false},
                                   {line, AK_UI_TEXT, false},
                                   {ak_tr(bound ? AK_STR_CHWIFI_L3 : AK_STR_PAIR_L3), AK_UI_TEXT_DIM, true},
                                   {ak_tr(bound ? AK_STR_CHWIFI_L4 : AK_STR_PAIR_L4), AK_UI_TEXT_DIM, false}};
    uint16_t            y       = LCD_DLG_TOP;
    for (size_t i = 0U; i < sizeof(lines) / sizeof(lines[0]); i++) {
        if (lines[i].gap) {
            y = (uint16_t)(y + LCD_L / 2U);
        }
        ak_lcd_line(y, lines[i].text, lines[i].color);
        y = (uint16_t)(y + LCD_L);
    }

    bool        confirm  = s_prov.choice_confirm;
    const char *label[2] = {ak_tr(AK_STR_PAIR_CANCEL), ak_tr(AK_STR_PAIR_CONFIRM)};
    for (uint16_t k = 0U; k < 2U; k++) {
        bool     sel = (k == 1U) == confirm;
        uint16_t bx  = (uint16_t)(LCD_M + k * (LCD_BTN_W + 8U));
        uint16_t bg  = sel ? AK_UI_SEL_BG : AK_UI_BG;
        if (sel) {
            ak_gfx_round_rect(bx, LCD_BTN_Y, LCD_BTN_W, LCD_BTN_H, LCD_R_ROW, bg);
        } else {
            ak_gfx_round_frame(bx, LCD_BTN_Y, LCD_BTN_W, LCD_BTN_H, LCD_R_ROW, 1U, AK_UI_FRAME, bg);
        }
        (void)ak_gfx_text_center(bx, LCD_BTN_Y, LCD_BTN_W, LCD_BTN_H, label[k], sel ? AK_UI_SEL_FG : AK_UI_TEXT, bg);
    }

    (void)snprintf(line, sizeof(line), ak_tr(AK_STR_PAIR_AUTO_CANCEL),
                   (unsigned)ak_provision_remaining_s(&s_prov, now_ms));
    (void)ak_gfx_text_center(LCD_M, LCD_BTN_NOTE_Y, AK_STATUSLCD_W - 2U * LCD_M, LCD_L, line, AK_UI_TEXT_DIM, AK_UI_BG);
    ak_footer_draw(ak_tr(AK_STR_PAIR_KEYS), AK_UI_TEXT_ACCENT, ak_tr(AK_STR_PAIR_HINT), AK_UI_TEXT_DIM);
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

    ak_gfx_clear(AK_UI_BG);
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
    if (ak_provision_unpair_pending(&s_prov, now_ms)) {
        ak_draw_unpair_confirm();
        ak_status_bar_draw();
        ak_statuslcd_flush();
        s_lcd_redraw_pending = false;
        return;
    }
    if (ak_provision_rebind_pending(&s_prov, now_ms)) {
        ak_draw_rebind_confirm();
        ak_status_bar_draw();
        ak_statuslcd_flush();
        s_lcd_redraw_pending = false;
        return;
    }
    if (ak_freset_up(now_ms)) {
        ak_draw_freset_confirm(ak_freset_stage(&s_freset, now_ms));
        ak_status_bar_draw();
        ak_statuslcd_flush();
        s_lcd_redraw_pending = false;
        return;
    }
#if AK_TRANSPORT_HAS_BLE
    if (s_ble_pair.state == AK_BLE_PAIR_COMPARE) {
        ak_draw_ble_pair_confirm();
        ak_status_bar_draw();
        ak_statuslcd_flush();
        s_lcd_redraw_pending = false;
        return;
    }
#endif
    if (s_devinfo_page) {
        ak_settings_draw_devinfo(now_ms);
        ak_statuslcd_flush();
        s_lcd_redraw_pending = false;
        return;
    }
    if (s_wifi_help) {
        ak_settings_draw_wifi_help();
        ak_statuslcd_flush();
        s_lcd_redraw_pending = false;
        return;
    }
    if (s_wifi_page) {
        ak_settings_draw_wifi(now_ms);
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
    if (ak_bind_active(&s_bind)) {
        ak_bind_draw(now_ms);
        ak_statuslcd_flush();
        s_lcd_redraw_pending = false;
        return;
    }
    (void)ak_settings_sync_rebind();
    ak_title_bar(ak_tr(AK_STR_SET_TITLE), AK_UI_NO_WARN, NULL);

    static const ak_str_id_t led_mode_str[AK_LED_MODE_COUNT] = {AK_STR_LED_OFF, AK_STR_LED_ON, AK_STR_LED_AUTO};
    static const ak_str_id_t led_time_str[AK_LED_TIMEOUT_COUNT] = {AK_STR_TIME_30S, AK_STR_TIME_60S,
                                                                   AK_STR_TIME_5MIN, AK_STR_TIME_15MIN};
    char value[32];
    (void)snprintf(value, sizeof(value), "< %3u%% >", (unsigned)s_settings.volume);
    ak_settings_row(AK_SETTINGS_ITEM_VOLUME, ak_tr(AK_STR_SET_VOLUME), value, AK_UI_TEXT_ACCENT);
    (void)snprintf(value, sizeof(value), "< %3u%% >", (unsigned)s_settings.brightness);
    ak_settings_row(AK_SETTINGS_ITEM_BRIGHTNESS, ak_tr(AK_STR_SET_BRIGHTNESS), value, AK_UI_TEXT_ACCENT);
    (void)snprintf(value, sizeof(value), "< %s >",
                   ak_tr(s_settings.voice_chat ? AK_STR_VOICE_CHAT : AK_STR_VOICE_ASR));
    ak_settings_row(AK_SETTINGS_ITEM_VOICE_MODE, ak_tr(AK_STR_SET_VOICE_MODE), value, AK_UI_TEXT_ACCENT);
    (void)snprintf(value, sizeof(value), "< %s >",
                   ak_tr(led_mode_str[s_settings.led_mode < AK_LED_MODE_COUNT ? s_settings.led_mode : 0U]));
    ak_settings_row(AK_SETTINGS_ITEM_LED_POWER, ak_tr(AK_STR_SET_LED_POWER), value, AK_UI_TEXT_ACCENT);
    (void)snprintf(value, sizeof(value), "< %s >",
                   ak_tr(led_time_str[s_settings.led_timeout_idx < AK_LED_TIMEOUT_COUNT ? s_settings.led_timeout_idx
                                                                                         : 0U]));
    ak_settings_row(AK_SETTINGS_ITEM_LED_TIMEOUT, ak_tr(AK_STR_SET_LED_TIMEOUT), value,
                    s_settings.led_mode == (uint8_t)AK_LED_MODE_AUTO ? AK_UI_TEXT_ACCENT : AK_UI_TEXT_DIM);
    /* Shown in its own language either way, so it can always be found again. */
    (void)snprintf(value, sizeof(value), "< %s >", ak_lang_name((ak_lang_t)s_settings.lang));
    ak_settings_row(AK_SETTINGS_ITEM_LANGUAGE, ak_tr(AK_STR_SET_LANGUAGE), value, AK_UI_TEXT_ACCENT);
    (void)snprintf(value, sizeof(value), "< %u px >",
                   (unsigned)ak_text_size_px((ak_text_size_t)s_settings.font_size));
    ak_settings_row(AK_SETTINGS_ITEM_FONT_SIZE, ak_tr(AK_STR_SET_FONT_SIZE), value, AK_UI_TEXT_ACCENT);
    (void)snprintf(value, sizeof(value), "< %s >", ak_tr(s_settings.animation ? AK_STR_VAL_ON : AK_STR_VAL_OFF));
    ak_settings_row(AK_SETTINGS_ITEM_ANIMATION, ak_tr(AK_STR_SET_ANIMATION), value, AK_UI_TEXT_ACCENT);
    if (s_scene_active >= 0) {
        char name[64];
        (void)snprintf(value, sizeof(value), "%s%s >",
                       ak_lcd_fit(s_scenes[s_scene_active].name, (uint16_t)(LCD_LIST_W / 2U), true, name, sizeof(name)),
                       ak_scene_dirty() ? "*" : "");
    } else {
        (void)snprintf(value, sizeof(value), ">");
    }
    ak_settings_row(AK_SETTINGS_ITEM_SCENE, ak_tr(AK_STR_SET_SCENE), value, AK_UI_TEXT_DIM);
    ak_settings_row(AK_SETTINGS_ITEM_BIND_SESSION, ak_tr(AK_STR_SET_BIND), ">", AK_UI_TEXT_DIM);
    ak_settings_row(AK_SETTINGS_ITEM_WIFI, ak_tr(AK_STR_SET_WIFI), ">", AK_UI_TEXT_DIM);
    ak_settings_row(AK_SETTINGS_ITEM_DEVICE_INFO, ak_tr(AK_STR_SET_DEVINFO), ">", AK_UI_TEXT_DIM);
    ak_settings_draw_ble_pair_row(now_ms);
    ak_settings_draw_pairing_row(now_ms);
    ak_settings_row(AK_SETTINGS_ITEM_REBIND, ak_tr(AK_STR_SET_REBIND), ">", AK_UI_TEXT_DIM);
    ak_settings_row(AK_SETTINGS_ITEM_FACTORY_RESET, ak_tr(AK_STR_SET_FRESET), ">", AK_UI_TEXT_DIM);
    ak_settings_row(AK_SETTINGS_ITEM_TEST, ak_tr(AK_STR_SET_TEST), ">", AK_UI_TEXT_DIM);
    ak_settings_row(AK_SETTINGS_ITEM_EXIT, ak_tr(AK_STR_SET_EXIT), "", AK_UI_TEXT_DIM);
    if (s_settings_confirmed && item_value_row(s_settings.selected) && ak_settings_row_y(s_settings.selected) != 0U) {
        /* "Applied" centred in the selected row, between label and value. */
        (void)ak_gfx_text_center(LCD_LIST_X, ak_settings_row_y(s_settings.selected), LCD_LIST_W, LCD_B,
                                 ak_tr(AK_STR_SET_APPLIED), AK_UI_SEL_FG, AK_UI_SEL_BG);
    }
    ak_scroll_bar_draw(lcd_list_y0(), (uint16_t)(lcd_list_rows() * LCD_R - 2U), ak_settings_visible_count(&s_settings),
                       lcd_list_rows(),
                       (uint16_t)ak_settings_first_row(ak_settings_visible_pos(&s_settings, s_settings.selected),
                                                       lcd_list_rows()));

    if (s_settings.editing) {
        ak_footer_draw(ak_tr(AK_STR_SET_HINT_ADJUST), AK_UI_TEXT_ACCENT, ak_tr(AK_STR_SET_HINT_COMMIT), AK_UI_TEXT_DIM);
    } else if (s_settings.selected == AK_SETTINGS_ITEM_PAIRING) {
        bool bound = ak_cloud_activated();
        ak_footer_draw(ak_tr(bound ? AK_STR_SET_HINT_CHWIFI1 : AK_STR_SET_HINT_PAIRING), AK_UI_TEXT_ACCENT,
                       ak_tr(bound ? AK_STR_SET_HINT_CHWIFI2 : AK_STR_SET_HINT_PAIRING2), AK_UI_TEXT_DIM);
    } else if (s_settings.selected == AK_SETTINGS_ITEM_BLE_PAIR) {
        ak_footer_draw(ak_tr(AK_STR_SET_HINT_BLE_PAIR1), AK_UI_TEXT_ACCENT, ak_tr(AK_STR_SET_HINT_BLE_PAIR2),
                       AK_UI_TEXT_DIM);
    } else if (s_settings.selected == AK_SETTINGS_ITEM_FACTORY_RESET) {
        /* What goes; click opens the full list (both confirm dialogs). */
        ak_footer_draw(ak_tr(AK_STR_SET_HINT_FRESET1), AK_UI_TEXT_ACCENT, ak_tr(AK_STR_SET_HINT_FRESET2),
                       AK_UI_TEXT_DIM);
    } else if (s_status_tmp_on && ak_bind_flashing(&s_bind, now_ms)) {
        /* Just bound: the Companion's message, while the slot key flashes. */
        ak_footer_draw(s_status_tmp_text, s_status_tmp_color, ak_tr(AK_STR_SET_HINT_CLICK), AK_UI_TEXT_DIM);
    } else {
        ak_footer_draw(ak_tr(AK_STR_SET_HINT_SELECT), AK_UI_TEXT_ACCENT, ak_tr(AK_STR_SET_HINT_CLICK), AK_UI_TEXT_DIM);
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

/* Settings > Unbind & re-pair. Activated: ask (⑦, K6 flashes yellow), then
 * clear the Tuya binding. Not activated: the same as Settings > Pairing mode.
 * No License: the Tuya app cannot pair this device, so only say so. */
static void ak_rebind_clicked(void)
{
    uint32_t now_ms = tal_system_get_millisecond();
    switch (ak_provision_rebind_action(ak_cloud_license_usable(), ak_cloud_activated())) {
    case AK_PAIR_ACTION_NEED_LICENSE:
        ak_status_bar_set(ak_tr(AK_STR_REBIND_NEED_LICENSE), AK_UI_ST_NOTE);
        ak_settings_draw();
        break;
    case AK_PAIR_ACTION_REBIND:
        if (!ak_provision_rebind_request(&s_prov, now_ms)) {
            ak_status_bar_set(ak_tr(AK_STR_ST_PAIR_REQ_BUSY), AK_UI_ST_NOTE);
        }
        ak_prov_changed();
        break;
    case AK_PAIR_ACTION_PAIRING:
    default:
        if (ak_provision_is_open(&s_prov, now_ms)) {
            ak_provision_close(&s_prov);
        } else {
            ak_provision_request(&s_prov, now_ms);
        }
        ak_prov_changed();
        break;
    }
}

/* Settings > Reset system: open the first of the two ⑦ confirms. Refused
 * while another system dialog is up, during an OTA, or while a reset already
 * runs. */
static void ak_freset_clicked(void)
{
    uint32_t now_ms = tal_system_get_millisecond();
    bool     busy   = s_prov.state == AK_PROV_CONFIRMING || ak_provision_reset_pending(&s_prov, now_ms) ||
                ak_provision_unpair_pending(&s_prov, now_ms) || ak_provision_rebind_pending(&s_prov, now_ms) ||
                s_freset_reboot_at_ms != 0U;
#if AK_TRANSPORT_HAS_UART
    busy = busy || ak_ota_link_busy();
#endif
#if AK_TRANSPORT_HAS_BLE
    busy = busy || s_ble_pair.state == AK_BLE_PAIR_COMPARE;
#endif
    if (busy || !ak_freset_request(&s_freset, now_ms)) {
        ak_status_bar_set(ak_tr(AK_STR_ST_PAIR_REQ_BUSY), AK_UI_ST_NOTE);
        ak_settings_draw();
        return;
    }
    PR_NOTICE("reset system: first confirm shown");
    ak_ui_sync(now_ms);
    ak_settings_draw();
}

/* Settings > BLE pairing: open the 120 s over-the-air pairing window, or
 * close it when it is open. Needs AK BLE running (not while the Tuya pairing
 * window owns BLE, nor after the Companion chose USB this boot). */
static void ak_ble_pair_clicked(void)
{
#if AK_TRANSPORT_HAS_BLE
    uint32_t now_ms = tal_system_get_millisecond();
    if (ak_ble_pair_is_open(&s_ble_pair, now_ms)) {
        ak_ble_pair_close(&s_ble_pair);
        PR_NOTICE("ble pair: window closed on the device");
        ak_status_bar_set(ak_tr(AK_STR_ST_BLE_PAIR_CLOSED), AK_UI_ST_NOTE);
    } else if (!AK_BLE_RUNTIME_ACTIVE()) {
        ak_status_bar_set(ak_tr(AK_STR_ST_BLE_PAIR_OFF), AK_UI_ST_NOTE);
    } else {
        ak_ble_pair_open(&s_ble_pair, now_ms);
        PR_NOTICE("ble pair: window open for %u s", (unsigned)(AK_BLE_PAIR_WINDOW_MS / 1000U));
        ak_status_bar_set(ak_tr(AK_STR_ST_BLE_PAIR_OPEN), AK_UI_ST_INFO);
    }
#else
    ak_status_bar_set(ak_tr(AK_STR_TEST_BLE_NA), AK_UI_ST_NOTE);
#endif
    ak_settings_draw();
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
    /* Bind session slot page: K1-K3 pick the slot, an EC11 click goes back,
     * other keys only count as activity; while the request is out, nothing. */
    if (ak_bind_active(&s_bind)) {
        uint32_t now_ms = tal_system_get_millisecond();
        if (strcmp(id, AK_ENC_SW_ID) == 0) {
            if (event == AK_INPUT_EVENT_DOWN) {
                s_center_long_consumed = false;
            } else if (event == AK_INPUT_EVENT_LONG) {
                s_center_long_consumed = true;
            } else if (event == AK_INPUT_EVENT_UP) {
                if (s_center_long_consumed) {
                    s_center_long_consumed = false;
                } else {
                    ak_bind_apply(ak_bind_key(&s_bind, 6, now_ms), NULL, 0U);
                }
            }
        } else if (event == AK_INPUT_EVENT_DOWN) {
            ak_bind_apply(ak_bind_key(&s_bind, ak_keymap_slot_for_id(id), now_ms), NULL, 0U);
        }
        return true;
    }
    /* Credentials page: K4 or a knob click goes back; K6 (window open, a
     * License stored) allows exactly one overwrite for 60 s (FW-02). */
    if (s_devinfo_page) {
#if AK_TRANSPORT_DUAL || AK_TRANSPORT_BLE_GATT
        /* K1 twice: clear the BLE pairing (device-local only). */
        if (event == AK_INPUT_EVENT_DOWN && strcmp(id, "k1") == 0 && s_ble_key_set) {
            if (s_cred_unpair_armed) {
                s_cred_unpair_armed = false;
                ble_key_clear();
                ak_status_bar_set(ak_tr(AK_STR_ST_BLE_CLEARED), AK_UI_ST_NOTE);
                send_device_settings(); /* last change wins: tell the Companion */
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
            s_devinfo_page = false;
            ak_settings_draw();
        } else if (strcmp(id, AK_ENC_SW_ID) == 0 && event == AK_INPUT_EVENT_DOWN) {
            s_center_long_consumed = false;
        } else if (strcmp(id, AK_ENC_SW_ID) == 0 && event == AK_INPUT_EVENT_LONG) {
            s_center_long_consumed = true;
        }
        return true;
    }
    /* Wi-Fi help (K4 on Saved Wi-Fi or on the pairing row): K1, K4 or a knob
     * click goes back to where it was opened. */
    if (s_wifi_help) {
        if (event == AK_INPUT_EVENT_DOWN &&
            (strcmp(id, "k1") == 0 || strcmp(id, "k4") == 0 || strcmp(id, AK_ENC_SW_ID) == 0)) {
            s_wifi_help = false;
        }
        ak_settings_draw();
        return true;
    }
    /* Saved Wi-Fi page (FR-15): K1 back, K6 delete (second press within 3 s
     * confirms), the knob selects. Everything else only disarms. */
    if (s_wifi_page) {
        uint32_t now_ms = tal_system_get_millisecond();
        if (event == AK_INPUT_EVENT_DOWN && strcmp(id, "k4") == 0) {
            s_wifi_help = true;
            ak_wifi_page_disarm();
        } else if (event == AK_INPUT_EVENT_DOWN && strcmp(id, "k1") == 0) {
            s_wifi_page = false;
            ak_wifi_page_disarm();
        } else if (event == AK_INPUT_EVENT_DOWN && strcmp(id, "k6") == 0) {
            ak_wifi_saved_t list[AK_WIFI_STORE_MAX];
            uint8_t         n = ak_wifi_page_count(list);
            if (n > 0U && s_wifi_sel < n) {
                const char *ssid = list[s_wifi_sel].ssid;
                if (s_wifi_del_armed && (int32_t)(s_wifi_del_deadline_ms - now_ms) > 0 &&
                    strcmp(s_wifi_del_ssid, ssid) == 0) {
                    (void)ak_wifi_saved_forget(ssid);
                    ak_wifi_page_disarm();
                    ak_status_bar_set(ak_tr(AK_STR_WIFI_DELETED), AK_UI_ST_NOTE);
                    send_wifi_list(); /* last change wins: tell the Companion */
                } else {
                    s_wifi_del_armed = true;
                    (void)snprintf(s_wifi_del_ssid, sizeof(s_wifi_del_ssid), "%s", ssid);
                    s_wifi_del_deadline_ms = now_ms + AK_WIFI_DEL_CONFIRM_MS;
                }
            }
        } else if (event == AK_INPUT_EVENT_DOWN) {
            ak_wifi_page_disarm();
        }
        ak_settings_draw();
        return true;
    }
    /* Pairing confirm dialog (⑦): K6 confirms, K1 cancels (as every system
     * confirm). */
    if (s_prov.state == AK_PROV_CONFIRMING && (strcmp(id, "k6") == 0 || strcmp(id, "k1") == 0)) {
        bool confirm = false;
        if (ak_confirm_key(id, event, &confirm)) {
            if (confirm) {
                ak_prov_confirm_dialog();
            } else {
                ak_provision_close(&s_prov);
            }
            ak_prov_changed();
        }
        return true;
    }
    if (event == AK_INPUT_EVENT_DOWN && strcmp(id, "k4") == 0 && s_prov.state != AK_PROV_CONFIRMING &&
        !s_settings.editing && s_settings.selected == AK_SETTINGS_ITEM_PAIRING) {
        s_wifi_help = true;
        ak_settings_draw();
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
            (void)ak_settings_sync_rebind();
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
            case AK_SETTINGS_RESULT_DEVICE_INFO:
                s_devinfo_page = true;
                ak_devinfo_page_open();
                ak_settings_draw();
                break;
            case AK_SETTINGS_RESULT_SCENE:
                ak_scene_picker_open(); /* closes Settings when the picker opens */
                break;
            case AK_SETTINGS_RESULT_BIND_SESSION:
                ak_bind_open(&s_bind, tal_system_get_millisecond());
                ak_settings_draw();
                break;
            case AK_SETTINGS_RESULT_WIFI:
                s_wifi_page = true;
                s_wifi_sel  = 0U;
                ak_wifi_page_disarm();
                ak_settings_draw();
                break;
            case AK_SETTINGS_RESULT_FACTORY_RESET:
                ak_freset_clicked();
                break;
            case AK_SETTINGS_RESULT_REBIND:
                ak_rebind_clicked();
                break;
            case AK_SETTINGS_RESULT_BLE_PAIR:
                ak_ble_pair_clicked();
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
                    ak_status_bar_set(ak_tr(s_settings.voice_chat ? AK_STR_NOTE_CHAT : AK_STR_NOTE_ASR), AK_UI_ST_INFO);
                }
                if (result == AK_SETTINGS_RESULT_CONFIRMED && s_settings.selected == AK_SETTINGS_ITEM_LANGUAGE) {
                    ak_language_apply((ak_lang_t)s_settings.lang, true);
                }
                if (result == AK_SETTINGS_RESULT_CONFIRMED && s_settings.selected == AK_SETTINGS_ITEM_FONT_SIZE) {
                    ak_font_size_apply((ak_text_size_t)s_settings.font_size, true);
                }
                if (result == AK_SETTINGS_RESULT_CONFIRMED && s_settings.selected == AK_SETTINGS_ITEM_ANIMATION) {
                    ak_ghost_setting_apply(s_settings.animation, true);
                }
                if (result == AK_SETTINGS_RESULT_CONFIRMED &&
                    (s_settings.selected == AK_SETTINGS_ITEM_LED_POWER ||
                     s_settings.selected == AK_SETTINGS_ITEM_LED_TIMEOUT)) {
                    ak_led_setting_commit();
                    send_device_settings(); /* expose the new LED power setting to the Companion */
                    ak_status_bar_set(ak_tr(s_settings.led_mode == (uint8_t)AK_LED_MODE_OFF  ? AK_STR_NOTE_LEDS_OFF
                                            : s_settings.led_mode == (uint8_t)AK_LED_MODE_ON ? AK_STR_NOTE_LEDS_ON
                                                                                             : AK_STR_NOTE_LEDS_AUTO),
                                      AK_UI_ST_INFO);
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
    if (s_devinfo_page) {
        ak_scroll_update(&s_devinfo_scroll,
                         s_devinfo_n > lcd_list_rows() ? (uint16_t)(s_devinfo_n - lcd_list_rows()) : 0U);
        if (ak_scroll_step(&s_devinfo_scroll, detents) != AK_SCROLL_NONE) {
            ak_settings_draw();
        }
        return true;
    }
    if (ak_bind_active(&s_bind)) {
        (void)ak_bind_key(&s_bind, -1, tal_system_get_millisecond()); /* activity only */
        return true;
    }
    if (s_wifi_help) {
        return true;
    }
    if (s_wifi_page) {
        ak_wifi_saved_t list[AK_WIFI_STORE_MAX];
        uint8_t         n = ak_wifi_page_count(list);
        if (n > 0U) {
            int32_t next = (int32_t)s_wifi_sel + (detents > 0 ? 1 : -1);
            s_wifi_sel   = (uint8_t)(next < 0 ? n - 1 : next >= (int32_t)n ? 0 : next);
        }
        ak_wifi_page_disarm();
        ak_settings_draw();
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
    (void)ak_settings_sync_rebind();
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
#define AK_TEST_Y_KEYS   ((uint16_t)(LCD_TITLE_H + 4U))
#define AK_TEST_ROW_Y(k) ((uint16_t)(AK_TEST_Y_KEYS + LCD_B + 6U + (k)*LCD_L)) /* line box, height L */
#define AK_TEST_Y_ENC    AK_TEST_ROW_Y(0U)
#define AK_TEST_Y_WS     AK_TEST_ROW_Y(1U)
#define AK_TEST_Y_BAT    AK_TEST_ROW_Y(2U)
#define AK_TEST_Y_UART   AK_TEST_ROW_Y(3U)
#define AK_TEST_Y_WIFI   AK_TEST_ROW_Y(4U)
#define AK_TEST_Y_BLE    AK_TEST_ROW_Y(5U)
#define AK_TEST_CELL_W   28U
#define AK_TEST_CELL_GAP 6U

/* Key cells right-aligned to the margin, clear of the "Keys" label. */
static uint16_t ak_test_keys_x0(void)
{
    uint16_t a = (uint16_t)(LCD_M + ak_text_width(ak_tr(AK_STR_TEST_KEYS)) + 10U);
    uint16_t b = (uint16_t)(AK_STATUSLCD_W - LCD_M - 7U * AK_TEST_CELL_W - 6U * AK_TEST_CELL_GAP);
    return a > b ? a : b;
}

/* One test-page line: cleared and redrawn alone (partial refresh). */
static void ak_test_row(uint16_t y, const char *s, uint16_t color)
{
    ak_gfx_fill_rect(0U, y, AK_STATUSLCD_W, LCD_L, AK_UI_BG);
    ak_lcd_line(y, s, color);
}

/* Draw the one static key cell: held Purple, idle a dark grey outline; label centred. */
static void ak_test_draw_key_cell(int i, bool held)
{
    static const char *const k_lbl[7] = {"1", "2", "3", "4", "5", "6", "E"};
    uint16_t                 x        = (uint16_t)(ak_test_keys_x0() + i * (AK_TEST_CELL_W + AK_TEST_CELL_GAP));
    uint16_t                 bg       = held ? AK_UI_CELL_HELD : AK_UI_BG;
    if (held) {
        ak_gfx_round_rect(x, AK_TEST_Y_KEYS, AK_TEST_CELL_W, LCD_B, LCD_R_TAG, bg);
    } else {
        ak_gfx_round_frame(x, AK_TEST_Y_KEYS, AK_TEST_CELL_W, LCD_B, LCD_R_TAG, 1U, AK_UI_FRAME, bg);
    }
    (void)ak_gfx_text_center(x, AK_TEST_Y_KEYS, AK_TEST_CELL_W, LCD_B, k_lbl[i],
                             held ? AK_UI_CELL_HELD_FG : AK_UI_CELL_FG, bg);
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
    (void)snprintf(line, sizeof(line), "FX %s  %s %ld  (EC11 %+ld)",
                   s_test_fx_names[s_test_fx],
                   s_test_fx == 0 ? "hue" : "spd",
                   (long)s_test_fx_param[s_test_fx], (long)s_test_enc_count);
    ak_test_row(AK_TEST_Y_ENC, line, AK_UI_TEXT_ACCENT);
    ak_statuslcd_flush();
}

/* Draw the full static test-page frame ONCE on entry. */
static void ak_test_draw_frame(void)
{
    char line[48];

    if (!ak_lcd_draw_begin()) {
        return;
    }

    ak_gfx_clear(AK_UI_BG);
    ak_title_bar(ak_tr(AK_STR_TEST_TITLE), AK_UI_NO_WARN, NULL);

    (void)ak_gfx_text_box(LCD_M, AK_TEST_Y_KEYS, LCD_B, ak_tr(AK_STR_TEST_KEYS), AK_UI_TEXT_DIM, AK_UI_BG);
    for (int i = 0; i < 7; i++) {
        ak_test_draw_key_cell(i, false);
    }

    (void)snprintf(line, sizeof(line), "FX %s  %s %ld  (EC11 %+ld)",
                   s_test_fx_names[s_test_fx],
                   s_test_fx == 0 ? "hue" : "spd",
                   (long)s_test_fx_param[s_test_fx], (long)s_test_enc_count);
    ak_test_row(AK_TEST_Y_ENC, line, AK_UI_TEXT_ACCENT);

    (void)snprintf(line, sizeof(line), "WS2812 x%u: %s", (unsigned)AK_LEDS_COUNT,
                   ak_leds_available() ? "K1-K6 effect, EC11 tune" : "unavailable");
    ak_test_row(AK_TEST_Y_WS, line, ak_leds_available() ? AK_UI_TEXT : AK_UI_TEXT_ACCENT);

    /* Battery/WiFi/BLE drawn by their periodic refreshers below; seed labels. */
    ak_test_row(AK_TEST_Y_BAT, ak_tr(AK_STR_TEST_BATTERY_WAIT), AK_UI_TEXT);
    (void)snprintf(line, sizeof(line), "UART0 (%s): rx %lu bytes", AK_TRANSPORT_NAME,
                   (unsigned long)s_test_uart_rx);
    ak_test_row(AK_TEST_Y_UART, line, AK_UI_TEXT);
    ak_test_row(AK_TEST_Y_WIFI, ak_tr(AK_STR_TEST_WIFI_WAIT), AK_UI_TEXT);
    ak_test_row(AK_TEST_Y_BLE, ak_tr(AK_STR_TEST_BLE_WAIT), AK_UI_TEXT);

    ak_footer_draw(ak_tr(AK_STR_TEST_HINT), AK_UI_TEXT_ACCENT, ak_tr(AK_STR_TEST_EXIT), AK_UI_TEXT_DIM);
    ak_statuslcd_flush();
}

/* Repaint the slow rows (battery/UART/WiFi/BLE) on the periodic tick. */
static void ak_test_draw_slow(void)
{
    char line[48];

    if (!ak_lcd_draw_begin()) {
        return;
    }

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
    ak_test_row(AK_TEST_Y_BAT, line, AK_UI_TEXT);

    (void)snprintf(line, sizeof(line), "UART0 (%s): rx %lu bytes", AK_TRANSPORT_NAME,
                   (unsigned long)s_test_uart_rx);
    ak_test_row(AK_TEST_Y_UART, line, AK_UI_TEXT);

    (void)snprintf(line, sizeof(line), "WiFi: %s %s",
                   ak_wifi_state_str(ak_wifi_current_state()), ak_wifi_current_ssid());
    ak_test_row(AK_TEST_Y_WIFI, line, AK_UI_TEXT);

#if AK_TRANSPORT_HAS_BLE
    (void)snprintf(line, sizeof(line), "BLE: %s",
                   ak_ble_gatt_link_state_str(ak_ble_gatt_link_state()));
#else
    (void)snprintf(line, sizeof(line), "%s", ak_tr(AK_STR_TEST_BLE_NA));
#endif
    ak_test_row(AK_TEST_Y_BLE, line, AK_UI_TEXT);

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
#define AK_TEST_LP_Y_PIN   ((uint16_t)(LCD_TITLE_H + 4U))
#define AK_TEST_LP_Y_CHAIN ((uint16_t)(AK_TEST_LP_Y_PIN + LCD_L))

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
    (void)snprintf(line, sizeof(line), "P18 LED_PWR_EN: %s", high ? "HIGH" : "LOW");
    ak_test_row(AK_TEST_LP_Y_PIN, line, high ? AK_UI_TEXT : AK_UI_TEXT_DIM);
    (void)snprintf(line, sizeof(line), "Chain: %s", ak_leds_available() ? ak_leds_power_state_str() : "unavailable");
    ak_test_row(AK_TEST_LP_Y_CHAIN, line, AK_UI_TEXT);
    ak_statuslcd_flush();
}

static void ak_test_draw_ledpwr(void)
{
    char line[48];
    if (!ak_lcd_draw_begin()) {
        return;
    }
    ak_gfx_clear(AK_UI_BG);
    ak_title_bar(ak_tr(AK_STR_SET_LED_POWER), AK_UI_NO_WARN, NULL);
    /* Two live rows, a half line, three notes, a half line, the warning. */
    uint16_t y = (uint16_t)(AK_TEST_LP_Y_CHAIN + LCD_L + LCD_L / 2U);
    ak_lcd_line(y, "On : P18 high, settle 10 ms, colours", AK_UI_TEXT_DIM);
    ak_lcd_line((uint16_t)(y + LCD_L), "Off: dark frame, DIN low, P18 low", AK_UI_TEXT_DIM);
    (void)snprintf(line, sizeof(line), "Setting %s / %s: not used here",
                   ak_led_mode_str((ak_led_mode_t)s_settings.led_mode),
                   ak_led_policy_timeout_str(s_settings.led_timeout_idx));
    ak_lcd_line((uint16_t)(y + 2U * LCD_L), line, AK_UI_TEXT_DIM);
    y = (uint16_t)(y + 3U * LCD_L + LCD_L / 2U);
    ak_lcd_line(y, ak_tr(AK_STR_TEST_NO_SWITCH1), AK_UI_TEXT_ACCENT);
    ak_lcd_line((uint16_t)(y + LCD_L), ak_tr(AK_STR_TEST_NO_SWITCH2), AK_UI_TEXT_ACCENT);
    ak_footer_draw(ak_tr(AK_STR_TEST_LP_KEYS), AK_UI_TEXT_ACCENT, ak_tr(AK_STR_TEST_LP_HINT), AK_UI_TEXT_DIM);
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
         * click) steps HW -> LED power -> HW. */
        if (event == AK_INPUT_EVENT_UP && !s_test_enc_long && !s_test_exit_req) {
            s_test_view = (ak_test_view_t)((s_test_view + 1) % AK_TEST_VIEW_COUNT);
            if (s_test_view == AK_TEST_VIEW_LEDPWR) {
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

static void ak_session_labels_fit(void);

static void ak_session_label_set(uint8_t slot, const char *alias)
{
    if (slot >= LCD_SESSION_ROWS) {
        return;
    }
    if (alias == NULL || alias[0] == '\0') {
        s_session_aliases[slot][0] = '\0';
        ak_session_labels_fit();
        return;
    }

    char buf[LCD_SESSION_ALIAS_BYTES];
    size_t n = 0U;
    for (const char *p = alias; *p != '\0' && n + 1U < sizeof(buf); p++) {
        buf[n++] = (unsigned char)*p < 0x20U ? ' ' : *p;
    }
    buf[n] = '\0';
    ak_text_utf8_trim(buf);
    memcpy(s_session_aliases[slot], buf, strlen(buf) + 1U);
    ak_session_labels_fit();
}

/* ── FR-14: chat ghost animation (ak_ghost.h) ─────────────────────────────
 * Selecting a chat floats + blinks its ghost; a status change plays a short
 * hop. Only the ghost's 32 x 36 px area is redrawn and sent with
 * ak_statuslcd_flush_rect (about 2.3 KB, ~0.5 ms of SPI at 48 MHz per step),
 * never the full 150 KB frame. ak_ghost_service runs from the app loop and
 * never blocks: it skips a step while a DMA is in flight. Animations run only
 * while the main page is the last full frame on screen and the keyboard is in
 * ① (or on the Chat page); ②③④⑦, settings, OTA, a playing reply, low battery
 * or the setting off stop them at once. */
#define LCD_GHOST_X  16U
#define LCD_GHOST_DY             LCD_CARD_GHOST_DY /* ghost top below the card top, at rest */
#define LCD_GHOST_AREA_Y(slot_y) ((uint16_t)((slot_y) + LCD_GHOST_DY - AK_GHOST_AMPLITUDE_PX))
#define LCD_GHOST_AREA_H         ((uint16_t)(AK_SESSION_ICON_H + 2U * AK_GHOST_AMPLITUDE_PX))

static ak_ghost_t s_ghost;
static uint32_t   s_ghost_main_gen = UINT32_MAX; /* frame gen of the last main-page flush */

static uint16_t ak_session_slot_y(uint8_t i)
{
    return lcd_card_y(i);
}

/* Ghost of slot i at its current pose, area cleared first. No flush. */
/* The ghost of slot i on the card colour `bg`: White body, Black eyes (brand
 * rule), whatever the card. */
static void ak_ghost_draw_slot(uint8_t i, uint16_t bg)
{
    uint16_t        y = ak_session_slot_y(i);
    ak_ghost_pose_t p = ak_ghost_pose(&s_ghost, i);
    ak_gfx_fill_rect(LCD_GHOST_X, LCD_GHOST_AREA_Y(y), AK_SESSION_ICON_W, LCD_GHOST_AREA_H, bg);
    ak_gfx_icon_shape(LCD_GHOST_X, (uint16_t)((int)y + (int)LCD_GHOST_DY + p.dy),
                      p.blink ? AK_GHOST_FRAME_BLINK : AK_GHOST_FRAME_OPEN,
                      p.blink ? AK_GHOST_SHAPE_BLINK : AK_GHOST_SHAPE_OPEN, AK_SESSION_ICON_W, AK_SESSION_ICON_H,
                      AK_UI_GHOST_BODY, AK_UI_GHOST_EYES, bg);
}

/* Card colour of slot i: Purple when selected, else black. */
static uint16_t ak_card_bg(uint16_t i)
{
    return i == s_selected_session ? AK_UI_CARD_SEL_BG : AK_UI_CARD_BG;
}

static bool ak_ghost_allowed(uint32_t now_ms)
{
    if (!s_settings.animation || s_settings_active || s_ghost_main_gen != ak_statuslcd_frame_gen() ||
        ak_ui_state(&s_ui) != AK_UI_STANDBY || s_lcd_redraw_pending ||
        (int32_t)(s_lcd_audio_guard_until_ms - now_ms) > 0 || ak_asr_audio_busy()) {
        return false;
    }
    if (ak_battery_available() && ak_battery_percent() <= AK_GHOST_LOW_BATTERY_PCT) {
        ak_battery_info_t bi;
        ak_battery_get_info(&bi);
        if (!bi.charging) {
            return false;
        }
    }
    return true;
}

static void ak_ghost_service(uint32_t now_ms)
{
    if (!ak_ghost_active(&s_ghost) || !ak_statuslcd_ready() || ak_statuslcd_flush_busy()) {
        return; /* resting (the usual case), or try again next loop */
    }
    uint8_t changed = ak_ghost_tick(&s_ghost, now_ms, ak_ghost_allowed(now_ms));
    for (uint8_t i = 0U; i < AK_GHOST_SLOTS; i++) {
        if ((changed & (1U << i)) == 0U) {
            continue;
        }
        uint16_t y = ak_session_slot_y(i);
        ak_ghost_draw_slot(i, ak_card_bg(i)); /* the state dot sits right of the ghost area */
        if (!ak_statuslcd_flush_rect(LCD_GHOST_X, LCD_GHOST_AREA_Y(y), AK_SESSION_ICON_W, LCD_GHOST_AREA_H)) {
            ak_ghost_set_enabled(&s_ghost, false); /* no partial refresh: stay static */
            s_lcd_redraw_pending = true;
            return;
        }
    }
}

/* Sidebar: three rounded cards, black with a dark grey frame, the selected
 * one Purple; the session state as an 8 px dot in the top-right corner
 * (FR-09 colour); ghost, then the slot label centred below it. */
static void ak_draw_session_sidebar(void)
{
    for (uint16_t i = 0; i < LCD_SESSION_ROWS; i++) {
        bool     selected = (i == s_selected_session);
        uint16_t y        = lcd_card_y(i);
        uint16_t h        = lcd_card_hi(i);
        uint16_t bg       = ak_card_bg(i);
        uint16_t fg       = selected ? AK_UI_CARD_SEL_FG : AK_UI_CARD_FG;

        if (selected) {
            ak_gfx_round_rect(LCD_CARD_X, y, LCD_CARD_W, h, LCD_R_PANEL, bg);
        } else {
            ak_gfx_round_frame(LCD_CARD_X, y, LCD_CARD_W, h, LCD_R_PANEL, 1U, AK_UI_FRAME, bg);
        }
        ak_gfx_round_rect(LCD_CARD_DOT_X, (uint16_t)(y + LCD_CARD_DOT_DY), LCD_CARD_DOT, LCD_CARD_DOT, LCD_R_TAG,
                          ak_status_color(s_sessions[i].state));
        ak_ghost_draw_slot((uint8_t)i, bg);
        uint16_t lb = (uint16_t)(y + LCD_CARD_GHOST_DY + AK_SESSION_ICON_H + 1U);
        (void)ak_gfx_text_center((uint16_t)(LCD_CARD_X + 2U), lb, LCD_CARD_LABEL_W, (uint16_t)(y + h - 3U - lb),
                                 s_session_labels[i], fg, bg);
    }
}

/* ── LCD menu screen ─────────────────────────────────────────────────────
 * Header: kind + title, queue and page/pages. Then up to 3 context lines,
 * the page's items (direct menus tag each with its key, K2..K5), one hint
 * line, and the status bar. UTF-8 text from the Companion is drawn as is
 * (ak_gfx_text: 16 px Unifont) and wrapped / clipped by pixel width. */
#define LCD_MENU_COLS     38U
#define LCD_MENU_TEXT_W   (LCD_MENU_COLS * AK_TEXT_ASCII_W) /* 304 px */
/* Context / item / hint rows by font size: common/include/ak_layout.h. */

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

/* Settings > Bind session, slot page: three rounded cards (K1-K3 tag, the
 * slot name and the session bound to it). The status message of the flow
 * (no reply, error) replaces the first hint line while it lasts. */
static void ak_bind_draw(uint32_t now_ms)
{
    ak_title_bar(ak_tr(AK_STR_BIND_TITLE), AK_UI_NO_WARN, NULL);
    bool     waiting = s_bind.state != AK_BIND_PICK;
    uint16_t ch      = lcd_bind_card_h();
    uint16_t w       = (uint16_t)(AK_STATUSLCD_W - 2U * LCD_M);
    uint16_t tw      = (uint16_t)(ak_text_width("K2") + 2U * LCD_PAD);
    uint16_t th      = (uint16_t)(LCD_B - 4U);
    for (uint8_t i = 0U; i < LCD_SESSION_ROWS; i++) {
        uint16_t y      = (uint16_t)(LCD_BIND_TOP + i * (ch + 4U));
        bool     picked = waiting && s_bind.slot == i + 1U;
        uint16_t bg     = picked ? AK_UI_SEL_BG : AK_UI_BG;
        bool     dim    = waiting && !picked; /* another slot is being bound */
        char     k[3]   = {'K', (char)('1' + i), '\0'};
        char     fit[LCD_SESSION_NAME_BYTES + 4U];
        if (picked) {
            ak_gfx_round_rect(LCD_M, y, w, ch, LCD_R_PANEL, bg);
        } else {
            ak_gfx_round_frame(LCD_M, y, w, ch, LCD_R_PANEL, 1U, AK_UI_FRAME, bg);
        }
        uint16_t ty = (uint16_t)(y + (ch - th) / 2U);
        if (dim) {
            ak_gfx_round_frame((uint16_t)(LCD_M + 6U), ty, tw, th, LCD_R_TAG, 1U, AK_UI_FRAME, bg);
        } else {
            ak_gfx_round_rect((uint16_t)(LCD_M + 6U), ty, tw, th, LCD_R_TAG, AK_UI_TAG_BG);
        }
        (void)ak_gfx_text_center((uint16_t)(LCD_M + 6U), ty, tw, th, k, dim ? AK_UI_TEXT_DIM : AK_UI_TAG_FG,
                                 dim ? bg : AK_UI_TAG_BG);
        uint16_t lx = (uint16_t)(LCD_M + 6U + tw + 8U);
        uint16_t ly = (uint16_t)(y + (ch - 2U * LCD_L) / 2U);
        (void)ak_gfx_text_box(lx, ly, LCD_L, s_session_labels[i], picked ? AK_UI_SEL_FG : AK_UI_TEXT, bg);
        bool bound = s_session_names[i][0] != '\0';
        (void)ak_gfx_text_box(lx, (uint16_t)(ly + LCD_L), LCD_L,
                              ak_lcd_fit(bound ? s_session_names[i] : ak_tr(AK_STR_BIND_UNBOUND),
                                         (uint16_t)(LCD_M + w - LCD_PAD - lx), true, fit, sizeof(fit)),
                              picked ? AK_UI_SEL_FG : (bound ? AK_UI_TEXT_ACCENT : AK_UI_TEXT_DIM), bg);
    }
    bool tmp = s_status_tmp_on && (int32_t)(s_status_tmp_until_ms - now_ms) > 0;
    ak_footer_draw(tmp ? s_status_tmp_text : ak_tr(waiting ? AK_STR_BIND_LOADING : AK_STR_BIND_HINT1),
                   tmp ? s_status_tmp_color : AK_UI_TEXT_ACCENT, ak_tr(AK_STR_BIND_HINT2), AK_UI_TEXT_DIM);
}

/* Draw up to `rows` wrapped lines of `text` (wrapped at max_px) from the line
 * box at y, L apart, starting at wrapped line `skip`. With `ellipsis`, the
 * last line ends in "..." when text remains (a scrolled text shows a scroll
 * bar instead). Returns the number of lines drawn. */
static uint8_t ak_lcd_wrap_draw(const char *text, uint16_t y, uint16_t max_px, uint8_t rows, uint16_t color,
                                uint16_t skip, bool ellipsis)
{
    char        line[LCD_MENU_COLS * 3U + 8U];
    const char *p = text;
    uint8_t     n = 0U;
    for (uint16_t i = 0U; i < skip && *p != '\0'; i++) {
        while (*p == ' ') {
            p++;
        }
        size_t len = ak_text_wrap(p, max_px);
        p += len > 0U ? len : (*p != '\0' ? 1U : 0U);
    }
    while (*p != '\0' && n < rows) {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        size_t len  = ak_text_wrap(p, max_px);
        bool   more = ellipsis && p[len] != '\0' && n + 1U == rows;
        if (more) {
            len = ak_text_wrap(p, (uint16_t)(max_px - ak_text_width("...")));
        }
        if (len >= sizeof(line) - 4U) {
            len = sizeof(line) - 4U;
            while (len > 0U && ((unsigned char)p[len] & 0xC0U) == 0x80U) {
                len--;
            }
        }
        memcpy(line, p, len);
        memcpy(line + len, more ? "..." : "", more ? 4U : 1U);
        (void)ak_gfx_text_box(LCD_M, (uint16_t)(y + n * LCD_L), LCD_L, line, color, AK_UI_BG);
        p += len > 0U ? len : 1U;
        n++;
    }
    return n;
}

/* Key tag colour of a menu item: the key-LED colour for confirm / cancel /
 * attention (functional), Purple for the voice item, else White. */
static uint16_t ak_menu_role_lcd(uint8_t role)
{
    switch (role) {
    case AK_MENU_ROLE_CONFIRM:
        return AK_UI_TAG_CONFIRM;
    case AK_MENU_ROLE_CANCEL:
    case AK_MENU_ROLE_DANGER:
        return AK_UI_TAG_CANCEL;
    case AK_MENU_ROLE_ATTENTION:
        return AK_UI_TAG_ATTENTION;
    case AK_MENU_ROLE_VOICE:
        return AK_UI_TAG_VOICE;
    default:
        return AK_UI_TAG_BG; /* selectable option */
    }
}

static void ak_menu_draw(void)
{
    static const ak_str_id_t kind_tag[] = {AK_STR_MENU_OPTIONS, AK_STR_MENU_APPROVE, AK_STR_MENU_QUESTION,
                                           AK_STR_MENU_MODEL,   AK_STR_MENU_MODE,    AK_STR_MENU_INSTANCE,
                                           AK_STR_MENU_SESSION};
    const ak_menu_t         *m          = &s_menu;
    const ak_menu_pg_t      *pg         = &m->pg[m->cur_page];
    char                     line[LCD_MENU_COLS * 2U + 8U];
    char                     text[AK_MENU_CTX_MAX + 1U];
    char                     fit[LCD_MENU_COLS * 2U + 8U];
    char                     right[24];

    ak_gfx_clear(AK_UI_BG);
    const ak_menu_pg_t *head = pg->count > 0U ? pg : &m->pg[0]; /* page not here yet */
    ak_lcd_text(head->title, text, sizeof(text));
    (void)snprintf(line, sizeof(line), "%s %s", ak_tr(kind_tag[(unsigned)head->kind < sizeof(kind_tag) / sizeof(kind_tag[0]) ? head->kind : 0]), text);
    ak_text_utf8_trim(line);
    right[0] = '\0';
    if (m->pages > 1U || m->queue[0] != '\0') {
        ak_lcd_text(m->queue, text, sizeof(text));
        if (m->pages > 1U) {
            (void)snprintf(right, sizeof(right), "%s%s%u/%u", text, text[0] != '\0' ? " " : "",
                           (unsigned)(m->cur_page + 1U), (unsigned)m->pages);
        } else {
            (void)snprintf(right, sizeof(right), "%s", text);
        }
        ak_text_utf8_trim(right);
    }
    ak_title_bar(line, AK_UI_NO_WARN, right);

    if (m->state == AK_MENU_WAIT) {
        ak_gfx_text(144U, 110U, "...", AK_UI_TEXT, AK_UI_BG, 2);
        return;
    }

    /* Context: up to 3 lines, wrapped by width. Long content (diffs, code) is
     * never sent; `more` says it is on the PC screen. */
    ak_lcd_text(m->ctx, text, sizeof(text));
    uint8_t rows = ak_lcd_wrap_draw(text, LCD_MENU_CTX_Y, LCD_MENU_TEXT_W, 3U, AK_UI_TEXT, 0U, true);
    if (m->more && rows < 3U) {
        ak_lcd_line((uint16_t)(LCD_MENU_CTX_Y + rows * LCD_L), ak_tr(AK_STR_MENU_SEE_SCREEN), AK_UI_TEXT_ACCENT);
    }

    if (pg->count == 0U) {
        ak_gfx_text(144U, (uint16_t)(LCD_MENU_ITEM_Y + 40U), "...", AK_UI_TEXT, AK_UI_BG, 2);
    }
    uint16_t x  = LCD_M;
    uint16_t w  = (uint16_t)(AK_STATUSLCD_W - 2U * LCD_M);
    uint16_t tw = (uint16_t)(ak_text_width("K2") + 2U * LCD_PAD);
    for (uint8_t i = 0U; i < pg->count; i++) {
        const ak_menu_item_t *it   = &pg->items[i];
        uint16_t              y    = (uint16_t)(LCD_MENU_ITEM_Y + i * LCD_R);
        bool                  hi   = i == m->cur_idx;
        uint16_t              bg   = hi ? AK_UI_SEL_BG : AK_UI_BG;
        uint16_t              tag  = ak_menu_role_lcd(it->role);
        char                  k[3] = {'K', (char)('2' + i), '\0'}; /* item i sits on K(i+2) */
        if (hi) {
            ak_gfx_round_rect(x, y, w, LCD_B, LCD_R_ROW, bg);
        } else {
            ak_gfx_round_frame(x, y, w, LCD_B, LCD_R_ROW, 1U, AK_UI_FRAME, bg);
        }
        if (hi && tag == AK_UI_TAG_VOICE) { /* a Purple tag on the Purple row: outline it */
            ak_gfx_round_frame((uint16_t)(x + 3U), (uint16_t)(y + 2U), tw, (uint16_t)(LCD_B - 4U), LCD_R_TAG, 1U,
                               AK_UI_SEL_FG, tag);
        } else {
            ak_gfx_round_rect((uint16_t)(x + 3U), (uint16_t)(y + 2U), tw, (uint16_t)(LCD_B - 4U), LCD_R_TAG, tag);
        }
        (void)ak_gfx_text_center((uint16_t)(x + 3U), (uint16_t)(y + 2U), tw, (uint16_t)(LCD_B - 4U), k,
                                 tag == AK_UI_TAG_VOICE ? AK_UI_TAG_VOICE_FG
                                 : tag == AK_UI_TAG_BG  ? AK_UI_TAG_FG
                                                        : AK_UI_TAG_FN_FG,
                                 tag);
        const char *mark = m->multi ? (ak_menu_item_ticked(m, m->cur_page, i) ? "[x] " : "[ ] ")
                           : ak_menu_item_ticked(m, m->cur_page, i) ? "* "
                                                                    : "";
        ak_lcd_text(it->label, text, sizeof(text));
        (void)snprintf(line, sizeof(line), "%s%s", mark, text);
        ak_text_utf8_trim(line);
        uint16_t lx = (uint16_t)(x + 3U + tw + LCD_PAD);
        (void)ak_gfx_text_box(lx, y, LCD_B, ak_lcd_fit(line, (uint16_t)(x + w - LCD_PAD - lx), false, fit, sizeof(fit)),
                              hi ? AK_UI_SEL_FG : AK_UI_TEXT, bg); /* the tag carries the role colour */
    }

    const char *hint;
    uint16_t    hint_color = AK_UI_TEXT_DIM;
    if (ak_menu_armed(m)) {
        hint       = ak_tr(AK_STR_MENU_ARMED);
        hint_color = AK_UI_TEXT_ACCENT;
    } else if (!ak_menu_enter_allowed(m)) {
        hint = ak_tr(AK_STR_MENU_HINT_ANSWER);
    } else if (m->multi) {
        hint = ak_tr(AK_STR_MENU_HINT_MULTI);
    } else {
        hint = ak_tr(AK_STR_MENU_HINT_SINGLE);
    }
    ak_lcd_line(LCD_HINT_ABOVE_STATUS_Y, hint, hint_color);
}

/* FR-17: the ③ text scrolls with the knob when it has more lines than
 * LCD_TEXT_ROWS. A new text starts at the top; K4 deleting characters
 * only shortens it, so the view is clamped. */
static ak_scroll_t s_confirm_scroll;

static uint16_t ak_confirm_text_lines(const char *utf8)
{
    char text[ASR_TRANSCRIPT_MAX];
    ak_lcd_text(utf8, text, sizeof(text));
    return ak_text_wrap_count(text, LCD_CONFIRM_TEXT_W);
}

static uint16_t ak_confirm_max_first(const char *utf8)
{
    uint16_t lines = utf8 != NULL ? ak_confirm_text_lines(utf8) : 0U;
    return lines > LCD_TEXT_ROWS ? (uint16_t)(lines - LCD_TEXT_ROWS) : 0U;
}

/* The ③ text on screen (a dictated menu answer), NULL when none. */
static const char *ak_confirm_text(void)
{
    if (ak_ui_state(&s_ui) != AK_UI_CONFIRM) {
        return NULL;
    }
    return s_ui.dictating ? s_dictated : NULL;
}

static void ak_confirm_scroll_reset(const char *utf8)
{
    ak_scroll_reset(&s_confirm_scroll, ak_confirm_max_first(utf8), false);
}

/* EC11 on ③: true when the rotation scrolled (or hit an end of) a text longer
 * than one screen; a text that fits keeps the rotation's old meaning. */
static bool ak_confirm_scroll(int32_t detents)
{
    if (ak_ui_state(&s_ui) == AK_UI_STANDBY && s_input_text[0] != '\0' && s_voice_mode != AK_VOICE_MODE_CHAT) {
        /* The input strip scrolls when its text is longer than the strip shows. */
        ak_scroll_update(&s_input_scroll, ak_inputbar_max_first(s_input_text));
        ak_scroll_result_t r = ak_scroll_step(&s_input_scroll, detents);
        if (r == AK_SCROLL_NONE) {
            return false;
        }
        if (ak_scroll_feedback(r)) {
            ak_render_status();
        }
        return true;
    }
    const char *utf8 = ak_confirm_text();
    if (utf8 == NULL) {
        return false;
    }
    ak_scroll_update(&s_confirm_scroll, ak_confirm_max_first(utf8));
    ak_scroll_result_t r = ak_scroll_step(&s_confirm_scroll, detents);
    if (r == AK_SCROLL_NONE) {
        return false;
    }
    if (ak_scroll_feedback(r)) {
        ak_render_status();
    }
    return true;
}

/* Header + wrapped text of a ③ screen (voice input or dictated answer). */
static void ak_confirm_text_draw(const char *title, const char *utf8)
{
    char text[ASR_TRANSCRIPT_MAX];

    ak_gfx_clear(AK_UI_BG);
    ak_title_bar(title, AK_UI_NO_WARN, NULL);
    if (utf8 == NULL) {
        return;
    }
    ak_lcd_text(utf8, text, sizeof(text));
    uint16_t lines = ak_text_wrap_count(text, LCD_CONFIRM_TEXT_W);
    ak_scroll_update(&s_confirm_scroll, lines > LCD_TEXT_ROWS ? (uint16_t)(lines - LCD_TEXT_ROWS) : 0U);
    uint8_t shown = ak_lcd_wrap_draw(text, LCD_TEXT_TOP, LCD_CONFIRM_TEXT_W, (uint8_t)LCD_TEXT_ROWS, AK_UI_TEXT,
                                     s_confirm_scroll.first, false);
    ak_scroll_bar_draw(LCD_TEXT_TOP, (uint16_t)(LCD_TEXT_ROWS * LCD_L), lines, shown, s_confirm_scroll.first);
}

/* Dictated answer (②/③ for a menu's voice item): the recognised text, or
 * how to speak, in double size centred on the page. */
static void ak_answer_voice_draw(void)
{
    bool confirm = ak_ui_state(&s_ui) == AK_UI_CONFIRM;
    ak_confirm_text_draw(ak_tr(AK_STR_DICT_TITLE), confirm ? s_dictated : NULL);
    if (!confirm) {
        /* Two lines at scale 2 (2F tall, F/2 apart), the block centred between
         * the title and the hint; each line's ink band centred in its box. */
        const char *big[2] = {ak_tr(AK_STR_DICT_HOLD), ak_tr(AK_STR_DICT_SPEAK)};
        uint8_t     it = 0U, ib = 0U;
        ak_text_ink(&it, &ib);
        uint16_t bh    = (uint16_t)(2U * LCD_F);
        uint16_t gap   = (uint16_t)(LCD_F / 2U);
        uint16_t block = (uint16_t)(2U * bh + gap);
        uint16_t y0    = (uint16_t)(LCD_TITLE_H + (LCD_HINT_ABOVE_STATUS_Y - LCD_TITLE_H - block) / 2U);
        for (uint16_t k = 0U; k < 2U; k++) {
            char        fit[64];
            const char *s   = ak_lcd_fit(big[k], (AK_STATUSLCD_W - 2U * LCD_M) / 2U, false, fit, sizeof(fit));
            uint16_t    x   = (uint16_t)((AK_STATUSLCD_W - 2U * ak_text_width(s)) / 2U);
            uint16_t    by  = (uint16_t)(y0 + k * (bh + gap) - 2U);
            uint16_t    top = (uint16_t)(by + (bh + 4U - 2U * (ib - it + 1U)) / 2U - 2U * it);
            ak_gfx_text(x, (uint16_t)(top + 2U * ak_text_dy()), s, AK_UI_TEXT, AK_UI_BG, 2);
        }
    }
    ak_lcd_line(LCD_HINT_ABOVE_STATUS_Y, confirm ? ak_tr(AK_STR_DICT_CONFIRM_HINT) : ak_tr(AK_STR_DICT_BACK_HINT),
                AK_UI_TEXT_DIM);
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
    if (ak_provision_unpair_pending(&s_prov, now_ms)) {
        ak_draw_unpair_confirm();
        ak_status_bar_draw();
        ak_statuslcd_flush();
        s_lcd_redraw_pending = false;
        return;
    }
    if (ak_provision_rebind_pending(&s_prov, now_ms)) {
        ak_draw_rebind_confirm();
        ak_status_bar_draw();
        ak_statuslcd_flush();
        s_lcd_redraw_pending = false;
        return;
    }
    if (ak_freset_up(now_ms)) {
        ak_draw_freset_confirm(ak_freset_stage(&s_freset, now_ms));
        ak_status_bar_draw();
        ak_statuslcd_flush();
        s_lcd_redraw_pending = false;
        return;
    }
#if AK_TRANSPORT_HAS_BLE
    if (s_ble_pair.state == AK_BLE_PAIR_COMPARE) {
        ak_draw_ble_pair_confirm();
        ak_status_bar_draw();
        ak_statuslcd_flush();
        s_lcd_redraw_pending = false;
        return;
    }
#endif
    if (s_menu.state == AK_MENU_OPEN && s_ui.dictating) {
        ak_answer_voice_draw();
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
    ble_connected = s_ak_ble_active && ak_ble_companion_authed();
#else
    ble_connected = ak_ble_companion_authed();
#endif
#endif

    bool companion_connected = usb_connected || ble_connected;
    uint16_t header_color        = companion_connected ? AK_UI_HEADER_BG : AK_UI_HEADER_OFF_BG;
    uint16_t header_fg           = companion_connected ? AK_UI_HEADER_FG : AK_UI_HEADER_OFF_FG;

    /* Header contract: fixed title + firmware version + WiFi/USB/BLE icons.
     * WiFi only updates its icon. Purple after USB or BLE connects; black with
     * grey text and a dark grey bottom rule while neither PC transport is. */
    ak_statuslcd_header("Ki-board", header_color, header_fg, s_wifi_connected, usb_connected, ble_connected,
                        ak_battery_percent(), ak_battery_available());
    /* The version 6 px after the title, well left of the WiFi icon (x=214). */
    (void)ak_gfx_text_box((uint16_t)(LCD_M + ak_text_width("Ki-board") + 6U), 0U, LCD_TITLE_H, "v" APP_FW_VERSION,
                          header_fg, header_color);
    if (!companion_connected) {
        ak_gfx_fill_rect(0U, (uint16_t)(LCD_TITLE_H - 1U), AK_STATUSLCD_W, 1U, AK_UI_HEADER_OFF_RULE);
    }
    ak_draw_session_sidebar();

    if (s_voice_mode == AK_VOICE_MODE_CHAT) {
        ak_chat_page_draw();
        ak_status_bar_draw();
        ak_statuslcd_flush();
        s_ghost_main_gen     = ak_statuslcd_frame_gen(); /* sidebar on screen: ghosts may animate */
        s_lcd_redraw_pending = false;
        return;
    }
    char     fit[64];
    uint16_t row_w = (uint16_t)(AK_STATUSLCD_W - LCD_CHAT_X - LCD_M);
    (void)snprintf(line, sizeof(line), ak_tr(AK_STR_ST_SESSION),
                   s_session_labels[s_selected_session],
                   session->agent[0] ? session->agent : "-");
    (void)ak_gfx_text_box(LCD_CHAT_X, LCD_CHAT_ROW1_Y, LCD_L, ak_lcd_fit(line, row_w, false, fit, sizeof(fit)),
                          AK_UI_TEXT, AK_UI_BG);
    (void)snprintf(line, sizeof(line), ak_tr(AK_STR_ST_STATUS),
                   session->state[0] ? session->state : "-");
    (void)ak_gfx_text_box(LCD_CHAT_X, LCD_CHAT_ROW2_Y, LCD_L, ak_lcd_fit(line, row_w, false, fit, sizeof(fit)),
                          AK_UI_TEXT_DIM, AK_UI_BG); /* the card dot carries the state colour */
    if (s_scene_active >= 0 && s_scenes[s_scene_active].home == AK_SCENE_HOME_KEYS) {
        ak_keys_page_draw(); /* FR-03 D2: what each key does in this scene */
    } else {
        ak_chat_history_draw(session);
    }
    if (s_input_text[0] != '\0') {
        ak_inputbar_draw(s_input_text, &s_input_scroll); /* voice input waits above the status bar */
    }
    ak_status_bar_draw();
    ak_statuslcd_flush();
    s_ghost_main_gen     = ak_statuslcd_frame_gen(); /* sidebar on screen: ghosts may animate */
    s_lcd_redraw_pending = false;
}

/* Boot only: render, then wait until the panel has the whole frame.
 * ak_statuslcd_flush() returns while SPI DMA still sends the 150 KB frame
 * (~26 ms at 48 MHz; the SDK's spi_task gives it 100 ms). Starting the next
 * boot step (LEDs, cloud, BLE, ASR, Wi-Fi) during that transfer let the
 * panel's write address slip part-way through the frame: on 0.3.90 the first
 * home frame showed card A intact and everything from card B down shifted
 * sideways with wrap (status text split across both edges). No boot step may
 * start while a frame is in flight; check_boot_order.py enforces it. */
static void ak_render_status_sync(void)
{
    ak_render_status();
    (void)ak_statuslcd_flush_wait();
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
    if (s_pc_fill) {
        /* K6 / K4 must not hit the PC input box of the chat just left. */
        s_pc_fill = false;
        ak_input_changed();
    }
    ak_ghost_select(&s_ghost, selected, tal_system_get_millisecond());
    char feedback[48];
    (void)snprintf(feedback, sizeof(feedback), ak_tr(AK_STR_ST_SELECTED_SESSION),
                   s_session_labels[selected]);
    ak_status_bar_set(feedback, AK_UI_ST_INFO);
    ak_render_status();
}

/* Long press K1-K3 reached its threshold: show the switch at once (blue key
 * light, status bar). The firmware does not know the instance's name, so the bar
 * shows its number (Ctrl+N). */
static void ak_instance_switch_feedback(const char *id, uint8_t n)
{
    s_inst_flash_slot     = ak_keymap_slot_for_id(id);
    s_inst_flash_until_ms = tal_system_get_millisecond() + AK_INSTANCE_FLASH_MS;
    ak_leds_activity();
    char text[48];
    (void)snprintf(text, sizeof(text), ak_tr(AK_STR_ST_SWITCH_INSTANCE), (unsigned)n);
    ak_status_bar_set(text, AK_UI_ST_INFO);
    ak_render_status();
}

static void ak_voice_ui_set_state(ak_voice_ui_state_t state)
{
    s_voice_ui_state = state;
    s_voice_ui_state_since_ms = tal_system_get_millisecond();
    switch (state) {
    case AK_VOICE_UI_LISTENING:
        ak_voice_status(ak_tr(AK_STR_ST_LISTENING), AK_UI_ST_ACTIVE);
        break;
    case AK_VOICE_UI_RECOGNIZING:
        ak_voice_status(ak_tr(AK_STR_ST_RECOGNIZING), AK_UI_ST_ACTIVE);
        break;
    case AK_VOICE_UI_IDLE:
    default:
        ak_voice_status(ak_tr(AK_STR_ST_READY), AK_UI_ST_OK);
        break;
    }
    ak_render_status();
}

/* ---- Post-ASR input confirm (③, firmware-led) ---------------------------
 * Entered after a successful recognition; LED4 (K4=cancel) breathes red and
 * LED6 (K6=confirm) blinks green to cue the gestures (ak_session_lights,
 * painted by ak_menu_loop_tick). ak_ui_tick times the state out. */

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
static void ak_answer_voice_lights(ak_rgb_t frame[AK_LIGHT_COUNT])
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
    if (s_voice_status_on && (int32_t)(now - s_voice_status_until_ms) >= 0) {
        s_voice_status_on    = false;
        s_lcd_redraw_pending = true; /* back to the standing hint */
    }
    if (s_pc_fill && (int32_t)(now - s_pc_fill_until_ms) >= 0) {
        s_pc_fill = false; /* the PC input box is no longer ours: K6 / K4 work as usual */
        ak_input_changed();
        s_lcd_redraw_pending = true;
    }
    bool bind_flash = ak_bind_flashing(&s_bind, now);
    ak_bind_apply(ak_bind_tick(&s_bind, now), NULL, 0U);
    if (bind_flash && !ak_bind_flashing(&s_bind, now) && s_settings_active) {
        s_lcd_redraw_pending = true; /* the "bound" line goes back to the hints */
    }

    if (!ak_leds_available()) {
        return;
    }
    ak_rgb_t frame[AK_LIGHT_COUNT];
    if (ak_provision_rebind_pending(&s_prov, now)) {
        /* Unbind confirm: K6 flashes yellow (~4 Hz double confirm), K1 red. */
        memset(frame, 0, sizeof(frame));
        frame[0] = ak_light_rgb(AK_LIGHT_RED, AK_LIGHT_FULL_PCT);
        frame[5] = ((now / AK_LIGHT_FLASH_MS) & 1U) != 0U ? ak_light_rgb(AK_LIGHT_YELLOW, AK_LIGHT_FULL_PCT)
                                                          : (ak_rgb_t){0, 0, 0};
    } else if (s_combo.active && !s_combo.fired) {
        /* K1 + K6 held: both glow yellow until the confirm appears (5 s). */
        memset(frame, 0, sizeof(frame));
        frame[0] = ak_light_rgb(AK_LIGHT_YELLOW, AK_LIGHT_FULL_PCT);
        frame[5] = ak_light_rgb(AK_LIGHT_YELLOW, AK_LIGHT_FULL_PCT);
    } else if (ak_menu_owns_keys(&s_menu)) {
        ak_menu_lights(&s_menu, now, frame);
    } else if (s_settings_active && s_wifi_page) {
        /* Saved Wi-Fi: K1 back (red), the knob selects (blue), K6 deletes
         * (red); K6 flashes yellow ~4 Hz while the second press is awaited. */
        memset(frame, 0, sizeof(frame));
        ak_wifi_saved_t unused[AK_WIFI_STORE_MAX];
        bool            any = ak_wifi_page_count(unused) > 0U;
        frame[0]            = ak_light_rgb(AK_LIGHT_RED, AK_LIGHT_FULL_PCT);
        frame[6]            = ak_light_rgb(AK_LIGHT_BLUE, AK_LIGHT_FULL_PCT);
        if (any && s_wifi_del_armed && (int32_t)(s_wifi_del_deadline_ms - now) > 0) {
            frame[5] = ((now / AK_LIGHT_FLASH_MS) & 1U) != 0U ? ak_light_rgb(AK_LIGHT_YELLOW, AK_LIGHT_FULL_PCT)
                                                              : (ak_rgb_t){0, 0, 0};
        } else if (any) {
            frame[5] = ak_light_rgb(AK_LIGHT_RED, AK_LIGHT_FULL_PCT);
        }
    } else if (s_settings_active && ak_bind_active(&s_bind)) {
        /* Bind session slot page: K1-K3 blue; once picked only that slot. */
        memset(frame, 0, sizeof(frame));
        for (uint8_t i = 0U; i < AK_BIND_SLOTS; i++) {
            if (s_bind.state == AK_BIND_PICK || s_bind.slot == i + 1U) {
                frame[i] = ak_light_rgb(AK_LIGHT_BLUE, AK_LIGHT_FULL_PCT);
            }
        }
    } else if (s_ui.dictating) {
        ak_answer_voice_lights(frame);
    } else {
        uint8_t waiting = 0U;
        for (uint8_t i = 0U; i < LCD_SESSION_ROWS; i++) {
            if (strcmp(s_sessions[i].state, "waiting") == 0) {
                waiting |= (uint8_t)(1U << i);
            }
        }
        ak_session_lights(waiting, ak_ui_k4_waiting(&s_ui, s_menu_parked), ak_input_confirm(), now - s_input_since_ms,
                          now, frame);
        /* FR-03 idle colours: only in ① standby, only under keys that show
         * nothing more important. */
        if (s_scene_active >= 0 && ak_ui_state(&s_ui) == AK_UI_STANDBY && !ak_input_confirm()) {
            const uint8_t *idle = s_scenes[s_scene_active].idle;
            for (uint32_t i = 0U; i < AK_LIGHT_COUNT && i < AK_SCENE_IDLE_LEN; i++) {
                if (idle[i] != AK_LIGHT_OFF && frame[i].r == 0U && frame[i].g == 0U && frame[i].b == 0U) {
                    frame[i] = ak_light_rgb((ak_light_t)idle[i], AK_SCENE_IDLE_PCT);
                }
            }
        }
    }
    if (ak_bind_flashing(&s_bind, now) && !ak_menu_owns_keys(&s_menu)) {
        /* Just bound / unbound: that slot key blinks green. */
        uint8_t slot    = s_bind.flash_slot;
        uint8_t on      = ak_bind_flash_on(&s_bind, now);
        frame[slot - 1U] = on != 0U ? ak_light_rgb(AK_LIGHT_GREEN, AK_LIGHT_FULL_PCT) : (ak_rgb_t){0, 0, 0};
    }
    if (s_inst_flash_slot >= 0) {
        if ((int32_t)(s_inst_flash_until_ms - now) > 0) {
            frame[s_inst_flash_slot] = ak_light_rgb(AK_LIGHT_BLUE, AK_LIGHT_FULL_PCT);
        } else {
            s_inst_flash_slot = -1;
        }
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
            ak_confirm_scroll_reset(s_dictated);
            ak_ui_voice_end(&s_ui, AK_UI_VOICE_TEXT, false, now_ms);
            ak_voice_status(ak_tr(AK_STR_ST_DICTATED_HINT), AK_UI_ST_OK);
        } else if (!is_error) {
            /* Keyboard mode: the text goes into the strip on the home page (no page
             * of its own). A PC-mode slot is filled by the Companion, so the strip
             * stays as it is; without a Companion link the strip takes it anyway. */
            bool strip = s_voice_mode != AK_VOICE_MODE_CHAT &&
                         (s_input_text[0] != '\0' || !s_slot_pc[s_selected_session] || !ak_companion_linked());
            if (strip) {
                bool clipped = false;
                ak_input_begin(now_ms);
                (void)ak_input_append(s_input_text, s_asr_pending_text, &clipped);
                ak_input_changed();
                ak_voice_status(ak_tr(clipped ? AK_STR_ST_INPUT_FULL : AK_STR_ST_CONFIRM_HINT),
                                  clipped ? AK_UI_ST_WARN : AK_UI_ST_OK);
            }
            ak_ui_voice_end(&s_ui, AK_UI_VOICE_TEXT, false, now_ms);
        } else {
            ak_ui_voice_end(&s_ui, AK_UI_VOICE_ERROR, s_voice_mode == AK_VOICE_MODE_CHAT, now_ms);
            if (strcmp(s_asr_pending_text, "asr_empty") == 0) {
                ak_voice_status(ak_tr(AK_STR_ST_NO_SPEECH), AK_UI_ST_NOTE);
            } else {
                ak_voice_status(ak_tr(AK_STR_ST_ASR_FAILED), AK_UI_ST_ERROR);
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
        if (s_ui.pc_wait) {
            send_voice("cancel"); /* a late PC result must not open ③ */
        }
        ak_ui_voice_end(&s_ui, AK_UI_VOICE_ERROR, s_voice_mode == AK_VOICE_MODE_CHAT, now_ms);
        ak_ui_sync(now_ms);
        ak_voice_status(ak_tr(AK_STR_ST_ASR_TIMEOUT), AK_UI_ST_NOTE);
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
            ak_status_bar_set(ak_tr(AK_STR_ST_ASR_FALLBACK), AK_UI_ST_NOTE);
#endif
        } else if (s_voice_route.effective == AK_VOICE_CLOUD) {
            PR_NOTICE("Tuya ASR %s; using board microphone", was_fallback ? "restored" : "ready");
#if AK_ENABLE_DISPLAY
            ak_status_bar_set(was_fallback ? ak_tr(AK_STR_ST_ASR_RESTORED) : ak_tr(AK_STR_ST_ASR_READY), AK_UI_ST_OK);
#endif
        } else {
            PR_NOTICE("PC ASR selected explicitly");
#if AK_ENABLE_DISPLAY
            ak_status_bar_set(ak_tr(AK_STR_ST_PC_ASR), AK_UI_ST_INFO);
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

    /* Activated ("Change Wi-Fi"): open now. A new network written over USB
     * only reconnects netmgr; tuya_iot keeps devId and keys and just
     * reconnects MQTT, so no restart and no new activation are needed. */
    if (!ak_provision_pairing_needs_restart(ak_license_present(), ak_cloud_activated())) {
        (void)ak_provision_confirm(&s_prov, now_ms);
        return;
    }
    if (ak_reset_netcfg_mark_reopen() != OPRT_OK) {
        ak_provision_close(&s_prov);
        ak_status_bar_set(ak_tr(AK_STR_ST_PAIRING_STORAGE), AK_UI_ST_ERROR);
        return;
    }
    PR_NOTICE("pairing: restarting to open the window with Tuya BLE/SoftAP netcfg");
    ak_status_bar_set(ak_tr(AK_STR_ST_PAIRING_RESTART), AK_UI_ST_NOTE);
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
    if (persist) {
        s_chat_degraded = false; /* an explicit choice (Settings / Companion) wins */
    }
    if (before == AK_VOICE_MODE_CHAT && mode != AK_VOICE_MODE_CHAT) {
        ak_asr_interrupt(); /* stop a reply still playing */
    }
    s_voice_mode = mode;
#if AK_ENABLE_DISPLAY
    s_settings.voice_chat = (mode == AK_VOICE_MODE_CHAT);
    if (mode == AK_VOICE_MODE_CHAT) {
        ak_input_clear(); /* no input strip in Chat */
        ak_pending_confirm_exit();
    }
    memset(&s_chat_view, 0, sizeof(s_chat_view));
    s_chat_line_count = 0U;
    ak_scroll_reset(&s_chat_scroll, 0U, true);
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
#if AK_ENABLE_DISPLAY
    if (persist) {
        send_device_settings(); /* last change wins: tell the Companion at once */
    }
#endif
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
        ak_status_bar_set(ak_tr(AK_STR_NOTE_LANGUAGE), AK_UI_ST_INFO);
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

#if AK_ENABLE_DISPLAY
/* Re-fit the sidebar labels to the current font size (aliases are kept as received). */
static void ak_session_labels_fit(void)
{
    for (uint8_t slot = 0U; slot < LCD_SESSION_ROWS; slot++) {
        char *label = s_session_labels[slot];
        if (s_session_aliases[slot][0] == '\0') {
            label[0] = (char)('A' + slot);
            label[1] = '\0';
            continue;
        }
        memcpy(label, s_session_aliases[slot], sizeof(s_session_aliases[slot]));
        label[ak_text_wrap(label, LCD_CARD_LABEL_W)] = '\0'; /* 9 / 8 / 7 ASCII, or 4 / 4 / 3 CJK */
    }
}
#endif

/* LCD font size (Settings > Font size, Companion device_settings font_size).
 * Called from the Settings commit, a Companion frame and the boot-time KV
 * restore (persist=false). Every layout reads the size when it draws, so
 * the change shows at once; text wrapped earlier (sidebar labels, the Chat
 * page) is wrapped again. The last change wins on both sides. */
static void ak_font_size_apply(ak_text_size_t size, bool persist)
{
    ak_text_size_t before = ak_text_size_get();
    ak_text_size_set(size);
    s_settings.font_size = (uint8_t)ak_text_size_get();
    if (persist && before != ak_text_size_get()) {
        uint8_t b = ak_text_size_px(ak_text_size_get());
        if (ak_kv_set(AK_FONT_SIZE_KV_KEY, &b, 1U) != OPRT_OK) {
            PR_WARN("font size: KV write failed (applied until reboot)");
        }
    }
    PR_NOTICE("font size -> %u px", (unsigned)ak_text_size_px(ak_text_size_get()));
#if AK_ENABLE_DISPLAY
    ak_session_labels_fit();
    if (before != ak_text_size_get()) {
        ak_chat_rebuild_lines();
        ak_chat_scroll_sync(true); /* line heights changed: back to the newest line */
    }
    if (persist) {
        send_device_settings(); /* also the echo for a Companion request */
    }
    if (persist && before != ak_text_size_get()) {
        char note[32];
        (void)snprintf(note, sizeof(note), ak_tr(AK_STR_NOTE_FONT_SIZE),
                       (unsigned)ak_text_size_px(ak_text_size_get()));
        ak_status_bar_set(note, AK_UI_ST_INFO);
        if (s_settings_active) {
            ak_settings_draw();
        } else {
            ak_render_status();
        }
    }
#endif
}

#if AK_ENABLE_DISPLAY
/* FR-14 Settings > Animation / Companion device_settings "animation". */
static void ak_ghost_setting_apply(bool on, bool persist)
{
    bool before         = s_settings.animation;
    s_settings.animation = on;
    ak_ghost_set_enabled(&s_ghost, on);
    if (persist && before != on) {
        uint8_t b = on ? 1U : 0U;
        if (ak_kv_set(AK_GHOST_KV_KEY, &b, 1U) != OPRT_OK) {
            PR_WARN("animation: KV write failed (applied until reboot)");
        }
    }
    PR_NOTICE("animation -> %s", on ? "on" : "off");
    if (persist) {
        send_device_settings(); /* also the echo for a Companion request */
        if (before != on) {
            ak_status_bar_set(ak_tr(on ? AK_STR_NOTE_ANIM_ON : AK_STR_NOTE_ANIM_OFF), AK_UI_ST_INFO);
            if (s_settings_active) {
                ak_settings_draw();
            } else {
                ak_render_status();
            }
        }
    }
}

static void ak_ghost_setting_load(void)
{
    uint8_t *blob = NULL;
    size_t   len  = 0;
    bool     on   = true;
    if (ak_kv_get(AK_GHOST_KV_KEY, &blob, &len) == OPRT_OK) {
        if (len == 1U && blob != NULL && blob[0] <= 1U) {
            on = blob[0] == 1U;
        } else {
            PR_WARN("animation: invalid KV value; using on");
        }
    }
    if (blob != NULL) {
        ak_kv_free(blob);
    }
    ak_ghost_init(&s_ghost, on);
    ak_ghost_setting_apply(on, false);
}
#endif

static void ak_font_size_load(void)
{
    uint8_t       *blob = NULL;
    size_t         len  = 0;
    ak_text_size_t size = AK_TEXT_SIZE_DEFAULT;
    if (ak_kv_get(AK_FONT_SIZE_KV_KEY, &blob, &len) == OPRT_OK &&
        (len != 1U || blob == NULL || !ak_text_size_from_px(blob[0], &size))) {
        PR_WARN("font size: invalid KV value; using %u px", (unsigned)ak_text_size_px(AK_TEXT_SIZE_DEFAULT));
        size = AK_TEXT_SIZE_DEFAULT;
    }
    if (blob != NULL) {
        ak_kv_free(blob);
    }
    ak_font_size_apply(size, false);
}

/* Poll the Chat turn: refresh the page and mirror each finished utterance /
 * reply to the Companion as display-only tuya_chat frames. */
static void ak_chat_poll(void)
{
#if AK_ENABLE_DISPLAY
    if (s_voice_mode != AK_VOICE_MODE_CHAT || !ak_asr_chat_take(&s_chat_view)) {
        return;
    }
    bool new_turn = false;
    if (s_chat_view.turn != s_chat_sent_turn) {
        ak_leds_activity(); /* a new Chat turn is a voice event */
        s_chat_sent_turn  = s_chat_view.turn;
        s_chat_sent_reply = false;
        new_turn          = true;
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
    ak_chat_scroll_sync(new_turn);
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
    char    key_hex[2U * AK_BLE_KEY_LEN + 1U];
    char    key_id[AK_BLE_KEY_ID_HEX];
    char    frame[FRAME_BUFFER_SIZE];

    if (ak_secure_random(NULL, key, sizeof(key)) != 0) {
        send_prov_status_uart("ble_pair", "no_random");
        return;
    }
    if (!ble_key_store(key)) {
        mbedtls_platform_zeroize(key, sizeof(key));
        send_prov_status_uart("ble_pair", "storage");
        return;
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
    ak_status_bar_set(ak_tr(AK_STR_ST_BLE_PAIRED), AK_UI_ST_OK);
    ak_prov_refresh_countdown(tal_system_get_millisecond());
    send_device_settings(); /* ble_paired: true */
#endif
#else
    (void)seq; /* BLE pairing needs the USB link of the DUAL profile */
    PR_WARN("ble pair: not available in this build");
#endif
}

/* K6 / K4 on the "Reset network?" dialog. */
/* Companion ble_unpair answered on the device (FR-02). */
static void ak_prov_unpair_resolve(bool confirm)
{
    if (!ak_provision_unpair_resolve(&s_prov, confirm, tal_system_get_millisecond())) {
        PR_NOTICE("ble_unpair: cancelled on the device");
        send_prov_status_uart("ble_unpair", "cancelled");
    } else {
#if AK_TRANSPORT_DUAL || AK_TRANSPORT_BLE_GATT
        ble_key_clear();
        ak_status_bar_set(ak_tr(AK_STR_ST_BLE_CLEARED), AK_UI_ST_NOTE);
#endif
        send_device_settings(); /* ble_paired: false */
    }
    ak_prov_changed();
}

/* Settings > Unbind & re-pair confirmed with K6: clear Wi-Fi and the Tuya
 * binding (License and the FR-15 saved networks stay), then reboot into the
 * pairing window so the Tuya app can pair over BLE. Same SDK path as a
 * Companion reset_netcfg: tuya_iot_reset -> RESET_COMPLETE -> reboot. */
static void ak_prov_rebind_resolve(bool confirm)
{
    if (!ak_provision_rebind_resolve(&s_prov, confirm, tal_system_get_millisecond())) {
        PR_NOTICE("rebind: cancelled on the device");
        ak_prov_changed();
        return;
    }
    if (ak_reset_netcfg_mark_reopen() != OPRT_OK) {
#if AK_ENABLE_DISPLAY
        ak_status_bar_set(ak_tr(AK_STR_ST_PAIRING_STORAGE), AK_UI_ST_ERROR);
#endif
        ak_prov_changed();
        return;
    }
    if (!ak_cloud_activated()) {
        /* Nothing to unbind (never activated, or a binding already cleared):
         * restart straight into the pairing window, as Settings > Pairing
         * mode does. tuya_iot_reset would not reboot an unactivated client. */
        PR_NOTICE("rebind: not activated; restarting into the pairing window");
#if AK_ENABLE_DISPLAY
        ak_status_bar_set(ak_tr(AK_STR_ST_PAIRING_RESTART), AK_UI_ST_NOTE);
#endif
        ak_prov_changed();
        tal_system_sleep(1500U); /* let the LCD show the message before the reset */
        tal_system_reset();
        return;
    }
    OPERATE_RET rr = ak_cloud_enter_pairing();
    if (rr != OPRT_OK) {
        (void)ak_reset_netcfg_take_reopen(); /* no reboot follows: drop the marker */
        PR_WARN("rebind: unbind failed or unsupported: %d", rr);
#if AK_ENABLE_DISPLAY
        ak_status_bar_set(ak_tr(AK_STR_REBIND_FAILED), AK_UI_ST_ERROR);
#endif
    } else {
        PR_NOTICE("rebind: Tuya binding cleared (License kept); reboot into the pairing window follows");
#if AK_ENABLE_DISPLAY
        ak_status_bar_set(ak_tr(AK_STR_REBIND_RUNNING), AK_UI_ST_NOTE);
#endif
    }
    ak_prov_changed();
}

/* FACTORY-RESET begin: KV deletes and the SDK reset only. No raw flash write
 * or erase here (the boot splash at 0x7ce000 stays); a host test checks. */

/* ak_freset_wipe's delete callback: a missing key counts as deleted. */
static int ak_freset_kv_del(const char *key, void *ctx)
{
    (void)ctx;
    uint8_t *blob = NULL;
    size_t   len  = 0U;
    if (ak_kv_get(key, &blob, &len) != OPRT_OK) {
        return 0; /* not stored */
    }
    if (blob != NULL) {
        ak_kv_free(blob);
    }
    OPERATE_RET rt = ak_kv_del(key);
    if (rt != OPRT_OK) {
        PR_WARN("reset system: deleting %s failed: %d", key, rt);
        return -1;
    }
    return 0;
}

/* Both confirms given: clear every setting, pairing and the Tuya License
 * (KV), then restart as a new board without a License. The SDK client keeps
 * its License copy in RAM, so the cloud unbind below still works after the
 * KV keys are gone. Activated on Tuya: the SDK reset also
 * unbinds in the cloud when online and removes the local activation data; it
 * reboots on RESET_COMPLETE. Not activated: reboot right away. */
static void ak_freset_run(void)
{
    uint32_t now_ms = tal_system_get_millisecond();
    PR_NOTICE("reset system: confirmed twice; clearing settings, Wi-Fi, Tuya binding, Tuya License and BLE pairing");
#if AK_ENABLE_DISPLAY
    ak_status_bar_set(ak_tr(AK_STR_FRESET_RUNNING), AK_UI_ST_NOTE);
    s_lcd_redraw_pending = true;
#endif
    ak_wifi_saved_clear(); /* RAM copy too, so nothing writes the list back */
#if AK_TRANSPORT_DUAL || AK_TRANSPORT_BLE_GATT
    ble_key_clear(); /* ends any BLE session under the old key */
#endif
    size_t failed = ak_freset_wipe(ak_freset_kv_del, NULL);
    if (failed != 0U) {
        PR_WARN("reset system: %u KV key(s) could not be deleted", (unsigned)failed);
    }
    if (ak_cloud_activated()) {
        OPERATE_RET rr = ak_cloud_enter_pairing();
        PR_NOTICE("reset system: Tuya reset requested (%d); the SDK reboots when it is done", rr);
        /* shortcut: 15 s for the SDK to finish (cloud unbind when online, local
         * clean-up either way); then the local clean-up runs directly and the
         * board reboots. Upgrade if real boards need longer. */
        s_freset_reboot_at_ms = now_ms + 15000U;
        if (s_freset_reboot_at_ms == 0U) {
            s_freset_reboot_at_ms = 1U;
        }
        return;
    }
    PR_NOTICE("reset system: not activated on Tuya; restarting");
    tal_system_sleep(1500U); /* let the LCD show the message before the reset */
    tal_system_reset();
}

/* Main loop: the SDK reset did not reboot in time. */
static void ak_freset_tick(uint32_t now_ms)
{
    if (s_freset_reboot_at_ms == 0U || (int32_t)(now_ms - s_freset_reboot_at_ms) < 0) {
        return;
    }
    s_freset_reboot_at_ms = 0U;
    PR_WARN("reset system: no reset-complete from the SDK; removing the local activation data and restarting");
    (void)ak_cloud_reset_local(); /* reboots itself through RESET_COMPLETE when it removes something */
    tal_system_reset();
}
/* FACTORY-RESET end */

/* K6 / K1 on either Reset system dialog. */
static void ak_freset_resolve_key(bool confirm)
{
    switch (ak_freset_resolve(&s_freset, confirm, tal_system_get_millisecond())) {
    case AK_FRESET_NEXT:
        PR_NOTICE("reset system: second confirm shown");
        break;
    case AK_FRESET_RUN:
        ak_freset_run();
        break;
    case AK_FRESET_CANCELLED:
        PR_NOTICE("reset system: cancelled on the device");
#if AK_ENABLE_DISPLAY
        ak_status_bar_set(ak_tr(AK_STR_FRESET_CANCELLED), AK_UI_ST_NOTE);
#endif
        break;
    case AK_FRESET_NONE:
    default:
        break;
    }
    ak_ui_sync(tal_system_get_millisecond());
#if AK_ENABLE_DISPLAY
    if (s_settings_active) {
        ak_settings_draw();
    } else {
        s_lcd_redraw_pending = true;
    }
#endif
}

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
    char     frame[AK_FRAME_BUFFER_LARGE]; /* the SSID may need JSON escapes */
    uint32_t sequence;
    if (transport_begin_sequenced(&sequence)) {
        transport_end_sequenced(frame, ak_proto_wifi_status(frame, sizeof(frame), sequence, state, ssid));
    }
}

static void send_pid_status_frame(const char *state, const char *reason)
{
    char     frame[FRAME_BUFFER_SIZE];
    uint32_t sequence;
    if (transport_begin_sequenced(&sequence)) {
        transport_end_sequenced(frame, ak_proto_pid_status(frame, sizeof(frame), sequence, state, reason));
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

#if AK_ENABLE_DISPLAY
/* ── FR-03 scenes (ak_scene.h) ──────────────────────────────────────────
 * Five KV slots, one key each (ak_scn0..4, 96 B ciphertext) plus the active
 * slot number (ak_scnact). Live settings stay in ak_keymap / ak_vmode /
 * ak_ledpwr; a switch copies the slot into them, writing ak_scnact last. */
#define AK_SCENE_ACTIVE_KV_KEY "ak_scnact"

static char       s_scene_frame[520]; /* scene_state with 5 full slots is about 450 B */

static void ak_scene_kv_key(uint8_t slot, char key[12])
{
    (void)snprintf(key, 12, "ak_scn%u", (unsigned)slot);
}

static void ak_scenes_load(void)
{
    for (uint8_t i = 0U; i < AK_SCENE_SLOTS; i++) {
        char     key[12];
        uint8_t *blob = NULL;
        size_t   len  = 0;
        ak_scene_kv_key(i, key);
        s_scene_used[i] = ak_kv_get(key, &blob, &len) == OPRT_OK && blob != NULL &&
                          ak_scene_from_blob(&s_scenes[i], blob, (unsigned)len);
        if (blob != NULL) {
            ak_kv_free(blob);
            if (!s_scene_used[i]) {
                PR_WARN("scene: slot %u damaged; treated as empty", (unsigned)i);
            }
        }
    }
    uint8_t *blob = NULL;
    size_t   len  = 0;
    s_scene_active = -1;
    if (ak_kv_get(AK_SCENE_ACTIVE_KV_KEY, &blob, &len) == OPRT_OK && blob != NULL && len == 1U &&
        blob[0] < AK_SCENE_SLOTS && s_scene_used[blob[0]]) {
        s_scene_active = blob[0];
    }
    if (blob != NULL) {
        ak_kv_free(blob);
    }
    PR_NOTICE("scene: active slot %d", s_scene_active);
}

static bool ak_scene_dirty(void)
{
    return s_scene_active >= 0 && ak_scene_differs(&s_scenes[s_scene_active], &s_keymap, (uint8_t)s_voice_mode,
                                                   s_settings.led_mode, s_settings.led_timeout_idx);
}

static void send_scene_state(const char *error, const char *req)
{
    const ak_scene_t *slots[AK_SCENE_SLOTS];
    for (uint8_t i = 0U; i < AK_SCENE_SLOTS; i++) {
        slots[i] = s_scene_used[i] ? &s_scenes[i] : NULL;
    }
    bool     dirty = ak_scene_dirty();
    uint32_t sequence;
    if (!transport_begin_sequenced(&sequence)) {
        return;
    }
    transport_end_sequenced(s_scene_frame, ak_proto_scene_state(s_scene_frame, sizeof(s_scene_frame), sequence,
                                                                s_scene_active, dirty, slots, AK_SCENE_SLOTS, error,
                                                                req));
}

/* A continuous voice session (when built in) blocks a scene switch. */
static bool ak_scene_dictating(void)
{
    return false;
}

/* Voice input waiting for a decision (text in the strip or a PC fill) blocks a scene switch: the new keymap
 * or voice mode could remove the keys that send or delete it, and Chat mode would clear the strip. The
 * text stays; the scene "busy" notice shows. */
static bool ak_scene_input_pending(void)
{
#if AK_ENABLE_DISPLAY
    return ak_input_confirm();
#else
    return false;
#endif
}

/* A switch only happens in ① standby with nothing modal or in progress. */
static bool ak_scene_switch_allowed(void)
{
    uint32_t now = tal_system_get_millisecond();
    return ak_ui_state(&s_ui) == AK_UI_STANDBY && !s_settings_active && s_menu.state == AK_MENU_IDLE &&
           !ak_bind_active(&s_bind) && !ak_scene_dictating() && !ak_scene_input_pending() && !s_combo.active &&
           !ak_provision_reset_pending(&s_prov, now) &&
           !ak_provision_unpair_pending(&s_prov, now) && !ak_provision_rebind_pending(&s_prov, now)
#if AK_TRANSPORT_HAS_UART
           && !ak_ota_link_busy()
#endif
        ;
}

/* Apply slot `slot` to the live settings. Returns the scene_state error, or
 * NULL on success. Live state changes even when a KV write fails (logged, as
 * for the keymap frame). */
static const char *ak_scene_switch(uint8_t slot)
{
    if (slot >= AK_SCENE_SLOTS || !s_scene_used[slot]) {
        return "empty";
    }
    if (!ak_scene_switch_allowed()) {
        return "busy";
    }
    const ak_scene_t *sc = &s_scenes[slot];
    s_keymap             = sc->keymap;
    for (int gi = 0; gi < AK_KEYMAP_SLOTS; gi++) {
        ak_gesture_init(&s_key_gesture[gi]);
    }
    uint8_t  blob[AK_KEYMAP_BLOB_SIZE];
    unsigned n = ak_keymap_to_blob(&s_keymap, blob, sizeof(blob));
    if (n != AK_KEYMAP_BLOB_SIZE || ak_kv_set(AK_KEYMAP_KV_KEY, blob, n) != OPRT_OK) {
        PR_WARN("scene: keymap KV write failed (applied in RAM)");
    }
    if ((ak_voice_mode_t)sc->voice_mode != s_voice_mode) {
        ak_voice_mode_apply((ak_voice_mode_t)sc->voice_mode, true); /* stores + reports */
    }
    if (sc->led_mode != s_settings.led_mode || sc->led_timeout_idx != s_settings.led_timeout_idx) {
        s_settings.led_mode        = sc->led_mode;
        s_settings.led_timeout_idx = sc->led_timeout_idx;
        ak_led_setting_commit();
        send_device_settings();
    }
    /* Idle colours and the home layout are read from the slot when drawn. */
    uint8_t act = slot;
    if (ak_kv_set(AK_SCENE_ACTIVE_KV_KEY, &act, 1U) != OPRT_OK) {
        PR_WARN("scene: active slot KV write failed");
    }
    s_scene_active = slot;
    ak_leds_activity();
    char note[48];
    (void)snprintf(note, sizeof(note), ak_tr(AK_STR_ST_SCENE), sc->name);
    ak_status_bar_set(note, AK_UI_ST_OK);
    ak_render_status();
    PR_NOTICE("scene: switched to slot %u (%s rev %u)", (unsigned)slot, sc->id, (unsigned)sc->rev);
    return NULL;
}

/* FR-03 D1: open the scene picker (Settings > Scene, or a scene_menu key).
 * It is an ordinary LCD menu built on the keyboard: K2..K5 pick, the knob
 * moves, K6 / knob click confirm, K1 = back; a Companion menu replaces it.
 * Only from ① standby or the settings page, never over another menu. */
static void ak_scene_picker_open(void)
{
    static ak_menu_page_t pages[AK_SCENE_PICKER_PAGES];
    uint32_t              now = tal_system_get_millisecond();
    ak_ui_state_t         st  = ak_ui_state(&s_ui);
    bool from_settings        = s_settings_active && !s_test_active && !s_devinfo_page && !s_wifi_page &&
                         !ak_bind_active(&s_bind);
    if (s_menu.state != AK_MENU_IDLE || (st != AK_UI_STANDBY && !from_settings) || ak_scene_dictating() ||
        ak_scene_input_pending() ||
        (!from_settings && ak_bind_active(&s_bind)) ||
        s_combo.active || ak_provision_reset_pending(&s_prov, now) || ak_provision_unpair_pending(&s_prov, now) ||
        ak_provision_rebind_pending(&s_prov, now) || s_prov.state == AK_PROV_CONFIRMING
#if AK_TRANSPORT_HAS_UART
        || ak_ota_link_busy()
#endif
    ) {
        ak_status_tmp_set(ak_tr(AK_STR_ST_SCENE_BUSY), AK_UI_ST_WARN, AK_MENU_NOTICE_MS);
        if (!s_settings_active) {
            ak_render_status();
        }
        return;
    }
    uint8_t n = ak_scene_picker_build(s_scenes, s_scene_used, s_scene_active, ak_tr(AK_STR_SET_SCENE), pages,
                                      s_scene_pick_slot);
    if (n == 0U) {
        ak_status_tmp_set(ak_tr(AK_STR_ST_SCENE_NONE), AK_UI_ST_NOTE, AK_MENU_NOTICE_MS);
        if (!s_settings_active) {
            ak_render_status();
        } else {
            ak_settings_draw();
        }
        return;
    }
    if (s_settings_active) {
        s_settings_active    = false;
        s_settings_confirmed = false;
    }
    for (uint8_t i = 0U; i < n; i++) {
        (void)ak_menu_on_page(&s_menu, &pages[i], now);
    }
    ak_ui_menu_arrived(&s_ui, true);
    ak_ui_sync(now);
    ak_menu_set_hidden(&s_menu, !ak_ui_menu_visible(&s_ui), now);
    ak_leds_activity();
    ak_gestures_reset();
    ak_render_status();
    PR_NOTICE("scene: picker opened (%u page(s))", (unsigned)n);
}

/* scene_put / scene_switch / scene_del / scene_get / scene_list. */
static void ak_scene_frame(const char *type, const char *payload, uint32_t seq)
{
    const char *err  = NULL;
    uint32_t    slot = 0;
    if (strcmp(type, "scene_put") == 0) {
        uint8_t    s;
        ak_scene_t sc;
        uint8_t    blob[AK_SCENE_BLOB_LEN];
        char       key[12];
        if (!ak_scene_parse_put(payload, &s, &sc)) {
            PR_WARN("scene_put rejected: invalid field");
            err = "invalid";
        } else {
            ak_scene_kv_key(s, key);
            if (ak_scene_to_blob(&sc, blob, sizeof(blob)) != AK_SCENE_BLOB_LEN ||
                ak_kv_set(key, blob, sizeof(blob)) != OPRT_OK) {
                PR_WARN("scene_put: KV write failed for slot %u", (unsigned)s);
                err = "storage";
            } else {
                s_scenes[s]     = sc;
                s_scene_used[s] = true;
                PR_NOTICE("scene: slot %u <- %s rev %u", (unsigned)s, sc.id, (unsigned)sc.rev);
            }
        }
    } else if (ak_json_get_uint(payload, "slot", AK_SCENE_SLOTS - 1U, &slot) != AK_JSON_OK &&
               strcmp(type, "scene_list") != 0) {
        err = "invalid";
    } else if (strcmp(type, "scene_switch") == 0) {
        err = ak_scene_switch((uint8_t)slot);
    } else if (strcmp(type, "scene_del") == 0) {
        char key[12];
        ak_scene_kv_key((uint8_t)slot, key);
        if ((int)slot == s_scene_active) {
            err = "active";
        } else if (s_scene_used[slot] && ak_kv_del(key) != OPRT_OK) {
            err = "storage";
        } else {
            s_scene_used[slot] = false;
        }
    } else if (strcmp(type, "scene_get") == 0) {
        if (!s_scene_used[slot]) {
            err = "empty";
        } else {
            uint32_t sequence;
            if (transport_begin_sequenced(&sequence)) {
                transport_end_sequenced(s_scene_frame, ak_proto_scene_data(s_scene_frame, sizeof(s_scene_frame),
                                                                           sequence, (uint8_t)slot,
                                                                           &s_scenes[slot]));
            }
            send_ack(seq);
            return;
        }
    }
    send_scene_state(err, err != NULL ? type : NULL);
    send_ack(seq);
}
#endif

#if AK_TRANSPORT_HAS_UART
/* ── Local OTA glue (ak_ota_link.h) ──────────────────────────────────────── */
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
        ak_status_bar_set(line, AK_UI_ST_ERROR);
    } else if (what == AK_OTA_UI_UPDATED) {
        (void)snprintf(line, sizeof(line), ak_tr(AK_STR_OTA_UPDATED), text);
        ak_status_bar_set(line, AK_UI_ST_OK);
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
            ak_status_bar_set(ak_tr(AK_STR_ST_LICENSE_EXISTS), AK_UI_ST_NOTE);
            ak_prov_refresh_countdown(tal_system_get_millisecond());
#endif
            return;
        }
        if (verdict != AK_PROV_ALLOW && verdict != AK_PROV_NEEDS_DEVICE_CONFIRM) {
            PR_WARN("%s rejected: pairing window %s", type, ak_provision_verdict_str(verdict));
            send_prov_status_uart(type, ak_provision_verdict_str(verdict));
#if AK_ENABLE_DISPLAY
            ak_status_bar_set(ak_tr(AK_STR_ST_PAIRING_FIRST), AK_UI_ST_NOTE);
            ak_prov_refresh_countdown(tal_system_get_millisecond());
#endif
            return;
        }
    }

    if (strcmp(type, "hello") == 0) {
        bool pc_asr = false;
        s_pc_asr_results = ak_json_get_bool(payload, "pc_asr", &pc_asr) == AK_JSON_OK && pc_asr;
        if (!from_uart) {
            s_hello_ble = true; /* Settings > Device info: the BLE link has shaken hands */
        }
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
                ak_ble_pair_close(&s_ble_pair); /* no BLE left to pair over */
                }
                PR_NOTICE("dual: Companion selected USB; AK BLE disabled for this boot");
            }
#endif
            if (!s_pc_connected) {
                s_pc_connected = true;
                ak_transport_feedback(AK_STR_ST_USB_CONNECTED, AK_UI_ST_OK);
                send_system_info_uart();
            }
        }
        /* Respond only after the selected transport is locked, so a USB hello
         * is never mirrored to BLE during the transition. */
        send_hello_ack();
#if AK_ENABLE_DISPLAY
        send_scene_state(NULL, NULL); /* FR-03: the Companion syncs its favourites from this */
#endif
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
        s_pc_asr_results = false;
        if (!from_uart) {
            s_hello_ble = false;
        }
        if (from_uart && s_pc_connected) {
            s_pc_connected = false;
            ak_transport_feedback(AK_STR_ST_USB_DISCONNECTED, AK_UI_ST_NOTE);
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
    } else if (strncmp(type, "scene_", 6) == 0) {
#if AK_ENABLE_DISPLAY
        if (strcmp(type, "scene_put") == 0 || strcmp(type, "scene_switch") == 0 ||
            strcmp(type, "scene_del") == 0 || strcmp(type, "scene_get") == 0 || strcmp(type, "scene_list") == 0) {
            ak_scene_frame(type, payload, seq);
        }
#endif
    } else if (strcmp(type, "led") == 0) {
        /* No Companion-driven LED effects exist yet; a led frame still counts
         * as something to show and wakes Auto LED power (FW-05 follow-up). */
        ak_leds_activity();
    } else if (strcmp(type, "device_settings") == 0) {
#if AK_ENABLE_DISPLAY
        uint32_t     volume     = 0U;
        uint32_t     brightness = 0U;
        uint32_t     off_s      = 0U;
        ak_json_rc_t vol_rc     = ak_json_get_uint(payload, "volume", 100U, &volume);
        ak_json_rc_t bri_rc     = ak_json_get_uint(payload, "brightness", 100U, &brightness);
        ak_json_rc_t off_rc     = ak_json_get_uint(payload, "led_auto_off_s", 3600U, &off_s);
        uint32_t     font_px    = 0U;
        ak_json_rc_t font_rc    = ak_json_get_uint(payload, "font_size", 64U, &font_px);
        bool         anim_on    = true;
        ak_json_rc_t anim_rc    = ak_json_get_bool(payload, "animation", &anim_on);
        ak_text_size_t font     = AK_TEXT_SIZE_DEFAULT;
        char         lang_str[8];
        char         voice_str[8];
        char         led_str[8];
        ak_json_rc_t lang_rc    = ak_json_get_str(payload, "language", lang_str, sizeof(lang_str));
        ak_json_rc_t voice_rc   = ak_json_get_str(payload, "voice_mode", voice_str, sizeof(voice_str));
        ak_json_rc_t led_rc     = ak_json_get_str(payload, "led_power", led_str, sizeof(led_str));
        ak_lang_t    lang       = AK_LANG_DEFAULT;
        ak_voice_mode_t voice   = AK_VOICE_MODE_ASR;
        ak_led_mode_t   led     = AK_LED_MODE_AUTO;
        uint8_t         off_idx = s_settings.led_timeout_idx;
        bool         changed    = false;

        /* Any present field out of range rejects the whole frame (no partial apply). */
        if (vol_rc == AK_JSON_INVALID || bri_rc == AK_JSON_INVALID || off_rc == AK_JSON_INVALID ||
            lang_rc == AK_JSON_INVALID || voice_rc == AK_JSON_INVALID || led_rc == AK_JSON_INVALID ||
            font_rc == AK_JSON_INVALID || (font_rc == AK_JSON_OK && !ak_text_size_from_px(font_px, &font)) ||
            anim_rc == AK_JSON_INVALID ||
            (lang_rc == AK_JSON_OK && !ak_lang_from_wire(lang_str, &lang)) ||
            (voice_rc == AK_JSON_OK && !ak_voice_mode_from_wire(voice_str, &voice)) ||
            (led_rc == AK_JSON_OK && !ak_led_mode_from_wire(led_str, &led)) ||
            (off_rc == AK_JSON_OK && !ak_led_policy_timeout_from_s(off_s, &off_idx))) {
            PR_WARN("device settings rejected: a field is out of range");
            return;
        }
        if (voice_rc == AK_JSON_OK && voice != s_voice_mode) {
            /* Same path as Settings > Voice mode; reports the new state. */
            ak_voice_mode_apply(voice, true);
            ak_status_bar_set(ak_tr(voice == AK_VOICE_MODE_CHAT ? AK_STR_NOTE_CHAT : AK_STR_NOTE_ASR), AK_UI_ST_INFO);
            changed = true;
        }
        if ((led_rc == AK_JSON_OK && (uint8_t)led != s_settings.led_mode) ||
            (off_rc == AK_JSON_OK && off_idx != s_settings.led_timeout_idx)) {
            if (led_rc == AK_JSON_OK) {
                s_settings.led_mode = (uint8_t)led;
            }
            s_settings.led_timeout_idx = off_idx;
            ak_led_setting_commit(); /* applies and stores, as the Settings commit does */
            changed = true;
        }
        if (lang_rc == AK_JSON_OK) {
            /* Last change wins; ak_language_apply reports the state and redraws. */
            ak_language_apply(lang, true);
        }
        if (font_rc == AK_JSON_OK) {
            /* Same as the language: reports the state and redraws. */
            ak_font_size_apply(font, true);
        }
        if (anim_rc == AK_JSON_OK) {
            ak_ghost_setting_apply(anim_on, true); /* reports the state */
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
        if (vol_rc == AK_JSON_OK || bri_rc == AK_JSON_OK || led_rc == AK_JSON_OK || off_rc == AK_JSON_OK) {
            send_device_settings();
        }
#endif
    } else if (strcmp(type, "bindings") == 0) {
#if AK_ENABLE_DISPLAY
        static const char *fields[LCD_SESSION_ROWS] = {
            "alias_a", "alias_b", "alias_c"
        };
        static const char *name_fields[LCD_SESSION_ROWS] = {"name_a", "name_b", "name_c"};
        static const char *mode_fields[LCD_SESSION_ROWS] = {"mode_a", "mode_b", "mode_c"};
        bool changed = false;
        for (uint8_t i = 0U; i < LCD_SESSION_ROWS; i++) {
            char alias[LCD_SESSION_ALIAS_BYTES] = "";
            char name[LCD_SESSION_NAME_BYTES]   = "";
            char mode[4]                        = "";
            /* "pc": this slot's voice text is typed into the PC input box. */
            if (ak_json_get_str_clip(payload, mode_fields[i], mode, sizeof(mode)) == AK_JSON_OK) {
                s_slot_pc[i] = strcmp(mode, "pc") == 0;
            }
            /* Display-only label: clip to the LCD buffer at a UTF-8 boundary. */
            if (ak_json_get_str_clip(payload, fields[i], alias, sizeof(alias)) == AK_JSON_OK) {
                ak_session_label_set(i, alias);
                changed = true;
            }
            if (ak_json_get_str_clip(payload, name_fields[i], name, sizeof(name)) == AK_JSON_OK) {
                ak_lcd_text(name, s_session_names[i], sizeof(s_session_names[i]));
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
    } else if (strcmp(type, "wifi_list") == 0) {
        /* FR-15: read-only, SSIDs without passwords, so any authenticated
         * source may ask (USB or the paired BLE channel). */
        send_wifi_list();
    } else if (strcmp(type, "wifi_forget") == 0) {
        /* FR-15: delete one remembered network. USB only (ak_rx.c), no window:
         * it removes data and never adds a credential. */
        char ssid[AK_WIFI_SSID_MAX + 1U] = "";
        if (ak_json_get_str(payload, "ssid", ssid, sizeof(ssid)) != AK_JSON_OK || ssid[0] == '\0') {
            PR_WARN("wifi_forget rejected: ssid missing or too long");
            return;
        }
        (void)ak_wifi_saved_forget(ssid);
        send_wifi_list();
        send_ack(seq);
    } else if (strcmp(type, "wifi_clear") == 0) {
        ak_wifi_saved_clear();
        send_wifi_list();
        send_ack(seq);
    } else if (strcmp(type, "pid") == 0) {
        /* PC writes the Tuya product ID: {"type":"pid","payload":{"pid":".."}}.
         * USB only (ak_rx gate); accepted only while no PID is in effect or
         * stored, never replaced or cleared (ak_pid.h). Takes effect after a
         * restart. The value is not echoed and not logged. */
        char pid[AK_PID_LEN + 2U] = "";
        if (ak_json_get_str(payload, "pid", pid, sizeof(pid)) != AK_JSON_OK || strlen(pid) != AK_PID_LEN) {
            pid[0] = '\0'; /* too long / missing / wrong length: rejected as invalid below */
        }
        ak_pid_verdict_t pv = ak_pid_write(source, pid);
        if (pv == AK_PID_WRITE_OK) {
            send_pid_status_frame("written_reboot_required", "");
            send_device_settings(); /* pid: pending */
            send_ack(seq);
#if AK_ENABLE_DISPLAY
            ak_status_bar_set(ak_tr(AK_STR_ST_PID_SAVED), AK_UI_ST_OK);
#endif
        } else {
            PR_WARN("pid write rejected: %s", ak_pid_verdict_str(pv));
            send_pid_status_frame("rejected", ak_pid_verdict_str(pv));
        }
        memset(pid, 0, sizeof(pid));
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
            ak_status_bar_set(ak_tr(AK_STR_ST_LICENSE_SAVED), AK_UI_ST_OK);
        } else {
            ak_status_bar_set(ak_tr(AK_STR_ST_LICENSE_WRITE_FAILED), AK_UI_ST_ERROR);
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
    } else if (strcmp(type, "prov_open") == 0) {
        /* FR-02: the Companion asks for the pairing window. USB only (ak_rx
         * gate). Same ⑦ as Settings > Pairing mode: the settings page opens on
         * the Pairing row with its K6 / K1 confirm; nothing opens without K6. */
        uint32_t now = tal_system_get_millisecond();
        if (ak_provision_is_open(&s_prov, now)) {
            ak_prov_changed(); /* already open: re-report prov_status */
        } else if (s_prov.state == AK_PROV_CONFIRMING || ak_provision_reset_pending(&s_prov, now) ||
                   ak_provision_unpair_pending(&s_prov, now) || ak_provision_rebind_pending(&s_prov, now) ||
                   ak_freset_up(now) || s_freset_reboot_at_ms != 0U || s_test_active
#if AK_TRANSPORT_HAS_UART
                   || ak_ota_link_busy()
#endif
        ) {
            send_prov_status_uart(type, "busy");
            ak_status_bar_set(ak_tr(AK_STR_ST_PAIR_REQ_BUSY), AK_UI_ST_NOTE);
        } else {
            ak_pending_confirm_exit(); /* settings ends ② / ③ */
            s_settings_active    = true;
            s_settings_confirmed = false;
            s_devinfo_page          = false;
            s_wifi_page          = false;
            s_wifi_help          = false;
            s_settings.editing   = false;
            s_settings.selected  = AK_SETTINGS_ITEM_PAIRING;
            ak_provision_request(&s_prov, now);
            send_prov_status_uart(type, ak_provision_verdict_str(AK_PROV_NEEDS_DEVICE_CONFIRM));
            ak_prov_changed();
            ak_settings_draw();
        }
    } else if (strcmp(type, "ble_unpair") == 0) {
        /* FR-02: clear the keyboard's BLE pairing key. USB only (ak_rx gate);
         * the device asks first (⑦, K6 / K1, 10 s). */
#if AK_TRANSPORT_DUAL || AK_TRANSPORT_BLE_GATT
        if (!s_ble_key_set) {
            send_device_settings(); /* nothing to clear: report ble_paired false */
        } else if (s_test_active ||
#if AK_TRANSPORT_HAS_UART
                   ak_ota_link_busy() ||
#endif
                   !ak_provision_unpair_request(&s_prov, tal_system_get_millisecond())) {
            send_prov_status_uart(type, "busy");
            ak_status_bar_set(ak_tr(AK_STR_ST_PAIR_REQ_BUSY), AK_UI_ST_NOTE);
        } else {
            ak_leds_activity();
            send_prov_status_uart(type, ak_provision_verdict_str(AK_PROV_NEEDS_DEVICE_CONFIRM));
            ak_prov_changed();
        }
#else
        send_prov_status_uart(type, "unsupported");
#endif
    } else if (strcmp(type, "menu") == 0 || strcmp(type, "menu_close") == 0 || strcmp(type, "notice") == 0 ||
               strcmp(type, "menu_parked") == 0) {
        /* LCD menu frames: USB, or BLE after the secure channel (on_rx_line
         * only delivers authenticated BLE plaintext, <= 511 bytes). */
#if AK_ENABLE_DISPLAY
        ak_menu_on_frame(type, payload);
#endif
    } else if (strcmp(type, "bind_done") == 0) {
        /* Settings > Bind session: the Companion's answer to bind_session. */
#if AK_ENABLE_DISPLAY
        ak_bind_on_done(payload);
#endif
    } else if (strcmp(type, "input_restore") == 0) {
        /* The Companion hands voice text (or the result of a PC fill) back:
         *   text, src "pc"    PC ASR text: appended to the input strip
         *   text, src "retry" a K6 send (or a PC fill) failed: back into the strip,
         *                     red, and out of the chat history again
         *   filled:true       the PC input box was filled: K6 = Enter, K4 = Backspace / clear
         *   no text, src "pc" nothing recognised: ends the ② wait with the reason
         * The strip text is never replaced: new words are appended. */
#if AK_ENABLE_DISPLAY
        char     text[AK_INPUT_TEXT_MAX + 1U];
        char     msg[AK_MENU_NOTICE_MAX + 1U] = "";
        char     src[8]                       = "";
        bool     filled                       = false;
        uint32_t now                          = tal_system_get_millisecond();
        (void)ak_json_get_str_clip(payload, "src", src, sizeof(src));
        (void)ak_json_get_bool(payload, "filled", &filled);
        bool pc = strcmp(src, "pc") == 0;
        if (ak_json_get_str_clip(payload, "text", text, sizeof(text)) != AK_JSON_OK) {
            text[0] = '\0';
            if (!filled) {
                PR_WARN("input_restore rejected: no text");
                return;
            }
        }
        (void)ak_json_get_str_clip(payload, "msg", msg, sizeof(msg));
        s_voice_ui_state          = AK_VOICE_UI_IDLE; /* no ASR timeout while the strip waits */
        s_voice_ui_state_since_ms = now;
        if (text[0] == '\0') {
            if (filled) {
                (void)ak_ui_input_restore(&s_ui, now); /* releases a PC wait */
                ak_leds_activity();
                ak_pc_fill_start(now);
                ak_voice_status(msg[0] != '\0' ? msg : ak_tr(AK_STR_ST_PC_FILLED), AK_UI_ST_OK);
            } else if (pc && s_ui.pc_wait) {
                /* Nothing recognised / PC ASR failed: ② -> ① with the reason. */
                ak_ui_voice_end(&s_ui, AK_UI_VOICE_ERROR, false, now);
                ak_voice_status(msg[0] != '\0' ? msg : ak_tr(AK_STR_ST_NO_SPEECH), AK_UI_ST_NOTE);
            } else {
                PR_WARN("input_restore rejected: no text");
                return;
            }
        } else {
            bool clipped = false;
            (void)ak_ui_input_restore(&s_ui, now); /* releases a PC wait */
            if (!pc) {
                ak_chat_history_drop_sent(); /* a failed send is not a sent message */
            }
            s_pc_fill = false;
            ak_input_begin(now);
            (void)ak_input_append(s_input_text, text, &clipped);
            ak_input_changed();
            ak_leds_activity();
            if (clipped) {
                ak_voice_status(ak_tr(AK_STR_ST_INPUT_FULL), AK_UI_ST_WARN);
            } else if (pc) {
                ak_voice_status(msg[0] != '\0' ? msg : ak_tr(AK_STR_ST_CONFIRM_HINT), AK_UI_ST_OK);
            } else {
                ak_voice_status(msg[0] != '\0' ? msg : ak_tr(AK_STR_ST_INPUT_NOT_SENT), AK_UI_ST_ERROR);
            }
        }
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
            ak_ghost_status(&s_ghost, session_index, tal_system_get_millisecond());
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

#if AK_TRANSPORT_HAS_BLE
/* Carry out one step of an over-the-air pairing: send its plaintext reply
 * (pairing frames only, never data) and update the screen. */
static void ble_pair_apply(ak_ble_pair_act_t act)
{
    if (s_ble_pair_reply[0] != '\0' && transport_lock()) {
        (void)ak_ble_gatt_write(s_ble_pair_reply, (uint32_t)strlen(s_ble_pair_reply));
        transport_unlock();
    }
    s_ble_pair_reply[0] = '\0';
    switch (act) {
    case AK_BLE_PAIR_ACT_SHOW:
        PR_NOTICE("ble pair: code shown; waiting for K6 and the Companion");
        ak_leds_activity();
        break;
    case AK_BLE_PAIR_ACT_STORE: {
        /* Store K first; only a successful KV write lets sec_pair_done go out,
         * so the Companion never keeps a key the keyboard lacks (R6b). */
        uint8_t key[AK_BLE_KEY_LEN];
        char    key_id[AK_BLE_KEY_ID_HEX];
        bool    ok = ak_ble_pair_take_key(&s_ble_pair, key) && ble_key_store(key);
        ak_ble_key_id(key, key_id);
        mbedtls_platform_zeroize(key, sizeof(key));
        if (!ok) {
            PR_WARN("ble pair: storing the key failed; telling the Companion not to keep it");
        }
        ble_pair_apply(ak_ble_pair_stored(&s_ble_pair, ok, tal_system_get_millisecond(), s_ble_pair_reply,
                                          sizeof(s_ble_pair_reply)));
        if (ok) {
            PR_NOTICE("ble pair: paired over the air, key %s", key_id);
            ak_status_bar_set(ak_tr(AK_STR_ST_BLE_PAIRED), AK_UI_ST_OK);
            send_device_settings(); /* ble_paired: true */
        }
        break;
    }
    case AK_BLE_PAIR_ACT_DONE:
        break; /* sec_pair_done was sent above; the status follows in ACT_STORE */
    case AK_BLE_PAIR_ACT_FAILED:
        PR_WARN("ble pair: attempt failed (%u of %u)%s", (unsigned)s_ble_pair.fails,
                (unsigned)AK_BLE_PAIR_MAX_FAILS, s_ble_pair.state == AK_BLE_PAIR_CLOSED ? "; window closed" : "");
        ak_status_bar_set(ak_tr(AK_STR_ST_BLE_PAIR_FAILED), AK_UI_ST_NOTE);
        break;
    case AK_BLE_PAIR_ACT_REPLY:
    case AK_BLE_PAIR_ACT_NONE:
    default:
        return;
    }
#if AK_ENABLE_DISPLAY
    if (s_settings_active) {
        ak_settings_draw();
    } else {
        s_lcd_redraw_pending = true;
    }
#endif
}
#endif

/* Every assembled line enters here. UART lines are processed directly; BLE
 * lines must pass the secure channel first: unpaired or unauthenticated BLE
 * links get nothing processed and nothing sent back except the handshake. */
static void on_rx_line(const char *line, ak_rx_source_t source)
{
#if AK_TRANSPORT_DUAL || AK_TRANSPORT_BLE_GATT
    /* Over-the-air pairing frames (plaintext, before any session). Outside
     * the pairing window ak_ble_pair drops them without a reply. */
    if (source == AK_RX_SRC_BLE && s_ble_sec.state != AK_BLE_SEC_AUTHED && ak_ble_pair_owns_line(line)) {
        ble_pair_apply(ak_ble_pair_on_line(&s_ble_pair, line, tal_system_get_millisecond(), ak_secure_random, NULL,
                                           s_ble_pair_reply, sizeof(s_ble_pair_reply)));
        return;
    }
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

/* FR-15 once per loop: tell the roamer whether it may use the radio, push a
 * changed list to the Companion, and keep the Saved Wi-Fi page current. */
static void ak_wifi_poll(uint32_t now)
{
    /* Not while the pairing window / its confirm is up (FR-12 B: Tuya or the
     * Companion owns the radio then), during an OTA, or on the test page. */
    bool allowed = s_prov.state == AK_PROV_CLOSED;
#if AK_TRANSPORT_HAS_UART
    allowed = allowed && !ak_ota_link_busy();
#endif
#if AK_ENABLE_DISPLAY
    allowed = allowed && !s_test_active;
#endif
    ak_wifi_set_roam_allowed(allowed);
    if (s_pc_connected && (s_wifi_list_dirty || ak_wifi_saved_revision() != s_wifi_sent_rev)) {
        s_wifi_list_dirty = false;
        send_wifi_list();
    }
#if AK_ENABLE_DISPLAY
    if (s_settings_active && s_wifi_page) {
        static uint32_t s_page_rev = 0xFFFFFFFFU;
        bool            expired    = s_wifi_del_armed && (int32_t)(now - s_wifi_del_deadline_ms) >= 0;
        if (expired) {
            ak_wifi_page_disarm();
        }
        if (expired || s_page_rev != ak_wifi_saved_revision()) {
            s_page_rev = ak_wifi_saved_revision();
            ak_settings_draw();
        }
    }
#else
    (void)now;
#endif
}

/* K1 + K6 held 5 s (any screen): the same confirm as Settings > Unbind &
 * re-pair. Not while a firmware update or another system confirm runs. */
static void ak_recovery_combo_fired(uint32_t now)
{
#if AK_TRANSPORT_HAS_UART
    if (ak_ota_link_busy()) {
        return;
    }
#endif
    if (ak_provision_rebind_action(ak_cloud_license_usable(), ak_cloud_activated()) == AK_PAIR_ACTION_NEED_LICENSE) {
        PR_WARN("recovery combo: no License; configure over USB");
#if AK_ENABLE_DISPLAY
        ak_status_bar_set(ak_tr(AK_STR_REBIND_NEED_LICENSE), AK_UI_ST_NOTE);
        ak_render_status();
#endif
        return;
    }
    if (s_prov.state == AK_PROV_CONFIRMING) {
        ak_provision_close(&s_prov); /* the recovery confirm replaces the pairing-mode one */
    }
    if (!ak_provision_rebind_request(&s_prov, now)) {
        PR_WARN("recovery combo: another confirm is up; ignored");
        return;
    }
    PR_NOTICE("recovery combo: unbind confirm (K6 release = unbind, K1 = cancel, %u s)",
              (unsigned)(AK_PROV_RESET_CONFIRM_MS / 1000U));
    ak_prov_changed();
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
    ak_pid_load(); /* runtime PID (KV) or the compile-time one; needs ak_kv_init */
    ak_language_load(); /* FR-01: before the first LCD draw; needs ak_kv_init */
    ak_font_size_load(); /* likewise: every layout depends on it */

    PR_NOTICE("======== Agentic Keyboard for Kiro Crew: product fw %s ========", APP_FW_VERSION);
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
#if AK_ENABLE_DISPLAY
    ak_scenes_load(); /* FR-03: slot copies + active slot; live settings stay as loaded */
#endif
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
        ak_splash_report_region();
        ak_splash_show();
        ak_status_bar_set(ak_tr(AK_STR_ST_LCD_READY), AK_UI_ST_OK);
        ak_render_status_sync();
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
    ak_ghost_setting_load(); /* FR-14: after ak_settings_init, which defaults it to on */
#endif

#if AK_ENABLE_DISPLAY
    ak_status_bar_set(ak_tr(AK_STR_ST_CLOUD_STARTING), AK_UI_ST_INFO);
    ak_render_status_sync();
#endif

    /* Tuya IoT / cloud bring-up (stub unless AK_ASR_BOARD_CLOUD is set). MUST run
     * BEFORE ak_asr_init: ai_chat_init needs an initialized IoT / AI-service
     * context (tuya_iot_init + netmgr), or it dereferences a NULL handle and
     * hard-faults at boot (the black-screen crash). No-op on the v1/BLE profiles. */
    OPERATE_RET cloud_rt = ak_cloud_init();
#if AK_ENABLE_DISPLAY
    ak_status_bar_set(cloud_rt == OPRT_OK      ? ak_tr(AK_STR_ST_CLOUD_OK)
                      : ak_cloud_pid_missing() ? ak_tr(AK_STR_ST_PID_MISSING)
                                               : ak_tr(AK_STR_ST_CLOUD_FAILED),
                      cloud_rt == OPRT_OK ? AK_UI_ST_OK : (ak_cloud_pid_missing() ? AK_UI_ST_NOTE : AK_UI_ST_ERROR));
    ak_render_status_sync();
#endif
#if AK_TRANSPORT_DUAL
    /* The T5 exposes one global BLE callback set. While a pairing window is
     * open, leave BLE with Tuya for the phone app; UART0 remains available to
     * Companion. Otherwise start AK BLE, activated or not, so Companion can
     * choose BLE on a board that never saw the Tuya cloud. If Companion chooses
     * USB, its hello frame stops AK advertising and locks this boot to USB.
     * Wi-Fi/MQTT availability is deliberately not part of this decision. */
    OPERATE_RET ble_rt = OPRT_OK;
#if AK_ASR_BOARD_CLOUD
    if (ak_cloud_pid_missing()) {
        /* No product ID: netmgr and Tuya's BLE stack never started, so nobody
         * else owns the radio. Bring AK's own BLE up as in a non-cloud build
         * so the Companion works over BLE too (USB is up already). */
        ble_rt          = ak_ble_gatt_init();
        s_ak_ble_active = (ble_rt == OPRT_OK);
    } else if (cloud_rt != OPRT_OK || !ak_cloud_network_ready() || ak_cloud_boot_window()) {
        /* The cloud stack did not come up, or this boot opens the pairing
         * window: Tuya BLE (or nothing) owns the radio. Taking the callbacks
         * here would silently block the phone app. The runtime path below
         * takes over once the window closed (ak_provision_ble_takeover_allowed).
         * 0.3.63 took over at boot on an activated device even in a
         * pairing-window boot; up to 0.3.90 an unactivated board never took
         * BLE at all, so a new board was invisible to the Companion. */
        PR_NOTICE("dual: AK BLE deferred (activated %d, pairing window at boot %d); Tuya owns BLE, UART0 available",
                  (int)ak_cloud_activated(), (int)ak_cloud_boot_window());
    } else {
        ble_rt = ak_ble_gatt_takeover();
        s_ak_ble_active = (ble_rt == OPRT_OK);
        PR_NOTICE("dual: AK BLE started at boot (activated %d)", (int)ak_cloud_activated());
    }
#else
    ble_rt = ak_ble_gatt_init();
    s_ak_ble_active = (ble_rt == OPRT_OK);
#endif
    if (ble_rt != OPRT_OK) {
        PR_ERR("dual: delayed AK BLE setup failed: %d (UART remains available)", ble_rt);
        ak_transport_feedback(AK_STR_ST_BLE_INIT_FAILED, AK_UI_ST_NOTE);
        (void)ak_statuslcd_flush_wait(); /* it renders: finish the frame first */
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
        ak_status_bar_set(ak_tr(AK_STR_ST_ASR_STARTING), AK_UI_ST_INFO);
        ak_render_status_sync();
#endif
        OPERATE_RET asr_rt = ak_asr_init(on_asr_text, on_asr_error);
        (void)asr_rt;
#if AK_ENABLE_DISPLAY
        if (asr_rt == OPRT_OK) {
            s_settings.volume = ak_asr_get_volume();
        }
        ak_status_bar_set(asr_rt == OPRT_OK ? ak_tr(AK_STR_ST_ASR_OK) : ak_tr(AK_STR_ST_ASR_INIT_FAILED),
                          asr_rt == OPRT_OK ? AK_UI_ST_OK : AK_UI_ST_ERROR);
        ak_render_status_sync();
#endif
    }
    PR_NOTICE("Voice preference: %s (cloud backend %s)", ak_voice_source_str(s_voice_route.preferred),
              ak_asr_available() ? "ready" : "unavailable");

    /* Wi-Fi provisioning (netmgr in the cloud profile, stub otherwise). The PC
     * pushes credentials over a `wifi` frame; results return as wifi_status. */
#if AK_ENABLE_DISPLAY
    ak_status_bar_set(ak_tr(AK_STR_ST_WIFI_STARTING), AK_UI_ST_INFO);
    ak_render_status_sync();
#endif
    OPERATE_RET wifi_rt = ak_wifi_init(on_wifi_status);
    (void)wifi_rt;
    PR_NOTICE("Wi-Fi provisioning: %s", ak_wifi_available() ? "ready" : "stub/off");
#if AK_ENABLE_DISPLAY
    if (wifi_rt != OPRT_OK) {
        ak_status_bar_set(ak_tr(AK_STR_ST_WIFI_INIT_FAILED), AK_UI_ST_ERROR);
    } else {
        ak_status_bar_set_wifi(s_wifi_lcd_state);
    }
    ak_render_status_sync();
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
        (void)ak_statuslcd_flush_wait(); /* it renders: finish the frame first */
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
        ak_status_bar_set(mode == AK_VOICE_MODE_CHAT ? ak_tr(AK_STR_ST_BOOT_CHAT) : ak_tr(AK_STR_ST_BOOT_ASR),
                          AK_UI_ST_INFO);
        ak_render_status_sync();
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
#if AK_ENABLE_DISPLAY
            if (s_chat_degraded && ak_asr_available() && !ak_input_confirm() && ak_ui_state(&s_ui) == AK_UI_STANDBY &&
                s_voice_ui_state == AK_VOICE_UI_IDLE) {
                s_chat_degraded = false;
                ak_voice_mode_apply(AK_VOICE_MODE_CHAT, false); /* the cloud is back */
                ak_status_tmp_set(ak_tr(AK_STR_ST_CHAT_RESTORED), AK_UI_ST_OK, AK_MENU_NOTICE_MS);
                ak_render_status();
            }
#endif
        }
        ak_devinfo_poll(now);

#if AK_ENABLE_DISPLAY
        /* The activation state changed while Settings is open (Tuya pairing finished, or the
         * binding was cleared): list or hide "Unbind device" now. */
        if (s_settings_active && !s_test_active && ak_settings_sync_rebind()) {
            ak_settings_draw();
        }
        /* Reclaim a finished background flush without ever sleeping, so the
         * next redraw rarely has to wait and the loop keeps reading UART. */
        ak_statuslcd_flush_poll();
        ak_ghost_service(now); /* FR-14: no-op while every ghost rests */
        ak_voice_ui_tick(now);
        if (s_lcd_redraw_pending &&
            (int32_t)(s_lcd_audio_guard_until_ms - now) <= 0 &&
            !ak_asr_audio_busy()) {
            ak_render_status();
        }
        if (s_wifi_status_dirty) {
            s_wifi_status_dirty = false;
            ak_wifi_state_t wifi_state = s_wifi_lcd_state;
#if AK_TRANSPORT_HAS_BLE
            /* An open BLE pairing window keeps its "pick it on the PC" message;
             * the Wi-Fi state is shown again when the window closes. */
            if (ak_ble_pair_is_open(&s_ble_pair, now)) {
                s_wifi_status_held = true;
            } else
#endif
            {
                ak_status_bar_set_wifi(wifi_state);
            }
            ak_refresh_link_header();
        }
        if (s_activation_ui_pending) {
            char state[sizeof(s_activation_ui_state)];
            (void)snprintf(state, sizeof(state), "%s", s_activation_ui_state);
            s_activation_ui_pending = false;
            if (strcmp(state, "scan_qr") == 0) {
                ak_status_bar_set(ak_tr(AK_STR_ST_SCAN_QR), AK_UI_ST_INFO);
            } else if (strcmp(state, "token_received") == 0) {
                ak_status_bar_set(ak_tr(AK_STR_ST_ACTIVATING), AK_UI_ST_INFO);
            } else if (strcmp(state, "activated") == 0) {
                ak_status_bar_set(ak_tr(AK_STR_ST_ACTIVATED), AK_UI_ST_OK);
            } else if (strcmp(state, "cloud_ready") == 0) {
                ak_status_bar_set(ak_tr(AK_STR_ST_CLOUD_READY), AK_UI_ST_OK);
            } else if (strcmp(state, "cloud_disconnected") == 0) {
                ak_status_bar_set(ak_tr(AK_STR_ST_CLOUD_DISCONNECTED), AK_UI_ST_NOTE);
            } else if (strcmp(state, "resetting") == 0) {
                ak_status_bar_set(ak_tr(AK_STR_ST_RESETTING), AK_UI_ST_NOTE);
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
                ak_transport_feedback(provisioning ? AK_STR_ST_PAIRING_TUYA_APP : AK_STR_ST_PROVISIONED,
                                      provisioning ? AK_UI_ST_NOTE : AK_UI_ST_OK);
                send_system_info_uart();
            }
        }

#if AK_TRANSPORT_DUAL && AK_ASR_BOARD_CLOUD
        if (!s_ak_ble_active && !s_usb_selected && ak_cloud_network_ready() &&
            ak_provision_ble_takeover_allowed(&s_prov, ak_cloud_activated(), ak_cloud_provisioning()) &&
            (s_ak_ble_retry_at_ms == 0U ||
             (int32_t)(now - s_ak_ble_retry_at_ms) >= 0)) {
            OPERATE_RET takeover_rt = ak_ble_gatt_takeover();
            if (takeover_rt == OPRT_OK) {
                s_ak_ble_active = true;
                PR_NOTICE("dual: pairing window closed; AK BLE available for Companion");
            } else {
                s_ak_ble_retry_at_ms = now + 1000U;
                PR_WARN("dual: AK BLE takeover after the pairing window failed: %d; retrying in 1s",
                        takeover_rt);
            }
        }
        /* Before activation, Tuya BLE may advertise only inside a pairing window.
         * Its stack starts at boot and its advertising is not gated by the SDK,
         * so stop it whenever the window is closed (checked every loop pass, a
         * cheap host-state read; shortcut: upgrade to an SDK gate if one exists). */
        if (!s_ak_ble_active && !ak_cloud_activated() && !ak_provision_is_open(&s_prov, now)) {
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
                static bool                s_last_authed;
                ak_ble_link_state_t        link   = ak_ble_gatt_link_state();
                bool                       authed = ak_ble_companion_authed();
                if (link != s_last_link || authed != s_last_authed) {
                    ak_ble_link_state_t previous = s_last_link;
                    s_last_link   = link;
                    s_last_authed = authed;
                    switch (link) {
                    case AK_BLE_LINK_LIVE:
                        /* Subscribed but not yet authenticated: could be the Tuya
                         * phone app on the shared service, not the Companion. */
                        ak_transport_feedback(authed ? AK_STR_ST_BLE_CONNECTED : AK_STR_ST_BLE_AUTH_WAIT,
                                              authed ? AK_UI_ST_OK : AK_UI_ST_INFO);
                        break;
                    case AK_BLE_LINK_CONNECTED:
                        ak_transport_feedback(AK_STR_ST_BLE_SUBSCRIBING, AK_UI_ST_INFO);
                        break;
                    case AK_BLE_LINK_ADVERTISING:
                    default:
                        ak_transport_feedback(previous == (ak_ble_link_state_t)-1 ? AK_STR_ST_BLE_ADVERTISING
                                                                                  : AK_STR_ST_BLE_DISCONNECTED,
                                              AK_UI_ST_NOTE);
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
        ak_wifi_poll(now);
        ak_freset_tick(now);
        if (ak_combo_tick(&s_combo, now)) {
            ak_recovery_combo_fired(now);
        }
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
#if AK_TRANSPORT_HAS_BLE
        /* Over-the-air BLE pairing: time-outs, and the window countdown on
         * the settings row (repainted alone, once a second). */
        {
            static uint32_t s_ble_pair_shown_s;
            bool            was_open = ak_ble_pair_is_open(&s_ble_pair, now);
            ble_pair_apply(ak_ble_pair_tick(&s_ble_pair, now, s_ble_pair_reply, sizeof(s_ble_pair_reply)));
            uint32_t left = ak_ble_pair_remaining_s(&s_ble_pair, now);
            if (was_open && !ak_ble_pair_is_open(&s_ble_pair, now)) {
                PR_NOTICE("ble pair: window closed");
                s_ble_pair_shown_s = 0U;
                if (s_wifi_status_held) { /* a Wi-Fi change waited for the window */
                    s_wifi_status_held  = false;
                    s_wifi_status_dirty = true;
                }
                if (s_settings_active) {
                    ak_settings_draw();
                }
            } else if (left != s_ble_pair_shown_s) {
                s_ble_pair_shown_s = left;
                if (s_settings_active && s_ble_pair.state != AK_BLE_PAIR_COMPARE && ak_statuslcd_ready() &&
                    ak_lcd_draw_begin()) {
                    ak_settings_draw_ble_pair_row(now);
                    ak_statuslcd_flush();
                }
            }
        }
#endif

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
                ble_pair_apply(ak_ble_pair_link_down(&s_ble_pair));
            }
            int got_ble = ak_transport_read_ble(rx, sizeof(rx));
            if (ak_ble_gatt_take_link_reset()) {
                ak_rx_reader_reset(&s_rx_ble);
                ble_sec_drop(); /* the central changed under us */
                ble_pair_apply(ak_ble_pair_link_down(&s_ble_pair));
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
            s_pc_asr_results = false;
            PR_NOTICE("USB companion timed out; marking link disconnected");
            ak_transport_feedback(AK_STR_ST_USB_TIMEOUT, AK_UI_ST_NOTE);
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
