/**
 * @file ak_protocol.h
 * @brief JSONL protocol v1 frame builders (shared/protocol/protocol.md).
 *
 * Pure, bounded JSON construction (host-testable): each function JSON-escapes
 * every dynamic string, writes one newline-terminated JSON line into buf, and
 * returns the complete required length (>= size means truncated).
 */

#ifndef AK_PROTOCOL_H
#define AK_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ak_devinfo.h"
#include "ak_scene.h"
/* Board -> PC events. */
/* ── Frame buffers (board -> PC) ─────────────────────────────────────────────
 * transport_write_frame_unlocked() drops a frame whose required length is not
 * below the buffer size, so every sender sizes its buffer for the worst case.
 * AK_FRAME_BUFFER_SIZE holds the small fixed frames (key, encoder, voice, ...);
 * AK_FRAME_BUFFER_LARGE the ones that carry a user string (SSID, status text).
 * hello_ack grows with every capability, so its size is computed from the very
 * strings the builder writes (AK_HELLO_ACK_BUFFER_SIZE): adding a capability
 * here resizes the buffer. test_frame_budgets checks all of them at their
 * maximum field values. */
#define AK_FRAME_BUFFER_SIZE  192U
#define AK_FRAME_BUFFER_LARGE 384U

#define AK_HELLO_ACK_HEAD       "{\"v\":1,\"type\":\"hello_ack\",\"payload\":{\"fw\":"
#define AK_HELLO_ACK_TRANSPORT  ",\"transport\":"
#define AK_HELLO_ACK_CAPS_OPEN  ",\"capabilities\":["
#define AK_HELLO_CAPS_BASE      ",\"keymap\",\"voice_state\",\"device_settings\""
#define AK_HELLO_CAP_SYSTEM     ",\"system_info\""
#define AK_HELLO_CAPS_MENU      ",\"menu\",\"pc_asr_wait\",\"bind_session\""
#define AK_HELLO_CAP_DICTATION  ""
#define AK_HELLO_CAP_SCENES     ",\"scenes\""
#define AK_HELLO_ACK_TAIL       "]}}\n"

/* `fw` and `transport` are string literals (sizeof gives their length). The
 * transport name appears twice; the result counts every optional capability, plus the terminating NUL. */
#define AK_HELLO_ACK_BUFFER_SIZE(fw, transport)                                                          \
    (sizeof(AK_HELLO_ACK_HEAD) - 1U + (sizeof(fw) - 1U) + 2U + sizeof(AK_HELLO_ACK_TRANSPORT) - 1U +    \
     (sizeof(transport) - 1U) + 2U + sizeof(AK_HELLO_ACK_CAPS_OPEN) - 1U + (sizeof(transport) - 1U) +   \
     2U + sizeof(AK_HELLO_CAPS_BASE) - 1U + sizeof(AK_HELLO_CAP_SYSTEM) - 1U +                          \
     sizeof(AK_HELLO_CAPS_MENU) - 1U + sizeof(AK_HELLO_CAP_DICTATION) - 1U +                            \
     sizeof(AK_HELLO_CAP_SCENES) - 1U + sizeof(AK_HELLO_ACK_TAIL) - 1U + 1U)

/* Board -> PC events. */
int ak_proto_key(char *buf, size_t size, uint32_t seq, const char *id, const char *action, uint32_t held_ms);
/* Key gesture frame: action = "single"/"double"/"long"; bind (resolved action
   name) is omitted when NULL. held_ms is always 0 for gestures. */
int ak_proto_key_gesture(char *buf, size_t size, uint32_t seq, const char *id, const char *gesture,
                         const char *bind);
/** Same, plus `text` (the ③ input-confirm text the Companion sends on a
 *  `confirm`); NULL omits the field. */
int ak_proto_key_gesture_text(char *buf, size_t size, uint32_t seq, const char *id, const char *gesture,
                              const char *bind, const char *text, uint8_t slot);
/** Same as ak_proto_key_gesture, but with the real press time (`held_ms`) and
 *  the wait between the deciding release and this report (`wait_ms`, 0 when it
 *  was reported at once). Companion [KEY-TIMING] logs both. */
int ak_proto_key_gesture_timed(char *buf, size_t size, uint32_t seq, const char *id, const char *gesture,
                               const char *bind, uint32_t held_ms, uint32_t wait_ms);
/* Board -> PC: EC11 rotary encoder step. `detents` is a signed step count
   (positive = clockwise, negative = counter-clockwise). Replaces ak_proto_nav. */
int ak_proto_encoder(char *buf, size_t size, uint32_t seq, int32_t detents);
int ak_proto_voice(char *buf, size_t size, uint32_t seq, const char *state, uint8_t slot);

/* Board -> PC: Settings > Bind session picked slot `slot` (1..3); the
   Companion answers with an instance / session menu or bind_done. */
int ak_proto_bind_session(char *buf, size_t size, uint32_t seq, uint8_t slot);
/* Board -> PC: recognized speech text from the board-side (Tuya cloud) ASR.
   The builder JSON-escapes arbitrary text, including quotes and newlines. */
int ak_proto_asr(char *buf, size_t size, uint32_t seq, const char *text, uint8_t slot);
int ak_proto_asr_error(char *buf, size_t size, uint32_t seq, const char *code);
/* Board -> PC: report the board's CURRENT voice routing ("cloud" = board mic +
   Tuya cloud ASR, "system" = PC's own ASR). Sent on change and on hello, so the
   companion UI reflects a board-side switch. */
/* voice_source: effective route (cloud/system) plus the voice mode (asr/chat). */
int ak_proto_voice_source(char *buf, size_t size, uint32_t seq, const char *source, const char *mode);

/* tuya_chat (board -> PC, UART0 only): one Chat-mode turn, role "user" or
 * "agent". Display-only: the Companion must never inject it (CP-01). */
int ak_proto_tuya_chat(char *buf, size_t size, uint32_t seq, const char *role, const char *text, bool final);
/* Board -> PC: report Wi-Fi provisioning/connection status. `state` is one of
   "connecting" / "connected" / "failed" / "disconnected"; `ssid` is the target
   AP (may be empty). Sent when a wifi config frame is applied and on change. */
int ak_proto_wifi_status(char *buf, size_t size, uint32_t seq, const char *state, const char *ssid);
/* wifi_list_state (board -> PC, FR-15): the remembered networks, most recently
   connected first. SSID, security name and the "current" flag only; a
   password has no field here and never leaves the board. */
typedef struct {
    const char *ssid;
    uint8_t     sec; /* AK_WIFI_SEC_UNKNOWN or a scan security value */
    bool        current;
} ak_proto_wifi_entry_t;
int ak_proto_wifi_list(char *buf, size_t size, uint32_t seq, const ak_proto_wifi_entry_t *entries, size_t n,
                       uint32_t max);
/* Board -> Companion: Tuya activation lifecycle. `qr_url` is populated only
   for `scan_qr`; it contains PID+UUID but never AuthKey. UART0-only. */
int ak_proto_activation_status(char *buf, size_t size, uint32_t seq,
                               const char *state, const char *qr_url);
/* Board -> PC: report device-license (UUID/AuthKey) provisioning result.
   `state` is "written" / "failed" / "present" (already had one). No secret is
   ever echoed back. */
/* prov_status (board -> PC, UART0 only): pairing window state. `rejected` /
 * `reason` are included only when non-empty (a refused provisioning command). */
int ak_proto_prov_status(char *buf, size_t size, uint32_t seq, const char *state, uint32_t remaining_s,
                         const char *rejected, const char *reason);
/* ble_pair_key (board -> PC, UART0 only): the new BLE pairing key, sent once
 * right after it is created inside the pairing window. */
int ak_proto_ble_pair_key(char *buf, size_t size, uint32_t seq, const char *key_hex, const char *key_id);
int ak_proto_license_status(char *buf, size_t size, uint32_t seq, const char *state);
/* Board -> UART0: explicit development diagnostic. UUID is complete, while
   authkey_tail contains at most four characters and never the full secret. */
int ak_proto_license_info(char *buf, size_t size, uint32_t seq, const char *source,
                          const char *uuid, const char *authkey_tail);
/* Board -> UART0: consolidated system feedback for diagnostics and Companion logs. */
int ak_proto_system_info(char *buf, size_t size, uint32_t seq,
                         const char *fw, const char *transport,
                         const char *wifi_name, const char *wifi_state,
                         const char *ble_name, const char *ble_state,
                         const char *cloud_state, const char *license_state);
/* Board -> PC: report the settings currently applied by the AK (FR-02).
 * led_power is "off" / "on" / "auto"; led_auto_off_s the Auto power-off delay;
 * language "zh" / "en" (ak_lang_wire_str); voice_mode "asr" / "chat";
 * font_size the LCD font size in px (12 / 14 / 16, ak_text_size_px); animation the
 * chat ghost animation setting (FR-14). The
 * last two are status only: whether a License is stored and whether a BLE
 * pairing key is set (never the values). */
typedef struct {
    uint8_t     volume;
    uint8_t     brightness;
    const char *led_power;
    uint32_t    led_auto_off_s;
    const char *language;
    const char *voice_mode;
    uint8_t     font_size;
    bool        animation;
    bool        license_present;
    const char *pid;        /* "present" / "pending" (restart to apply) / "missing" */
    bool        ble_paired;
} ak_proto_settings_t;

int ak_proto_device_settings(char *buf, size_t size, uint32_t seq, const ak_proto_settings_t *s);

/**
 * Board -> PC `device_info` (read-only, no secrets): the same snapshot as
 * Settings > Device info. Sent after hello_ack and whenever it changes, on USB
 * and on the authenticated BLE channel. Worst case (an SSID of control
 * characters) stays below AK_FRAME_BUFFER_DEVINFO, the BLE limit of 511 bytes.
 */
#define AK_FRAME_BUFFER_DEVINFO 512U
int ak_proto_device_info(char *buf, size_t size, uint32_t seq, const ak_devinfo_t *d);

/* pid_status (board -> PC, UART0 only): result of a `pid` write. `state` is
 * "written_reboot_required" or "rejected"; `reason` ("source" / "exists" /
 * "invalid" / "storage") is included only when non-empty. The PID value is
 * never echoed. */
int ak_proto_pid_status(char *buf, size_t size, uint32_t seq, const char *state, const char *reason);

/* Handshake / heartbeat. */
int ak_proto_hello_ack(char *buf, size_t size, const char *fw, const char *transport);
/* Same, plus the "menu" capability when `menu` is true (firmware that shows
 * LCD menus; shared/protocol/protocol.md "LCD 菜单与按键灯光"). */
int ak_proto_hello_ack_caps(char *buf, size_t size, const char *fw, const char *transport, bool menu);

/* Board -> PC: menu answer. `selected` are global item indexes, ascending
 * (n may be 0 for an empty multi-select). `text` = the dictated answer for
 * the voice item; omitted when NULL. */
int ak_proto_menu_result(char *buf, size_t size, uint32_t seq, const char *id, const uint8_t *selected, size_t n,
                         const char *text);
/* Board -> PC: menu closed without a decision; reason "esc" / "timeout". */
int ak_proto_menu_dismissed(char *buf, size_t size, uint32_t seq, const char *id, const char *reason);
int ak_proto_ping(char *buf, size_t size, uint32_t seq, uint32_t uptime_ms);
int ak_proto_pong(char *buf, size_t size);
int ak_proto_ack(char *buf, size_t size, uint32_t seq);

/* Generic probe feedback: a lifecycle state line and a free-form log line.
   Used by bring-up / feature-probe firmware to report on UART0 (COMx). */
int ak_proto_status(char *buf, size_t size, const char *probe, const char *state);
int ak_proto_log(char *buf, size_t size, uint32_t seq, const char *tag, const char *msg);

/* FR-03 scenes. `slots[i]` NULL = empty slot i; `active` -1 = none.
 * `error` / `req` are included only when non-NULL. */
int ak_proto_scene_state(char *buf, size_t size, uint32_t seq, int active, bool dirty,
                         const ak_scene_t *const slots[], size_t n_slots, const char *error, const char *req);
int ak_proto_scene_data(char *buf, size_t size, uint32_t seq, uint8_t slot, const ak_scene_t *scene);

#endif /* AK_PROTOCOL_H */
