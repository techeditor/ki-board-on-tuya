/**
 * @file test_ak.c
 * @brief Host unit tests for the pure modules: ak_input_fsm, ak_protocol,
 *        ak_rx. Compiled and run on the host (no SDK/hardware).
 */

#include <stdio.h>
#include <string.h>

#include "ak_input_fsm.h"
#include "ak_protocol.h"
#include "ak_rx.h"

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

    ak_proto_hello_ack(buf, sizeof(buf), "0.1.0", "uart");
    CHECK_STR(buf,
              "{\"v\":1,\"type\":\"hello_ack\",\"payload\":{\"fw\":\"0.1.0\",\"transport\":\"uart\","
              "\"capabilities\":[\"uart\",\"keymap\",\"voice_state\"]}}\n",
              "hello_ack frame");
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

int main(void)
{
    printf("== ak host tests ==\n");
    test_short_press();
    test_long_press();
    test_debounce_bounce();
    test_protocol();
    test_rx();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
