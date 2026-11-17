/**
 * @file test_ak.c
 * @brief Host unit tests for the pure modules: ak_input_fsm, ak_protocol,
 *        ak_rx, ak_validate. Compiled and run on the host (no SDK/hardware).
 */

#include <stdio.h>
#include <string.h>

#include "ak_font8x8.h"
#include "ak_font.h"
#include "ak_font_data.h"
#include "ak_font_ui.h"
#include "ak_i18n.h"
#include "ak_text.h"
#include "ak_icons16.h"
#include "ak_session_icons.h"
#include "ak_input_fsm.h"
#include "ak_keymap.h"
#include "ak_menu.h"
#include "ak_ui.h"
#include "ak_ble_sec.h"
#include "ak_byte_ring.h"
#include "mbedtls/base64.h"
#include "mbedtls/ccm.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"
#include "ak_led_power.h"
#include "ak_bat_gauge.h"
#include "ak_led_policy.h"
#include "ak_cred.h"
#include "ak_protocol.h"
#include "ak_provision.h"
#include "ak_rx.h"
#include "ak_settings.h"
#include "ak_validate.h"
#include "ak_window_marker.h"
#include "ak_voice_mode.h"
#include "ak_voice_route.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                                                   \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

#define CHECK_STR(got, want, msg)                                                                                      \
    do {                                                                                                               \
        if (strcmp((got), (want)) != 0) {                                                                              \
            printf("  FAIL: %s\n    got : %s    want: %s\n", msg, got, want);                                          \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

/* Drive the FSM with a raw level, stepping the clock 5 ms per poll. */
static ak_input_event_t step(ak_input_fsm_t *fsm, int pressed, uint32_t *t, uint32_t *held)
{
    ak_input_event_t ev = ak_input_fsm_update(fsm, pressed ? true : false, *t, held);
    *t += 5;
    return ev;
}

static void test_short_press(void)
{
    printf("test_short_press\n");
    ak_input_fsm_t fsm;
    uint32_t       t = 100, held = 0;
    ak_input_fsm_init(&fsm, false, t);

    CHECK(step(&fsm, 0, &t, &held) == AK_INPUT_EVENT_NONE, "idle no event");

    int down_seen = 0;
    for (int i = 0; i < 8; i++) {
        if (step(&fsm, 1, &t, &held) == AK_INPUT_EVENT_DOWN) {
            down_seen = 1;
            break;
        }
    }
    CHECK(down_seen, "DOWN emitted after debounce");

    int up_seen = 0, long_seen = 0;
    for (int i = 0; i < 8; i++) {
        ak_input_event_t ev = step(&fsm, 0, &t, &held);
        if (ev == AK_INPUT_EVENT_LONG)
            long_seen = 1;
        if (ev == AK_INPUT_EVENT_UP) {
            up_seen = 1;
            break;
        }
    }
    CHECK(up_seen, "UP emitted after release");
    CHECK(!long_seen, "no LONG on short press");
}

static void test_long_press(void)
{
    printf("test_long_press\n");
    ak_input_fsm_t fsm;
    uint32_t       t = 0, held = 0;
    ak_input_fsm_init(&fsm, false, t);

    int      down = 0, lng = 0, up = 0;
    uint32_t long_held = 0;
    for (int i = 0; i < 200; i++) { /* hold ~1 s */
        ak_input_event_t ev = step(&fsm, 1, &t, &held);
        if (ev == AK_INPUT_EVENT_DOWN)
            down++;
        if (ev == AK_INPUT_EVENT_LONG) {
            lng++;
            long_held = held;
        }
    }
    CHECK(down == 1, "exactly one DOWN");
    CHECK(lng == 1, "exactly one LONG (fires once)");
    CHECK(long_held >= AK_FSM_LONG_PRESS_MS, "LONG held_ms >= threshold");

    for (int i = 0; i < 8; i++) {
        if (step(&fsm, 0, &t, &held) == AK_INPUT_EVENT_UP) {
            up++;
            break;
        }
    }
    CHECK(up == 1, "UP after long hold");
    CHECK(held >= AK_FSM_LONG_PRESS_MS, "UP held_ms >= threshold");
}

static void test_debounce_bounce(void)
{
    printf("test_debounce_bounce\n");
    ak_input_fsm_t fsm;
    uint32_t       t = 0, held = 0;
    ak_input_fsm_init(&fsm, false, t);

    int spurious = 0;
    for (int i = 0; i < 6; i++) {
        if (step(&fsm, i % 2, &t, &held) != AK_INPUT_EVENT_NONE)
            spurious++;
    }
    CHECK(spurious == 0, "no event while bouncing within debounce window");
}

static void test_protocol(void)
{
    printf("test_protocol\n");
    char buf[192];

    ak_proto_key(buf, sizeof(buf), 7, "k1", "down", 0);
    CHECK_STR(buf,
              "{\"v\":1,\"seq\":7,\"type\":\"key\",\"payload\":{\"id\":\"k1\",\"action\":\"down\","
              "\"held_ms\":0}}\n",
              "key frame");

    ak_proto_key_gesture(buf, sizeof(buf), 30, "k1", "single", "chat1");
    CHECK_STR(buf,
              "{\"v\":1,\"seq\":30,\"type\":\"key\",\"payload\":{\"id\":\"k1\",\"action\":\"single\","
              "\"bind\":\"chat1\",\"held_ms\":0}}\n",
              "key gesture frame with bind");

    ak_proto_key_gesture(buf, sizeof(buf), 31, "k2", "long", NULL);
    CHECK_STR(buf,
              "{\"v\":1,\"seq\":31,\"type\":\"key\",\"payload\":{\"id\":\"k2\",\"action\":\"long\","
              "\"held_ms\":0}}\n",
              "key gesture frame without bind");

    ak_proto_key_gesture_text(buf, sizeof(buf), 32, "k6", "single", "confirm", "say \"hi\" 你好");
    CHECK_STR(buf,
              "{\"v\":1,\"seq\":32,\"type\":\"key\",\"payload\":{\"id\":\"k6\",\"action\":\"single\","
              "\"bind\":\"confirm\",\"text\":\"say \\\"hi\\\" 你好\",\"held_ms\":0}}\n",
              "confirm gesture frame carries the ③ text");

    ak_proto_encoder(buf, sizeof(buf), 8, 1);
    CHECK_STR(buf,
              "{\"v\":1,\"seq\":8,\"type\":\"encoder\",\"payload\":{\"detents\":1}}\n",
              "encoder frame (cw)");

    ak_proto_encoder(buf, sizeof(buf), 9, -3);
    CHECK_STR(buf,
              "{\"v\":1,\"seq\":9,\"type\":\"encoder\",\"payload\":{\"detents\":-3}}\n",
              "encoder frame (ccw multi-step)");

    ak_proto_ping(buf, sizeof(buf), 3, 6000);
    CHECK_STR(buf, "{\"v\":1,\"seq\":3,\"type\":\"ping\",\"payload\":{\"uptime_ms\":6000}}\n", "ping frame");

    ak_proto_pong(buf, sizeof(buf));
    CHECK_STR(buf, "{\"v\":1,\"type\":\"pong\"}\n", "pong frame");

    ak_proto_ack(buf, sizeof(buf), 42);
    CHECK_STR(buf, "{\"v\":1,\"type\":\"ack\",\"payload\":{\"seq\":42}}\n", "ack frame");

    ak_proto_asr(buf, sizeof(buf), 11, "hello world");
    CHECK_STR(buf,
              "{\"v\":1,\"seq\":11,\"type\":\"asr\",\"payload\":{\"text\":\"hello world\"}}\n",
              "asr frame");

    ak_proto_asr(buf, sizeof(buf), 15, "say \"hi\"\\\n\t\x01");
    CHECK_STR(buf,
              "{\"v\":1,\"seq\":15,\"type\":\"asr\",\"payload\":{\"text\":"
              "\"say \\\"hi\\\"\\\\\\n\\t\\u0001\"}}\n",
              "asr frame escapes JSON syntax and control bytes");

    ak_proto_wifi_status(buf, sizeof(buf), 16, "connected", "Lab\"AP\\west\n");
    CHECK_STR(buf,
              "{\"v\":1,\"seq\":16,\"type\":\"wifi_status\",\"payload\":{\"state\":\"connected\","
              "\"ssid\":\"Lab\\\"AP\\\\west\\n\"}}\n",
              "wifi SSID is JSON escaped");

    ak_proto_asr(buf, sizeof(buf), 17, NULL);
    CHECK_STR(buf,
              "{\"v\":1,\"seq\":17,\"type\":\"asr\",\"payload\":{\"text\":\"\"}}\n",
              "null dynamic string becomes empty JSON string");

    {
        char small[16];
        int  required = ak_proto_asr(small, sizeof(small), 18, "a long \"quoted\" value");
        CHECK(required >= (int)sizeof(small), "truncated frame returns required length");
        CHECK(small[sizeof(small) - 1U] == '\0', "truncated frame remains null terminated");
    }

    {
        char worst_text[256];
        char large_frame[2048];
        memset(worst_text, 0x01, sizeof(worst_text) - 1U);
        worst_text[sizeof(worst_text) - 1U] = '\0';
        int required = ak_proto_asr(large_frame, sizeof(large_frame), 19, worst_text);
        CHECK(required > 191, "worst-case escaped ASR exceeds legacy frame size");
        CHECK(required < (int)sizeof(large_frame), "2 KiB ASR frame holds worst-case 255-byte text");
        CHECK(large_frame[required - 1] == '\n', "large ASR frame remains complete JSONL");
    }

    ak_proto_voice_source(buf, sizeof(buf), 12, "cloud", "asr");
    CHECK_STR(buf,
              "{\"v\":1,\"seq\":12,\"type\":\"voice_source\",\"payload\":{\"source\":\"cloud\",\"mode\":\"asr\"}}\n",
              "voice_source frame");

    ak_proto_tuya_chat(buf, sizeof(buf), 30, "agent", "Hi \"there\"", true);
    CHECK_STR(buf,
              "{\"v\":1,\"seq\":30,\"type\":\"tuya_chat\",\"payload\":{\"role\":\"agent\",\"text\":\"Hi \\\"there\\\"\","
              "\"final\":true}}\n",
              "tuya_chat frame escapes text");

    ak_proto_wifi_status(buf, sizeof(buf), 13, "connected", "MyAP");
    CHECK_STR(buf,
              "{\"v\":1,\"seq\":13,\"type\":\"wifi_status\",\"payload\":{\"state\":\"connected\","
              "\"ssid\":\"MyAP\"}}\n",
              "wifi_status frame");

    ak_proto_activation_status(buf, sizeof(buf), 20, "scan_qr",
                               "https://smartapp.tuya.com/s/p?p=pid&uuid=uuid123&v=2.0");
    CHECK_STR(buf,
              "{\"v\":1,\"seq\":20,\"type\":\"activation_status\",\"payload\":{"
              "\"state\":\"scan_qr\",\"qr_url\":"
              "\"https://smartapp.tuya.com/s/p?p=pid&uuid=uuid123&v=2.0\"}}\n",
              "activation status frame");

    ak_proto_license_status(buf, sizeof(buf), 14, "written");
    CHECK_STR(buf,
              "{\"v\":1,\"seq\":14,\"type\":\"license_status\",\"payload\":{\"state\":\"written\"}}\n",
              "license_status frame");

    ak_proto_prov_status(buf, sizeof(buf), 16, "open", 172U, NULL, NULL);
    CHECK_STR(buf,
              "{\"v\":1,\"seq\":16,\"type\":\"prov_status\",\"payload\":{\"state\":\"open\",\"remaining_s\":172}}\n",
              "prov_status frame");
    ak_proto_prov_status(buf, sizeof(buf), 17, "closed", 0U, "wifi", "closed");
    CHECK_STR(buf,
              "{\"v\":1,\"seq\":17,\"type\":\"prov_status\",\"payload\":{\"state\":\"closed\",\"remaining_s\":0,"
              "\"rejected\":\"wifi\",\"reason\":\"closed\"}}\n",
              "prov_status rejection frame");

    ak_proto_ble_pair_key(buf, sizeof(buf), 18, "00ff", "02d449a3");
    CHECK_STR(buf,
              "{\"v\":1,\"seq\":18,\"type\":\"ble_pair_key\",\"payload\":{\"key\":\"00ff\",\"key_id\":\"02d449a3\"}}\n",
              "ble_pair_key frame");

    ak_proto_license_info(buf, sizeof(buf), 15, "flashed", "uuid123", "CDEF");
    CHECK_STR(buf,
              "{\"v\":1,\"seq\":15,\"type\":\"license_info\",\"payload\":{"
              "\"source\":\"flashed\",\"uuid\":\"uuid123\",\"authkey_tail\":\"CDEF\"}}\n",
              "license_info frame");

    {
        char info[384];
        ak_proto_system_info(info, sizeof(info), 15, "0.1.1", "dual",
                             "Lab\"AP", "connected", "Agentic Keyboard-A1B2C3",
                             "advertising", "ready", "present");
        CHECK_STR(info,
                  "{\"v\":1,\"seq\":15,\"type\":\"system_info\",\"payload\":{"
                  "\"fw\":\"0.1.1\",\"transport\":\"dual\","
                  "\"wifi_name\":\"Lab\\\"AP\",\"wifi_state\":\"connected\","
                  "\"ble_name\":\"Agentic Keyboard-A1B2C3\","
                  "\"ble_state\":\"advertising\",\"cloud\":\"ready\","
                  "\"license\":\"present\"}}\n",
                  "system_info frame");
    }

    ak_proto_device_settings(buf, sizeof(buf), 15, 70, 90, "auto", 60U, "zh");
    CHECK_STR(buf,
              "{\"v\":1,\"seq\":15,\"type\":\"device_settings_state\",\"payload\":{\"volume\":70,\"brightness\":90,"
              "\"led_power\":\"auto\",\"led_auto_off_s\":60,\"language\":\"zh\"}}\n",
              "device settings frame");
    CHECK(strcmp(ak_led_mode_wire_str(AK_LED_MODE_OFF), "off") == 0 &&
              strcmp(ak_led_mode_wire_str(AK_LED_MODE_ON), "on") == 0 &&
              strcmp(ak_led_mode_wire_str(AK_LED_MODE_AUTO), "auto") == 0,
          "led_power wire names");

    ak_proto_hello_ack(buf, sizeof(buf), "0.1.0", "uart");
    CHECK_STR(buf,
              "{\"v\":1,\"type\":\"hello_ack\",\"payload\":{\"fw\":\"0.1.0\",\"transport\":\"uart\","
              "\"capabilities\":[\"uart\",\"keymap\",\"voice_state\",\"device_settings\",\"system_info\"]}}\n",
              "hello_ack frame");

    ak_proto_hello_ack(buf, sizeof(buf), "0.1.0", "ble_gatt");
    CHECK_STR(buf,
              "{\"v\":1,\"type\":\"hello_ack\",\"payload\":{\"fw\":\"0.1.0\",\"transport\":\"ble_gatt\","
              "\"capabilities\":[\"ble_gatt\",\"keymap\",\"voice_state\",\"device_settings\"]}}\n",
              "BLE handshake excludes UART0 system_info capability");

    ak_proto_hello_ack_caps(buf, sizeof(buf), "0.1.0", "dual", true);
    CHECK_STR(buf,
              "{\"v\":1,\"type\":\"hello_ack\",\"payload\":{\"fw\":\"0.1.0\",\"transport\":\"dual\","
              "\"capabilities\":[\"dual\",\"keymap\",\"voice_state\",\"device_settings\",\"system_info\",\"menu\"]}}\n",
              "hello_ack advertises menu");
    ak_proto_hello_ack_caps(buf, sizeof(buf), "0.1.0", "ble_gatt", true);
    CHECK(strstr(buf, "\"menu\"]") != NULL, "menu capability on BLE too");

    {
        const uint8_t sel[] = {0, 2, 47};
        ak_proto_menu_result(buf, sizeof(buf), 51, "a1b2c3", sel, 3, NULL);
        CHECK_STR(buf, "{\"v\":1,\"seq\":51,\"type\":\"menu_result\",\"payload\":{\"id\":\"a1b2c3\",\"selected\":[0,2,47]}}\n",
                  "menu_result frame");
        ak_proto_menu_result(buf, sizeof(buf), 52, "q9", NULL, 0, NULL);
        CHECK_STR(buf, "{\"v\":1,\"seq\":52,\"type\":\"menu_result\",\"payload\":{\"id\":\"q9\",\"selected\":[]}}\n",
                  "empty multi-select result");
        ak_proto_menu_dismissed(buf, sizeof(buf), 53, "a1b2c3", "esc");
        CHECK_STR(buf,
                  "{\"v\":1,\"seq\":53,\"type\":\"menu_result\",\"payload\":{\"id\":\"a1b2c3\",\"dismissed\":true,"
                  "\"reason\":\"esc\"}}\n",
                  "menu_result dismissed frame");
        const uint8_t vi[] = {3};
        ak_proto_menu_result(buf, sizeof(buf), 54, "q9", vi, 1, "use \"main\"");
        CHECK_STR(buf,
                  "{\"v\":1,\"seq\":54,\"type\":\"menu_result\",\"payload\":{\"id\":\"q9\",\"selected\":[3],"
                  "\"text\":\"use \\\"main\\\"\"}}\n",
                  "dictated answer carries escaped text");
        uint8_t all[AK_MENU_ITEMS_MAX];
        for (unsigned i = 0; i < AK_MENU_ITEMS_MAX; i++) {
            all[i] = (uint8_t)i;
        }
        char big[256];
        int  n = ak_proto_menu_result(big, sizeof(big), 4294967295U, "abcdefghijklmno", all, AK_MENU_ITEMS_MAX, NULL);
        CHECK(n > 0 && n < (int)sizeof(big), "48-item result fits a 256-byte frame buffer");
    }

    ak_proto_status(buf, sizeof(buf), "ble_gatt_link", "ready");
    CHECK_STR(buf,
              "{\"v\":1,\"type\":\"status\",\"payload\":{\"probe\":\"ble_gatt_link\",\"state\":\"ready\"}}\n",
              "status frame");

    ak_proto_log(buf, sizeof(buf), 5, "ble", "central connected");
    CHECK_STR(buf,
              "{\"v\":1,\"seq\":5,\"type\":\"log\",\"payload\":{\"tag\":\"ble\",\"msg\":\"central connected\"}}\n",
              "log frame");
}

static char           g_lines[8][AK_RX_LINE_MAX];
static ak_rx_source_t g_sources[8];
static int            g_nlines;

static void capture_line(const char *line, ak_rx_source_t source)
{
    if (g_nlines < 8) {
        strncpy(g_lines[g_nlines], line, AK_RX_LINE_MAX - 1);
        g_lines[g_nlines][AK_RX_LINE_MAX - 1] = '\0';
        g_sources[g_nlines]                   = source;
        g_nlines++;
    }
}

static void feed_str(ak_rx_reader_t *r, const char *s)
{
    ak_rx_reader_feed(r, (const uint8_t *)s, strlen(s), capture_line);
}

/* Mirror of on_rx_line's admission steps: envelope, then source policy.
 * Returns true when the firmware would execute the frame. */
static bool admitted(const char *line, ak_rx_source_t source, char *type_out, size_t type_size)
{
    ak_rx_frame_t frame;
    if (!ak_rx_parse_frame(line, &frame)) {
        return false;
    }
    if (type_out != NULL) {
        snprintf(type_out, type_size, "%s", frame.type);
    }
    return ak_rx_source_allows(frame.type, source);
}

static void test_rx(void)
{
    printf("test_rx\n");
    ak_rx_reader_t r;
    ak_rx_reader_init(&r, AK_RX_SRC_UART);
    g_nlines = 0;

    /* Line split across two feeds; second feed completes it and adds another. */
    feed_str(&r, "{\"v\":1,\"ty");
    CHECK(g_nlines == 0, "partial line not delivered yet");
    feed_str(&r, "pe\":\"ping\"}\n{\"v\":1,\"type\":\"hello\"}\r\n");
    CHECK(g_nlines == 2, "two lines assembled across chunks (CRLF ok)");
    CHECK(g_sources[0] == AK_RX_SRC_UART && g_sources[1] == AK_RX_SRC_UART, "lines carry the reader source");

    ak_rx_frame_t f;
    CHECK(ak_rx_parse_frame(g_lines[0], &f) && strcmp(f.type, "ping") == 0, "line 0 type=ping");
    CHECK(strcmp(f.payload, "{}") == 0 && f.seq == 0U, "absent payload/seq default");
    CHECK(ak_rx_parse_frame(g_lines[1], &f) && strcmp(f.type, "hello") == 0, "line 1 type=hello");

    const char *disp = "{\"v\":1,\"type\":\"display\",\"payload\":{\"chat_slot\":2,\"agent_name\":\"planner\",\"state\":"
                       "\"running\"},\"seq\":42}";
    char     agent[32];
    char     buf[16];
    uint32_t chat_slot = 0;
    CHECK(ak_rx_parse_frame(disp, &f) && f.seq == 42U, "envelope seq");
    CHECK(ak_json_get_str(f.payload, "agent_name", agent, sizeof(agent)) == AK_JSON_OK &&
              strcmp(agent, "planner") == 0,
          "extract agent_name string");
    CHECK(ak_json_get_uint(f.payload, "chat_slot", 3U, &chat_slot) == AK_JSON_OK && chat_slot == 2U,
          "extract Chat B slot");
    CHECK(ak_json_get_str(f.payload, "missing", buf, sizeof(buf)) == AK_JSON_ABSENT, "absent key");
    CHECK(ak_json_get_uint(f.payload, "seq", UINT32_MAX, &chat_slot) == AK_JSON_ABSENT,
          "top-level seq is not a payload member");

    const char *bindings =
        "{\"v\":1,\"type\":\"bindings\",\"payload\":{\"alias_a\":\"Plan\",\"alias_b\":\"\",\"alias_c\":\"Code\"}}";
    char alias[8];
    CHECK(ak_rx_parse_frame(bindings, &f), "bindings envelope");
    CHECK(ak_json_get_str(f.payload, "alias_a", alias, sizeof(alias)) == AK_JSON_OK && strcmp(alias, "Plan") == 0,
          "extract slot A alias");
    CHECK(ak_json_get_str(f.payload, "alias_b", alias, sizeof(alias)) == AK_JSON_OK && alias[0] == '\0',
          "extract empty slot B alias for default fallback");
    CHECK(ak_json_get_str(f.payload, "alias_c", alias, sizeof(alias)) == AK_JSON_OK && strcmp(alias, "Code") == 0,
          "extract slot C alias");

    const char *settings = "{\"v\":1,\"type\":\"device_settings\",\"payload\":{\"volume\":60,\"brightness\":80}}";
    uint32_t    value    = 0U;
    CHECK(ak_rx_parse_frame(settings, &f), "settings envelope");
    CHECK(ak_json_get_uint(f.payload, "volume", 100U, &value) == AK_JSON_OK && value == 60U, "extract device volume");
    CHECK(ak_json_get_uint(f.payload, "brightness", 100U, &value) == AK_JSON_OK && value == 80U,
          "extract screen brightness");

    /* Keymap keys contain dots; the real Companion frame must parse. */
    const char *keymap = "{\"v\":1,\"type\":\"keymap\",\"payload\":{\"k1.single\":\"chat1\",\"k5.kind\":\"voice\"}}";
    CHECK(ak_rx_parse_frame(keymap, &f) &&
              ak_json_get_str(f.payload, "k5.kind", buf, sizeof(buf)) == AK_JSON_OK && strcmp(buf, "voice") == 0,
          "dotted keymap field");

    /* Whitespace the Companion's serializer may emit is accepted. */
    CHECK(ak_rx_parse_frame(" { \"v\" : 1 , \"type\" : \"ping\" } ", &f) && strcmp(f.type, "ping") == 0,
          "whitespace around tokens");
}

/* Regression tests for the attack paths in firmware-review FW-01 / FW-06
 * (evidence/fw-rx-parser-poc.c). Each must be rejected. */
static void test_rx_attacks(void)
{
    printf("test_rx_attacks\n");
    ak_rx_reader_t uart;
    ak_rx_reader_t ble;
    ak_rx_frame_t  f;
    char           type[AK_RX_TYPE_MAX];
    char           buf[16];
    uint32_t       value = 0;

    /* FW-01 PoC: a BLE central sends a provisioning half line (no '\n') and
     * waits for the Companion's next USB line to complete it. With one reader
     * per source the USB line is delivered alone, stamped UART. */
    ak_rx_reader_init(&uart, AK_RX_SRC_UART);
    ak_rx_reader_init(&ble, AK_RX_SRC_BLE);
    g_nlines = 0;
    feed_str(&ble, "{\"v\":1,\"type\":\"wifi\",\"payload\":{\"ssid\":\"evil\",\"pass\":\"x\"}}");
    feed_str(&uart, "{\"v\":1,\"type\":\"hello\"}\n");
    CHECK(g_nlines == 1, "FW-01: only the USB line is delivered");
    CHECK(g_sources[0] == AK_RX_SRC_UART && admitted(g_lines[0], g_sources[0], type, sizeof(type)) &&
              strcmp(type, "hello") == 0,
          "FW-01: USB line stays a hello");
    /* When the attacker finishes its own line, it arrives stamped BLE and the
     * source policy refuses it. */
    feed_str(&ble, "\n");
    CHECK(g_nlines == 2 && g_sources[1] == AK_RX_SRC_BLE, "FW-01: BLE line stamped BLE");
    CHECK(!admitted(g_lines[1], g_sources[1], type, sizeof(type)) && strcmp(type, "wifi") == 0,
          "FW-01: wifi from BLE rejected");

    /* Same attack against the old shared assembler shape: the spliced bytes
     * form two concatenated objects, which the strict envelope refuses even
     * if they were ever stamped UART. */
    CHECK(!ak_rx_parse_frame("{\"v\":1,\"type\":\"wifi\",\"payload\":{\"ssid\":\"evil\",\"pass\":\"x\"}}"
                             "{\"v\":1,\"type\":\"hello\"}",
                             &f),
          "FW-01: spliced line is not one object");

    /* A disconnect drops the BLE partial line; the next central's line is clean. */
    g_nlines = 0;
    feed_str(&ble, "{\"v\":1,\"type\":\"reset_net");
    ak_rx_reader_reset(&ble);
    feed_str(&ble, "{\"v\":1,\"type\":\"ping\"}\n");
    CHECK(g_nlines == 1 && admitted(g_lines[0], AK_RX_SRC_BLE, type, sizeof(type)) && strcmp(type, "ping") == 0,
          "FW-08: reset drops the stale half line");

    /* Source policy for every sensitive command. */
    static const char *const sensitive[] = {"wifi", "license", "license_read", "reset_netcfg", "ble_pair"};
    for (size_t i = 0; i < sizeof(sensitive) / sizeof(sensitive[0]); i++) {
        CHECK(!ak_rx_source_allows(sensitive[i], AK_RX_SRC_BLE), "sensitive command refused from BLE");
        CHECK(ak_rx_source_allows(sensitive[i], AK_RX_SRC_UART), "sensitive command allowed from UART");
    }
    CHECK(ak_rx_source_allows("keymap", AK_RX_SRC_BLE) && ak_rx_source_allows("ping", AK_RX_SRC_BLE),
          "non-sensitive commands allowed from BLE");

    /* FW-06: nested "type" before the top-level one must not win. */
    CHECK(ak_rx_parse_frame("{\"v\":1,\"payload\":{\"type\":\"wifi\"},\"type\":\"ping\"}", &f) &&
              strcmp(f.type, "ping") == 0,
          "FW-06: top-level type wins over nested");
    CHECK(!ak_rx_parse_frame("{\"v\":1,\"payload\":{\"type\":\"wifi\"}}", &f), "FW-06: nested type alone rejected");
    CHECK(!ak_rx_parse_frame("{\"v\":1,\"type\":\"ping\",\"type\":\"wifi\"}", &f), "FW-06: duplicate type rejected");
    CHECK(!ak_rx_parse_frame("{\"v\":1,\"type\":\"pingpingpingpingpingpingXYZ\"}", &f),
          "FW-06: overlong type rejected, not truncated");
    CHECK(!ak_rx_parse_frame("{\"v\":1,\"type\":\"Wifi\"}", &f), "type outside [a-z0-9_] rejected");
    CHECK(!ak_rx_parse_frame("{\"type\":\"ping\"}", &f), "missing v rejected");
    CHECK(!ak_rx_parse_frame("{\"v\":2,\"type\":\"ping\"}", &f), "v!=1 rejected");
    CHECK(!ak_rx_parse_frame("{\"v\":1,\"type\":\"ping\",\"payload\":\"x\"}", &f), "non-object payload rejected");
    CHECK(!ak_rx_parse_frame("{\"v\":1,\"type\":\"ping\"} x", &f), "trailing garbage rejected");
    CHECK(!ak_rx_parse_frame("{\"v\":1,\"type\":\"ping\",}", &f), "trailing comma rejected");
    CHECK(!ak_rx_parse_frame("{\"v\":1,\"type\":\"ping\",\"x\":[[[[[[[[[[1]]]]]]]]]]}", &f),
          "excessive nesting rejected");

    /* FW-06: a key name appearing inside a value is not a member. */
    const char *spoof = "{\"agent_name\":\"state\",\"text\":\"x\"}";
    CHECK(ak_json_get_str(spoof, "state", buf, sizeof(buf)) == AK_JSON_ABSENT, "FW-06: key text in value ignored");
    const char *spoof2 = "{\"text\":\"x\",\"agent_name\":\"{\\\"state\\\":\\\"a\\\"}\",\"state\":\"idle\"}";
    CHECK(ak_json_get_str(spoof2, "state", buf, sizeof(buf)) == AK_JSON_OK && strcmp(buf, "idle") == 0,
          "FW-06: escaped key inside a string ignored");

    /* FW-06: escapes are decoded, not stripped. */
    char out[16];
    CHECK(ak_json_get_str("{\"pass\":\"caf\\u00e9\\\\x\\n\"}", "pass", out, sizeof(out)) == AK_JSON_OK &&
              strcmp(out, "caf\xc3\xa9\\x\n") == 0,
          "FW-06: \\u00e9, \\\\ and \\n decoded");
    CHECK(ak_json_get_str("{\"t\":\"\\ud83d\\ude00\"}", "t", out, sizeof(out)) == AK_JSON_OK &&
              strcmp(out, "\xf0\x9f\x98\x80") == 0,
          "surrogate pair decoded to 4-byte UTF-8");
    CHECK(ak_json_get_str("{\"t\":\"\\ud83d\"}", "t", out, sizeof(out)) == AK_JSON_INVALID, "lone surrogate rejected");
    CHECK(ak_json_get_str("{\"t\":\"a\\u0000b\"}", "t", out, sizeof(out)) == AK_JSON_INVALID, "\\u0000 rejected");
    CHECK(ak_json_get_str("{\"t\":\"a\\qb\"}", "t", out, sizeof(out)) == AK_JSON_INVALID, "unknown escape rejected");
    CHECK(ak_json_get_str("{\"t\":\"a\tb\"}", "t", out, sizeof(out)) == AK_JSON_INVALID, "raw control char rejected");

    /* FW-06: a value that does not fit is an error, never silently truncated. */
    char small[8];
    CHECK(ak_json_get_str("{\"pass\":\"abcdefghijklmnop\"}", "pass", small, sizeof(small)) == AK_JSON_INVALID &&
              small[0] == '\0',
          "FW-06: oversized string rejected and cleared");
    CHECK(ak_json_get_str("{\"pass\":\"abcdefg\"}", "pass", small, sizeof(small)) == AK_JSON_OK, "exact fit accepted");
    CHECK(ak_json_get_str("{\"pass\":1}", "pass", small, sizeof(small)) == AK_JSON_INVALID, "wrong type rejected");

    /* Display-only text clips on a UTF-8 boundary instead. */
    char clip[8];
    CHECK(ak_json_get_str_clip("{\"t\":\"\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\"}", "t", clip, sizeof(clip)) ==
                  AK_JSON_OK &&
              strcmp(clip, "\xe4\xbd\xa0\xe5\xa5\xbd") == 0,
          "clip keeps whole UTF-8 characters");

    /* FW-06: numbers wrap no more; range and form are enforced. */
    CHECK(!ak_rx_parse_frame("{\"v\":1,\"type\":\"ack\",\"seq\":4294967297}", &f), "FW-06: seq overflow rejected");
    CHECK(ak_rx_parse_frame("{\"v\":1,\"type\":\"ack\",\"seq\":4294967295}", &f) && f.seq == 4294967295U,
          "seq at UINT32_MAX accepted");
    CHECK(ak_json_get_uint("{\"volume\":101}", "volume", 100U, &value) == AK_JSON_INVALID, "value above max rejected");
    CHECK(ak_json_get_uint("{\"volume\":100}", "volume", 100U, &value) == AK_JSON_OK && value == 100U,
          "value at max accepted");
    CHECK(ak_json_get_uint("{\"volume\":-1}", "volume", 100U, &value) == AK_JSON_INVALID, "negative rejected");
    CHECK(ak_json_get_uint("{\"volume\":1.5}", "volume", 100U, &value) == AK_JSON_INVALID, "fraction rejected");
    CHECK(ak_json_get_uint("{\"volume\":07}", "volume", 100U, &value) == AK_JSON_INVALID, "leading zero rejected");
    CHECK(ak_json_get_uint("{\"volume\":\"7\"}", "volume", 100U, &value) == AK_JSON_INVALID, "string number rejected");

    /* FW-06: oversized lines are dropped whole and counted. */
    ak_rx_reader_init(&uart, AK_RX_SRC_UART);
    g_nlines = 0;
    char big[AK_RX_LINE_MAX + 64];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    feed_str(&uart, big);
    feed_str(&uart, "\n{\"v\":1,\"type\":\"ping\"}\n");
    CHECK(g_nlines == 1 && uart.dropped_lines == 1U && admitted(g_lines[0], AK_RX_SRC_UART, type, sizeof(type)),
          "oversized line dropped, next line intact");

    /* BLE lines are sealed, so the BLE reader allows up to
     * AK_RX_SEALED_LINE_MAX - 1 chars; UART keeps AK_RX_LINE_MAX - 1. */
    char sealed_big[AK_RX_SEALED_LINE_MAX + 2];
    memset(sealed_big, 'a', AK_RX_SEALED_LINE_MAX - 1U);
    sealed_big[AK_RX_SEALED_LINE_MAX - 1U] = '\n';
    sealed_big[AK_RX_SEALED_LINE_MAX]      = '\0';
    ak_rx_reader_t ble_big;
    ak_rx_reader_init(&ble_big, AK_RX_SRC_BLE);
    g_nlines = 0;
    feed_str(&ble_big, sealed_big);
    CHECK(g_nlines == 1 && ble_big.dropped_lines == 0U, "BLE reader keeps a sealed-size line");
    ak_rx_reader_t uart_big;
    ak_rx_reader_init(&uart_big, AK_RX_SRC_UART);
    feed_str(&uart_big, sealed_big);
    CHECK(g_nlines == 1 && uart_big.dropped_lines == 1U, "UART reader still drops it");
    sealed_big[AK_RX_SEALED_LINE_MAX - 1U] = 'a';
    sealed_big[AK_RX_SEALED_LINE_MAX]      = '\n';
    sealed_big[AK_RX_SEALED_LINE_MAX + 1U] = '\0';
    feed_str(&ble_big, sealed_big);
    CHECK(g_nlines == 1 && ble_big.dropped_lines == 1U, "BLE reader drops a longer line");

    /* A NUL byte would hide the rest of the line from the parser. */
    g_nlines = 0;
    static const uint8_t with_nul[] = "{\"v\":1,\"type\":\"ping\"}\0{\"v\":1,\"type\":\"wifi\"}\n";
    ak_rx_reader_feed(&uart, with_nul, sizeof(with_nul) - 1U, capture_line);
    CHECK(g_nlines == 0 && uart.dropped_lines == 2U, "line with NUL dropped");

    /* The largest display frame the Companion sends fits the line limit. */
    char text[128];
    char agent[32];
    char line[AK_RX_LINE_MAX + 1];
    memset(text, 'x', sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';
    memset(agent, 'p', sizeof(agent) - 1);
    agent[sizeof(agent) - 1] = '\0';
    int n = snprintf(line, sizeof(line),
                     "{\"v\":1,\"type\":\"display\",\"payload\":{\"chat_slot\":3,\"agent_name\":\"%s\","
                     "\"state\":\"waiting_for_user\",\"text\":\"%s\"}}\n",
                     agent, text);
    g_nlines = 0;
    feed_str(&uart, line);
    CHECK(n > 0 && (size_t)n < AK_RX_LINE_MAX && g_nlines == 1 && ak_rx_parse_frame(g_lines[0], &f),
          "max display frame fits the line limit");
}

/* FW-08: BLE RX ring. One writer per index, whole-write drop, and a
 * disconnect cut so one central's bytes never reach the next one. */
static void test_byte_ring(void)
{
    printf("test_byte_ring\n");
    static ak_byte_ring_t r;
    uint8_t               out[AK_BYTE_RING_SIZE + 8];
    ak_byte_ring_init(&r);

    CHECK(ak_byte_ring_push(&r, (const uint8_t *)"abc", 3) && ak_byte_ring_pop(&r, out, sizeof(out)) == 3U &&
              memcmp(out, "abc", 3) == 0,
          "push / pop");
    CHECK(!ak_byte_ring_take_cut(&r), "no cut without a disconnect");

    /* A write that does not fit is dropped whole, never half-queued, and the
     * producer never moves the consumer's tail. */
    uint8_t big[AK_BYTE_RING_SIZE];
    memset(big, 'x', sizeof(big));
    CHECK(ak_byte_ring_push(&r, big, AK_BYTE_RING_SIZE - 4U), "fill to 4 bytes free");
    uint32_t tail_before = r.tail;
    CHECK(!ak_byte_ring_push(&r, (const uint8_t *)"12345", 5) && r.dropped == 1U && r.tail == tail_before,
          "FW-08: oversize write dropped whole; tail untouched");
    CHECK(ak_byte_ring_push(&r, (const uint8_t *)"1234", 4), "exact fit accepted");
    CHECK(ak_byte_ring_pop(&r, out, sizeof(out)) == AK_BYTE_RING_SIZE && memcmp(out + AK_BYTE_RING_SIZE - 4U, "1234", 4) == 0,
          "full ring drains in order");

    /* FW-01/FW-08: half line from central A, disconnect, line from central B. */
    ak_byte_ring_push(&r, (const uint8_t *)"{\"v\":1,\"type\":\"wif", 18);
    ak_byte_ring_cut(&r);
    ak_byte_ring_push(&r, (const uint8_t *)"{\"v\":1,\"type\":\"ping\"}\n", 23);
    uint32_t n = ak_byte_ring_pop(&r, out, sizeof(out));
    CHECK(n == 23U && memcmp(out, "{\"v\":1,\"type\":\"ping\"}\n", 23) == 0, "FW-08: bytes before the cut are discarded");
    CHECK(ak_byte_ring_take_cut(&r) && !ak_byte_ring_take_cut(&r), "cut reported once");

    /* A cut with an already-drained ring discards nothing new. */
    ak_byte_ring_cut(&r);
    ak_byte_ring_push(&r, (const uint8_t *)"z", 1);
    CHECK(ak_byte_ring_pop(&r, out, sizeof(out)) == 1U && out[0] == 'z' && ak_byte_ring_take_cut(&r),
          "cut on an empty ring keeps later bytes");

    /* Free-running indices survive uint32 wrap. */
    ak_byte_ring_init(&r);
    r.head = r.tail = r.cut_at = 0xFFFFFFF0U;
    CHECK(ak_byte_ring_push(&r, big, 40) && ak_byte_ring_pop(&r, out, sizeof(out)) == 40U, "index wrap");
}

/* LED power setting on top of the FW-05 sequence. The sim mirrors ak_leds.c:
 * the policy output picks the frame (buffer or zeros) and whether it wants
 * power; ak_led_power decides when P18 moves and when a frame may go out. */
typedef struct {
    ak_led_policy_t pol;
    ak_led_power_t  pwr;
    bool            buf_lit;   /* effect buffer has a non-zero channel */
    int             sent;      /* frames clocked out */
    bool            sent_dark; /* last frame was all zero on the wire */
} led_sim_t;

static void led_sim_send(led_sim_t *s, uint32_t t)
{
    s->sent++;
    s->sent_dark = ak_led_policy_output(&s->pol) == AK_LED_OUT_DARK || !s->buf_lit;
    ak_led_power_sent(&s->pwr, t);
}

static void led_sim_show(led_sim_t *s, uint32_t t)
{
    bool lit = ak_led_policy_frame_lit(ak_led_policy_output(&s->pol), s->buf_lit);
    if (ak_led_power_frame(&s->pwr, lit, t)) {
        led_sim_send(s, t);
    }
}

static void led_sim_tick(led_sim_t *s, uint32_t t)
{
    if (ak_led_policy_tick(&s->pol, t)) {
        led_sim_show(s, t);
        return;
    }
    if (ak_led_power_tick(&s->pwr, t)) {
        led_sim_send(s, t);
    }
}

/* Run the 1 ms loop over [from, to). */
static void led_sim_run(led_sim_t *s, uint32_t from, uint32_t to)
{
    for (uint32_t t = from; t != to; t++) {
        led_sim_tick(s, t);
    }
}

static void led_sim_init(led_sim_t *s, ak_led_mode_t mode, uint8_t idx, uint32_t t)
{
    memset(s, 0, sizeof(*s));
    ak_led_power_init(&s->pwr);
    ak_led_policy_init(&s->pol, mode, idx, t);
}

/* An input event as tuya_app_main handles it: wake first, then handle it. */
static int led_sim_event(led_sim_t *s, uint32_t t, int handled)
{
    if (ak_led_policy_activity(&s->pol, t)) {
        led_sim_show(s, t);
    }
    return handled + 1;
}

static void test_led_policy(void)
{
    printf("test_led_policy\n");
    led_sim_t s;
    uint32_t  t = 5000U;

    /* Defaults and labels. */
    led_sim_init(&s, AK_LED_MODE_DEFAULT, AK_LED_TIMEOUT_IDX_DEFAULT, t);
    CHECK(s.pol.mode == AK_LED_MODE_AUTO && ak_led_policy_timeout_ms(s.pol.timeout_idx) == 60000U,
          "default is Auto with a 60 s auto-off");
    CHECK(ak_led_policy_output(&s.pol) == AK_LED_OUT_SHOW, "Auto starts awake");
    CHECK(ak_led_policy_timeout_ms(0) == 30000U && ak_led_policy_timeout_ms(2) == 300000U &&
              ak_led_policy_timeout_ms(3) == 900000U,
          "auto-off choices 30 s / 5 min / 15 min");
    CHECK(strcmp(ak_led_mode_str(AK_LED_MODE_OFF), "Off") == 0 && strcmp(ak_led_policy_timeout_str(3), "15 min") == 0,
          "labels");

    /* Auto: lit effect powers up, then the idle timeout powers it down. */
    s.buf_lit = true;
    led_sim_show(&s, t);
    led_sim_run(&s, t, t + AK_LED_SETTLE_MS + 1U);
    CHECK(s.sent == 1 && !s.sent_dark && s.pwr.enable, "Auto: effect shown after the settle time");
    led_sim_run(&s, t + AK_LED_SETTLE_MS + 1U, t + 60000U);
    CHECK(s.pwr.enable && ak_led_policy_output(&s.pol) == AK_LED_OUT_SHOW, "still on just before the timeout");
    led_sim_run(&s, t + 60000U, t + 60000U + 5U);
    CHECK(s.sent == 2 && s.sent_dark && !s.pwr.enable && s.pwr.state == AK_LED_PWR_OFF,
          "timeout: dark frame (DIN low) first, then P18 low");
    CHECK(ak_led_policy_output(&s.pol) == AK_LED_OUT_DARK, "idle keeps the chain dark");

    /* Effect updates while idle are kept, not shown, and do not wake it. */
    t += 70000U;
    led_sim_show(&s, t);
    led_sim_run(&s, t, t + 500U);
    CHECK(s.sent == 2 && !s.pwr.enable, "effect change while idle stays dark");

    /* A key wakes it; the key itself is still handled; the kept effect returns
     * after the power-up sequence (settle before the first frame). */
    t += 500U;
    int handled = led_sim_event(&s, t, 0);
    CHECK(handled == 1, "the waking event is still handled");
    CHECK(ak_led_policy_output(&s.pol) == AK_LED_OUT_SHOW && s.pwr.enable && s.pwr.state == AK_LED_PWR_SETTLING,
          "wake: P18 high, no data yet");
    led_sim_run(&s, t, t + AK_LED_SETTLE_MS);
    CHECK(s.sent == 2, "no frame before VDD_LED settles");
    led_sim_run(&s, t + AK_LED_SETTLE_MS, t + AK_LED_SETTLE_MS + 1U);
    CHECK(s.sent == 3 && !s.sent_dark, "kept effect shown after the settle time");
    handled = led_sim_event(&s, t + 20U, handled);
    CHECK(handled == 2 && s.sent == 3, "an event while awake only restarts the timer");

    /* Activity restarts the idle timer. */
    t += 20U;
    led_sim_run(&s, t, t + 59000U);
    (void)led_sim_event(&s, t + 59000U, 0);
    led_sim_run(&s, t + 59000U, t + 61000U);
    CHECK(s.pwr.enable, "activity at 59 s: still on at 61 s");
    led_sim_run(&s, t + 61000U, t + 119005U);
    CHECK(!s.pwr.enable, "off 60 s after the last activity");

    /* Wake right after power-off: the event is not lost, the frame waits for
     * the switch to finish turning off, then the normal power-up. */
    t += 119005U;
    led_sim_init(&s, AK_LED_MODE_AUTO, 0U, t);
    s.buf_lit = true;
    led_sim_show(&s, t);
    led_sim_run(&s, t, t + 30005U); /* 30 s auto-off */
    CHECK(!s.pwr.enable && s.sent == 2, "30 s auto-off");
    t += 30005U;
    (void)led_sim_event(&s, t, 0);
    CHECK(!s.pwr.enable && s.pwr.frame_pending, "woken during the switch-off hold: frame kept");
    led_sim_run(&s, t, t + AK_LED_OFF_HOLD_MS + AK_LED_SETTLE_MS + 5U);
    CHECK(s.pwr.enable && s.sent == 3 && !s.sent_dark, "shown after hold + settle");

    /* Off: the chain is cut and every effect request is ignored. */
    t += 1000U;
    if (ak_led_policy_set_mode(&s.pol, AK_LED_MODE_OFF, t)) {
        led_sim_show(&s, t);
    }
    led_sim_run(&s, t, t + 5U);
    CHECK(s.sent == 4 && s.sent_dark && !s.pwr.enable, "Off: dark frame, then P18 low");
    t += 5U;
    led_sim_show(&s, t);
    CHECK(!ak_led_policy_activity(&s.pol, t), "Off: activity does not wake");
    led_sim_run(&s, t, t + 2000U);
    CHECK(s.sent == 4 && !s.pwr.enable, "Off: effect requests ignored");

    /* On: powered all the time, even with a dark buffer and past any timeout. */
    t += 2000U;
    s.buf_lit = false;
    if (ak_led_policy_set_mode(&s.pol, AK_LED_MODE_ON, t)) {
        led_sim_show(&s, t);
    }
    led_sim_run(&s, t, t + 1000000U);
    CHECK(s.pwr.enable && s.pwr.state == AK_LED_PWR_ON && s.sent == 5 && s.sent_dark,
          "On: powered with a dark buffer for > 15 min");
    t += 1000000U;

    /* On -> Auto with a dark buffer powers down (FW-05 behaviour). */
    if (ak_led_policy_set_mode(&s.pol, AK_LED_MODE_AUTO, t)) {
        led_sim_show(&s, t);
    }
    led_sim_show(&s, t); /* the caller always re-shows after a commit */
    led_sim_run(&s, t, t + 5U);
    CHECK(!s.pwr.enable, "Auto with a dark buffer: off");

    /* Test page override: shows frames even in Off. */
    t += 5U;
    (void)ak_led_policy_set_mode(&s.pol, AK_LED_MODE_OFF, t);
    s.buf_lit = true;
    CHECK(ak_led_policy_set_override(&s.pol, true, t) && ak_led_policy_output(&s.pol) == AK_LED_OUT_SHOW,
          "override shows in Off");
    led_sim_show(&s, t);
    led_sim_run(&s, t, t + AK_LED_OFF_HOLD_MS + AK_LED_SETTLE_MS + 5U);
    CHECK(s.pwr.enable && !s.sent_dark, "override: test frame shown");
    CHECK(!ak_led_policy_tick(&s.pol, t + 2000000U), "override never times out");
    CHECK(ak_led_policy_set_override(&s.pol, false, t) && ak_led_policy_output(&s.pol) == AK_LED_OUT_DARK,
          "override off: back to Off");

    /* Wrap-safe idle timer. */
    ak_led_policy_t w;
    ak_led_policy_init(&w, AK_LED_MODE_AUTO, 0U, 0xFFFFF000U);
    CHECK(!ak_led_policy_tick(&w, 0x00000100U), "no timeout across the clock wrap");
    CHECK(ak_led_policy_tick(&w, 0xFFFFF000U + 30000U), "times out 30 s later across the wrap");

    /* KV blob. */
    uint8_t       blob[AK_LED_POLICY_BLOB_LEN];
    ak_led_mode_t m   = AK_LED_MODE_AUTO;
    uint8_t       idx = 0U;
    ak_led_policy_to_blob(AK_LED_MODE_ON, 3U, blob);
    CHECK(ak_led_policy_from_blob(blob, sizeof(blob), &m, &idx) && m == AK_LED_MODE_ON && idx == 3U,
          "blob round trip");
    CHECK(!ak_led_policy_from_blob(blob, sizeof(blob) - 1U, &m, &idx), "short blob rejected");
    blob[0] = 2U;
    CHECK(!ak_led_policy_from_blob(blob, sizeof(blob), &m, &idx), "unknown version rejected");
    ak_led_policy_to_blob((ak_led_mode_t)3, 0U, blob);
    CHECK(!ak_led_policy_from_blob(blob, sizeof(blob), &m, &idx), "bad mode rejected");
    ak_led_policy_to_blob(AK_LED_MODE_OFF, AK_LED_TIMEOUT_COUNT, blob);
    CHECK(!ak_led_policy_from_blob(blob, sizeof(blob), &m, &idx), "bad timeout rejected");
}

/* FW-05: v0.3.2 LED chain power switch. DIN must never carry data while the
 * switch is off or settling; the switch goes on before the first frame and
 * off only after the last (dark) frame has left the wire. */
static void test_led_power(void)
{
    printf("test_led_power\n");
    ak_led_power_t p;
    ak_led_power_init(&p);
    uint32_t t = 1000U;
    CHECK(!p.enable && p.state == AK_LED_PWR_OFF && !ak_led_power_din_may_drive(&p), "boots off, DIN idle");

    /* A dark frame while off sends nothing and keeps the switch off. */
    CHECK(!ak_led_power_frame(&p, false, t) && !p.enable && !p.frame_pending, "dark frame while off: no-op");

    /* Lit frame: switch on first, DIN stays idle during the settle time. */
    CHECK(!ak_led_power_frame(&p, true, t), "lit frame is not sent while the switch is off");
    CHECK(p.enable && p.state == AK_LED_PWR_SETTLING && !ak_led_power_din_may_drive(&p),
          "LED_PWR_EN high before any data; DIN idle");
    CHECK(!ak_led_power_tick(&p, t + AK_LED_SETTLE_MS - 1U) && !ak_led_power_din_may_drive(&p),
          "no data before VDD_LED has settled");
    CHECK(ak_led_power_tick(&p, t + AK_LED_SETTLE_MS) && ak_led_power_din_may_drive(&p), "first frame after settle");
    ak_led_power_sent(&p, t + AK_LED_SETTLE_MS);
    t += AK_LED_SETTLE_MS;

    /* Frame gap: back-to-back frames wait for AK_LED_FRAME_GAP_MS. */
    CHECK(!ak_led_power_frame(&p, true, t) && p.frame_pending, "second frame in the same ms is deferred");
    CHECK(ak_led_power_tick(&p, t + AK_LED_FRAME_GAP_MS), "deferred frame goes out after the gap");
    ak_led_power_sent(&p, t + AK_LED_FRAME_GAP_MS);
    t += AK_LED_FRAME_GAP_MS;

    /* All dark: the zero frame is sent while powered, then the switch drops. */
    t += 5U;
    CHECK(ak_led_power_frame(&p, false, t) && p.enable, "zero frame is sent with the chain still powered");
    ak_led_power_sent(&p, t);
    CHECK(p.state == AK_LED_PWR_STOPPING && p.enable, "switch stays on while the zero frame leaves");
    CHECK(!ak_led_power_tick(&p, t + AK_LED_FRAME_GAP_MS - 1U) || p.enable, "not cut mid-frame");
    ak_led_power_tick(&p, t + AK_LED_FRAME_GAP_MS);
    CHECK(!p.enable && p.state == AK_LED_PWR_OFF && !ak_led_power_din_may_drive(&p),
          "DIN idle, then LED_PWR_EN low");
    uint32_t off_at = t + AK_LED_FRAME_GAP_MS;

    /* Re-power waits for the switch to turn fully off. */
    CHECK(!ak_led_power_frame(&p, true, off_at + 10U) && !p.enable, "re-power held during the off time");
    CHECK(!ak_led_power_tick(&p, off_at + AK_LED_OFF_HOLD_MS - 1U) && !p.enable, "still held just before 200 ms");
    ak_led_power_tick(&p, off_at + AK_LED_OFF_HOLD_MS);
    CHECK(p.enable && p.state == AK_LED_PWR_SETTLING, "switch on again after the off time");

    /* Going dark during the settle time turns the switch off without data. */
    uint32_t s = off_at + AK_LED_OFF_HOLD_MS + 2U;
    CHECK(!ak_led_power_frame(&p, false, s) && !p.enable && p.state == AK_LED_PWR_OFF, "dark while settling: off, nothing sent");

    /* Lit again while stopping cancels the power-down. */
    ak_led_power_init(&p);
    ak_led_power_frame(&p, true, 0U);
    ak_led_power_tick(&p, AK_LED_SETTLE_MS);
    ak_led_power_sent(&p, AK_LED_SETTLE_MS);
    ak_led_power_frame(&p, false, AK_LED_SETTLE_MS + 2U);
    ak_led_power_sent(&p, AK_LED_SETTLE_MS + 2U);
    CHECK(ak_led_power_frame(&p, true, AK_LED_SETTLE_MS + 3U) && p.enable && p.state == AK_LED_PWR_ON,
          "lit frame during stopping keeps the chain on");

    /* uint32 clock wrap during the settle time. */
    ak_led_power_init(&p);
    uint32_t w = 0xFFFFFFFFU - 3U;
    ak_led_power_frame(&p, true, w);
    CHECK(!ak_led_power_tick(&p, w + 5U), "settle spans the wrap");
    CHECK(ak_led_power_tick(&p, w + AK_LED_SETTLE_MS), "settle ends after the wrap");
}

/* Pairing window (config-ui-design §4.7). Provisioning commands must be
 * refused outside the window and from BLE inside it. */
static void test_provision(void)
{
    printf("test_provision\n");
    static const ak_prov_cmd_t cmds[] = {AK_PROV_CMD_WIFI, AK_PROV_CMD_LICENSE, AK_PROV_CMD_LICENSE_READ,
                                         AK_PROV_CMD_RESET};
    ak_provision_t p;
    ak_provision_init(&p);
    uint32_t t = 1000U;

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        CHECK(ak_provision_check(&p, cmds[i], AK_RX_SRC_UART, false, t) == AK_PROV_DENY_CLOSED,
              "closed window refuses every provisioning command over UART");
        CHECK(ak_provision_check(&p, cmds[i], AK_RX_SRC_BLE, false, t) == AK_PROV_DENY_SOURCE,
              "BLE refused even before the window check");
    }

    /* Dialog: Cancel is preselected; a click on it closes. */
    ak_provision_request(&p, t);
    CHECK(p.state == AK_PROV_CONFIRMING && !p.choice_confirm, "request shows dialog with Cancel selected");
    CHECK(ak_provision_check(&p, AK_PROV_CMD_WIFI, AK_RX_SRC_UART, false, t) == AK_PROV_DENY_CLOSED,
          "dialog alone does not open the window");
    CHECK(!ak_provision_click(&p, t) && p.state == AK_PROV_CLOSED, "click on Cancel closes");

    /* Unanswered dialog times out to CLOSED. */
    ak_provision_request(&p, t);
    CHECK(!ak_provision_tick(&p, t + AK_PROV_CONFIRM_MS - 1U), "dialog still up before timeout");
    CHECK(ak_provision_tick(&p, t + AK_PROV_CONFIRM_MS) && p.state == AK_PROV_CLOSED, "dialog times out");
    CHECK(!ak_provision_confirm(&p, t + AK_PROV_CONFIRM_MS), "K6 after timeout does not open");

    /* Rotate to Confirm and click, or press K6: window opens for 180 s. */
    ak_provision_request(&p, t);
    ak_provision_select(&p, true);
    CHECK(ak_provision_click(&p, t + 10U) && ak_provision_is_open(&p, t + 10U), "click on Confirm opens");
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        CHECK(ak_provision_check(&p, cmds[i], AK_RX_SRC_UART, false, t + 20U) ==
                  (cmds[i] == AK_PROV_CMD_RESET ? AK_PROV_NEEDS_DEVICE_CONFIRM : AK_PROV_ALLOW),
              "open + UART allowed (reset only after a device confirm)");
        CHECK(ak_provision_check(&p, cmds[i], AK_RX_SRC_BLE, false, t + 20U) == AK_PROV_DENY_SOURCE,
              "open + BLE still refused");
    }
    CHECK(ak_provision_remaining_s(&p, t + 10U) == AK_PROV_WINDOW_MS / 1000U, "180 s on the clock");
    CHECK(ak_provision_check(&p, AK_PROV_CMD_WIFI, AK_RX_SRC_UART, false, t + 10U + AK_PROV_WINDOW_MS) ==
              AK_PROV_DENY_CLOSED,
          "refused at the deadline even before tick");
    CHECK(ak_provision_tick(&p, t + 10U + AK_PROV_WINDOW_MS) && p.state == AK_PROV_CLOSED, "window expires");

    /* K6 path and explicit exit. */
    ak_provision_request(&p, t);
    CHECK(ak_provision_confirm(&p, t), "K6 opens");
    ak_provision_close(&p);
    CHECK(ak_provision_check(&p, AK_PROV_CMD_LICENSE, AK_RX_SRC_UART, false, t) == AK_PROV_DENY_CLOSED,
          "exit pairing mode closes");

    /* cloud_ready shortens the window to 5 s, never lengthens it. */
    ak_provision_open(&p, t);
    ak_provision_on_cloud_ready(&p, t + 1000U);
    CHECK(ak_provision_is_open(&p, t + 1000U + AK_PROV_CLOSE_AFTER_READY_MS - 1U), "open until 5 s after ready");
    CHECK(!ak_provision_is_open(&p, t + 1000U + AK_PROV_CLOSE_AFTER_READY_MS), "closed 5 s after ready");
    ak_provision_open(&p, t);
    ak_provision_on_cloud_ready(&p, t + AK_PROV_WINDOW_MS - 1000U);
    CHECK(ak_provision_remaining_s(&p, t + AK_PROV_WINDOW_MS - 1000U) == 1U, "ready near the end keeps the deadline");
    ak_provision_close(&p);
    ak_provision_on_cloud_ready(&p, t);
    CHECK(p.state == AK_PROV_CLOSED, "cloud_ready does not open a closed window");

    /* uint32 millisecond wrap: the deadline crosses 0. */
    uint32_t near_wrap = 0xFFFFFFFFU - 1000U;
    ak_provision_open(&p, near_wrap);
    CHECK(ak_provision_is_open(&p, near_wrap + 2000U), "open across the clock wrap");
    CHECK(ak_provision_check(&p, AK_PROV_CMD_WIFI, AK_RX_SRC_UART, false, near_wrap + 2000U) == AK_PROV_ALLOW,
          "allowed across the clock wrap");
    CHECK(!ak_provision_is_open(&p, near_wrap + AK_PROV_WINDOW_MS), "expires after the wrap");
    CHECK(ak_provision_tick(&p, near_wrap + AK_PROV_WINDOW_MS + 1U), "tick closes after the wrap");

    /* FW-02: an existing License is not overwritten unless the user armed a
     * one-time overwrite on the device, and the permission expires. */
    ak_provision_open(&p, t);
    CHECK(ak_provision_check(&p, AK_PROV_CMD_LICENSE, AK_RX_SRC_UART, true, t) == AK_PROV_DENY_EXISTS,
          "FW-02: overwrite refused without device permission");
    CHECK(ak_provision_check(&p, AK_PROV_CMD_LICENSE, AK_RX_SRC_UART, false, t) == AK_PROV_ALLOW,
          "first License write allowed in the window");
    CHECK(ak_provision_arm_overwrite(&p, t), "arm overwrite inside the window");
    CHECK(ak_provision_check(&p, AK_PROV_CMD_LICENSE, AK_RX_SRC_UART, true, t + 1U) == AK_PROV_ALLOW,
          "armed overwrite allowed");
    CHECK(ak_provision_check(&p, AK_PROV_CMD_LICENSE, AK_RX_SRC_BLE, true, t + 1U) == AK_PROV_DENY_SOURCE,
          "armed overwrite still refused from BLE");
    ak_provision_consume_overwrite(&p);
    CHECK(ak_provision_check(&p, AK_PROV_CMD_LICENSE, AK_RX_SRC_UART, true, t + 2U) == AK_PROV_DENY_EXISTS,
          "overwrite permission is single use");
    ak_provision_arm_overwrite(&p, t);
    CHECK(ak_provision_tick(&p, t + AK_PROV_OVERWRITE_MS) &&
              ak_provision_check(&p, AK_PROV_CMD_LICENSE, AK_RX_SRC_UART, true, t + AK_PROV_OVERWRITE_MS) ==
                  AK_PROV_DENY_EXISTS,
          "overwrite permission expires after 60 s");
    ak_provision_close(&p);
    CHECK(!ak_provision_arm_overwrite(&p, t), "cannot arm overwrite with the window closed");

    /* FW-12: reset_netcfg from the Companion runs only after K6 on the device. */
    ak_provision_open(&p, t);
    CHECK(ak_provision_check(&p, AK_PROV_CMD_RESET, AK_RX_SRC_UART, false, t) == AK_PROV_NEEDS_DEVICE_CONFIRM,
          "FW-12: reset needs the device confirm");
    ak_provision_reset_request(&p, t);
    CHECK(ak_provision_reset_pending(&p, t + 1U), "reset dialog up");
    CHECK(ak_provision_check(&p, AK_PROV_CMD_RESET, AK_RX_SRC_UART, false, t + 1U) == AK_PROV_DENY_BUSY,
          "second reset while the dialog is up refused");
    CHECK(!ak_provision_reset_resolve(&p, false, t + 2U) && !ak_provision_reset_pending(&p, t + 2U),
          "K4 cancels the reset");
    ak_provision_reset_request(&p, t);
    CHECK(ak_provision_reset_resolve(&p, true, t + 3U), "K6 runs the reset");
    ak_provision_reset_request(&p, t);
    CHECK(ak_provision_tick(&p, t + AK_PROV_RESET_CONFIRM_MS) && !ak_provision_reset_resolve(&p, true, t + AK_PROV_RESET_CONFIRM_MS),
          "unanswered reset dialog expires; a late K6 does nothing");
    ak_provision_reset_request(&p, t);
    ak_provision_close(&p);
    CHECK(!ak_provision_reset_resolve(&p, true, t + 1U), "closing the window drops a pending reset");

    CHECK(strcmp(ak_provision_state_str(AK_PROV_OPEN), "open") == 0 &&
              strcmp(ak_provision_verdict_str(AK_PROV_DENY_CLOSED), "closed") == 0,
          "wire names");
}

static void test_cred(void)
{
    printf("test_cred\n");
    const char *pid     = "abcdefgh01234567";
    const char *uuid    = "abcdefghij0123456789";
    const char *authkey = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";
    ak_cred_status_t st = ak_cred_classify(pid, uuid, authkey);
    CHECK(st.pid && st.uuid && st.authkey, "valid credentials are SET");
    st = ak_cred_classify(NULL, NULL, NULL);
    CHECK(!st.pid && !st.uuid && !st.authkey, "NULL is NOT SET");
    st = ak_cred_classify("", "", "");
    CHECK(!st.pid && !st.uuid && !st.authkey, "empty is NOT SET");
    st = ak_cred_classify("abcdefgh0123456", "abcdefghij012345678", "ABCDEFGHIJKLMNOPQRSTUVWXYZ01234");
    CHECK(!st.pid && !st.uuid && !st.authkey, "one character short is NOT SET");
    st = ak_cred_classify("abcdefgh-1234567", "abcdefghij-123456789", "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123\xc3\xa9");
    CHECK(!st.pid && !st.uuid && !st.authkey, "'-' or non-ASCII is NOT SET");
    CHECK(!ak_cred_classify("ABCDEFGH01234567", uuid, authkey).pid, "PID is lower case only");
    /* Placeholders from ak_tuya_authkey.h.example. */
    st = ak_cred_classify("xxxxxxxxxxxxxxxx", "uuidxxxxxxxxxxxxxxxx", "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
    CHECK(!st.pid && !st.uuid && !st.authkey, "template placeholders are NOT SET");
    st = ak_cred_classify(pid, uuid, NULL);
    CHECK(st.pid && st.uuid && !st.authkey, "fields are judged independently");
}

/* Voice mode (config-ui-design §5.8). Chat must never fall back to PC
 * dictation, which would type the user's words into Kiro Crew (CP-01). */
static void test_voice_mode(void)
{
    printf("test_voice_mode\n");
    ak_voice_mode_t m = AK_VOICE_MODE_ASR;
    uint8_t         b = 1U;
    CHECK(ak_voice_mode_from_blob(&b, 1U, &m) && m == AK_VOICE_MODE_CHAT, "blob 1 = Chat");
    b = 0U;
    CHECK(ak_voice_mode_from_blob(&b, 1U, &m) && m == AK_VOICE_MODE_ASR, "blob 0 = ASR");
    b = 2U;
    CHECK(!ak_voice_mode_from_blob(&b, 1U, &m), "out-of-range blob rejected");
    uint8_t two[2] = {1U, 0U};
    CHECK(!ak_voice_mode_from_blob(two, 2U, &m) && !ak_voice_mode_from_blob(NULL, 1U, &m), "wrong length rejected");
    CHECK(ak_voice_mode_to_byte(AK_VOICE_MODE_CHAT) == 1U && ak_voice_mode_to_byte(AK_VOICE_MODE_ASR) == 0U,
          "blob encode");
    CHECK(strcmp(ak_voice_mode_str(AK_VOICE_MODE_CHAT), "chat") == 0 &&
              strcmp(ak_voice_mode_str(AK_VOICE_MODE_ASR), "asr") == 0,
          "wire names");

    /* K5 decision table: {ASR, Chat} x {cloud up, down} x {route}. */
    CHECK(ak_voice_mode_ptt_action(AK_VOICE_MODE_ASR, true, true) == AK_PTT_CLOUD_HOLD, "ASR + cloud route");
    CHECK(ak_voice_mode_ptt_action(AK_VOICE_MODE_ASR, false, false) == AK_PTT_SYSTEM_DICTATION,
          "ASR falls back to PC dictation");
    CHECK(ak_voice_mode_ptt_action(AK_VOICE_MODE_ASR, true, false) == AK_PTT_SYSTEM_DICTATION, "ASR + PC route");
    CHECK(ak_voice_mode_ptt_action(AK_VOICE_MODE_CHAT, true, false) == AK_PTT_CLOUD_HOLD,
          "Chat uses the cloud even when the ASR route is PC");
    CHECK(ak_voice_mode_ptt_action(AK_VOICE_MODE_CHAT, false, false) == AK_PTT_CHAT_UNAVAILABLE &&
              ak_voice_mode_ptt_action(AK_VOICE_MODE_CHAT, false, true) == AK_PTT_CHAT_UNAVAILABLE,
          "CP-01: Chat without cloud never starts PC dictation");

    /* Reply buffer: chunks join; overflow ends in "..." on a UTF-8 boundary. */
    static ak_chat_text_t t;
    ak_chat_text_reset(&t);
    ak_chat_text_append(&t, "Hello ", 6, sizeof(t.text));
    ak_chat_text_append(&t, "world", 5, sizeof(t.text));
    CHECK(strcmp(t.text, "Hello world") == 0 && !t.truncated, "chunks are joined");
    ak_chat_text_reset(&t);
    char chunk[100];
    memset(chunk, 'a', sizeof(chunk));
    for (int i = 0; i < 12; i++) {
        ak_chat_text_append(&t, chunk, sizeof(chunk), sizeof(t.text));
    }
    CHECK(t.truncated && t.len == sizeof(t.text) - 1U && strlen(t.text) == t.len &&
              strcmp(t.text + t.len - 3U, "...") == 0,
          "overflow fills the buffer and ends with ...");
    size_t len_before = t.len;
    ak_chat_text_append(&t, "more", 4, sizeof(t.text));
    CHECK(t.len == len_before, "nothing is appended after truncation");

    /* A 3-byte character straddling the limit is dropped whole. */
    static ak_chat_text_t u;
    ak_chat_text_reset(&u);
    ak_chat_text_append(&u, "abcd", 4, 12U); /* cap 12: 8 bytes before "..." */
    ak_chat_text_append(&u, "\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96", 9, 12U);
    CHECK(u.truncated && strcmp(u.text, "abcd\xe4\xbd\xa0...") == 0, "cut at a character boundary");
}

/* BLE secure channel (FW-01 link part / FW-13): handshake, AES-CCM framing,
 * tamper / replay / reorder / unpaired rejection. The Companion side is
 * re-implemented here with mbedTLS directly as an independent peer. */
static const uint8_t k_ble_key[AK_BLE_KEY_LEN] = {
    0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
    0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
};

static int fixed_rand(void *ctx, uint8_t *out, size_t len)
{
    (void)ctx;
    for (size_t i = 0; i < len; i++) {
        out[i] = (uint8_t)(0xB0U + i); /* Nb = b0 b1 ... bf */
    }
    return 0;
}

static int failing_rand(void *ctx, uint8_t *out, size_t len)
{
    (void)ctx;
    (void)out;
    (void)len;
    return -1;
}

/* Companion-side seal with counter `ctr` and key `k` (dir C->B). */
static void peer_seal(const uint8_t k[16], uint64_t ctr, const char *pt, char *out, size_t cap)
{
    uint8_t raw[8 + AK_RX_LINE_MAX + 16];
    size_t  len = strlen(pt);
    for (int i = 7; i >= 0; i--) {
        raw[i] = (uint8_t)(ctr & 0xFFU);
        ctr >>= 8;
    }
    uint8_t nonce[13] = {AK_BLE_DIR_C2B};
    memcpy(&nonce[5], raw, 8);
    mbedtls_ccm_context c;
    mbedtls_ccm_init(&c);
    mbedtls_ccm_setkey(&c, MBEDTLS_CIPHER_ID_AES, k, 128);
    mbedtls_ccm_encrypt_and_tag(&c, len, nonce, 13, raw, 8, (const unsigned char *)pt, raw + 8, raw + 8 + len, 16);
    mbedtls_ccm_free(&c);
    size_t olen = 0;
    out[0]      = '~';
    mbedtls_base64_encode((unsigned char *)out + 1, cap - 1, &olen, raw, 8 + len + 16);
}

/* Companion-side open of a board line (dir B->C). Returns plaintext length or -1. */
static int peer_open(const uint8_t k[16], uint64_t want_ctr, const char *line, char *pt, size_t cap)
{
    uint8_t raw[3000];
    size_t  n   = 0;
    size_t  len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') {
        len--;
    }
    if (line[0] != '~' || mbedtls_base64_decode(raw, sizeof(raw), &n, (const unsigned char *)line + 1, len - 1) != 0 ||
        n < 24) {
        return -1;
    }
    uint64_t ctr = 0;
    for (int i = 0; i < 8; i++) {
        ctr = (ctr << 8) | raw[i];
    }
    if (ctr != want_ctr || n - 24 + 1 > cap) {
        return -1;
    }
    uint8_t nonce[13] = {AK_BLE_DIR_B2C};
    memcpy(&nonce[5], raw, 8);
    mbedtls_ccm_context c;
    mbedtls_ccm_init(&c);
    mbedtls_ccm_setkey(&c, MBEDTLS_CIPHER_ID_AES, k, 128);
    int rc = mbedtls_ccm_auth_decrypt(&c, n - 24, nonce, 13, raw, 8, raw + 8, (unsigned char *)pt, raw + n - 16, 16);
    mbedtls_ccm_free(&c);
    if (rc != 0) {
        return -1;
    }
    pt[n - 24] = '\0';
    return (int)(n - 24);
}

static void peer_keys(const uint8_t nc[16], const uint8_t nb[16], uint8_t c2b[16], uint8_t b2c[16])
{
    uint8_t salt[32], okm[32];
    memcpy(salt, nc, 16);
    memcpy(salt + 16, nb, 16);
    mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), salt, 32, k_ble_key, 32,
                 (const unsigned char *)"AK-BLE-v1", 9, okm, 32);
    memcpy(c2b, okm, 16);
    memcpy(b2c, okm + 16, 16);
}

/* Run the handshake up to WAIT_CONFIRM; fills the peer's session keys. */
static void ble_handshake(ak_ble_sec_t *s, uint8_t c2b[16], uint8_t b2c[16])
{
    char    id[AK_BLE_KEY_ID_HEX], hello[160], plain[512], reply[160];
    uint8_t nc[16], nb[16];
    for (int i = 0; i < 16; i++) {
        nc[i] = (uint8_t)(0xA0 + i);
        nb[i] = (uint8_t)(0xB0 + i);
    }
    ak_ble_key_id(k_ble_key, id);
    snprintf(hello, sizeof(hello),
             "{\"v\":1,\"type\":\"sec_hello\",\"payload\":{\"key_id\":\"%s\",\"nonce\":\"a0a1a2a3a4a5a6a7a8a9aaabacadaeaf\"}}",
             id);
    ak_ble_sec_reset(s);
    CHECK(ak_ble_sec_on_line(s, k_ble_key, hello, fixed_rand, NULL, plain, sizeof(plain), reply, sizeof(reply)) ==
              AK_BLE_SEC_REPLY,
          "sec_hello answered");
    CHECK(strcmp(reply, "{\"v\":1,\"type\":\"sec_hello_ack\",\"payload\":{\"nonce\":\"b0b1b2b3b4b5b6b7b8b9babbbcbdbebf\"}}\n") ==
              0,
          "sec_hello_ack carries Nb");
    peer_keys(nc, nb, c2b, b2c);
}

static void test_ble_sec(void)
{
    printf("test_ble_sec\n");
    ak_ble_sec_t s;
    char         line[600], plain[512], reply[160];
    uint8_t      c2b[16], b2c[16];

    char id[AK_BLE_KEY_ID_HEX];
    ak_ble_key_id(k_ble_key, id);
    CHECK(strlen(id) == 8U, "key_id is 8 hex");
    uint8_t blob[AK_BLE_KEY_BLOB_LEN], back[AK_BLE_KEY_LEN];
    ak_ble_key_to_blob(k_ble_key, blob);
    CHECK(ak_ble_key_from_blob(blob, sizeof(blob), back) && memcmp(back, k_ble_key, 32) == 0, "key blob round trip");
    CHECK(!ak_ble_key_from_blob(blob, sizeof(blob) - 1, back), "short blob rejected");
    blob[0] = 9;
    CHECK(!ak_ble_key_from_blob(blob, sizeof(blob), back), "unknown blob version rejected");

    /* Unpaired: everything is dropped and nothing is sealed. */
    ak_ble_sec_reset(&s);
    CHECK(ak_ble_sec_on_line(&s, NULL, "{\"v\":1,\"type\":\"sec_hello\",\"payload\":{}}", fixed_rand, NULL, plain,
                             sizeof(plain), reply, sizeof(reply)) == AK_BLE_SEC_DROP,
          "unpaired: handshake dropped");
    CHECK(ak_ble_sec_on_line(&s, NULL, "{\"v\":1,\"type\":\"wifi\"}", fixed_rand, NULL, plain, sizeof(plain), reply,
                             sizeof(reply)) == AK_BLE_SEC_DROP,
          "unpaired: plain frame dropped");
    CHECK(ak_ble_sec_seal(&s, (const uint8_t *)"{}", 2, line, sizeof(line)) < 0, "unpaired: no uplink");

    /* Before the handshake a plain command is not accepted (and kills the session). */
    ak_ble_sec_reset(&s);
    CHECK(ak_ble_sec_on_line(&s, k_ble_key, "{\"v\":1,\"type\":\"keymap\",\"payload\":{}}", fixed_rand, NULL, plain,
                             sizeof(plain), reply, sizeof(reply)) == AK_BLE_SEC_DROP &&
              s.state == AK_BLE_SEC_FAILED,
          "plaintext before handshake rejected");
    CHECK(ak_ble_sec_on_line(&s, k_ble_key, "{\"v\":1,\"type\":\"sec_hello\"}", fixed_rand, NULL, plain,
                             sizeof(plain), reply, sizeof(reply)) == AK_BLE_SEC_DROP,
          "failed session ignores a later hello");

    /* Wrong key_id, malformed nonce, no randomness: fail closed. */
    ak_ble_sec_reset(&s);
    CHECK(ak_ble_sec_on_line(&s, k_ble_key,
                             "{\"v\":1,\"type\":\"sec_hello\",\"payload\":{\"key_id\":\"00000000\",\"nonce\":"
                             "\"a0a1a2a3a4a5a6a7a8a9aaabacadaeaf\"}}",
                             fixed_rand, NULL, plain, sizeof(plain), reply, sizeof(reply)) == AK_BLE_SEC_DROP,
          "wrong key_id rejected");
    char hello[200];
    snprintf(hello, sizeof(hello), "{\"v\":1,\"type\":\"sec_hello\",\"payload\":{\"key_id\":\"%s\",\"nonce\":\"A0\"}}", id);
    ak_ble_sec_reset(&s);
    CHECK(ak_ble_sec_on_line(&s, k_ble_key, hello, fixed_rand, NULL, plain, sizeof(plain), reply, sizeof(reply)) ==
              AK_BLE_SEC_DROP,
          "malformed nonce rejected");
    snprintf(hello, sizeof(hello),
             "{\"v\":1,\"type\":\"sec_hello\",\"payload\":{\"key_id\":\"%s\",\"nonce\":\"a0a1a2a3a4a5a6a7a8a9aaabacadaeaf\"}}",
             id);
    ak_ble_sec_reset(&s);
    CHECK(ak_ble_sec_on_line(&s, k_ble_key, hello, failing_rand, NULL, plain, sizeof(plain), reply, sizeof(reply)) ==
              AK_BLE_SEC_DROP,
          "no random source: handshake refused");

    /* Happy path: first sealed frame authenticates; uplink decrypts at the peer. */
    ble_handshake(&s, c2b, b2c);
    CHECK(ak_ble_sec_seal(&s, (const uint8_t *)"{}", 2, line, sizeof(line)) < 0, "no uplink before the confirm frame");
    peer_seal(c2b, 0, "{\"v\":1,\"type\":\"hello\"}", line, sizeof(line));
    CHECK(ak_ble_sec_on_line(&s, k_ble_key, line, fixed_rand, NULL, plain, sizeof(plain), reply, sizeof(reply)) ==
                  AK_BLE_SEC_AUTH_OK &&
              strcmp(plain, "{\"v\":1,\"type\":\"hello\"}") == 0,
          "confirm frame authenticates and decrypts");
    int n = ak_ble_sec_seal(&s, (const uint8_t *)"{\"v\":1,\"type\":\"hello_ack\"}\n", 27, line, sizeof(line));
    char pt[600];
    CHECK(n > 0 && line[n - 1] == '\n' && peer_open(b2c, 0, line, pt, sizeof(pt)) > 0 &&
              strcmp(pt, "{\"v\":1,\"type\":\"hello_ack\"}") == 0,
          "uplink seals with counter 0 and the board->Companion key");
    n = ak_ble_sec_seal(&s, (const uint8_t *)"{\"v\":1}", 7, line, sizeof(line));
    CHECK(n > 0 && peer_open(b2c, 1, line, pt, sizeof(pt)) > 0, "uplink counter advances");
    CHECK(peer_open(c2b, 1, line, pt, sizeof(pt)) < 0, "direction keys differ (no reflection)");

    peer_seal(c2b, 1, "{\"v\":1,\"type\":\"ping\"}", line, sizeof(line));
    char replay[600];
    strcpy(replay, line);
    CHECK(ak_ble_sec_on_line(&s, k_ble_key, line, NULL, NULL, plain, sizeof(plain), reply, sizeof(reply)) ==
              AK_BLE_SEC_DELIVER,
          "next frame delivered");
    CHECK(ak_ble_sec_on_line(&s, k_ble_key, replay, NULL, NULL, plain, sizeof(plain), reply, sizeof(reply)) ==
                  AK_BLE_SEC_DROP &&
              s.state == AK_BLE_SEC_FAILED,
          "replayed frame rejected and ends the session");
    CHECK(ak_ble_sec_seal(&s, (const uint8_t *)"{}", 2, line, sizeof(line)) < 0, "failed session sends nothing");

    /* Reorder: counter 2 before counter 1. */
    ble_handshake(&s, c2b, b2c);
    peer_seal(c2b, 0, "{\"v\":1,\"type\":\"hello\"}", line, sizeof(line));
    ak_ble_sec_on_line(&s, k_ble_key, line, NULL, NULL, plain, sizeof(plain), reply, sizeof(reply));
    peer_seal(c2b, 2, "{\"v\":1,\"type\":\"ping\"}", line, sizeof(line));
    CHECK(ak_ble_sec_on_line(&s, k_ble_key, line, NULL, NULL, plain, sizeof(plain), reply, sizeof(reply)) ==
              AK_BLE_SEC_DROP,
          "out-of-order counter rejected");

    /* Tamper: flip one ciphertext character, the tag, or the counter. */
    static const int flips[] = {14, -3, 2};
    for (size_t i = 0; i < sizeof(flips) / sizeof(flips[0]); i++) {
        ble_handshake(&s, c2b, b2c);
        peer_seal(c2b, 0, "{\"v\":1,\"type\":\"hello\"}", line, sizeof(line));
        size_t len = strlen(line);
        size_t at  = flips[i] < 0 ? len + (size_t)flips[i] : (size_t)flips[i];
        line[at]   = line[at] == 'A' ? 'B' : 'A';
        CHECK(ak_ble_sec_on_line(&s, k_ble_key, line, NULL, NULL, plain, sizeof(plain), reply, sizeof(reply)) ==
                  AK_BLE_SEC_DROP,
              "tampered frame rejected");
    }

    /* Wrong key (another pairing): the confirm frame does not verify. */
    ble_handshake(&s, c2b, b2c);
    uint8_t other[16];
    memset(other, 0x22, sizeof(other));
    peer_seal(other, 0, "{\"v\":1,\"type\":\"hello\"}", line, sizeof(line));
    CHECK(ak_ble_sec_on_line(&s, k_ble_key, line, NULL, NULL, plain, sizeof(plain), reply, sizeof(reply)) ==
              AK_BLE_SEC_DROP,
          "frame under a different key rejected");

    /* Plaintext after authentication is not accepted either. */
    ble_handshake(&s, c2b, b2c);
    peer_seal(c2b, 0, "{\"v\":1,\"type\":\"hello\"}", line, sizeof(line));
    ak_ble_sec_on_line(&s, k_ble_key, line, NULL, NULL, plain, sizeof(plain), reply, sizeof(reply));
    CHECK(ak_ble_sec_on_line(&s, k_ble_key, "{\"v\":1,\"type\":\"ping\"}", NULL, NULL, plain, sizeof(plain), reply,
                             sizeof(reply)) == AK_BLE_SEC_DROP,
          "plaintext after authentication rejected");

    /* Known-answer vector shared with the Companion's Rust tests. */
    ble_handshake(&s, c2b, b2c);
    peer_seal(c2b, 0, "{\"v\":1,\"type\":\"hello\"}", line, sizeof(line));
    ak_ble_sec_on_line(&s, k_ble_key, line, NULL, NULL, plain, sizeof(plain), reply, sizeof(reply));
    n = ak_ble_sec_seal(&s, (const uint8_t *)"{\"v\":1,\"type\":\"pong\"}", 21, line, sizeof(line));
    char kat_c2b[600];
    peer_seal(c2b, 0, "{\"v\":1,\"type\":\"hello\"}", kat_c2b, sizeof(kat_c2b));
    CHECK(strcmp(id, "02d449a3") == 0, "KAT key_id");
    CHECK(strcmp(kat_c2b, "~AAAAAAAAAABIKsSEzamy/5IfkncB8vTiABm/vaxWxe3jgFyEDf19f7GeP8QS9g==") == 0,
          "KAT Companion->board frame 0");
    CHECK(n > 0 && strcmp(line, "~AAAAAAAAAAAPO00yq03Bk/oI8kOXUQcD/thfAhbxonLLso7p5WyqQrgb4LUy\n") == 0,
          "KAT board->Companion frame 0");

    /* The largest plaintext frame (AK_RX_LINE_MAX - 1 bytes, e.g. a full
     * keymap) still fits one sealed BLE line. */
    char big_pt[AK_RX_LINE_MAX];
    char big_line[AK_RX_SEALED_LINE_MAX + 64];
    int  m = snprintf(big_pt, sizeof(big_pt), "{\"v\":1,\"type\":\"keymap\",\"payload\":{\"pad\":\"");
    memset(big_pt + m, 'x', sizeof(big_pt) - (size_t)m - 4U);
    memcpy(big_pt + sizeof(big_pt) - 4U, "\"}}", 4);
    CHECK(strlen(big_pt) == AK_RX_LINE_MAX - 1U, "max plaintext frame built");
    ble_handshake(&s, c2b, b2c);
    peer_seal(c2b, 0, big_pt, big_line, sizeof(big_line));
    ak_rx_reader_t ble_rd;
    ak_rx_reader_init(&ble_rd, AK_RX_SRC_BLE);
    g_nlines = 0;
    feed_str(&ble_rd, big_line);
    feed_str(&ble_rd, "\n");
    CHECK(strlen(big_line) < AK_RX_SEALED_LINE_MAX && g_nlines == 1 && ble_rd.dropped_lines == 0U,
          "sealed max frame passes the BLE line reader");
    CHECK(ak_ble_sec_on_line(&s, k_ble_key, big_line, NULL, NULL, plain, sizeof(plain), reply, sizeof(reply)) ==
                  AK_BLE_SEC_AUTH_OK &&
              strcmp(plain, big_pt) == 0,
          "sealed max frame decrypts whole");
}

static void test_validate(void)
{
    printf("test_validate\n");
    CHECK(ak_wifi_ssid_valid("MyAP"), "ssid ok");
    CHECK(ak_wifi_ssid_valid("12345678901234567890123456789012"), "ssid 32 bytes ok");
    CHECK(!ak_wifi_ssid_valid("123456789012345678901234567890123"), "ssid 33 bytes rejected");
    CHECK(!ak_wifi_ssid_valid(""), "empty ssid rejected");
    CHECK(!ak_wifi_ssid_valid("a\nb"), "ssid control char rejected");
    CHECK(!ak_wifi_ssid_valid(NULL), "NULL ssid rejected");

    CHECK(ak_wifi_pass_valid(""), "open network ok");
    CHECK(!ak_wifi_pass_valid("1234567"), "pass 7 rejected");
    CHECK(ak_wifi_pass_valid("12345678"), "pass 8 ok");
    CHECK(ak_wifi_pass_valid("123456789012345678901234567890123456789012345678901234567890123"), "pass 63 ok");
    CHECK(!ak_wifi_pass_valid("1234567890123456789012345678901234567890123456789012345678901234"), "pass 64 rejected");
    CHECK(!ak_wifi_pass_valid("12345678\x01"), "pass control char rejected");
    CHECK(!ak_wifi_pass_valid("caf\xc3\xa9word"), "pass non-ASCII rejected");

    CHECK(ak_license_uuid_valid("abcdefghij0123456789"), "uuid 20 ok");
    CHECK(ak_license_uuid_valid("abcdefgh01234567"), "uuid 16 ok");
    CHECK(!ak_license_uuid_valid("abcdefghij012345678"), "uuid 19 rejected");
    CHECK(!ak_license_uuid_valid("abcdefghij01234567890"), "uuid 21 rejected");
    CHECK(!ak_license_uuid_valid("abcdefghij-123456789"), "uuid '-' rejected");
    CHECK(!ak_license_uuid_valid(""), "empty uuid rejected");
    CHECK(ak_license_authkey_valid("ABCDEFGHIJKLMNOPQRSTUVWXYZ012345"), "authkey 32 ok");
    CHECK(!ak_license_authkey_valid("ABCDEFGHIJKLMNOPQRSTUVWXYZ01234"), "authkey 31 rejected");
    CHECK(!ak_license_authkey_valid("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456"), "authkey 33 rejected");
    CHECK(!ak_license_authkey_valid(NULL), "NULL authkey rejected");
}

static void test_status_icons(void)
{
    printf("test_status_icons\n");

    CHECK(AK_ICON_W == 16U && AK_ICON_H == 16U, "status icons are 16x16");
    CHECK(AK_ICON_BYTES == 32U, "1-bpp 16x16 icon occupies 32 bytes");
    CHECK(sizeof(AK_ICON_WIFI) == AK_ICON_BYTES, "WiFi icon byte count");
    CHECK(sizeof(AK_ICON_NO_WIFI) == AK_ICON_BYTES, "no-WiFi icon byte count");
    CHECK(sizeof(AK_ICON_USB) == AK_ICON_BYTES, "USB icon byte count");
    CHECK(sizeof(AK_ICON_NO_USB) == AK_ICON_BYTES, "no-USB icon byte count");
    CHECK(sizeof(AK_ICON_BLUETOOTH) == AK_ICON_BYTES, "BLE icon byte count");
    CHECK(sizeof(AK_ICON_NO_BLUETOOTH) == AK_ICON_BYTES, "no-BLE icon byte count");
    CHECK(sizeof(AK_SESSION_ICON_SESSION) == AK_SESSION_ICON_BYTES,
          "32x32 session icon byte count");
    CHECK(memcmp(AK_ICON_WIFI, AK_ICON_NO_WIFI, AK_ICON_BYTES) != 0,
          "WiFi connected/disconnected bitmaps differ");
    CHECK(memcmp(AK_ICON_USB, AK_ICON_NO_USB, AK_ICON_BYTES) != 0,
          "USB connected/disconnected bitmaps differ");
    CHECK(memcmp(AK_ICON_BLUETOOTH, AK_ICON_NO_BLUETOOTH, AK_ICON_BYTES) != 0,
          "BLE connected/disconnected bitmaps differ");
}

static void test_missing_glyph(void)
{
    static const uint8_t expected[AK_FONT_H] = {
        0x00, 0x7E, 0x42, 0x42, 0x42, 0x42, 0x7E, 0x00,
    };

    printf("test_missing_glyph\n");
    CHECK(AK_FONT_MISSING_GLYPH == 0x7FU, "missing glyph uses reserved font slot");
    CHECK(memcmp(ak_font8x8[AK_FONT_MISSING_GLYPH], expected, sizeof(expected)) == 0,
          "unsupported non-ASCII glyph renders as an outline square");
}

/* Feed a full short press (DOWN ... UP) into the detector; returns whether a
 * double-click completed. gap_ms advances the clock before the press starts. */
static bool dclick_short_press(ak_dclick_t *dc, uint32_t *t, uint32_t gap_ms)
{
    *t += gap_ms;
    ak_dclick_feed(dc, AK_INPUT_EVENT_DOWN, *t, 0);
    *t += 30; /* short hold, below long-press threshold */
    return ak_dclick_feed(dc, AK_INPUT_EVENT_UP, *t, 30);
}

static void test_dclick(void)
{
    printf("test_dclick\n");
    ak_dclick_t dc;
    uint32_t    t = 1000;

    /* Two short presses within the window -> double-click on the 2nd release. */
    ak_dclick_init(&dc);
    CHECK(!dclick_short_press(&dc, &t, 0), "first short press: no dclick");
    CHECK(dclick_short_press(&dc, &t, 100), "second short press in window: dclick");

    /* Third quick press must not re-trigger without a fresh first click. */
    CHECK(!dclick_short_press(&dc, &t, 100), "third press alone: no dclick");
    CHECK(dclick_short_press(&dc, &t, 100), "fourth press completes a new dclick");

    /* Gap too long between the two releases -> not a double-click. */
    ak_dclick_init(&dc);
    CHECK(!dclick_short_press(&dc, &t, 0), "first press (slow test): no dclick");
    CHECK(!dclick_short_press(&dc, &t, AK_DCLICK_WINDOW_MS + 100), "gap too long: no dclick");

    /* A long press never counts, and cancels a pending first click. */
    ak_dclick_init(&dc);
    CHECK(!dclick_short_press(&dc, &t, 0), "arm with one short press");
    ak_dclick_feed(&dc, AK_INPUT_EVENT_DOWN, t, 0);
    t += 900;
    ak_dclick_feed(&dc, AK_INPUT_EVENT_LONG, t, 900);
    CHECK(!ak_dclick_feed(&dc, AK_INPUT_EVENT_UP, t, 900), "long-press release: no dclick");
    /* The long press cancelled the pending click, so the next short press only
     * re-arms rather than completing a double-click. */
    CHECK(!dclick_short_press(&dc, &t, 50), "short press after long: only re-arms");
}

static void test_settings(void)
{
    printf("test_settings\n");
    ak_settings_model_t settings;

    ak_settings_init(&settings);
    CHECK(settings.selected == AK_SETTINGS_ITEM_VOLUME, "settings starts on volume");
    CHECK(!settings.editing, "settings starts not editing");
    CHECK(settings.volume == 70U, "default volume is 70 percent");
    CHECK(settings.brightness == 100U, "default brightness is 100 percent");

    /* Two-level model: rotation while NOT editing only moves the selection and
     * never changes a value. */
    CHECK(ak_settings_handle(&settings, AK_SETTINGS_ACTION_LEFT) ==
              AK_SETTINGS_RESULT_NONE && settings.volume == 70U,
          "rotate without editing does not change the value");
    CHECK(settings.selected == AK_SETTINGS_ITEM_EXIT,
          "rotate-up from volume wraps the selection to exit");
    ak_settings_handle(&settings, AK_SETTINGS_ACTION_RIGHT);
    CHECK(settings.selected == AK_SETTINGS_ITEM_VOLUME,
          "rotate-down wraps back to volume");

    /* First click on a value item enters edit mode. */
    CHECK(ak_settings_handle(&settings, AK_SETTINGS_ACTION_CONFIRM) ==
              AK_SETTINGS_RESULT_ENTER_EDIT && settings.editing,
          "click on a value item enters edit mode");
    /* Now rotation adjusts the value, selection stays put. */
    CHECK(ak_settings_handle(&settings, AK_SETTINGS_ACTION_LEFT) ==
              AK_SETTINGS_RESULT_CHANGED && settings.volume == 60U,
          "rotate while editing lowers volume by ten");
    CHECK(settings.selected == AK_SETTINGS_ITEM_VOLUME,
          "selection does not move while editing");
    for (unsigned i = 0; i < 8U; i++) {
        ak_settings_handle(&settings, AK_SETTINGS_ACTION_LEFT);
    }
    CHECK(settings.volume == 0U, "volume clamps at zero while editing");
    /* Second click commits and leaves edit mode. */
    CHECK(ak_settings_handle(&settings, AK_SETTINGS_ACTION_CONFIRM) ==
              AK_SETTINGS_RESULT_CONFIRMED && !settings.editing,
          "second click commits and leaves edit mode");

    ak_settings_handle(&settings, AK_SETTINGS_ACTION_DOWN);
    CHECK(settings.selected == AK_SETTINGS_ITEM_BRIGHTNESS, "rotate selects brightness");
    ak_settings_handle(&settings, AK_SETTINGS_ACTION_CONFIRM); /* enter edit */
    for (unsigned i = 0; i < 12U; i++) {
        ak_settings_handle(&settings, AK_SETTINGS_ACTION_LEFT);
    }
    CHECK(settings.brightness == AK_SETTINGS_BRIGHTNESS_MIN,
          "brightness keeps a visible ten-percent minimum");
    ak_settings_handle(&settings, AK_SETTINGS_ACTION_CONFIRM); /* leave edit */
    CHECK(!settings.editing, "brightness edit committed");

    /* Voice mode: a two-value item edited like the others. */
    ak_settings_handle(&settings, AK_SETTINGS_ACTION_DOWN);
    CHECK(settings.selected == AK_SETTINGS_ITEM_VOICE_MODE && !settings.voice_chat, "voice mode defaults to ASR");
    CHECK(ak_settings_handle(&settings, AK_SETTINGS_ACTION_CONFIRM) == AK_SETTINGS_RESULT_ENTER_EDIT,
          "click edits voice mode");
    CHECK(ak_settings_handle(&settings, AK_SETTINGS_ACTION_DOWN) == AK_SETTINGS_RESULT_CHANGED && settings.voice_chat,
          "rotate flips to Chat");
    CHECK(ak_settings_handle(&settings, AK_SETTINGS_ACTION_UP) == AK_SETTINGS_RESULT_CHANGED && !settings.voice_chat,
          "rotate flips back to ASR");
    ak_settings_handle(&settings, AK_SETTINGS_ACTION_DOWN);
    CHECK(ak_settings_handle(&settings, AK_SETTINGS_ACTION_CONFIRM) == AK_SETTINGS_RESULT_CONFIRMED &&
              settings.voice_chat && settings.selected == AK_SETTINGS_ITEM_VOICE_MODE,
          "click commits Chat");

    /* LED power: Off / On / Auto, default Auto; auto-off 30 s .. 15 min, default 60 s. */
    ak_settings_handle(&settings, AK_SETTINGS_ACTION_DOWN);
    CHECK(settings.selected == AK_SETTINGS_ITEM_LED_POWER && settings.led_mode == (uint8_t)AK_LED_MODE_AUTO,
          "LED power defaults to Auto");
    CHECK(ak_settings_handle(&settings, AK_SETTINGS_ACTION_CONFIRM) == AK_SETTINGS_RESULT_ENTER_EDIT,
          "click edits LED power");
    ak_settings_handle(&settings, AK_SETTINGS_ACTION_DOWN);
    CHECK(settings.led_mode == (uint8_t)AK_LED_MODE_OFF, "Auto -> Off (wraps)");
    ak_settings_handle(&settings, AK_SETTINGS_ACTION_DOWN);
    CHECK(settings.led_mode == (uint8_t)AK_LED_MODE_ON, "Off -> On");
    ak_settings_handle(&settings, AK_SETTINGS_ACTION_UP);
    ak_settings_handle(&settings, AK_SETTINGS_ACTION_UP);
    CHECK(settings.led_mode == (uint8_t)AK_LED_MODE_AUTO, "and back to Auto");
    CHECK(ak_settings_handle(&settings, AK_SETTINGS_ACTION_CONFIRM) == AK_SETTINGS_RESULT_CONFIRMED,
          "click commits LED power");
    ak_settings_handle(&settings, AK_SETTINGS_ACTION_DOWN);
    CHECK(settings.selected == AK_SETTINGS_ITEM_LED_TIMEOUT && settings.led_timeout_idx == 1U,
          "LED auto-off defaults to 60 s");
    ak_settings_handle(&settings, AK_SETTINGS_ACTION_CONFIRM);
    CHECK(ak_settings_handle(&settings, AK_SETTINGS_ACTION_UP) == AK_SETTINGS_RESULT_CHANGED &&
              settings.led_timeout_idx == 0U,
          "auto-off down to 30 s");
    CHECK(ak_settings_handle(&settings, AK_SETTINGS_ACTION_UP) == AK_SETTINGS_RESULT_NONE &&
              settings.led_timeout_idx == 0U,
          "auto-off clamps at 30 s");
    for (unsigned i = 0; i < 5U; i++) {
        ak_settings_handle(&settings, AK_SETTINGS_ACTION_DOWN);
    }
    CHECK(settings.led_timeout_idx == AK_LED_TIMEOUT_COUNT - 1U, "auto-off clamps at 15 min");
    CHECK(ak_settings_handle(&settings, AK_SETTINGS_ACTION_CONFIRM) == AK_SETTINGS_RESULT_CONFIRMED,
          "click commits auto-off");

    /* Language row: two values like the voice mode, applied by the app on commit. */
    ak_settings_handle(&settings, AK_SETTINGS_ACTION_DOWN);
    CHECK(settings.selected == AK_SETTINGS_ITEM_LANGUAGE && settings.lang == (uint8_t)AK_LANG_ZH,
          "language row follows auto-off and defaults to 中文");
    CHECK(ak_settings_handle(&settings, AK_SETTINGS_ACTION_CONFIRM) == AK_SETTINGS_RESULT_ENTER_EDIT,
          "click edits the language");
    CHECK(ak_settings_handle(&settings, AK_SETTINGS_ACTION_DOWN) == AK_SETTINGS_RESULT_CHANGED &&
              settings.lang == (uint8_t)AK_LANG_EN,
          "rotate flips to English");
    CHECK(ak_settings_handle(&settings, AK_SETTINGS_ACTION_UP) == AK_SETTINGS_RESULT_CHANGED &&
              settings.lang == (uint8_t)AK_LANG_ZH,
          "either direction flips back");
    CHECK(ak_settings_handle(&settings, AK_SETTINGS_ACTION_CONFIRM) == AK_SETTINGS_RESULT_CONFIRMED &&
              !settings.editing,
          "click commits the language");

    /* Action items execute directly on a single click (no edit mode). */
    ak_settings_handle(&settings, AK_SETTINGS_ACTION_DOWN);
    CHECK(settings.selected == AK_SETTINGS_ITEM_CREDENTIALS, "rotate selects credentials");
    CHECK(ak_settings_handle(&settings, AK_SETTINGS_ACTION_CONFIRM) == AK_SETTINGS_RESULT_CREDENTIALS &&
              !settings.editing,
          "click on credentials opens the status page");
    ak_settings_handle(&settings, AK_SETTINGS_ACTION_DOWN);
    CHECK(settings.selected == AK_SETTINGS_ITEM_PAIRING, "rotate selects pairing mode");
    CHECK(ak_settings_handle(&settings, AK_SETTINGS_ACTION_CONFIRM) ==
              AK_SETTINGS_RESULT_PAIRING && !settings.editing,
          "click on pairing mode asks the app to open the confirm dialog");

    ak_settings_handle(&settings, AK_SETTINGS_ACTION_DOWN);
    CHECK(settings.selected == AK_SETTINGS_ITEM_TEST, "rotate selects test");
    CHECK(ak_settings_handle(&settings, AK_SETTINGS_ACTION_CONFIRM) ==
              AK_SETTINGS_RESULT_ENTER_TEST && !settings.editing,
          "click on test enters the test page directly");

    ak_settings_handle(&settings, AK_SETTINGS_ACTION_DOWN);
    CHECK(settings.selected == AK_SETTINGS_ITEM_EXIT, "rotate selects exit");
    CHECK(ak_settings_handle(&settings, AK_SETTINGS_ACTION_CONFIRM) ==
              AK_SETTINGS_RESULT_EXIT && !settings.editing,
          "click on exit exits directly");
    ak_settings_handle(&settings, AK_SETTINGS_ACTION_DOWN);
    CHECK(settings.selected == AK_SETTINGS_ITEM_VOLUME, "selection wraps downward");
    ak_settings_handle(&settings, AK_SETTINGS_ACTION_UP);
    CHECK(settings.selected == AK_SETTINGS_ITEM_EXIT, "selection wraps upward");
}

static void test_voice_route(void)
{
    printf("test_voice_route\n");
    ak_voice_route_t route;

    ak_voice_route_init(&route);
    CHECK(route.preferred == AK_VOICE_CLOUD, "Tuya ASR is preferred by default");
    CHECK(route.effective == AK_VOICE_CLOUD, "initial route advertises Tuya ASR");
    CHECK(!route.fallback, "initial route is not yet degraded");

    CHECK(ak_voice_route_refresh(&route, false), "unavailable cloud changes route");
    CHECK(route.preferred == AK_VOICE_CLOUD, "fallback preserves Tuya preference");
    CHECK(route.effective == AK_VOICE_SYSTEM, "unavailable cloud falls back to PC");
    CHECK(route.fallback, "automatic PC route is marked fallback");
    CHECK(!ak_voice_route_refresh(&route, false), "repeated unavailable poll is stable");

    CHECK(ak_voice_route_refresh(&route, true), "cloud recovery changes route");
    CHECK(route.effective == AK_VOICE_CLOUD, "cloud recovery restores Tuya ASR");
    CHECK(!route.fallback, "recovered route clears fallback");

    ak_voice_route_set_preference(&route, AK_VOICE_SYSTEM);
    CHECK(ak_voice_route_refresh(&route, true), "manual PC preference changes route");
    CHECK(route.effective == AK_VOICE_SYSTEM, "manual PC selection stays on PC");
    CHECK(!route.fallback, "manual PC selection is not a fallback");
    CHECK(!ak_voice_route_refresh(&route, false), "cloud loss cannot alter manual PC route");

    ak_voice_route_set_preference(&route, AK_VOICE_CLOUD);
    CHECK(ak_voice_route_refresh(&route, false), "cloud preference degrades when unavailable");
    CHECK(route.effective == AK_VOICE_SYSTEM && route.fallback, "requested cloud echoes effective PC fallback");
    CHECK_STR(ak_voice_source_str(AK_VOICE_CLOUD), "cloud", "cloud source string");
    CHECK_STR(ak_voice_source_str(AK_VOICE_SYSTEM), "system", "system source string");
}

static void test_gesture(void)
{
    printf("test_gesture\n");
    ak_gesture_state_t g;
    const uint32_t     W = AK_GESTURE_DOUBLE_WINDOW_MS;
    const uint32_t     L = AK_FSM_LONG_PRESS_MS;

    /* single: reported once, only after the double window expires. */
    ak_gesture_init(&g);
    CHECK(ak_gesture_feed(&g, AK_INPUT_EVENT_DOWN, 0, 0) == AK_GESTURE_NONE, "down alone: nothing");
    CHECK(ak_gesture_feed(&g, AK_INPUT_EVENT_UP, 30, 30) == AK_GESTURE_NONE, "short release: deferred");
    CHECK(ak_gesture_tick(&g, 30 + W) == AK_GESTURE_NONE, "single not before window end");
    CHECK(ak_gesture_tick(&g, 30 + W + 1) == AK_GESTURE_SINGLE, "single at window expiry");
    CHECK(ak_gesture_tick(&g, 30 + W + 100) == AK_GESTURE_NONE, "single exactly once");

    /* double: second press inside the window; no single for the pair. */
    ak_gesture_init(&g);
    ak_gesture_feed(&g, AK_INPUT_EVENT_DOWN, 0, 0);
    ak_gesture_feed(&g, AK_INPUT_EVENT_UP, 20, 20);
    CHECK(ak_gesture_feed(&g, AK_INPUT_EVENT_DOWN, 20 + W, 0) == AK_GESTURE_NONE, "second down at window edge");
    /* the second press outlasts the window: still no single while it is held */
    CHECK(ak_gesture_tick(&g, 20 + W + 100) == AK_GESTURE_NONE, "held second press blocks single");
    CHECK(ak_gesture_feed(&g, AK_INPUT_EVENT_UP, 20 + W + 150, 150) == AK_GESTURE_DOUBLE, "second release: double");
    CHECK(ak_gesture_tick(&g, 2000) == AK_GESTURE_NONE, "no single after double");

    /* long: immediate on the threshold; release reports nothing. */
    ak_gesture_init(&g);
    ak_gesture_feed(&g, AK_INPUT_EVENT_DOWN, 0, 0);
    CHECK(ak_gesture_feed(&g, AK_INPUT_EVENT_LONG, L, L) == AK_GESTURE_LONG, "long at threshold");
    CHECK(ak_gesture_feed(&g, AK_INPUT_EVENT_UP, L + 500, L + 500) == AK_GESTURE_NONE, "long release: nothing");
    CHECK(ak_gesture_tick(&g, 5000) == AK_GESTURE_NONE, "no single after long");

    /* short press, then a hold inside the window: single THEN long. */
    ak_gesture_init(&g);
    ak_gesture_feed(&g, AK_INPUT_EVENT_DOWN, 0, 0);
    ak_gesture_feed(&g, AK_INPUT_EVENT_UP, 30, 30);
    ak_gesture_feed(&g, AK_INPUT_EVENT_DOWN, 100, 0);
    CHECK(ak_gesture_feed(&g, AK_INPUT_EVENT_LONG, 100 + L, L) == AK_GESTURE_SINGLE, "single reported first");
    CHECK(ak_gesture_tick(&g, 100 + L + 10) == AK_GESTURE_LONG, "then long on next tick");
    CHECK(ak_gesture_feed(&g, AK_INPUT_EVENT_UP, 100 + L + 300, L + 300) == AK_GESTURE_NONE, "hold release: nothing");
    CHECK(ak_gesture_tick(&g, 5000) == AK_GESTURE_NONE, "nothing else owed");

    /* second press after the window but before tick(): old single, new press. */
    ak_gesture_init(&g);
    ak_gesture_feed(&g, AK_INPUT_EVENT_DOWN, 0, 0);
    ak_gesture_feed(&g, AK_INPUT_EVENT_UP, 30, 30);
    CHECK(ak_gesture_feed(&g, AK_INPUT_EVENT_DOWN, 30 + W + 5, 0) == AK_GESTURE_SINGLE, "late down flushes single");
    ak_gesture_feed(&g, AK_INPUT_EVENT_UP, 30 + W + 40, 35);
    CHECK(ak_gesture_tick(&g, 30 + W + 40 + W + 1) == AK_GESTURE_SINGLE, "second press is its own single");

    CHECK_STR(ak_gesture_str(AK_GESTURE_SINGLE), "single", "single string");
    CHECK_STR(ak_gesture_str(AK_GESTURE_DOUBLE), "double", "double string");
    CHECK_STR(ak_gesture_str(AK_GESTURE_LONG), "long", "long string");
}

static void test_keymap(void)
{
    printf("test_keymap\n");
    ak_keymap_t map;
    ak_keymap_init_defaults(&map);

    CHECK(ak_keymap_slot_for_id("k1") == 0 && ak_keymap_slot_for_id("k6") == 5 &&
              ak_keymap_slot_for_id("enc") == 6,
          "slot ids map k1..k6/enc to 0..6");
    CHECK(ak_keymap_slot_for_id("k7") == -1 && ak_keymap_slot_for_id(NULL) == -1,
          "unknown id has no slot");
    CHECK(ak_keymap_slot_for_gesture("single") == AK_GESTURE_SLOT_SINGLE &&
              ak_keymap_slot_for_gesture("double") == AK_GESTURE_SLOT_DOUBLE &&
              ak_keymap_slot_for_gesture("long") == AK_GESTURE_SLOT_LONG,
          "gesture names map to slots");
    CHECK(ak_keymap_slot_for_gesture("triple") == -1, "unknown gesture has no slot");

    /* v0.3 defaults: K5=voice; K1-K3 single=chatN double=instanceN;
     * K4 single=cancel_char long=cancel_all; K6 single=confirm. */
    CHECK(ak_keymap_is_voice(&map, "k5"), "k5 is the voice key");
    CHECK(!ak_keymap_is_voice(&map, "k1"), "k1 is a normal key");
    CHECK(ak_keymap_action(&map, "k1", AK_GESTURE_SLOT_SINGLE) == ak_action_chat(1), "k1 single=chat1");
    CHECK(ak_keymap_action(&map, "k2", AK_GESTURE_SLOT_DOUBLE) == ak_action_instance(2), "k2 double=instance2");
    CHECK(ak_keymap_action(&map, "k4", AK_GESTURE_SLOT_SINGLE) == AK_ACTION_CANCEL_CHAR, "k4 single=cancel_char");
    CHECK(ak_keymap_action(&map, "k4", AK_GESTURE_SLOT_LONG) == AK_ACTION_CANCEL_ALL, "k4 long=cancel_all");
    CHECK(ak_keymap_action(&map, "k6", AK_GESTURE_SLOT_SINGLE) == AK_ACTION_CONFIRM, "k6 single=confirm");
    CHECK(ak_keymap_action(&map, "k6", AK_GESTURE_SLOT_DOUBLE) == AK_ACTION_MODEL_TOGGLE, "k6 double=model_toggle");
    CHECK(ak_keymap_action(&map, "k6", AK_GESTURE_SLOT_LONG) == AK_ACTION_MODEL_MENU, "k6 long=model_menu");
    CHECK(ak_keymap_action(&map, "k1", AK_GESTURE_SLOT_DOUBLE) == ak_action_instance(1), "k1 double still instance1");
    CHECK(ak_keymap_action(&map, "enc", AK_GESTURE_SLOT_LONG) == AK_ACTION_NONE, "enc long left for Settings");
    CHECK_STR(ak_keymap_action_str(AK_ACTION_MODEL_MENU), "model_menu", "model_menu wire name");
    CHECK_STR(ak_keymap_action_str(AK_ACTION_MODEL_TOGGLE), "model_toggle", "model_toggle wire name");
    CHECK(ak_keymap_action_str(ak_action_chat(1)) == NULL && ak_keymap_action_str(AK_ACTION_NONE) == NULL,
          "parameterised / none actions have no fixed name");
    CHECK(ak_keymap_action(&map, "k1", AK_GESTURE_SLOT_LONG) == AK_ACTION_NONE, "k1 long unbound");
    CHECK(ak_keymap_action(&map, "zzz", AK_GESTURE_SLOT_SINGLE) == AK_ACTION_NONE, "unknown id -> none");

    /* action encode/decode. */
    CHECK(ak_action_chat_n(ak_action_chat(3)) == 3, "chat round-trips N");
    CHECK(ak_action_instance_n(ak_action_instance(9)) == 9, "instance round-trips N");
    CHECK(ak_action_chat(0) == AK_ACTION_NONE && ak_action_chat(10) == AK_ACTION_NONE, "chat N out of range -> none");
    CHECK(ak_action_chat_n(AK_ACTION_CONFIRM) == 0, "confirm is not a chat");
    CHECK(ak_action_valid(AK_ACTION_CANCEL_ALL) && ak_action_valid(ak_action_instance(1)), "valid encodings");
    CHECK(!ak_action_valid(0x42), "garbage byte is not a valid action");

    /* action_from_str: wire names parse; unknown rejected (out untouched). */
    ak_action_t a = AK_ACTION_NONE;
    CHECK(ak_keymap_action_from_str("cancel_char", &a) && a == AK_ACTION_CANCEL_CHAR, "parse cancel_char");
    CHECK(ak_keymap_action_from_str("confirm", &a) && a == AK_ACTION_CONFIRM, "parse confirm");
    CHECK(ak_keymap_action_from_str("chat5", &a) && a == ak_action_chat(5), "parse chat5");
    CHECK(ak_keymap_action_from_str("instance2", &a) && a == ak_action_instance(2), "parse instance2");
    CHECK(ak_keymap_action_from_str("none", &a) && a == AK_ACTION_NONE, "parse none");
    CHECK(ak_keymap_action_from_str("model_menu", &a) && a == AK_ACTION_MODEL_MENU, "parse model_menu");
    CHECK(ak_keymap_action_from_str("model_toggle", &a) && a == AK_ACTION_MODEL_TOGGLE, "parse model_toggle");
    CHECK(ak_action_valid(AK_ACTION_MODEL_MENU) && ak_action_valid(AK_ACTION_MODEL_TOGGLE) && !ak_action_valid(0x06),
          "model actions are valid blob bytes; 0x06 is not");
    a = AK_ACTION_CONFIRM;
    CHECK(!ak_keymap_action_from_str("chat0", &a) && a == AK_ACTION_CONFIRM, "chat0 rejected, out untouched");
    CHECK(!ak_keymap_action_from_str("bogus", &a) && a == AK_ACTION_CONFIRM, "unknown action rejected");

    /* bind / set_voice. */
    ak_keymap_t m2;
    ak_keymap_init_defaults(&m2);
    CHECK(ak_keymap_bind(&m2, "k1", AK_GESTURE_SLOT_LONG, AK_ACTION_CONFIRM), "bind k1 long=confirm");
    CHECK(ak_keymap_action(&m2, "k1", AK_GESTURE_SLOT_LONG) == AK_ACTION_CONFIRM, "k1 long now confirm");
    CHECK(ak_keymap_set_voice(&m2, "k2") && ak_keymap_is_voice(&m2, "k2"), "k2 set to voice");
    CHECK(!ak_keymap_bind(&m2, "k9", AK_GESTURE_SLOT_SINGLE, AK_ACTION_CONFIRM), "bind unknown id fails");

    /* KV blob round-trip (v2: version + 7*(kind + 3 actions) = 29 bytes). */
    uint8_t blob[AK_KEYMAP_BLOB_SIZE];
    CHECK(AK_KEYMAP_BLOB_SIZE == 29u, "blob is 29 bytes");
    CHECK(ak_keymap_to_blob(&m2, blob, sizeof(blob)) == AK_KEYMAP_BLOB_SIZE, "blob serializes");
    CHECK(blob[0] == AK_KEYMAP_BLOB_VERSION, "blob carries version byte");
    ak_keymap_t m3;
    ak_keymap_init_defaults(&m3);
    CHECK(ak_keymap_from_blob(&m3, blob, AK_KEYMAP_BLOB_SIZE), "blob loads");
    CHECK(ak_keymap_is_voice(&m3, "k2") && ak_keymap_action(&m3, "k1", AK_GESTURE_SLOT_LONG) == AK_ACTION_CONFIRM,
          "round-trip preserves voice + binding");

    /* Corrupt/wrong-version/size/action blobs are rejected, map untouched. */
    ak_keymap_t m4;
    ak_keymap_init_defaults(&m4);
    uint8_t bad[AK_KEYMAP_BLOB_SIZE];
    memcpy(bad, blob, sizeof(bad));
    bad[0] = 99;
    CHECK(!ak_keymap_from_blob(&m4, bad, AK_KEYMAP_BLOB_SIZE), "wrong version rejected");
    CHECK(!ak_keymap_from_blob(&m4, blob, AK_KEYMAP_BLOB_SIZE - 1), "wrong size rejected");
    memcpy(bad, blob, sizeof(bad));
    bad[2] = 0x42; /* first action byte -> garbage */
    CHECK(!ak_keymap_from_blob(&m4, bad, AK_KEYMAP_BLOB_SIZE), "invalid action rejected");
    memcpy(bad, blob, sizeof(bad));
    bad[1] = 7; /* kind byte -> invalid */
    CHECK(!ak_keymap_from_blob(&m4, bad, AK_KEYMAP_BLOB_SIZE), "invalid kind rejected");
    CHECK(ak_keymap_action(&m4, "k1", AK_GESTURE_SLOT_SINGLE) == ak_action_chat(1), "rejected blob left defaults");
}

/* ---- ak_font: UTF-8 decoding, glyph lookup, bit extraction, drawing ---- */

static uint16_t g_px[40][80];
static void mock_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    for (uint16_t j = y; j < y + h && j < 40; j++) {
        for (uint16_t i = x; i < x + w && i < 80; i++) {
            g_px[j][i] = color;
        }
    }
}

static void test_font(void)
{
    printf("test_font\n");

    /* UTF-8 */
    const char *p = "A\xC3\xA9\xE5\xAD\x97";
    CHECK(ak_utf8_next(&p) == 0x41U, "utf8 ascii");
    CHECK(ak_utf8_next(&p) == 0xE9U, "utf8 2-byte");
    CHECK(ak_utf8_next(&p) == 0x5B57U, "utf8 3-byte");
    CHECK(ak_utf8_next(&p) == 0U && *p == '\0', "utf8 NUL does not advance");
    const char *bad = "\xE5\xAD";
    CHECK(ak_utf8_next(&bad) == 0xFFFDU && bad[0] == (char)0xAD, "truncated seq -> U+FFFD, 1 byte");
    CHECK(ak_utf8_next(&bad) == 0xFFFDU && bad[0] == '\0', "stray continuation -> U+FFFD");
    CHECK(ak_utf8_next(&bad) == 0U, "ends at NUL");
    const char *astral = "\xF0\x9F\x98\x80";
    CHECK(ak_utf8_next(&astral) == 0x1F600U && *astral == '\0', "utf8 4-byte");

    /* Hand-made 2 bpp font: glyph 'A' is 3x2, rows {1,2,3} {0,3,1} -> 0x6C 0xD0. */
    static const uint16_t codes[] = {0x41U, 0x7FU};
    static const uint8_t  adv[]   = {3U, 3U};
    static const uint32_t offs[]  = {0U, 2U};
    static const uint8_t  data[]  = {0x6CU, 0xD0U, 0xFFU, 0xFFU};
    static const ak_font_t tiny   = {"tiny", 2U, 2U, 3U, 2U, codes, adv, offs, data};
    CHECK(ak_font_level(&tiny, 0, 0, 0) == 1U && ak_font_level(&tiny, 0, 1, 0) == 2U &&
              ak_font_level(&tiny, 0, 2, 0) == 3U, "2bpp row 0");
    CHECK(ak_font_level(&tiny, 0, 0, 1) == 0U && ak_font_level(&tiny, 0, 1, 1) == 3U &&
              ak_font_level(&tiny, 0, 2, 1) == 1U, "2bpp row 1");
    CHECK(ak_font_level(&tiny, 0, 3, 0) == 0U && ak_font_level(&tiny, 0, 0, 2) == 0U, "outside the bitmap is 0");
    CHECK(ak_font_find(&tiny, 0x41U) == 0, "find hit");
    CHECK(ak_font_find(&tiny, 0x4E00U) == 1, "find miss -> missing-glyph box");
    CHECK(ak_font_find(&tiny, 0x1F600U) == 1, "astral code point -> box");
    CHECK(ak_font_find(&tiny, 0x0AU) == 1, "control char maps to space; tiny lacks it, so the box");
    CHECK(ak_font_text_width(&tiny, "AA?") == 9U, "width counts the box for unknown chars");

    /* Blend */
    CHECK(ak_font_blend565(0xFFFFU, 0x0000U, 0U, 3U) == 0x0000U, "blend level 0 = bg");
    CHECK(ak_font_blend565(0xFFFFU, 0x0000U, 3U, 3U) == 0xFFFFU, "blend max = fg");
    uint16_t mid = ak_font_blend565(0xFFFFU, 0x0000U, 1U, 2U);
    CHECK(((mid >> 11) & 0x1FU) == 16U && ((mid >> 5) & 0x3FU) == 32U && (mid & 0x1FU) == 16U, "blend halfway");

    /* Generated fonts: every sample character is a real glyph, not the box. */
    for (unsigned f = 0; f < AK_FONT_TEST_COUNT; f++) {
        const ak_font_t *font = ak_font_test_set[f];
        int box = ak_font_find(font, 0x10FFFFU);
        CHECK(box >= 0 && font->codes[box] == 0x7FU, "font has the missing-glyph box");
        for (unsigned l = 0; l < AK_FONT_SAMPLE_LINES; l++) {
            const char *s = ak_font_sample[l];
            uint32_t    cp;
            while ((cp = ak_utf8_next(&s)) != 0U) {
                int idx = ak_font_find(font, cp);
                if (idx < 0 || idx == box || font->adv[idx] == 0U) {
                    printf("  FAIL: %s lacks U+%04X\n", font->name, (unsigned)cp);
                    g_failures++;
                    break;
                }
            }
        }
        uint8_t max = (uint8_t)((1U << font->bpp) - 1U);
        int     seen_ink = 0;
        for (int i = 0; i < font->count; i++) {
            for (uint8_t y = 0; y < font->px; y++) {
                for (uint8_t x = 0; x < font->adv[i]; x++) {
                    uint8_t lv = ak_font_level(font, i, x, y);
                    if (lv > max) {
                        CHECK(0, "level exceeds bpp range");
                    }
                    if (lv && font->codes[i] == 0x5B57U) {
                        seen_ink = 1;
                    }
                }
            }
        }
        CHECK(seen_ink, "CJK glyph has ink");
    }

    /* Fixed-width Fusion font: 6 px Latin, 12 px CJK. Drawing paints the opaque cell and clips. */
    const ak_font_t *fu = ak_font_test_set[0];
    CHECK(ak_font_text_width(fu, "Ab\xE5\xAD\x97") == 24U, "Fusion width 6+6+12");
    memset(g_px, 0, sizeof(g_px));
    uint16_t end = ak_font_draw_text(fu, 2U, 3U, "I", 0xFFFFU, 0x1234U, 80U, mock_rect);
    CHECK(end == 8U, "draw returns end x");
    CHECK(g_px[3][2] == 0x1234U || g_px[3][2] == 0xFFFFU, "cell painted");
    CHECK(g_px[3][1] == 0U && g_px[3][8] == 0U && g_px[2][2] == 0U, "nothing outside the cell");
    int fg_px = 0;
    for (int y = 3; y < 15; y++) {
        for (int x = 2; x < 8; x++) {
            fg_px += (g_px[y][x] == 0xFFFFU);
        }
    }
    CHECK(fg_px > 4, "foreground pixels drawn");
    end = ak_font_draw_text(fu, 0U, 0U, "\xE5\xAD\x97\xE5\xAD\x97", 0xFFFFU, 0U, 20U, mock_rect);
    CHECK(end == 12U, "second CJK glyph is clipped at x_limit");
    CHECK(ak_font_draw_text(NULL, 5U, 0U, "x", 1U, 0U, 80U, mock_rect) == 5U, "NULL font is a no-op");
}

/* ---- ak_menu: frame parsing, state machine, key lights ---- */

#define MK(payload) "{" payload "}"

static bool parse_page(const char *payload, ak_menu_page_t *p)
{
    return ak_menu_parse(payload, p);
}

/* Press and release `key` 10 ms apart; returns the release output. */
static ak_menu_out_t tap(ak_menu_t *m, int key, uint32_t *t)
{
    ak_menu_out_t out;
    ak_menu_key(m, key, true, *t, &out);
    *t += 10U;
    ak_menu_key(m, key, false, *t, &out);
    *t += 10U;
    return out;
}

static void open_menu(ak_menu_t *m, const char *payload, uint32_t t)
{
    ak_menu_page_t p;
    CHECK(parse_page(payload, &p), "fixture page parses");
    CHECK(ak_menu_on_page(m, &p, t) == AK_MENU_APPLY_OPENED, "fixture opens");
}

static const char k_approval[] = MK("\"id\":\"a1b2c3\",\"kind\":\"approval\",\"title\":\"Run shell?\","
                                    "\"ctx\":\"git push origin feat/x\",\"queue\":\"1/3\",\"items\":["
                                    "{\"label\":\"Approve once\",\"role\":\"confirm\"},"
                                    "{\"label\":\"Reject\",\"role\":\"cancel\"},"
                                    "{\"label\":\"Reject once\",\"role\":\"danger\"}]");

static void test_menu_parse(void)
{
    printf("test_menu_parse\n");
    ak_menu_page_t p;

    CHECK(parse_page(k_approval, &p), "spec approval example parses");
    CHECK_STR(p.id, "a1b2c3", "id");
    CHECK(p.kind == AK_MENU_KIND_APPROVAL && p.count == 3 && p.page == 1 && p.pages == 1 && !p.multi && p.def == 0 &&
              p.timeout_s == 0,
          "defaults for omitted fields");
    CHECK_STR(p.queue, "1/3", "queue");
    CHECK(p.items[0].role == AK_MENU_ROLE_CONFIRM && p.items[1].role == AK_MENU_ROLE_CANCEL &&
              p.items[2].role == AK_MENU_ROLE_DANGER,
          "roles");

    CHECK(parse_page(MK("\"id\":\"m\",\"kind\":\"mode\",\"page\":2,\"pages\":2,\"select\":\"multi\",\"default\":7,"
                        "\"timeout_s\":900,\"items\":[{\"label\":\"Trust\",\"role\":\"attention\","
                        "\"needs_double_confirm\":true,\"checked\":true}]"),
                     &p),
          "full page parses");
    CHECK(p.page == 2 && p.pages == 2 && p.multi && p.def == 7 && p.timeout_s == 900 && p.items[0].double_confirm &&
              p.items[0].checked && p.items[0].role == AK_MENU_ROLE_ATTENTION,
          "page fields");

    /* Unknown kind / role are display fallbacks, not errors. */
    CHECK(parse_page(MK("\"id\":\"x\",\"kind\":\"future\",\"items\":[{\"label\":\"a\",\"role\":\"sparkle\"}]"), &p) &&
              p.kind == AK_MENU_KIND_OPTIONS && p.items[0].role == AK_MENU_ROLE_NORMAL,
          "unknown kind -> options, unknown role -> normal");

    /* Identifier fields that are not [a-z0-9_]{1,15} drop the frame. */
    CHECK(!parse_page(MK("\"id\":\"Bad-Id\",\"kind\":\"options\",\"items\":[{\"label\":\"a\"}]"), &p), "bad id");
    CHECK(!parse_page(MK("\"id\":\"abcdefghijklmnop\",\"kind\":\"options\",\"items\":[{\"label\":\"a\"}]"), &p),
          "16-byte id");
    CHECK(!parse_page(MK("\"id\":\"\",\"kind\":\"options\",\"items\":[{\"label\":\"a\"}]"), &p), "empty id");
    CHECK(!parse_page(MK("\"id\":\"a\",\"kind\":\"Options\",\"items\":[{\"label\":\"a\"}]"), &p), "bad kind chars");
    CHECK(!parse_page(MK("\"id\":\"a\",\"kind\":\"options\",\"items\":[{\"label\":\"a\",\"role\":\"CONFIRM\"}]"), &p),
          "illegal role characters");
    CHECK(!parse_page(MK("\"id\":\"a\",\"kind\":\"options\",\"items\":[{\"label\":\"a\",\"role\":3}]"), &p),
          "non-string role");
    CHECK(!parse_page(MK("\"id\":\"a\",\"kind\":\"options\",\"select\":\"many\",\"items\":[{\"label\":\"a\"}]"), &p),
          "unknown select");

    /* Missing required fields. */
    CHECK(!parse_page(MK("\"kind\":\"options\",\"items\":[{\"label\":\"a\"}]"), &p), "missing id");
    CHECK(!parse_page(MK("\"id\":\"a\",\"items\":[{\"label\":\"a\"}]"), &p), "missing kind");
    CHECK(!parse_page(MK("\"id\":\"a\",\"kind\":\"options\""), &p), "missing items");
    CHECK(!parse_page(MK("\"id\":\"a\",\"kind\":\"options\",\"items\":[{\"role\":\"confirm\"}]"), &p), "missing label");
    CHECK(!parse_page(MK("\"id\":\"a\",\"kind\":\"options\",\"items\":[]"), &p), "no items");
    CHECK(!parse_page(MK("\"id\":\"a\",\"kind\":\"options\",\"items\":{\"label\":\"a\"}"), &p), "items not an array");
    CHECK(!parse_page(MK("\"id\":\"a\",\"kind\":\"options\",\"items\":[\"a\"]"), &p), "item not an object");

    /* Out of range numbers / wrong types. */
    CHECK(!parse_page(MK("\"id\":\"a\",\"kind\":\"options\",\"items\":[{\"label\":\"1\"},{\"label\":\"2\"},"
                         "{\"label\":\"3\"},{\"label\":\"4\"},{\"label\":\"5\"}]"),
                      &p),
          "5 items on one page (one per K2..K5)");
    CHECK(parse_page(MK("\"id\":\"a\",\"kind\":\"options\",\"more\":true,\"pages\":12,\"items\":[{\"label\":\"a\"}]"),
                     &p) &&
              p.more && p.pages == 12,
          "12 pages and the see-screen flag");
    CHECK(!parse_page(MK("\"id\":\"a\",\"kind\":\"options\",\"more\":1,\"items\":[{\"label\":\"a\"}]"), &p),
          "more must be a JSON boolean");
    CHECK(!parse_page(MK("\"id\":\"a\",\"kind\":\"options\",\"page\":3,\"pages\":2,\"items\":[{\"label\":\"a\"}]"), &p),
          "page > pages");
    CHECK(!parse_page(MK("\"id\":\"a\",\"kind\":\"options\",\"pages\":13,\"items\":[{\"label\":\"a\"}]"), &p),
          "13 pages");
    {
        uint8_t parked = 0;
        CHECK(ak_menu_parse_parked(MK("\"count\":2"), &parked) && parked == 2, "menu_parked count");
        CHECK(!ak_menu_parse_parked(MK("\"count\":100"), &parked), "menu_parked count > 99");
        CHECK(!ak_menu_parse_parked(MK(""), &parked), "menu_parked needs count");
    }
    CHECK(!parse_page(MK("\"id\":\"a\",\"kind\":\"options\",\"page\":0,\"items\":[{\"label\":\"a\"}]"), &p), "page 0");
    CHECK(!parse_page(MK("\"id\":\"a\",\"kind\":\"options\",\"timeout_s\":901,\"items\":[{\"label\":\"a\"}]"), &p),
          "timeout > 900");
    CHECK(!parse_page(MK("\"id\":\"a\",\"kind\":\"options\",\"default\":48,\"items\":[{\"label\":\"a\"}]"), &p),
          "default > 47");
    CHECK(!parse_page(MK("\"id\":\"a\",\"kind\":\"options\",\"items\":[{\"label\":\"a\",\"checked\":1}]"), &p),
          "checked must be a JSON boolean");
    CHECK(!parse_page(MK("\"id\":\"a\",\"kind\":\"options\",\"items\":[{\"label\":\"a\",\"needs_double_confirm\":"
                         "\"true\"}]"),
                      &p),
          "needs_double_confirm must be a JSON boolean");
    CHECK(!parse_page(MK("\"id\":\"a\",\"kind\":\"options\",\"title\":5,\"items\":[{\"label\":\"a\"}]"), &p),
          "non-string title");

    /* Over-long display text is clipped at a UTF-8 boundary, not rejected. */
    CHECK(parse_page(MK("\"id\":\"a\",\"kind\":\"options\",\"title\":\"0123456789012345678901234567890123456789XYZ\","
                        "\"queue\":\"12345/67890\",\"items\":[{\"label\":\"\xe7\xa1\xae\xe8\xae\xa4\xe7\xa1\xae\xe8\xae"
                        "\xa4\xe7\xa1\xae\xe8\xae\xa4\xe7\xa1\xae\xe8\xae\xa4\xe7\xa1\xae\"}]"),
                     &p),
          "long display text accepted");
    CHECK(strlen(p.title) == AK_MENU_TITLE_MAX && strlen(p.queue) == AK_MENU_QUEUE_MAX, "title / queue clipped");
    CHECK(strlen(p.items[0].label) == 24U, "9 CJK chars (27 B) clip to 8 chars (24 B)");

    /* Whole frame through the envelope: an oversized line never reaches the parser. */
    {
        char line[700];
        int  n = snprintf(line, sizeof(line), "{\"v\":1,\"type\":\"menu\",\"payload\":{\"id\":\"a\",\"kind\":\"options\","
                                              "\"ctx\":\"");
        while (n < 520) {
            line[n++] = 'x';
        }
        n += snprintf(line + n, sizeof(line) - (size_t)n, "\",\"items\":[{\"label\":\"a\"}]}}\n");
        ak_rx_reader_t r;
        ak_rx_reader_init(&r, AK_RX_SRC_UART);
        g_nlines = 0;
        ak_rx_reader_feed(&r, (const uint8_t *)line, (size_t)n, capture_line);
        CHECK(g_nlines == 0 && r.dropped_lines == 1, "menu line over 511 bytes is dropped whole");
    }

    char id[AK_MENU_ID_MAX + 1U], msg[AK_MENU_CLOSE_MSG_MAX + 1U];
    CHECK(ak_menu_parse_close(MK("\"id\":\"a1b2c3\",\"msg\":\"Approved\""), id, msg) && strcmp(id, "a1b2c3") == 0 &&
              strcmp(msg, "Approved") == 0,
          "menu_close parses");
    CHECK(ak_menu_parse_close(MK("\"id\":\"a1\""), id, msg) && msg[0] == '\0', "menu_close msg optional");
    CHECK(!ak_menu_parse_close(MK("\"msg\":\"x\""), id, msg), "menu_close needs id");
    CHECK(!ak_menu_parse_close(MK("\"id\":\"A!\""), id, msg), "menu_close bad id");

    char              text[AK_MENU_NOTICE_MAX + 1U];
    ak_notice_level_t lvl;
    CHECK(ak_menu_parse_notice(MK("\"text\":\"Crew window not found\",\"level\":\"warn\""), text, &lvl) &&
              lvl == AK_NOTICE_WARN,
          "notice warn");
    CHECK(ak_menu_parse_notice(MK("\"text\":\"hi\""), text, &lvl) && lvl == AK_NOTICE_INFO, "notice defaults info");
    CHECK(!ak_menu_parse_notice(MK("\"text\":\"hi\",\"level\":\"loud\""), text, &lvl), "notice unknown level");
    CHECK(!ak_menu_parse_notice(MK("\"level\":\"info\""), text, &lvl), "notice needs text");

    /* JSON helpers used by the parser. */
    const char *arr = NULL, *el = NULL;
    bool        b   = false;
    CHECK(ak_json_get_array("{\"a\":[1,{\"x\":2},\"s\"]}", "a", &arr) == AK_JSON_OK, "get_array");
    CHECK(ak_json_array_at(arr, 1, &el) == AK_JSON_OK && *el == '{', "array_at 1 is the object");
    CHECK(ak_json_array_at(arr, 3, &el) == AK_JSON_ABSENT, "array_at past the end");
    CHECK(ak_json_get_array("{\"a\":{}}", "a", &arr) == AK_JSON_INVALID, "object is not an array");
    CHECK(ak_json_array_at("[]", 0, &el) == AK_JSON_ABSENT, "empty array");
    CHECK(ak_json_get_bool("{\"b\":true}", "b", &b) == AK_JSON_OK && b, "bool true");
    CHECK(ak_json_get_bool("{\"b\":false}", "b", &b) == AK_JSON_OK && !b, "bool false");
    CHECK(ak_json_get_bool("{\"b\":null}", "b", &b) == AK_JSON_INVALID, "null is not a bool");
    CHECK(ak_json_get_bool("{}", "b", &b) == AK_JSON_ABSENT, "absent bool");
}

static void test_menu_keys(void)
{
    printf("test_menu_keys\n");
    ak_menu_t     m;
    ak_menu_out_t out;
    uint32_t      t = 1000U;

    ak_menu_init(&m);
    open_menu(&m, k_approval, t);
    CHECK(ak_menu_owns_keys(&m), "approval owns the keys");

    /* Anti-mistouch: presses within 300 ms are ignored, release included. */
    ak_menu_key(&m, 1, true, t + 100U, &out);
    ak_menu_key(&m, 1, false, t + 400U, &out);
    CHECK(out.kind == AK_MENU_OUT_NONE && m.state == AK_MENU_OPEN, "K2 inside the guard is ignored");
    t += 400U;

    out = tap(&m, 4, &t);
    CHECK(out.kind == AK_MENU_OUT_NONE && m.state == AK_MENU_OPEN, "K5 without an item does nothing");

    /* Approval: K6 and the knob click do nothing (decision 2026-11-17). */
    ak_menu_encoder(&m, 1, t, &out);
    CHECK(out.redraw && m.cur_idx == 1, "EC11 moves the highlight");
    out = tap(&m, 5, &t);
    CHECK(out.kind == AK_MENU_OUT_NONE && m.state == AK_MENU_OPEN, "approval: K6 is inert");
    out = tap(&m, AK_MENU_KEY_ENC, &t);
    CHECK(out.kind == AK_MENU_OUT_NONE && m.state == AK_MENU_OPEN, "approval: EC11 click is inert");
    out = tap(&m, 1, &t);
    CHECK(out.kind == AK_MENU_OUT_RESULT && out.n == 1 && out.selected[0] == 0 && out.text == NULL,
          "K2 = Approve once");
    CHECK(m.state == AK_MENU_WAIT && !ak_menu_owns_keys(&m), "after the result keys go back to the keymap");
    CHECK(ak_menu_on_close(&m, "a1b2c3") && m.state == AK_MENU_IDLE, "menu_close after result");

    /* WAIT ends by itself after 3 s. */
    open_menu(&m, k_approval, t);
    t += 300U;
    out = tap(&m, 3, &t);
    CHECK(out.kind == AK_MENU_OUT_RESULT && out.selected[0] == 2, "K4 = item 3");
    ak_menu_tick(&m, m.wait_ms + AK_MENU_WAIT_MS - 1U, &out);
    CHECK(m.state == AK_MENU_WAIT, "still waiting before 3 s");
    ak_menu_tick(&m, m.wait_ms + AK_MENU_WAIT_MS, &out);
    CHECK(m.state == AK_MENU_IDLE && out.redraw, "wait over after 3 s");

    /* ESC. */
    open_menu(&m, k_approval, t);
    t += 300U;
    out = tap(&m, 0, &t);
    CHECK(out.kind == AK_MENU_OUT_DISMISS && strcmp(out.reason, "esc") == 0, "K1 = ESC");

    /* Options: default highlight, K6 and EC11 click choose it. */
    open_menu(&m, MK("\"id\":\"d\",\"kind\":\"options\",\"default\":1,\"items\":[{\"label\":\"a\"},{\"label\":\"b\"}]"),
              t);
    CHECK(m.cur_idx == 1, "highlight starts on default");
    t += 300U;
    out = tap(&m, 5, &t);
    CHECK(out.kind == AK_MENU_OUT_RESULT && out.selected[0] == 1, "K6 = Enter on the highlight");
    open_menu(&m, MK("\"id\":\"d2\",\"kind\":\"options\",\"items\":[{\"label\":\"a\"},{\"label\":\"b\"}]"), t);
    t += 300U;
    out = tap(&m, AK_MENU_KEY_ENC, &t);
    CHECK(out.kind == AK_MENU_OUT_RESULT && out.selected[0] == 0, "EC11 click = the highlighted item");

    /* Dictate item: choosing it starts a dictation; K6 sends the text. */
    open_menu(&m, MK("\"id\":\"v\",\"kind\":\"question\",\"items\":[{\"label\":\"main\"},"
                     "{\"label\":\"Dictate\",\"role\":\"voice\"}]"),
              t);
    t += 300U;
    out = tap(&m, 4, &t);
    CHECK(out.kind == AK_MENU_OUT_NONE, "K5 is a plain item key in a menu (no item 4 here)");
    out = tap(&m, 2, &t);
    CHECK(out.kind == AK_MENU_OUT_DICTATE && m.state == AK_MENU_OPEN && m.dictate_page == 0,
          "K3 on the voice item starts a dictation; the menu stays open");
    ak_menu_dictate_cancel(&m);
    CHECK(m.dictate_page == -1, "K1 during dictation abandons it");
    ak_menu_encoder(&m, 1, t, &out);
    out = tap(&m, 5, &t);
    CHECK(out.kind == AK_MENU_OUT_DICTATE, "K6 on the highlighted voice item dictates too");
    ak_menu_dictated(&m, "", t, &out);
    CHECK(out.kind == AK_MENU_OUT_NONE, "empty text is not an answer");
    ak_menu_dictated(&m, "use main", t, &out);
    CHECK(out.kind == AK_MENU_OUT_RESULT && out.selected[0] == 1 && out.text != NULL && strcmp(out.text, "use main") == 0,
          "dictated answer = voice item index + text");
    ak_menu_dictated(&m, "again", t, &out);
    CHECK(out.kind == AK_MENU_OUT_NONE, "answered once");
}

static void test_menu_pages(void)
{
    printf("test_menu_pages\n");
    ak_menu_t      m;
    ak_menu_out_t  out;
    ak_menu_page_t p;
    uint32_t       t = 5000U;

    ak_menu_init(&m);
    open_menu(&m, MK("\"id\":\"mm\",\"kind\":\"model\",\"title\":\"Model\",\"page\":1,\"pages\":3,\"default\":5,"
                     "\"items\":[{\"label\":\"Auto\"},{\"label\":\"m1\"},{\"label\":\"m2\"},{\"label\":\"m3\"}]"),
              t);
    CHECK(ak_menu_is_model(&m), "model menu");
    CHECK(m.cur_page == 0 && m.cur_idx == 0, "default on a missing page waits");
    CHECK(ak_menu_global_index(&m, 1, 0) == -1, "page 2 index unknown until it arrives");

    CHECK(parse_page(MK("\"id\":\"mm\",\"kind\":\"model\",\"page\":2,\"pages\":3,\"items\":[{\"label\":\"m4\"},"
                        "{\"label\":\"m5\",\"checked\":true},{\"label\":\"m6\"},{\"label\":\"m7\"}]"),
                     &p),
          "page 2 parses");
    CHECK(ak_menu_on_page(&m, &p, t) == AK_MENU_APPLY_UPDATED, "page 2 fills in");
    CHECK(m.cur_page == 1 && m.cur_idx == 1, "default 5 resolves to page 2 item 2");
    CHECK(ak_menu_item_ticked(&m, 1, 1), "checked item shows a tick");

    CHECK(parse_page(MK("\"id\":\"mm\",\"kind\":\"mode\",\"page\":3,\"pages\":4,\"items\":[{\"label\":\"x\"}]"), &p),
          "fixture");
    CHECK(ak_menu_on_page(&m, &p, t) == AK_MENU_APPLY_REJECTED, "pages mismatch for the same id is dropped");

    t += 300U;
    /* EC11 down past the page end turns the page (page 3 not here yet). */
    ak_menu_encoder(&m, 3, t, &out);
    CHECK(m.cur_page == 2 && m.cur_idx == 0 && m.pg[2].count == 0, "rotation crosses into page 3");
    out = tap(&m, 5, &t);
    CHECK(out.kind == AK_MENU_OUT_NONE, "K6 on a missing page does nothing");
    out = tap(&m, 1, &t);
    CHECK(out.kind == AK_MENU_OUT_NONE, "K2 on a missing page does nothing");
    ak_menu_encoder(&m, 1, t, &out);
    CHECK(m.cur_page == 2 && !out.redraw, "no wrap past the last page");
    ak_menu_encoder(&m, -1, t, &out);
    CHECK(m.cur_page == 1 && m.cur_idx == 3, "up from the page top enters the previous page's last item");

    /* K2..K5 pick on the page being shown. */
    out = tap(&m, 1, &t);
    CHECK(out.kind == AK_MENU_OUT_RESULT && out.selected[0] == 4, "K2 on page 2 = global 4");

    /* Double confirm on Trust with its own key. */
    open_menu(&m, MK("\"id\":\"t2\",\"kind\":\"mode\",\"page\":1,\"pages\":1,\"items\":[{\"label\":\"Normal\"},"
                     "{\"label\":\"Trust\",\"role\":\"attention\",\"needs_double_confirm\":true}]"),
              t);
    t += 300U;
    out = tap(&m, 2, &t);
    CHECK(out.kind == AK_MENU_OUT_NONE && ak_menu_armed(&m) && m.armed_key == 2, "first K3 on Trust arms");
    out = tap(&m, 2, &t);
    CHECK(out.kind == AK_MENU_OUT_RESULT && out.selected[0] == 1, "second K3 within 3 s answers");

    open_menu(&m, MK("\"id\":\"t3\",\"kind\":\"mode\",\"items\":[{\"label\":\"Trust\",\"needs_double_confirm\":true}]"),
              t);
    t += 300U;
    tap(&m, AK_MENU_KEY_ENC, &t);
    CHECK(ak_menu_armed(&m) && m.armed_key == AK_MENU_KEY_ENC, "EC11 click arms");
    out = tap(&m, 5, &t);
    CHECK(!ak_menu_armed(&m) && out.kind == AK_MENU_OUT_NONE && m.state == AK_MENU_OPEN,
          "another key (K6) cancels the arm and does not answer");
    tap(&m, AK_MENU_KEY_ENC, &t);
    ak_menu_tick(&m, t + AK_MENU_CONFIRM2_MS, &out);
    CHECK(!ak_menu_armed(&m) && out.redraw, "the arm lapses after 3 s");
    tap(&m, AK_MENU_KEY_ENC, &t);
    ak_menu_encoder(&m, 1, t, &out);
    CHECK(!ak_menu_armed(&m) && m.cur_idx == 0, "rotation cancels the arm without moving");
    tap(&m, AK_MENU_KEY_ENC, &t);
    ak_menu_key(&m, AK_MENU_KEY_ENC, true, t + 2990U, &out);
    ak_menu_tick(&m, t + 3100U, &out);
    ak_menu_key(&m, AK_MENU_KEY_ENC, false, t + 3200U, &out);
    CHECK(out.kind == AK_MENU_OUT_RESULT && out.selected[0] == 0, "second press begun inside 3 s confirms");
}

static void test_menu_multi_and_lifecycle(void)
{
    printf("test_menu_multi_and_lifecycle\n");
    ak_menu_t      m;
    ak_menu_out_t  out;
    ak_menu_page_t p;
    uint32_t       t = 0xFFFFFF00U; /* exercise clock wrap */

    ak_menu_init(&m);
    open_menu(&m, MK("\"id\":\"q9\",\"kind\":\"question\",\"select\":\"multi\",\"items\":[{\"label\":\"a\"},"
                     "{\"label\":\"b\",\"checked\":true},{\"label\":\"c\"}]"),
              t);
    t += 300U;
    tap(&m, 1, &t);               /* K2 ticks a */
    tap(&m, 2, &t);               /* K3 unticks b */
    ak_menu_encoder(&m, 1, t, &out);
    tap(&m, AK_MENU_KEY_ENC, &t); /* knob ticks the highlight (c) */
    CHECK(m.state == AK_MENU_OPEN, "ticking never answers");
    out = tap(&m, 5, &t);
    CHECK(out.kind == AK_MENU_OUT_RESULT && out.n == 2 && out.selected[0] == 0 && out.selected[1] == 2,
          "K6 submits the ticked indexes ascending");

    open_menu(&m, MK("\"id\":\"q10\",\"kind\":\"question\",\"select\":\"multi\",\"items\":[{\"label\":\"a\"}]"), t);
    t += 300U;
    out = tap(&m, 5, &t);
    CHECK(out.kind == AK_MENU_OUT_RESULT && out.n == 0, "empty multi submit (Companion: dismissed)");

    /* Timeout. */
    open_menu(&m, MK("\"id\":\"to\",\"kind\":\"options\",\"timeout_s\":30,\"items\":[{\"label\":\"a\"}]"), t);
    ak_menu_tick(&m, t + 29999U, &out);
    CHECK(out.kind == AK_MENU_OUT_NONE, "not yet timed out");
    ak_menu_tick(&m, t + 30000U, &out);
    CHECK(out.kind == AK_MENU_OUT_DISMISS && strcmp(out.reason, "timeout") == 0 && m.state == AK_MENU_WAIT,
          "timeout_s dismisses with reason timeout");
    CHECK(parse_page(MK("\"id\":\"to\",\"kind\":\"options\",\"items\":[{\"label\":\"a\"}]"), &p), "fixture");
    CHECK(ak_menu_on_page(&m, &p, t) == AK_MENU_APPLY_IGNORED, "a page of an answered menu is ignored");

    /* menu_close: other ids ignored; the current one closes without a result. */
    open_menu(&m, k_approval, t);
    CHECK(!ak_menu_on_close(&m, "zzz") && m.state == AK_MENU_OPEN, "menu_close for another id is ignored");
    CHECK(ak_menu_on_close(&m, "a1b2c3") && m.state == AK_MENU_IDLE, "menu_close closes the current menu");
    CHECK(!ak_menu_on_close(&m, "a1b2c3"), "second close is a no-op");

    /* Priority pre-emption: a new id replaces the open menu, no result for it. */
    open_menu(&m, MK("\"id\":\"low\",\"kind\":\"options\",\"items\":[{\"label\":\"a\"},{\"label\":\"b\"}]"), t);
    t += 300U;
    ak_menu_key(&m, 1, true, t, &out); /* K2 held when the new menu arrives */
    open_menu(&m, k_approval, t);
    CHECK_STR(m.id, "a1b2c3", "higher-priority menu replaced the open one");
    CHECK_STR(m.queue, "1/3", "queue position shown");
    ak_menu_key(&m, 1, false, t + 400U, &out);
    CHECK(out.kind == AK_MENU_OUT_NONE, "release of a key pressed for the old menu does nothing");

    /* Same id + page refreshes the content in place. */
    CHECK(parse_page(MK("\"id\":\"a1b2c3\",\"kind\":\"approval\",\"queue\":\"1/2\",\"items\":[{\"label\":\"Approve "
                        "once\",\"role\":\"confirm\"}]"),
                     &p),
          "refresh fixture");
    CHECK(ak_menu_on_page(&m, &p, t) == AK_MENU_APPLY_UPDATED && m.pg[0].count == 1, "refresh replaces the page");
    CHECK_STR(m.queue, "1/2", "queue refreshed");

    /* Hidden (queued behind another state): no key ownership; showing restarts the guard. */
    ak_menu_set_hidden(&m, true, t);
    CHECK(!ak_menu_owns_keys(&m), "hidden menu does not own keys");
    t += 5000U;
    ak_menu_set_hidden(&m, false, t);
    ak_menu_key(&m, 1, true, t + 100U, &out);
    ak_menu_key(&m, 1, false, t + 150U, &out);
    CHECK(out.kind == AK_MENU_OUT_NONE, "guard restarts when the menu re-appears");

    /* Total items capped at 48 across 12 pages. */
    open_menu(&m, MK("\"id\":\"big\",\"kind\":\"model\",\"page\":1,\"pages\":12,\"items\":[{\"label\":\"1\"},"
                     "{\"label\":\"2\"},{\"label\":\"3\"},{\"label\":\"4\"}]"),
              t);
    for (int pg = 2; pg <= 12; pg++) {
        char f[300];
        snprintf(f, sizeof(f),
                 "{\"id\":\"big\",\"kind\":\"model\",\"page\":%d,\"pages\":12,\"items\":[{\"label\":\"1\"},"
                 "{\"label\":\"2\"},{\"label\":\"3\"},{\"label\":\"4\"}]}",
                 pg);
        CHECK(parse_page(f, &p), "big page parses");
        CHECK(ak_menu_on_page(&m, &p, t) == AK_MENU_APPLY_UPDATED, "48 items fit");
    }
    CHECK(ak_menu_global_index(&m, 11, 3) == 47, "last global index 47");
}

static bool rgb_is(ak_rgb_t c, ak_light_t l, uint8_t pct)
{
    ak_rgb_t w = ak_light_rgb(l, pct);
    return c.r == w.r && c.g == w.g && c.b == w.b;
}

static void test_menu_lights(void)
{
    printf("test_menu_lights\n");
    ak_menu_t     m;
    ak_menu_out_t out;
    ak_rgb_t      l[AK_LIGHT_COUNT];
    uint32_t      t = 100U;

    CHECK(ak_menu_role_light(AK_MENU_ROLE_CONFIRM) == AK_LIGHT_GREEN &&
              ak_menu_role_light(AK_MENU_ROLE_CANCEL) == AK_LIGHT_RED &&
              ak_menu_role_light(AK_MENU_ROLE_DANGER) == AK_LIGHT_RED &&
              ak_menu_role_light(AK_MENU_ROLE_ATTENTION) == AK_LIGHT_YELLOW &&
              ak_menu_role_light(AK_MENU_ROLE_VOICE) == AK_LIGHT_PURPLE &&
              ak_menu_role_light(AK_MENU_ROLE_NORMAL) == AK_LIGHT_BLUE,
          "role -> colour table (selectable options blue)");

    ak_menu_init(&m);
    ak_menu_lights(&m, t, l);
    CHECK(rgb_is(l[0], AK_LIGHT_OFF, 0) && rgb_is(l[6], AK_LIGHT_OFF, 0), "no menu: menu lights all off");

    open_menu(&m, k_approval, t);
    ak_menu_lights(&m, t, l);
    CHECK(rgb_is(l[0], AK_LIGHT_RED, 100), "K1 red (ESC)");
    CHECK(rgb_is(l[1], AK_LIGHT_GREEN, 100), "highlighted confirm item full green");
    CHECK(rgb_is(l[2], AK_LIGHT_RED, AK_LIGHT_DIM_PCT) && rgb_is(l[3], AK_LIGHT_RED, AK_LIGHT_DIM_PCT),
          "other items at 40%");
    CHECK(rgb_is(l[4], AK_LIGHT_OFF, 0), "K5 has no item -> off");
    CHECK(rgb_is(l[5], AK_LIGHT_OFF, 0) && rgb_is(l[6], AK_LIGHT_OFF, 0), "approval: K6 and LED7 off");

    open_menu(&m, MK("\"id\":\"q\",\"kind\":\"question\",\"items\":[{\"label\":\"a\"},{\"label\":\"b\"},"
                     "{\"label\":\"Dictate\",\"role\":\"voice\"},{\"label\":\"Trust\",\"role\":\"attention\","
                     "\"needs_double_confirm\":true}]"),
              t);
    ak_menu_lights(&m, t, l);
    CHECK(rgb_is(l[1], AK_LIGHT_BLUE, 100) && rgb_is(l[2], AK_LIGHT_BLUE, AK_LIGHT_DIM_PCT), "options blue");
    CHECK(rgb_is(l[3], AK_LIGHT_PURPLE, AK_LIGHT_DIM_PCT), "dictate item purple");
    CHECK(rgb_is(l[4], AK_LIGHT_YELLOW, AK_LIGHT_DIM_PCT), "attention item yellow on K5");
    CHECK(rgb_is(l[5], AK_LIGHT_GREEN, 100), "K6 green (Enter)");
    CHECK(rgb_is(l[6], AK_LIGHT_BLUE, 100), "LED7 shows what the knob click picks");
    t += 300U;
    tap(&m, 4, &t);
    CHECK(ak_menu_armed(&m), "armed");
    ak_menu_lights(&m, m.armed_ms + 10U, l);
    CHECK(rgb_is(l[4], AK_LIGHT_YELLOW, 100), "armed: the item key flashes yellow (on phase)");
    ak_menu_lights(&m, m.armed_ms + 130U, l);
    CHECK(rgb_is(l[4], AK_LIGHT_OFF, 0), "armed: flash off phase (~4 Hz)");
    (void)out;

    /* Session lights. */
    ak_session_lights(0x05U, false, false, 0U, 500U, l);
    CHECK(rgb_is(l[0], AK_LIGHT_YELLOW, 50) && rgb_is(l[2], AK_LIGHT_YELLOW, 50) && rgb_is(l[1], AK_LIGHT_OFF, 0),
          "waiting slots A and C breathe yellow (quarter period = 50%)");
    ak_session_lights(0x01U, false, false, 0U, 1000U, l);
    CHECK(rgb_is(l[0], AK_LIGHT_YELLOW, 100), "breath peaks at half the 2 s period");
    ak_session_lights(0U, true, false, 0U, 1000U, l);
    CHECK(rgb_is(l[3], AK_LIGHT_YELLOW, 100), "K4 breathes yellow while a request waits");
    ak_session_lights(0U, true, true, 800U, 0U, l);
    CHECK(rgb_is(l[3], AK_LIGHT_RED, 100) && rgb_is(l[5], AK_LIGHT_OFF, 0),
          "input confirm keeps K4 red (cancel), K6 off phase");
    ak_session_lights(0U, false, true, 100U, 0U, l);
    CHECK(rgb_is(l[5], AK_LIGHT_GREEN, 100), "input confirm: K6 blink on");
}

static void test_ui_states(void)
{
    printf("test_ui_states\n");
    ak_ui_t ui;
    ak_ui_init(&ui);
    CHECK(ak_ui_state(&ui) == AK_UI_STANDBY, "starts in standby");

    /* ① -> ④ -> ① */
    ak_ui_menu_arrived(&ui, false);
    CHECK(ak_ui_state(&ui) == AK_UI_MENU && ak_ui_menu_visible(&ui), "a menu in standby shows at once");
    ak_ui_menu_arrived(&ui, true);
    CHECK(ak_ui_state(&ui) == AK_UI_MODEL_MENU, "the newest menu replaces the open one");
    ak_ui_menu_gone(&ui);
    CHECK(ak_ui_state(&ui) == AK_UI_STANDBY, "menu gone -> standby");

    /* ② / ③ hold a request and K4 breathes; it pops up back in ①. */
    CHECK(ak_ui_voice_start(&ui) && ak_ui_state(&ui) == AK_UI_VOICE, "voice key -> voice input");
    ak_ui_menu_arrived(&ui, false);
    CHECK(ak_ui_state(&ui) == AK_UI_VOICE && ui.menu_waiting && !ak_ui_menu_visible(&ui),
          "a request during voice input does not take the screen");
    CHECK(ak_ui_k4_waiting(&ui, 0) && !ak_ui_k4_recalls(&ui, 0), "K4 breathes; not a recall");
    ak_ui_voice_end(&ui, AK_UI_VOICE_TEXT, false, 100U);
    CHECK(ak_ui_state(&ui) == AK_UI_CONFIRM && ui.menu_waiting, "text -> input confirm, still queued");
    CHECK(!ak_ui_k4_waiting(&ui, 0), "input confirm keeps its own K4 hint");
    CHECK(!ak_ui_voice_start(&ui) || ak_ui_state(&ui) == AK_UI_VOICE, "speaking again re-enters voice input");
    ak_ui_voice_end(&ui, AK_UI_VOICE_TEXT, false, 200U);
    ak_ui_confirm_done(&ui);
    CHECK(ak_ui_state(&ui) == AK_UI_MENU && !ui.menu_waiting, "back in standby the held menu pops up");
    CHECK(!ak_ui_voice_start(&ui) && ak_ui_state(&ui) == AK_UI_MENU, "in a menu K5 is not the voice key");
    ak_ui_menu_gone(&ui);

    /* ③ timeout; Chat mode skips ③; voice failures return to ①. */
    ak_ui_voice_start(&ui);
    ak_ui_voice_end(&ui, AK_UI_VOICE_TEXT, false, 1000U);
    CHECK(!ak_ui_tick(&ui, 1000U + AK_UI_CONFIRM_TIMEOUT_MS - 1U), "confirm not yet timed out");
    CHECK(ak_ui_tick(&ui, 1000U + AK_UI_CONFIRM_TIMEOUT_MS) && ak_ui_state(&ui) == AK_UI_STANDBY,
          "confirm times out to standby");
    ak_ui_voice_start(&ui);
    ak_ui_voice_end(&ui, AK_UI_VOICE_TEXT, true, 0U);
    CHECK(ak_ui_state(&ui) == AK_UI_STANDBY, "Chat mode: no input confirm");
    ak_ui_voice_start(&ui);
    ak_ui_voice_end(&ui, AK_UI_VOICE_ERROR, false, 0U);
    CHECK(ak_ui_state(&ui) == AK_UI_STANDBY, "nothing heard -> standby");
    ak_ui_voice_start(&ui);
    ak_ui_voice_end(&ui, AK_UI_VOICE_NONE, false, 0U);
    CHECK(ak_ui_state(&ui) == AK_UI_STANDBY, "short press / PC dictation -> standby");

    /* A failed direct send returns the text: ① -> ③ only from standby. */
    CHECK(ak_ui_input_restore(&ui, 7000U) && ak_ui_state(&ui) == AK_UI_CONFIRM && ui.confirm_ms == 7000U,
          "input_restore re-enters confirm from standby");
    CHECK(!ak_ui_input_restore(&ui, 7100U) && ui.confirm_ms == 7000U, "input_restore refused outside standby");
    ak_ui_voice_abort(&ui);
    {
        char t[16] = "ab\xE4\xBD\xA0"; /* "ab" + U+4F60 */
        CHECK(ak_utf8_pop_last(t) && strcmp(t, "ab") == 0, "pop removes a whole 3-byte character");
        CHECK(ak_utf8_pop_last(t) && strcmp(t, "a") == 0, "pop removes an ASCII character");
        CHECK(ak_utf8_pop_last(t) && t[0] == '\0' && !ak_utf8_pop_last(t), "pop on empty is false");
    }

    /* Dictated answer: ④ -> ② -> ③ -> sent; K1 back to ④; retry on error. */
    ak_ui_menu_arrived(&ui, false);
    ak_ui_dictate(&ui);
    CHECK(ak_ui_state(&ui) == AK_UI_VOICE && ui.dictating && !ak_ui_menu_visible(&ui), "dictate -> voice input");
    CHECK(ak_ui_voice_start(&ui) && ak_ui_state(&ui) == AK_UI_VOICE, "voice key works while dictating");
    ak_ui_voice_end(&ui, AK_UI_VOICE_ERROR, false, 0U);
    CHECK(ak_ui_state(&ui) == AK_UI_VOICE && ui.dictating, "a failed dictation stays to retry");
    ak_ui_dictate_back(&ui);
    CHECK(ak_ui_state(&ui) == AK_UI_MENU && !ui.dictating, "K1 returns to the menu");
    ak_ui_dictate(&ui);
    ak_ui_voice_start(&ui);
    ak_ui_voice_end(&ui, AK_UI_VOICE_TEXT, true, 5000U);
    CHECK(ak_ui_state(&ui) == AK_UI_CONFIRM && ui.dictating, "dictated text -> confirm (also in Chat mode)");
    CHECK(ak_ui_tick(&ui, 5000U + AK_UI_CONFIRM_TIMEOUT_MS) && ak_ui_state(&ui) == AK_UI_MENU,
          "an unsent dictated answer times out back to the menu");
    ak_ui_dictate(&ui);
    ak_ui_voice_start(&ui);
    ak_ui_voice_end(&ui, AK_UI_VOICE_TEXT, false, 0U);
    ak_ui_menu_gone(&ui); /* sent: the menu finishes */
    CHECK(ak_ui_state(&ui) == AK_UI_STANDBY && !ui.dictating, "answer sent -> standby");
    ak_ui_menu_arrived(&ui, false);
    ak_ui_dictate(&ui);
    ak_ui_menu_arrived(&ui, false);
    CHECK(ak_ui_state(&ui) == AK_UI_MENU && !ui.dictating, "a new menu replaces the one being dictated");
    ak_ui_menu_gone(&ui);

    /* Overlays: ⑦ > ⑥ > base; menus wait behind them. */
    CHECK(ak_ui_set_modals(&ui, true, false) && ak_ui_state(&ui) == AK_UI_SETTINGS, "settings overlay");
    ak_ui_menu_arrived(&ui, false);
    CHECK(ak_ui_state(&ui) == AK_UI_SETTINGS && !ak_ui_menu_visible(&ui), "menu waits behind settings");
    CHECK(!ak_ui_k4_waiting(&ui, 3) && !ak_ui_k4_recalls(&ui, 3), "no K4 hint inside settings");
    ak_ui_set_modals(&ui, true, true);
    CHECK(ak_ui_state(&ui) == AK_UI_SYSTEM, "system confirm outranks settings");
    CHECK(!ak_ui_set_modals(&ui, true, true), "unchanged modals report no change");
    ak_ui_set_modals(&ui, false, false);
    CHECK(ak_ui_state(&ui) == AK_UI_MENU && ak_ui_menu_visible(&ui), "menu shows once the overlays close");
    ak_ui_menu_gone(&ui);

    /* ESC'd requests: K4 recall only in standby. */
    CHECK(ak_ui_k4_waiting(&ui, 1) && ak_ui_k4_recalls(&ui, 1), "parked request: K4 breathes and recalls");
    CHECK(!ak_ui_k4_waiting(&ui, 0) && !ak_ui_k4_recalls(&ui, 0), "nothing parked");
    ak_ui_voice_start(&ui);
    CHECK(ak_ui_k4_waiting(&ui, 1) && !ak_ui_k4_recalls(&ui, 1), "voice input: K4 shows, no recall");
    ak_ui_voice_abort(&ui);
    CHECK(ak_ui_state(&ui) == AK_UI_STANDBY, "abort voice -> standby");
    CHECK_STR(ak_ui_state_str(AK_UI_SYSTEM), "system", "state names");
}

/* Boot marker (FR-12 B). KV delete and the single-consume rule live in
 * ak_reset_netcfg.c and are not host-testable; here: the bytes, the validation
 * a boot must pass, and the window timing a marker-opened window follows. */
static void test_window_marker(void)
{
    uint8_t m[AK_WINDOW_MARKER_LEN];
    ak_window_marker_encode(m);
    CHECK(ak_window_marker_valid(m, sizeof(m)), "encoded marker is valid");
    CHECK(!ak_window_marker_valid(m, 1U), "legacy 1-byte marker is rejected");
    CHECK(!ak_window_marker_valid(m, 0U), "empty marker is rejected");
    CHECK(!ak_window_marker_valid(NULL, sizeof(m)), "NULL marker is rejected");
    for (size_t i = 0; i < sizeof(m); i++) {
        uint8_t bad[AK_WINDOW_MARKER_LEN];
        memcpy(bad, m, sizeof(bad));
        bad[i] ^= 0x01U;
        CHECK(!ak_window_marker_valid(bad, sizeof(bad)), "a flipped byte invalidates the marker");
    }

    /* A marker-opened boot window: 180 s, shortened to 5 s once cloud is ready. */
    ak_provision_t p;
    uint32_t       t = 5000U;
    ak_provision_init(&p);
    ak_provision_open(&p, t);
    CHECK(ak_provision_is_open(&p, t + AK_PROV_WINDOW_MS - 1U), "boot window open for 180 s");
    CHECK(!ak_provision_is_open(&p, t + AK_PROV_WINDOW_MS), "boot window closes at 180 s");
    ak_provision_on_cloud_ready(&p, t + 10000U);
    CHECK(!ak_provision_is_open(&p, t + 10000U + AK_PROV_CLOSE_AFTER_READY_MS), "boot window closes 5 s after cloud_ready");
}

static void test_bat_gauge(void)
{
    printf("test_bat_gauge\n");

    /* Divider x4 and calibration permille. */
    CHECK(ak_bat_node_to_cell_mv(1000, 1000) == 4000, "1000 mV tap = 4000 mV cell");
    CHECK(ak_bat_node_to_cell_mv(950, 1000) == 3800, "950 mV tap = 3800 mV cell");
    CHECK(ak_bat_node_to_cell_mv(1000, 1025) == 4100, "cal 1.025 scales the cell voltage");

    /* Curve: endpoints, breakpoints, interpolation, clamping. */
    CHECK(ak_bat_curve_percent(3000) == 0U, "below empty clamps to 0");
    CHECK(ak_bat_curve_percent(3300) == 0U, "3300 = 0%");
    CHECK(ak_bat_curve_percent(3600) == 10U, "3600 = 10%");
    CHECK(ak_bat_curve_percent(3830) == 50U, "3830 = 50%");
    CHECK(ak_bat_curve_percent(3850) == 55U, "3850 interpolates to 55%");
    CHECK(ak_bat_curve_percent(4130) == 94U, "4130 interpolates to 94%");
    CHECK(ak_bat_curve_percent(4200) == 100U, "4200 = 100%");
    CHECK(ak_bat_curve_percent(4400) == 100U, "above full clamps to 100");
    for (int32_t mv = 3300, last = 0; mv <= 4200; mv += 5) {
        int32_t p = ak_bat_curve_percent(mv);
        CHECK(p >= last, "curve is monotonic");
        last = p;
    }

    /* Trimmed mean: extremes and implausible reads are dropped. */
    int32_t s1[8] = {950, 951, 949, 950, 952, 948, 700, 1150};
    CHECK(ak_bat_trimmed_mean(s1, 8) == 950, "one low and one high outlier do not move the mean");
    int32_t s2[8] = {0, 950, 0, 952, 948, 950, 0, 950};
    CHECK(ak_bat_trimmed_mean(s2, 8) == 950, "0 mV failed reads are ignored");
    int32_t s3[8] = {0, 0, 0, 0, 950, 950, 950, 950};
    CHECK(ak_bat_trimmed_mean(s3, 8) == -1, "fewer than 5 plausible reads = no measurement");
    int32_t s4[8] = {-1, 5000, 1300, 100, 950, 951, 952, 953};
    CHECK(ak_bat_trimmed_mean(s4, 8) == -1, "out-of-range reads count as missing");
    CHECK(ak_bat_trimmed_mean(s1, 9) == -1 && ak_bat_trimmed_mean(NULL, 8) == -1, "bad arguments");

    /* Gauge: seed, then only falls while discharging. */
    ak_bat_gauge_t g;
    ak_bat_gauge_init(&g);
    CHECK(ak_bat_gauge_update(&g, 3830, false) == 50U, "first sample seeds 50%");
    for (int i = 0; i < 40; i++) {
        (void)ak_bat_gauge_update(&g, 3870, false); /* relaxes to 60% */
    }
    CHECK(g.percent == 50U, "small rise while discharging is held");
    uint8_t before = g.percent;
    (void)ak_bat_gauge_update(&g, 3600, false); /* one load spike, 10% */
    CHECK(g.percent >= before - 10U && g.percent > 30U, "one spike moves the EMA only an eighth of the way");
    ak_bat_gauge_init(&g);
    (void)ak_bat_gauge_update(&g, 3800, false);
    for (int i = 0; i < 60; i++) {
        (void)ak_bat_gauge_update(&g, 3740, false);
    }
    CHECK(g.percent == 30U, "a sustained drop is followed down");
    /* A big rise (battery swapped) restarts from the new reading at once. */
    CHECK(ak_bat_gauge_update(&g, 4080, false) == 90U, "rise above the hysteresis is accepted");
    CHECK(ak_bat_gauge_update(&g, 3790, false) == 86U, "next low sample only moves the EMA");

    /* Charging: reseeds on plug, follows upward, reseeds on unplug. */
    ak_bat_gauge_init(&g);
    (void)ak_bat_gauge_update(&g, 3740, false);
    CHECK(ak_bat_gauge_update(&g, 3900, true) == 66U, "plug-in reseeds at the charging voltage");
    (void)ak_bat_gauge_update(&g, 3950, true);
    CHECK(g.percent > 66U, "percent rises while charging");
    CHECK(ak_bat_gauge_update(&g, 3870, false) == 60U, "unplug reseeds at the resting voltage");
}

/* FR-01: language setting, string table and the UI font that draws it. */
static void test_i18n(void)
{
    printf("test_i18n\n");
    CHECK(ak_lang_get() == AK_LANG_ZH && AK_LANG_DEFAULT == AK_LANG_ZH, "default language is 中文");
    int missing = 0;
    int not_ascii_en = 0;
    for (int id = 0; id < (int)AK_STR_COUNT; id++) {
        const char *zh = ak_tr_lang((ak_str_id_t)id, AK_LANG_ZH);
        const char *en = ak_tr_lang((ak_str_id_t)id, AK_LANG_EN);
        if (zh[0] == '\0' || en[0] == '\0') {
            printf("  string %d missing a language\n", id);
            missing++;
        }
        if (ak_text_is_wide(en, strlen(en))) {
            not_ascii_en++; /* English keeps the 8x8 look */
        }
        /* Every character must be a real glyph of the UI font, not the box. */
        const char *p = zh;
        uint32_t    cp;
        while ((cp = ak_utf8_next(&p)) != 0U) {
            int idx = ak_font_find(&ak_font_ui, cp);
            if (idx < 0 || ak_font_ui.codes[idx] != cp) {
                printf("  string %d: U+%04X not in ak_font_ui (run tools/gen_font_ui.py)\n", id, (unsigned)cp);
                missing++;
            }
        }
    }
    CHECK(missing == 0, "every string has both languages and all its glyphs");
    CHECK(not_ascii_en == 0, "English strings are ASCII");
    /* Strings used as printf formats must take the same arguments in both. */
    int fmt_mismatch = 0;
    for (int id = 0; id < (int)AK_STR_COUNT; id++) {
        const char *a = ak_tr_lang((ak_str_id_t)id, AK_LANG_ZH);
        const char *b = ak_tr_lang((ak_str_id_t)id, AK_LANG_EN);
        for (;;) {
            a = strchr(a, '%');
            b = strchr(b, '%');
            if (a == NULL || b == NULL) {
                break;
            }
            size_t na = strspn(a + 1, "0123456789+-.lu%sdx") + 1U;
            size_t nb = strspn(b + 1, "0123456789+-.lu%sdx") + 1U;
            if (na != nb || strncmp(a, b, na) != 0) {
                break;
            }
            a += na;
            b += nb;
        }
        if (a != NULL || b != NULL) {
            printf("  string %d: printf conversions differ between languages\n", id);
            fmt_mismatch++;
        }
    }
    CHECK(fmt_mismatch == 0, "format strings take the same arguments in both languages");
    CHECK(ak_tr((ak_str_id_t)AK_STR_COUNT)[0] == '\0' && ak_tr_lang(AK_STR_SET_TITLE, AK_LANG_COUNT)[0] == '\0',
          "unknown id / language give an empty string");

    CHECK(strcmp(ak_tr(AK_STR_SET_LANGUAGE), "语言") == 0, "zh lookup");
    ak_lang_set(AK_LANG_EN);
    CHECK(ak_lang_get() == AK_LANG_EN && strcmp(ak_tr(AK_STR_SET_LANGUAGE), "Language") == 0, "en lookup");
    ak_lang_set(AK_LANG_COUNT);
    CHECK(ak_lang_get() == AK_LANG_EN, "out-of-range language ignored");
    ak_lang_set(AK_LANG_ZH);

    ak_lang_t l = AK_LANG_COUNT;
    CHECK(ak_lang_from_wire("en", &l) && l == AK_LANG_EN && ak_lang_from_wire("zh", &l) && l == AK_LANG_ZH,
          "wire names parse");
    l = AK_LANG_EN;
    CHECK(!ak_lang_from_wire("fr", &l) && !ak_lang_from_wire("", &l) && !ak_lang_from_wire(NULL, &l) &&
              l == AK_LANG_EN,
          "unknown wire names rejected, out untouched");
    CHECK(strcmp(ak_lang_wire_str(AK_LANG_ZH), "zh") == 0 && strcmp(ak_lang_wire_str(AK_LANG_EN), "en") == 0,
          "wire names");
    uint8_t b = ak_lang_to_byte(AK_LANG_EN);
    CHECK(ak_lang_from_blob(&b, 1U, &l) && l == AK_LANG_EN, "KV blob round trip");
    b = 2U;
    CHECK(!ak_lang_from_blob(&b, 1U, &l) && !ak_lang_from_blob(&b, 2U, &l) && !ak_lang_from_blob(NULL, 1U, &l),
          "bad KV blobs rejected");
    CHECK(strcmp(ak_lang_agent_param(AK_LANG_EN), "{\"lang\":\"en\",\"lang_name\":\"English\"}") == 0 &&
              strstr(ak_lang_agent_param(AK_LANG_ZH), "\"lang\":\"zh\"") != NULL,
          "agent custom.param JSON");

    CHECK(ak_font_ui.px == 12U && ak_font_ui.bpp == 1U && ak_font_ui.count > 3800U, "UI font: 12 px 1bpp, >3800 glyphs");
    int idx = ak_font_find(&ak_font_ui, 0x4E2DU); /* 中 */
    CHECK(idx >= 0 && ak_font_ui.adv[idx] == 12U && ak_font_ui.adv[ak_font_find(&ak_font_ui, 'A')] == 6U,
          "CJK 12 px, ASCII 6 px");
}

static void test_text(void)
{
    printf("test_text\n");
    CHECK(ak_text_width("abc") == 24U, "ASCII uses the 8x8 advance");
    CHECK(ak_text_width("中a") == 18U, "mixed text uses the UI font (12 + 6)");
    CHECK(ak_text_width_n("中文", 4U) == 12U, "a sequence cut by n is not counted");
    CHECK(!ak_text_is_wide("Settings", 8U) && ak_text_is_wide("设置", 6U), "wide detection");

    /* 29 ASCII columns: break at the last space. */
    const char *en = "The quick brown fox jumps over the lazy dog";
    size_t      n  = ak_text_wrap(en, 232U);
    CHECK(n == 26U && en[n - 1U] == ' ', "ASCII wraps after a space within 232 px");
    CHECK(ak_text_wrap("abcdefghijklmnopqrstuvwxyzABCDEFG", 232U) == 29U, "a long word is cut at 29 columns");

    /* 20 CJK characters: 19 fit (228 px). */
    const char *zh = "一二三四五六七八九十一二三四五六七八九十";
    n = ak_text_wrap(zh, 232U);
    CHECK(n == 57U, "19 CJK characters per 232 px line");
    /* A comma never starts a line: it stays with the character before it. */
    const char *zh2 = "一二三四五六七八九十一二三四五六七八九，好";
    n = ak_text_wrap(zh2, 232U);
    CHECK(n == 54U, "\"，\" is not moved to the next line start");
    /* Mixed: English word then CJK breaks before the CJK character. */
    n = ak_text_wrap("hello世界", 40U); /* hello=30 px, 世 would reach 42 */
    CHECK(n == 5U, "break before a CJK character");
    CHECK(ak_text_wrap("中", 4U) == 3U, "one character wider than the line still progresses");
    CHECK(ak_text_wrap("", 100U) == 0U, "empty text ends");
    /* Malformed bytes: each consumed singly, never more than the string. */
    n = ak_text_wrap("\xE4\xB8", 232U);
    CHECK(n == 2U, "truncated sequence wraps without over-reading");

    char buf[8];
    memcpy(buf, "ab\xE4\xB8", 5U);
    ak_text_utf8_trim(buf);
    CHECK(strcmp(buf, "ab") == 0, "trim drops a cut sequence");
    memcpy(buf, "a\xE4\xB8\xAD", 5U);
    ak_text_utf8_trim(buf);
    CHECK(strcmp(buf, "a\xE4\xB8\xAD") == 0, "trim keeps a whole one");
}

int main(void)
{
    printf("== ak host tests ==\n");
    test_short_press();
    test_long_press();
    test_debounce_bounce();
    test_dclick();
    test_gesture();
    test_keymap();
    test_settings();
    test_voice_route();
    test_protocol();
    test_rx();
    test_rx_attacks();
    test_validate();
    test_provision();
    test_window_marker();
    test_byte_ring();
    test_led_power();
    test_bat_gauge();
    test_led_policy();
    test_ble_sec();
    test_cred();
    test_voice_mode();
    test_status_icons();
    test_missing_glyph();
    test_font();
    test_i18n();
    test_text();
    test_menu_parse();
    test_menu_keys();
    test_menu_pages();
    test_menu_multi_and_lifecycle();
    test_menu_lights();
    test_ui_states();
    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
