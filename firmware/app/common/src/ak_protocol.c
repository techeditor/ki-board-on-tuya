/**
 * @file ak_protocol.c
 * @brief JSONL protocol v1 frame builders (host-testable, no SDK deps).
 */

#include "ak_protocol.h"

#include "ak_led_policy.h"
#include "ak_voice_mode.h"

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
    return ak_proto_key_gesture_text(buf, size, seq, id, gesture, bind, NULL, 0U);
}

/* held_ms / wait_ms are emitted only when `timed`. */
static int key_gesture_frame(char *buf, size_t size, uint32_t seq, const char *id, const char *gesture,
                             const char *bind, const char *text, uint8_t slot, bool timed, uint32_t held_ms, uint32_t wait_ms)
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
    if (slot != 0U) {
        writer_raw(&writer, ",\"slot\":");
        writer_u32(&writer, slot);
    }
    if (text != NULL) {
        writer_raw(&writer, ",\"text\":");
        writer_json_string(&writer, text);
    }
    writer_raw(&writer, ",\"held_ms\":");
    writer_u32(&writer, timed ? held_ms : 0U);
    if (timed) {
        writer_raw(&writer, ",\"wait_ms\":");
        writer_u32(&writer, wait_ms);
    }
    writer_raw(&writer, "}}\n");
    return writer_finish(&writer);
}

int ak_proto_key_gesture_text(char *buf, size_t size, uint32_t seq, const char *id, const char *gesture,
                              const char *bind, const char *text, uint8_t slot)
{
    return key_gesture_frame(buf, size, seq, id, gesture, bind, text, slot, false, 0U, 0U);
}

int ak_proto_key_gesture_timed(char *buf, size_t size, uint32_t seq, const char *id, const char *gesture,
                               const char *bind, uint32_t held_ms, uint32_t wait_ms)
{
    return key_gesture_frame(buf, size, seq, id, gesture, bind, NULL, 0U, true, held_ms, wait_ms);
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

int ak_proto_voice(char *buf, size_t size, uint32_t seq, const char *state, uint8_t slot)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"seq\":");
    writer_u32(&writer, seq);
    writer_raw(&writer, ",\"type\":\"voice\",\"payload\":{\"state\":");
    writer_json_string(&writer, state);
    if (slot != 0U) {
        writer_raw(&writer, ",\"slot\":");
        writer_u32(&writer, slot);
    }
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

int ak_proto_asr(char *buf, size_t size, uint32_t seq, const char *text, uint8_t slot)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"seq\":");
    writer_u32(&writer, seq);
    writer_raw(&writer, ",\"type\":\"asr\",\"payload\":{\"text\":");
    writer_json_string(&writer, text);
    if (slot != 0U) {
        writer_raw(&writer, ",\"slot\":");
        writer_u32(&writer, slot);
    }
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

int ak_proto_pid_status(char *buf, size_t size, uint32_t seq, const char *state, const char *reason)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"seq\":");
    writer_u32(&writer, seq);
    writer_raw(&writer, ",\"type\":\"pid_status\",\"payload\":{\"state\":");
    writer_json_string(&writer, state);
    if (reason != NULL && reason[0] != '\0') {
        writer_raw(&writer, ",\"reason\":");
        writer_json_string(&writer, reason);
    }
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
    writer_raw(&writer, ",\"animation\":");
    writer_raw(&writer, s->animation ? "true" : "false");
    writer_raw(&writer, ",\"license\":");
    writer_json_string(&writer, s->license_present ? "present" : "missing");
    writer_raw(&writer, ",\"pid\":");
    writer_json_string(&writer, s->pid != NULL ? s->pid : "missing");
    writer_raw(&writer, ",\"ble_paired\":");
    writer_raw(&writer, s->ble_paired ? "true" : "false");
    writer_raw(&writer, "}}\n");
    return writer_finish(&writer);
}

int ak_proto_device_info(char *buf, size_t size, uint32_t seq, const ak_devinfo_t *d)
{
    char             mac[AK_DEVINFO_MAC_STR];
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"seq\":");
    writer_u32(&writer, seq);
    writer_raw(&writer, ",\"type\":\"device_info\",\"payload\":{\"fw\":");
    writer_json_string(&writer, d->fw);
    writer_raw(&writer, ",\"companion\":");
    writer_json_string(&writer, ak_devinfo_link_str(d->companion));
    writer_raw(&writer, d->handshake ? ",\"handshake\":true" : ",\"handshake\":false");
    writer_raw(&writer, ",\"cloud\":");
    writer_json_string(&writer, ak_devinfo_cloud_str(d->cloud));
    writer_raw(&writer, ",\"wifi_ssid\":");
    writer_json_string(&writer, d->ssid);
    writer_raw(&writer, ",\"wifi_ip\":");
    writer_json_string(&writer, d->ip);
    if (d->rssi_valid) {
        writer_raw(&writer, ",\"wifi_rssi\":");
        writer_i32(&writer, d->rssi);
    }
    writer_raw(&writer, ",\"wifi_mac\":");
    if (d->wifi_mac_valid) {
        ak_devinfo_mac_str(d->wifi_mac, mac);
    } else {
        mac[0] = '\0';
    }
    writer_json_string(&writer, mac);
    writer_raw(&writer, ",\"ble_mac\":");
    if (d->ble_mac_valid) {
        ak_devinfo_mac_str(d->ble_mac, mac);
    } else {
        mac[0] = '\0';
    }
    writer_json_string(&writer, mac);
    writer_raw(&writer, ",\"ble_name\":");
    writer_json_string(&writer, d->ble_name);
    writer_raw(&writer, d->ble_paired ? ",\"ble_paired\":true" : ",\"ble_paired\":false");
    writer_raw(&writer, d->pid ? ",\"pid\":true" : ",\"pid\":false");
    writer_raw(&writer, d->uuid ? ",\"uuid\":true" : ",\"uuid\":false");
    writer_raw(&writer, d->authkey ? ",\"authkey\":true" : ",\"authkey\":false");
    writer_raw(&writer, d->uuid && d->authkey ? ",\"license\":true" : ",\"license\":false");
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
    writer_raw(&writer, AK_HELLO_ACK_HEAD);
    writer_json_string(&writer, fw);
    writer_raw(&writer, AK_HELLO_ACK_TRANSPORT);
    writer_json_string(&writer, transport);
    writer_raw(&writer, AK_HELLO_ACK_CAPS_OPEN);
    writer_json_string(&writer, transport);
    writer_raw(&writer, AK_HELLO_CAPS_BASE);
    if (transport != NULL &&
        (strcmp(transport, "uart") == 0 || strcmp(transport, "dual") == 0)) {
        writer_raw(&writer, AK_HELLO_CAP_SYSTEM);
    }
    if (menu) {
        /* Display builds wait in ② for the Companion's PC ASR text. */
        writer_raw(&writer, AK_HELLO_CAPS_MENU);
        writer_raw(&writer, AK_HELLO_CAP_SCENES); /* FR-03 */
    }
    writer_raw(&writer, AK_HELLO_ACK_TAIL);
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

/* ── FR-03 scenes ─────────────────────────────────────────────────────── */

int ak_proto_scene_state(char *buf, size_t size, uint32_t seq, int active, bool dirty,
                         const ak_scene_t *const slots[], size_t n_slots, const char *error, const char *req)
{
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"seq\":");
    writer_u32(&writer, seq);
    writer_raw(&writer, ",\"type\":\"scene_state\",\"payload\":{\"active\":");
    writer_i32(&writer, (int32_t)active);
    writer_raw(&writer, ",\"dirty\":");
    writer_raw(&writer, dirty ? "true" : "false");
    writer_raw(&writer, ",\"slots\":[");
    for (size_t i = 0; i < n_slots; i++) {
        const ak_scene_t *s = slots != NULL ? slots[i] : NULL;
        if (i > 0) {
            writer_char(&writer, ',');
        }
        writer_raw(&writer, "{\"id\":"); /* the array index is the slot number */
        writer_json_string(&writer, s != NULL ? s->id : "");
        writer_raw(&writer, ",\"rev\":");
        writer_u32(&writer, s != NULL ? s->rev : 0U);
        writer_raw(&writer, ",\"name\":");
        writer_json_string(&writer, s != NULL ? s->name : "");
        writer_char(&writer, '}');
    }
    writer_char(&writer, ']');
    if (error != NULL) {
        writer_raw(&writer, ",\"error\":");
        writer_json_string(&writer, error);
    }
    if (req != NULL) {
        writer_raw(&writer, ",\"req\":");
        writer_json_string(&writer, req);
    }
    writer_raw(&writer, "}}\n");
    return writer_finish(&writer);
}

int ak_proto_scene_data(char *buf, size_t size, uint32_t seq, uint8_t slot, const ak_scene_t *scene)
{
    char km[AK_SCENE_KM_HEX + 1U];
    char idle[AK_SCENE_IDLE_HEX + 1U];
    if (scene == NULL) {
        return -1;
    }
    ak_scene_km_to_hex(&scene->keymap, km);
    ak_scene_idle_to_hex(scene->idle, idle);
    ak_json_writer_t writer;
    writer_init(&writer, buf, size);
    writer_raw(&writer, "{\"v\":1,\"seq\":");
    writer_u32(&writer, seq);
    writer_raw(&writer, ",\"type\":\"scene_data\",\"payload\":{\"slot\":");
    writer_u32(&writer, slot);
    writer_raw(&writer, ",\"id\":");
    writer_json_string(&writer, scene->id);
    writer_raw(&writer, ",\"rev\":");
    writer_u32(&writer, scene->rev);
    writer_raw(&writer, ",\"name\":");
    writer_json_string(&writer, scene->name);
    writer_raw(&writer, ",\"km\":");
    writer_json_string(&writer, km);
    writer_raw(&writer, ",\"voice\":");
    writer_json_string(&writer, ak_voice_mode_str((ak_voice_mode_t)scene->voice_mode));
    writer_raw(&writer, ",\"led\":");
    writer_json_string(&writer, ak_led_mode_wire_str((ak_led_mode_t)scene->led_mode));
    writer_raw(&writer, ",\"led_off_s\":");
    writer_u32(&writer, ak_led_policy_timeout_ms(scene->led_timeout_idx) / 1000U);
    writer_raw(&writer, ",\"idle\":");
    writer_json_string(&writer, idle);
    writer_raw(&writer, ",\"home\":");
    writer_json_string(&writer, ak_scene_home_wire(scene->home));
    writer_raw(&writer, "}}\n");
    return writer_finish(&writer);
}
