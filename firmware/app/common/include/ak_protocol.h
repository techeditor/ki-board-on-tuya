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

#include <stddef.h>
#include <stdint.h>

/* Board -> PC events. */
int ak_proto_key(char *buf, size_t size, uint32_t seq, const char *id, const char *action, uint32_t held_ms);
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
int ak_proto_voice_source(char *buf, size_t size, uint32_t seq, const char *source);
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
/* Board -> PC: report the runtime values currently applied by the AK. */
int ak_proto_device_settings(char *buf, size_t size, uint32_t seq, uint8_t volume, uint8_t brightness);

/* Handshake / heartbeat. */
int ak_proto_hello_ack(char *buf, size_t size, const char *fw, const char *transport);
int ak_proto_ping(char *buf, size_t size, uint32_t seq, uint32_t uptime_ms);
int ak_proto_pong(char *buf, size_t size);
int ak_proto_ack(char *buf, size_t size, uint32_t seq);

/* Generic probe feedback: a lifecycle state line and a free-form log line.
   Used by bring-up / feature-probe firmware to report on UART0 (COM6). */
int ak_proto_status(char *buf, size_t size, const char *probe, const char *state);
int ak_proto_log(char *buf, size_t size, uint32_t seq, const char *tag, const char *msg);

#endif /* AK_PROTOCOL_H */
