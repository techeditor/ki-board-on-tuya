/**
 * @file ak_protocol.c
 * @brief JSONL protocol v1 frame builders (host-testable, no SDK deps).
 */

#include "ak_protocol.h"

#include <stdio.h>

int ak_proto_key(char *buf, size_t size, uint32_t seq, const char *id, const char *action, uint32_t held_ms)
{
    return snprintf(buf, size,
                    "{\"v\":1,\"seq\":%u,\"type\":\"key\",\"payload\":{\"id\":\"%s\","
                    "\"action\":\"%s\",\"held_ms\":%u}}\n",
                    (unsigned)seq, id, action, (unsigned)held_ms);
}

int ak_proto_nav(char *buf, size_t size, uint32_t seq, const char *dir, const char *action, uint32_t held_ms)
{
    return snprintf(buf, size,
                    "{\"v\":1,\"seq\":%u,\"type\":\"nav\",\"payload\":{\"dir\":\"%s\","
                    "\"action\":\"%s\",\"held_ms\":%u}}\n",
                    (unsigned)seq, dir, action, (unsigned)held_ms);
}

int ak_proto_voice(char *buf, size_t size, uint32_t seq, const char *state)
{
    return snprintf(buf, size, "{\"v\":1,\"seq\":%u,\"type\":\"voice\",\"payload\":{\"state\":\"%s\"}}\n",
                    (unsigned)seq, state);
}

int ak_proto_hello_ack(char *buf, size_t size, const char *fw, const char *transport)
{
    return snprintf(buf, size,
                    "{\"v\":1,\"type\":\"hello_ack\",\"payload\":{\"fw\":\"%s\","
                    "\"transport\":\"%s\",\"capabilities\":[\"%s\",\"keymap\",\"voice_state\"]}}\n",
                    fw, transport, transport);
}

int ak_proto_ping(char *buf, size_t size, uint32_t seq, uint32_t uptime_ms)
{
    return snprintf(buf, size, "{\"v\":1,\"seq\":%u,\"type\":\"ping\",\"payload\":{\"uptime_ms\":%u}}\n", (unsigned)seq,
                    (unsigned)uptime_ms);
}

int ak_proto_pong(char *buf, size_t size)
{
    return snprintf(buf, size, "{\"v\":1,\"type\":\"pong\"}\n");
}

int ak_proto_ack(char *buf, size_t size, uint32_t seq)
{
    return snprintf(buf, size, "{\"v\":1,\"type\":\"ack\",\"payload\":{\"seq\":%u}}\n", (unsigned)seq);
}
