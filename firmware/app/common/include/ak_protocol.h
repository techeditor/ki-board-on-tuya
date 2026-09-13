/**
 * @file ak_protocol.h
 * @brief JSONL protocol v1 frame builders (shared/protocol/protocol.md).
 *
 * Pure string formatting (host-testable): each function writes one
 * newline-terminated JSON line into buf and returns the snprintf length
 * (>= size means truncated). No SDK or transport dependency.
 */

#ifndef AK_PROTOCOL_H
#define AK_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/* Board -> PC events. */
int ak_proto_key(char *buf, size_t size, uint32_t seq, const char *id, const char *action, uint32_t held_ms);
int ak_proto_nav(char *buf, size_t size, uint32_t seq, const char *dir, const char *action, uint32_t held_ms);
int ak_proto_voice(char *buf, size_t size, uint32_t seq, const char *state);

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
