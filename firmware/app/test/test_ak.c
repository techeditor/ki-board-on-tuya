/**
 * @file test_ak.c
 * @brief Host unit tests for the pure modules: ak_input_fsm, ak_protocol,
 *        ak_rx. Compiled and run on the host (no SDK/hardware).
 */

#include <stdio.h>
#include <string.h>

#include "ak_icons16.h"
#include "ak_session_icons.h"
#include "ak_input_fsm.h"
#include "ak_protocol.h"
#include "ak_rx.h"
#include "ak_settings.h"
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

    ak_proto_nav(buf, sizeof(buf), 8, "center", "long", 900);
    CHECK_STR(buf,
              "{\"v\":1,\"seq\":8,\"type\":\"nav\",\"payload\":{\"dir\":\"center\",\"action\":"
              "\"long\",\"held_ms\":900}}\n",
              "nav frame");

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

    ak_proto_voice_source(buf, sizeof(buf), 12, "cloud");
    CHECK_STR(buf,
              "{\"v\":1,\"seq\":12,\"type\":\"voice_source\",\"payload\":{\"source\":\"cloud\"}}\n",
              "voice_source frame");

    ak_proto_wifi_status(buf, sizeof(buf), 13, "connected", "MyAP");
    CHECK_STR(buf,
              "{\"v\":1,\"seq\":13,\"type\":\"wifi_status\",\"payload\":{\"state\":\"connected\","
              "\"ssid\":\"MyAP\"}}\n",
              "wifi_status frame");

    ak_proto_license_status(buf, sizeof(buf), 14, "written");
    CHECK_STR(buf,
              "{\"v\":1,\"seq\":14,\"type\":\"license_status\",\"payload\":{\"state\":\"written\"}}\n",
              "license_status frame");

    ak_proto_hello_ack(buf, sizeof(buf), "0.1.0", "uart");
    CHECK_STR(buf,
              "{\"v\":1,\"type\":\"hello_ack\",\"payload\":{\"fw\":\"0.1.0\",\"transport\":\"uart\","
              "\"capabilities\":[\"uart\",\"keymap\",\"voice_state\"]}}\n",
              "hello_ack frame");

    ak_proto_status(buf, sizeof(buf), "ble_gatt_link", "ready");
    CHECK_STR(buf,
              "{\"v\":1,\"type\":\"status\",\"payload\":{\"probe\":\"ble_gatt_link\",\"state\":\"ready\"}}\n",
              "status frame");

    ak_proto_log(buf, sizeof(buf), 5, "ble", "central connected");
    CHECK_STR(buf,
              "{\"v\":1,\"seq\":5,\"type\":\"log\",\"payload\":{\"tag\":\"ble\",\"msg\":\"central connected\"}}\n",
              "log frame");
}

static char g_lines[8][AK_RX_LINE_MAX];
static int  g_nlines;

static void capture_line(const char *line)
{
    if (g_nlines < 8) {
        strncpy(g_lines[g_nlines], line, AK_RX_LINE_MAX - 1);
        g_lines[g_nlines][AK_RX_LINE_MAX - 1] = '\0';
        g_nlines++;
    }
}

static void test_rx(void)
{
    printf("test_rx\n");
    ak_rx_reader_t r;
    ak_rx_reader_init(&r);
    g_nlines = 0;

    /* Line split across two feeds; second feed completes it and adds another. */
    const char *chunk1 = "{\"v\":1,\"ty";
    const char *chunk2 = "pe\":\"ping\"}\n{\"type\":\"hello\"}\r\n";
    ak_rx_reader_feed(&r, (const uint8_t *)chunk1, strlen(chunk1), capture_line);
    CHECK(g_nlines == 0, "partial line not delivered yet");
    ak_rx_reader_feed(&r, (const uint8_t *)chunk2, strlen(chunk2), capture_line);
    CHECK(g_nlines == 2, "two lines assembled across chunks (CRLF ok)");

    char type[16];
    CHECK(ak_json_str(g_lines[0], "type", type, sizeof(type)) && strcmp(type, "ping") == 0, "line 0 type=ping");
    CHECK(ak_json_str(g_lines[1], "type", type, sizeof(type)) && strcmp(type, "hello") == 0, "line 1 type=hello");

    const char *disp = "{\"type\":\"display\",\"agent_name\":\"planner\",\"state\":\"running\",\"seq\":42}";
    char        agent[32];
    uint32_t    seq = 0;
    CHECK(ak_json_str(disp, "agent_name", agent, sizeof(agent)) && strcmp(agent, "planner") == 0,
          "extract agent_name string");
    CHECK(ak_json_uint(disp, "seq", &seq) && seq == 42, "extract seq uint");
    CHECK(!ak_json_str(disp, "missing", type, sizeof(type)), "absent key returns false");
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
    CHECK(sizeof(AK_SESSION_ICON_KIRO_GHOST) == AK_SESSION_ICON_BYTES,
          "32x32 Kiro ghost icon byte count");
    CHECK(memcmp(AK_ICON_WIFI, AK_ICON_NO_WIFI, AK_ICON_BYTES) != 0,
          "WiFi connected/disconnected bitmaps differ");
    CHECK(memcmp(AK_ICON_USB, AK_ICON_NO_USB, AK_ICON_BYTES) != 0,
          "USB connected/disconnected bitmaps differ");
    CHECK(memcmp(AK_ICON_BLUETOOTH, AK_ICON_NO_BLUETOOTH, AK_ICON_BYTES) != 0,
          "BLE connected/disconnected bitmaps differ");
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
    CHECK(settings.volume == 70U, "default volume is 70 percent");
    CHECK(settings.brightness == 100U, "default brightness is 100 percent");

    CHECK(ak_settings_handle(&settings, AK_SETTINGS_ACTION_LEFT) ==
              AK_SETTINGS_RESULT_CHANGED && settings.volume == 60U,
          "left lowers volume by ten");
    for (unsigned i = 0; i < 8U; i++) {
        ak_settings_handle(&settings, AK_SETTINGS_ACTION_LEFT);
    }
    CHECK(settings.volume == 0U, "volume clamps at zero");

    ak_settings_handle(&settings, AK_SETTINGS_ACTION_DOWN);
    CHECK(settings.selected == AK_SETTINGS_ITEM_BRIGHTNESS, "down selects brightness");
    for (unsigned i = 0; i < 12U; i++) {
        ak_settings_handle(&settings, AK_SETTINGS_ACTION_LEFT);
    }
    CHECK(settings.brightness == AK_SETTINGS_BRIGHTNESS_MIN,
          "brightness keeps a visible ten-percent minimum");
    CHECK(ak_settings_handle(&settings, AK_SETTINGS_ACTION_CONFIRM) ==
              AK_SETTINGS_RESULT_CONFIRMED,
          "center confirms a value");

    ak_settings_handle(&settings, AK_SETTINGS_ACTION_DOWN);
    CHECK(settings.selected == AK_SETTINGS_ITEM_EXIT, "down selects exit");
    CHECK(ak_settings_handle(&settings, AK_SETTINGS_ACTION_CONFIRM) ==
              AK_SETTINGS_RESULT_EXIT,
          "center confirms exit");
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

int main(void)
{
    printf("== ak host tests ==\n");
    test_short_press();
    test_long_press();
    test_debounce_bounce();
    test_dclick();
    test_settings();
    test_voice_route();
    test_protocol();
    test_rx();
    test_status_icons();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
