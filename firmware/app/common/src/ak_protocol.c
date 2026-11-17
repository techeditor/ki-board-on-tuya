/**
 * @file ak_protocol.c
 * @brief JSONL protocol v1 frame builders (host-testable, no SDK deps).
 */

#include "ak_protocol.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char  *buf;
    size_t size;
    size_t used;
    size_t required;
} ak_json_writer_t;

static void writer_init(ak_json_writer_t *writer, char *buf, size_t size)
{
    writer->buf      = buf;
    writer->size     = size;
    writer->used     = 0U;
    writer->required = 0U;
    if (size > 0U) {
        buf[0] = '\0';
    }
}

static void writer_char(ak_json_writer_t *writer, char value)
{
    if (writer->size > 0U && writer->used + 1U < writer->size) {
        writer->buf[writer->used++] = value;
    }
    writer->required++;
}

static void writer_raw(ak_json_writer_t *writer, const char *value)
{
    if (value == NULL) {
        return;
    }
    while (*value != '\0') {
        writer_char(writer, *value++);
    }
}

static void writer_u32(ak_json_writer_t *writer, uint32_t value)
{
    char number[11];
    (void)snprintf(number, sizeof(number), "%u", (unsigned)value);
    writer_raw(writer, number);
}

static void writer_i32(ak_json_writer_t *writer, int32_t value)
{
    char number[12];
    (void)snprintf(number, sizeof(number), "%d", (int)value);
    writer_raw(writer, number);
}

static void writer_json_string(ak_json_writer_t *writer, const char *value)
{
    static const char hex[] = "0123456789abcdef";

    writer_char(writer, '"');
    if (value != NULL) {
        while (*value != '\0') {
            unsigned char c = (unsigned char)*value++;
            switch (c) {
            case '"':
                writer_raw(writer, "\\\"");
                break;
            case '\\':
                writer_raw(writer, "\\\\");
                break;
            case '\b':
                writer_raw(writer, "\\b");
                break;
            case '\f':
                writer_raw(writer, "\\f");
                break;
            case '\n':
                writer_raw(writer, "\\n");
                break;
            case '\r':
                writer_raw(writer, "\\r");
                break;
            case '\t':
                writer_raw(writer, "\\t");
                break;
            default:
                if (c < 0x20U) {
                    writer_raw(writer, "\\u00");
                    writer_char(writer, hex[c >> 4]);
                    writer_char(writer, hex[c & 0x0FU]);
                } else {
                    /* UTF-8 bytes pass through unchanged; JSON control bytes
                     * and syntax-significant ASCII are escaped above. */
                    writer_char(writer, (char)c);
                }
                break;
            }
        }
    }
    writer_char(writer, '"');
}

static int writer_finish(ak_json_writer_t *writer)
{
    if (writer->size > 0U) {
        writer->buf[writer->used] = '\0';
    }
    return writer->required > (size_t)INT_MAX ? INT_MAX : (int)writer->required;
}

int ak_proto_key(char *buf, size_t size, uint32_t seq, const char *id, const char *action, uint32_t held_ms)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"seq\":");
    writer_u32(&writer, seq);
    writer_raw(&writer, ",\"type\":\"key\",\"payload\":{\"id\":");
    writer_json_string(&writer, id);
    writer_raw(&writer, ",\"action\":");
    writer_json_string(&writer, action);
    writer_raw(&writer, ",\"held_ms\":");
    writer_u32(&writer, held_ms);
    writer_raw(&writer, "}}\n");
    return writer_finish(&writer);
}

int ak_proto_key_gesture(char *buf, size_t size, uint32_t seq, const char *id, const char *gesture,
                         const char *bind)
{
    return ak_proto_key_gesture_text(buf, size, seq, id, gesture, bind, NULL);
}

int ak_proto_key_gesture_text(char *buf, size_t size, uint32_t seq, const char *id, const char *gesture,
                              const char *bind, const char *text)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"seq\":");
    writer_u32(&writer, seq);
    writer_raw(&writer, ",\"type\":\"key\",\"payload\":{\"id\":");
    writer_json_string(&writer, id);
    writer_raw(&writer, ",\"action\":");
    writer_json_string(&writer, gesture);
    if (bind != NULL) {
        writer_raw(&writer, ",\"bind\":");
        writer_json_string(&writer, bind);
    }
    if (text != NULL) {
        writer_raw(&writer, ",\"text\":");
        writer_json_string(&writer, text);
    }
    writer_raw(&writer, ",\"held_ms\":0}}\n");
    return writer_finish(&writer);
}

int ak_proto_encoder(char *buf, size_t size, uint32_t seq, int32_t detents)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"seq\":");
    writer_u32(&writer, seq);
    writer_raw(&writer, ",\"type\":\"encoder\",\"payload\":{\"detents\":");
    writer_i32(&writer, detents);
    writer_raw(&writer, "}}\n");
    return writer_finish(&writer);
}

int ak_proto_voice(char *buf, size_t size, uint32_t seq, const char *state)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"seq\":");
    writer_u32(&writer, seq);
    writer_raw(&writer, ",\"type\":\"voice\",\"payload\":{\"state\":");
    writer_json_string(&writer, state);
    writer_raw(&writer, "}}\n");
    return writer_finish(&writer);
}

int ak_proto_bind_session(char *buf, size_t size, uint32_t seq, uint8_t slot)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"seq\":");
    writer_u32(&writer, seq);
    writer_raw(&writer, ",\"type\":\"bind_session\",\"payload\":{\"slot\":");
    writer_u32(&writer, slot);
    writer_raw(&writer, "}}\n");
    return writer_finish(&writer);
}

int ak_proto_asr(char *buf, size_t size, uint32_t seq, const char *text)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"seq\":");
    writer_u32(&writer, seq);
    writer_raw(&writer, ",\"type\":\"asr\",\"payload\":{\"text\":");
    writer_json_string(&writer, text);
    writer_raw(&writer, "}}\n");
    return writer_finish(&writer);
}

int ak_proto_asr_error(char *buf, size_t size, uint32_t seq, const char *code)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"seq\":");
    writer_u32(&writer, seq);
    writer_raw(&writer, ",\"type\":\"asr\",\"payload\":{\"error\":");
    writer_json_string(&writer, code);
    writer_raw(&writer, "}}\n");
    return writer_finish(&writer);
}

int ak_proto_voice_source(char *buf, size_t size, uint32_t seq, const char *source, const char *mode)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"seq\":");
    writer_u32(&writer, seq);
    writer_raw(&writer, ",\"type\":\"voice_source\",\"payload\":{\"source\":");
    writer_json_string(&writer, source);
    writer_raw(&writer, ",\"mode\":");
    writer_json_string(&writer, mode);
    writer_raw(&writer, "}}\n");
    return writer_finish(&writer);
}

int ak_proto_tuya_chat(char *buf, size_t size, uint32_t seq, const char *role, const char *text, bool final)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"seq\":");
    writer_u32(&writer, seq);
    writer_raw(&writer, ",\"type\":\"tuya_chat\",\"payload\":{\"role\":");
    writer_json_string(&writer, role);
    writer_raw(&writer, ",\"text\":");
    writer_json_string(&writer, text);
    writer_raw(&writer, final ? ",\"final\":true}}\n" : ",\"final\":false}}\n");
    return writer_finish(&writer);
}

int ak_proto_wifi_status(char *buf, size_t size, uint32_t seq, const char *state, const char *ssid)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"seq\":");
    writer_u32(&writer, seq);
    writer_raw(&writer, ",\"type\":\"wifi_status\",\"payload\":{\"state\":");
    writer_json_string(&writer, state);
    writer_raw(&writer, ",\"ssid\":");
    writer_json_string(&writer, ssid);
    writer_raw(&writer, "}}\n");
    return writer_finish(&writer);
}

static const char *wifi_sec_name(uint8_t sec)
{
    static const char *const names[] = {"open", "wep", "wpa", "wpa2", "wpa_wpa2", "wpa3"};
    return sec < sizeof(names) / sizeof(names[0]) ? names[sec] : "unknown";
}

int ak_proto_wifi_list(char *buf, size_t size, uint32_t seq, const ak_proto_wifi_entry_t *entries, size_t n,
                       uint32_t max)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"seq\":");
    writer_u32(&writer, seq);
    writer_raw(&writer, ",\"type\":\"wifi_list_state\",\"payload\":{\"max\":");
    writer_u32(&writer, max);
    writer_raw(&writer, ",\"entries\":[");
    for (size_t i = 0U; entries != NULL && i < n; i++) {
        if (i > 0U) {
            writer_char(&writer, ',');
        }
        writer_raw(&writer, "{\"ssid\":");
        writer_json_string(&writer, entries[i].ssid);
        writer_raw(&writer, ",\"sec\":");
        writer_json_string(&writer, wifi_sec_name(entries[i].sec));
        writer_raw(&writer, ",\"current\":");
        writer_raw(&writer, entries[i].current ? "true" : "false");
        writer_char(&writer, '}');
    }
    writer_raw(&writer, "]}}\n");
    return writer_finish(&writer);
}

int ak_proto_activation_status(char *buf, size_t size, uint32_t seq,
                               const char *state, const char *qr_url)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"seq\":");
    writer_u32(&writer, seq);
    writer_raw(&writer, ",\"type\":\"activation_status\",\"payload\":{\"state\":");
    writer_json_string(&writer, state);
    writer_raw(&writer, ",\"qr_url\":");
    writer_json_string(&writer, qr_url);
    writer_raw(&writer, "}}\n");
    return writer_finish(&writer);
}

int ak_proto_license_status(char *buf, size_t size, uint32_t seq, const char *state)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"seq\":");
    writer_u32(&writer, seq);
    writer_raw(&writer, ",\"type\":\"license_status\",\"payload\":{\"state\":");
    writer_json_string(&writer, state);
    writer_raw(&writer, "}}\n");
    return writer_finish(&writer);
}

int ak_proto_prov_status(char *buf, size_t size, uint32_t seq, const char *state, uint32_t remaining_s,
                         const char *rejected, const char *reason)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"seq\":");
    writer_u32(&writer, seq);
    writer_raw(&writer, ",\"type\":\"prov_status\",\"payload\":{\"state\":");
    writer_json_string(&writer, state);
    writer_raw(&writer, ",\"remaining_s\":");
    writer_u32(&writer, remaining_s);
    if (rejected != NULL && rejected[0] != '\0') {
        writer_raw(&writer, ",\"rejected\":");
        writer_json_string(&writer, rejected);
        writer_raw(&writer, ",\"reason\":");
        writer_json_string(&writer, reason == NULL ? "" : reason);
    }
    writer_raw(&writer, "}}\n");
    return writer_finish(&writer);
}

int ak_proto_ble_pair_key(char *buf, size_t size, uint32_t seq, const char *key_hex, const char *key_id)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"seq\":");
    writer_u32(&writer, seq);
    writer_raw(&writer, ",\"type\":\"ble_pair_key\",\"payload\":{\"key\":");
    writer_json_string(&writer, key_hex);
    writer_raw(&writer, ",\"key_id\":");
    writer_json_string(&writer, key_id);
    writer_raw(&writer, "}}\n");
    return writer_finish(&writer);
}

int ak_proto_license_info(char *buf, size_t size, uint32_t seq, const char *source,
                          const char *uuid, const char *authkey_tail)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"seq\":");
    writer_u32(&writer, seq);
    writer_raw(&writer, ",\"type\":\"license_info\",\"payload\":{\"source\":");
    writer_json_string(&writer, source);
    writer_raw(&writer, ",\"uuid\":");
    writer_json_string(&writer, uuid);
    writer_raw(&writer, ",\"authkey_tail\":");
    writer_json_string(&writer, authkey_tail);
    writer_raw(&writer, "}}\n");
    return writer_finish(&writer);
}

int ak_proto_system_info(char *buf, size_t size, uint32_t seq,
                         const char *fw, const char *transport,
                         const char *wifi_name, const char *wifi_state,
                         const char *ble_name, const char *ble_state,
                         const char *cloud_state, const char *license_state)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"seq\":");
    writer_u32(&writer, seq);
    writer_raw(&writer, ",\"type\":\"system_info\",\"payload\":{\"fw\":");
    writer_json_string(&writer, fw);
    writer_raw(&writer, ",\"transport\":");
    writer_json_string(&writer, transport);
    writer_raw(&writer, ",\"wifi_name\":");
    writer_json_string(&writer, wifi_name);
    writer_raw(&writer, ",\"wifi_state\":");
    writer_json_string(&writer, wifi_state);
    writer_raw(&writer, ",\"ble_name\":");
    writer_json_string(&writer, ble_name);
    writer_raw(&writer, ",\"ble_state\":");
    writer_json_string(&writer, ble_state);
    writer_raw(&writer, ",\"cloud\":");
    writer_json_string(&writer, cloud_state);
    writer_raw(&writer, ",\"license\":");
    writer_json_string(&writer, license_state);
    writer_raw(&writer, "}}\n");
    return writer_finish(&writer);
}

int ak_proto_device_settings(char *buf, size_t size, uint32_t seq, const ak_proto_settings_t *s)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"seq\":");
    writer_u32(&writer, seq);
    writer_raw(&writer, ",\"type\":\"device_settings_state\",\"payload\":{\"volume\":");
    writer_u32(&writer, s->volume);
    writer_raw(&writer, ",\"brightness\":");
    writer_u32(&writer, s->brightness);
    writer_raw(&writer, ",\"led_power\":");
    writer_json_string(&writer, s->led_power);
    writer_raw(&writer, ",\"led_auto_off_s\":");
    writer_u32(&writer, s->led_auto_off_s);
    writer_raw(&writer, ",\"language\":");
    writer_json_string(&writer, s->language);
    writer_raw(&writer, ",\"voice_mode\":");
    writer_json_string(&writer, s->voice_mode);
    writer_raw(&writer, ",\"font_size\":");
    writer_u32(&writer, s->font_size);
    writer_raw(&writer, ",\"license\":");
    writer_json_string(&writer, s->license_present ? "present" : "missing");
    writer_raw(&writer, ",\"ble_paired\":");
    writer_raw(&writer, s->ble_paired ? "true" : "false");
    writer_raw(&writer, "}}\n");
    return writer_finish(&writer);
}

int ak_proto_hello_ack(char *buf, size_t size, const char *fw, const char *transport)
{
    return ak_proto_hello_ack_caps(buf, size, fw, transport, false);
}

int ak_proto_hello_ack_caps(char *buf, size_t size, const char *fw, const char *transport, bool menu)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"type\":\"hello_ack\",\"payload\":{\"fw\":");
    writer_json_string(&writer, fw);
    writer_raw(&writer, ",\"transport\":");
    writer_json_string(&writer, transport);
    writer_raw(&writer, ",\"capabilities\":[");
    writer_json_string(&writer, transport);
    writer_raw(&writer, ",\"keymap\",\"voice_state\",\"device_settings\"");
    if (transport != NULL &&
        (strcmp(transport, "uart") == 0 || strcmp(transport, "dual") == 0)) {
        writer_raw(&writer, ",\"system_info\"");
    }
    if (menu) {
        /* Display builds wait in ② for the Companion's PC ASR text. */
        writer_raw(&writer, ",\"menu\",\"pc_asr_wait\",\"bind_session\"");
    }
    writer_raw(&writer, "]}}\n");
    return writer_finish(&writer);
}

int ak_proto_menu_result(char *buf, size_t size, uint32_t seq, const char *id, const uint8_t *selected, size_t n,
                         const char *text)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"seq\":");
    writer_u32(&writer, seq);
    writer_raw(&writer, ",\"type\":\"menu_result\",\"payload\":{\"id\":");
    writer_json_string(&writer, id);
    writer_raw(&writer, ",\"selected\":[");
    for (size_t i = 0; i < n && selected != NULL; i++) {
        if (i > 0) {
            writer_char(&writer, ',');
        }
        writer_u32(&writer, selected[i]);
    }
    writer_char(&writer, ']');
    if (text != NULL) {
        writer_raw(&writer, ",\"text\":");
        writer_json_string(&writer, text);
    }
    writer_raw(&writer, "}}\n");
    return writer_finish(&writer);
}

int ak_proto_menu_dismissed(char *buf, size_t size, uint32_t seq, const char *id, const char *reason)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"seq\":");
    writer_u32(&writer, seq);
    writer_raw(&writer, ",\"type\":\"menu_result\",\"payload\":{\"id\":");
    writer_json_string(&writer, id);
    writer_raw(&writer, ",\"dismissed\":true,\"reason\":");
    writer_json_string(&writer, reason);
    writer_raw(&writer, "}}\n");
    return writer_finish(&writer);
}

int ak_proto_ping(char *buf, size_t size, uint32_t seq, uint32_t uptime_ms)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"seq\":");
    writer_u32(&writer, seq);
    writer_raw(&writer, ",\"type\":\"ping\",\"payload\":{\"uptime_ms\":");
    writer_u32(&writer, uptime_ms);
    writer_raw(&writer, "}}\n");
    return writer_finish(&writer);
}

int ak_proto_pong(char *buf, size_t size)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"type\":\"pong\"}\n");
    return writer_finish(&writer);
}

int ak_proto_ack(char *buf, size_t size, uint32_t seq)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"type\":\"ack\",\"payload\":{\"seq\":");
    writer_u32(&writer, seq);
    writer_raw(&writer, "}}\n");
    return writer_finish(&writer);
}

int ak_proto_status(char *buf, size_t size, const char *probe, const char *state)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"type\":\"status\",\"payload\":{\"probe\":");
    writer_json_string(&writer, probe);
    writer_raw(&writer, ",\"state\":");
    writer_json_string(&writer, state);
    writer_raw(&writer, "}}\n");
    return writer_finish(&writer);
}

int ak_proto_log(char *buf, size_t size, uint32_t seq, const char *tag, const char *msg)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"seq\":");
    writer_u32(&writer, seq);
    writer_raw(&writer, ",\"type\":\"log\",\"payload\":{\"tag\":");
    writer_json_string(&writer, tag);
    writer_raw(&writer, ",\"msg\":");
    writer_json_string(&writer, msg);
    writer_raw(&writer, "}}\n");
    return writer_finish(&writer);
}
