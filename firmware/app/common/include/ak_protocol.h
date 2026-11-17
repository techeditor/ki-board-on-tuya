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

/* Board -> PC events. */
int ak_proto_key(char *buf, size_t size, uint32_t seq, const char *id, const char *action, uint32_t held_ms);
/* Key gesture frame: action = "single"/"double"/"long"; bind (resolved action
   name) is omitted when NULL. held_ms is always 0 for gestures. */
int ak_proto_key_gesture(char *buf, size_t size, uint32_t seq, const char *id, const char *gesture,
                         const char *bind);
/** Same, plus `text` (the ③ input-confirm text the Companion sends on a
 *  `confirm`); NULL omits the field. */
int ak_proto_key_gesture_text(char *buf, size_t size, uint32_t seq, const char *id, const char *gesture,
                              const char *bind, const char *text);
/* Board -> PC: EC11 rotary encoder step. `detents` is a signed step count
   (positive = clockwise, negative = counter-clockwise). Replaces ak_proto_nav. */
int ak_proto_encoder(char *buf, size_t size, uint32_t seq, int32_t detents);
int ak_proto_voice(char *buf, size_t size, uint32_t seq, const char *state);
/* Board -> PC: recognized speech text from the board-side (Tuya cloud) ASR.
   The builder JSON-escapes arbitrary text, including quotes and newlines. */
int ak_proto_asr(char *buf, size_t size, uint32_t seq, const char *text);
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
/* Board -> PC: report the runtime values currently applied by the AK.
 * led_power is "off" / "on" / "auto"; led_auto_off_s the Auto power-off delay;
 * language the interface language "zh" / "en" (ak_lang_wire_str). */
int ak_proto_device_settings(char *buf, size_t size, uint32_t seq, uint8_t volume, uint8_t brightness,
                             const char *led_power, uint32_t led_auto_off_s, const char *language);

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
   Used by bring-up / feature-probe firmware to report on UART0 (COM6). */
int ak_proto_status(char *buf, size_t size, const char *probe, const char *state);
int ak_proto_log(char *buf, size_t size, uint32_t seq, const char *tag, const char *msg);

#endif /* AK_PROTOCOL_H */
