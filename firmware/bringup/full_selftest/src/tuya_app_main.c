/**
 * @file tuya_app_main.c
 * @brief Combined hardware self-test for the Agentic Keyboard baseboard.
 *
 * One flashable image that exercises every block validated by the standalone
 * bring-up apps: input (6 keys + EC11 encoder), LCD (ST7789), WS2812 LEDs,
 * UART0 loopback, and Wi-Fi/BLE radio. A single non-blocking main loop scans
 * inputs continuously; each test mode animates from elapsed-time checks so
 * input stays responsive.
 *
 * v0.3 replaced the five-way joystick with an EC11 rotary encoder, so the mode
 * switch moved from "long-press NAV CENTER" to "long-press the EC11 push
 * button (enc_sw) >= 800 ms". The current test name and instructions are shown
 * on the LCD at all times.
 *
 * IO mapping comes from common/include/agentic_board_io.h (mirrors
 * hardware/pinmap/). UART0 (P10/P11, 115200 8N1) carries JSON test events; the
 * Tuya debug log stays on UART1 at 460800.
 */

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "agentic_board_io.h"
#include "ak_gfx.h"
#include "bringup_uart.h"

#include "tal_api.h"
#include "tal_bluetooth.h"
#include "tal_wifi.h"
#include "tkl_gpio.h"
#include "tkl_output.h"

#include "tdd_disp_st7789.h"
#include "tdl_display_draw.h"
#include "tdl_display_manage.h"

#include "tdd_pixel_ws2812.h"
#include "tdl_pixel_color_manage.h"
#include "tdl_pixel_dev_manage.h"
#include "tdd_pixel_type.h"

/* ------------------------------------------------------------------ config */

#define APP_TASK_STACK_SIZE   (8U * 1024U)
#define POLL_INTERVAL_MS      5U
#define INPUT_DEBOUNCE_MS     20U
#define LONG_PRESS_MS         800U
#define HEARTBEAT_INTERVAL_MS 2000U

#define LCD_PATTERN_MS  1500U
#define WS_PATTERN_MS   1000U
#define UART_REFRESH_MS 300U
#define WIFI_SCAN_MS    5000U

/*
 * Landscape orientation: the panel is native 240(w)x320(h) portrait; the UI is
 * rotated 90 deg clockwise in hardware via the ST7789 MADCTL byte (see
 * s_st7789_init_seq_noinv), so the logical drawing surface is 320(w)x240(h).
 * The SPI flush addresses CASET/RASET straight from the frame buffer's own
 * width/height, so we just draw into a 320x240 buffer.
 */
#define SCREEN_W    (AK_LCD_NATIVE_HEIGHT) /* 320 */
#define SCREEN_H    (AK_LCD_NATIVE_WIDTH)  /* 240 */
#define FRAME_BYTES (SCREEN_W * SCREEN_H * 2U)

#define TITLE_H   30U
#define CONTENT_Y 52U

#define DISPLAY_NAME "selftest_lcd"
#define WS_DEV_NAME  "selftest_ws2812"
#define WS_LEVEL     16U /* keep 7x white peak current low */

/* ------------------------------------------------------------------ modes */

typedef enum {
    TEST_INPUT = 0,
    TEST_LCD,
    TEST_WS2812,
    TEST_UART,
    TEST_WIRELESS,
    TEST_COUNT,
} TEST_MODE_E;

static const char *const s_mode_names[TEST_COUNT] = {
    "INPUT", "LCD", "WS2812", "UART", "RADIO",
};

static const uint16_t s_mode_colors[TEST_COUNT] = {
    AK_COL_DKBLUE, AK_COL_MAGENTA, AK_COL_ORANGE, AK_COL_GREEN, AK_COL_CYAN,
};

/* ------------------------------------------------------------------ inputs */

typedef enum {
    EV_NONE = 0,
    EV_DOWN,
    EV_UP,
} INPUT_EVENT_E;

typedef struct {
    const char       *id;
    const char       *label;
    bool              is_enc_sw; /* the EC11 push button (mode-switch key) */
    TUYA_GPIO_NUM_E   pin;
    TUYA_GPIO_LEVEL_E active;
    TUYA_GPIO_LEVEL_E stable;
    TUYA_GPIO_LEVEL_E candidate;
    uint32_t          candidate_since;
    uint32_t          pressed_since;
    bool              pressed; /* debounced active state, for the UI */
} INPUT_STATE_T;

/* enc_sw is the last entry; long-pressing it advances the test mode. */
#define ENC_SW_INDEX 6U

static INPUT_STATE_T s_inputs[] = {
    {"k1", "K1", false, AK_KEY1_PIN, AK_KEY_ACTIVE_LEVEL, AK_KEY_INACTIVE_LEVEL, AK_KEY_INACTIVE_LEVEL, 0, 0, false},
    {"k2", "K2", false, AK_KEY2_PIN, AK_KEY_ACTIVE_LEVEL, AK_KEY_INACTIVE_LEVEL, AK_KEY_INACTIVE_LEVEL, 0, 0, false},
    {"k3", "K3", false, AK_KEY3_PIN, AK_KEY_ACTIVE_LEVEL, AK_KEY_INACTIVE_LEVEL, AK_KEY_INACTIVE_LEVEL, 0, 0, false},
    {"k4", "K4", false, AK_KEY4_PIN, AK_KEY_ACTIVE_LEVEL, AK_KEY_INACTIVE_LEVEL, AK_KEY_INACTIVE_LEVEL, 0, 0, false},
    {"k5", "K5", false, AK_KEY5_PIN, AK_KEY_ACTIVE_LEVEL, AK_KEY_INACTIVE_LEVEL, AK_KEY_INACTIVE_LEVEL, 0, 0, false},
    {"k6", "K6", false, AK_KEY6_PIN, AK_KEY_ACTIVE_LEVEL, AK_KEY_INACTIVE_LEVEL, AK_KEY_INACTIVE_LEVEL, 0, 0, false},
    {"enc_sw", "ENC", true, AK_EC11_SW_PIN, AK_EC11_SW_ACTIVE_LEVEL, AK_KEY_INACTIVE_LEVEL, AK_KEY_INACTIVE_LEVEL, 0, 0,
     false},
};

/* ---- EC11 quadrature phases A/B (not in the key table) ---- */
typedef struct {
    TUYA_GPIO_LEVEL_E stable;
    TUYA_GPIO_LEVEL_E candidate;
    uint32_t          candidate_since;
} PHASE_STATE_T;

#define ENC_DEBOUNCE_MS          2U
#define ENC_SUBSTEPS_PER_DETENT  4

static PHASE_STATE_T s_phase_a;
static PHASE_STATE_T s_phase_b;
static uint8_t       s_enc_code;
static int32_t       s_enc_accum;
static int32_t       s_enc_position; /* net detents, for the UI */
static const char   *s_enc_last_dir = "-";

static const int8_t s_quad_table[16] = {
    0, -1, +1, 0, +1, 0, 0, -1, -1, 0, 0, +1, 0, +1, -1, 0,
};

/* ------------------------------------------------------------------ state */

static THREAD_HANDLE          s_app_thread = NULL;
static TDL_DISP_HANDLE_T      s_display    = NULL;
static TDL_DISP_DEV_INFO_T    s_info;
static TDL_DISP_FRAME_BUFF_T *s_frame  = NULL;
static PIXEL_HANDLE_T         s_pixels = NULL;
static bool                   s_lcd_ok = false;
static bool                   s_ws_ok  = false;

static TEST_MODE_E s_mode        = TEST_INPUT;
static uint32_t    s_sequence    = 0;
static uint32_t    s_mode_since  = 0;
static uint32_t    s_last_tick   = 0; /* generic per-mode timer */
static uint32_t    s_step        = 0; /* generic per-mode step index */
static bool        s_input_dirty = true;

/* CENTER long-press tracking (works in every mode). */
static bool     s_center_active   = false;
static uint32_t s_center_since    = 0;
static bool     s_center_consumed = false;

/* UART loopback counters. */
static uint32_t s_uart_rx   = 0;
static uint32_t s_uart_echo = 0;
static char     s_uart_last[24];

/* Radio state. */
static const char *s_ble_state = "init";
static uint32_t    s_ap_count  = 0;
static int         s_best_rssi = 0;

/* ------------------------------------------------------------ LCD helpers */

static void lcd_flush(void)
{
    if (s_lcd_ok) {
        tdl_disp_dev_flush(s_display, s_frame);
    }
}

static void draw_header(void)
{
    char title[16];
    (void)snprintf(title, sizeof(title), "%u/%u %s", (unsigned)(s_mode + 1), (unsigned)TEST_COUNT,
                   s_mode_names[s_mode]);

    ak_gfx_fill_rect(0, 0, SCREEN_W, TITLE_H, s_mode_colors[s_mode]);
    ak_gfx_text(6, 7, title, AK_COL_WHITE, s_mode_colors[s_mode], 2);

    ak_gfx_fill_rect(0, TITLE_H, SCREEN_W, CONTENT_Y - TITLE_H, AK_COL_BLACK);
    ak_gfx_text(6, TITLE_H + 6, "Hold ENC = next", AK_COL_CYAN, AK_COL_BLACK, 1);
}

static void clear_content(void)
{
    ak_gfx_fill_rect(0, CONTENT_Y, SCREEN_W, (uint16_t)(SCREEN_H - CONTENT_Y), AK_COL_BLACK);
}

/* ------------------------------------------------------------ UART events */

static void emit(const char *fmt, ...)
{
    char    buf[192];
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    bringup_uart_write(buf, (uint32_t)strlen(buf));
}

static void emit_mode(void)
{
    emit("{\"v\":1,\"type\":\"selftest\",\"payload\":{\"mode\":\"%s\",\"index\":%u,\"count\":%u}}\n",
         s_mode_names[s_mode], (unsigned)(s_mode + 1), (unsigned)TEST_COUNT);
}

/* ------------------------------------------------------------ input scan */

static OPERATE_RET gpio_input_init_one(TUYA_GPIO_NUM_E pin, TUYA_GPIO_LEVEL_E *level)
{
    TUYA_GPIO_BASE_CFG_T cfg = {
        .mode   = TUYA_GPIO_PULLUP,
        .direct = TUYA_GPIO_INPUT,
        .level  = TUYA_GPIO_LEVEL_HIGH,
    };
    OPERATE_RET rt = tkl_gpio_init(pin, &cfg);
    if (rt != OPRT_OK) {
        return rt;
    }
    return tkl_gpio_read(pin, level);
}

static OPERATE_RET inputs_init(void)
{
    TUYA_GPIO_LEVEL_E level;
    uint32_t          now = tal_system_get_millisecond();

    for (uint32_t i = 0; i < CNTSOF(s_inputs); i++) {
        OPERATE_RET rt = gpio_input_init_one(s_inputs[i].pin, &level);
        if (rt != OPRT_OK) {
            return rt;
        }
        s_inputs[i].stable          = level;
        s_inputs[i].candidate       = level;
        s_inputs[i].candidate_since = now;
        s_inputs[i].pressed         = (level == s_inputs[i].active);
    }

    OPERATE_RET rt = gpio_input_init_one(AK_EC11_A_PIN, &level);
    if (rt != OPRT_OK) {
        return rt;
    }
    s_phase_a.stable = s_phase_a.candidate = level;
    s_phase_a.candidate_since              = now;

    rt = gpio_input_init_one(AK_EC11_B_PIN, &level);
    if (rt != OPRT_OK) {
        return rt;
    }
    s_phase_b.stable = s_phase_b.candidate = level;
    s_phase_b.candidate_since              = now;

    s_enc_code     = (uint8_t)(((s_phase_a.stable == TUYA_GPIO_LEVEL_HIGH ? 1U : 0U) << 1) |
                           (s_phase_b.stable == TUYA_GPIO_LEVEL_HIGH ? 1U : 0U));
    s_enc_accum    = 0;
    s_enc_position = 0;
    return OPRT_OK;
}

/* Returns EV_DOWN/EV_UP on a debounced transition, else EV_NONE. */
static INPUT_EVENT_E scan_input(INPUT_STATE_T *in, uint32_t now, uint32_t *held_ms)
{
    TUYA_GPIO_LEVEL_E raw;
    if (tkl_gpio_read(in->pin, &raw) != OPRT_OK) {
        return EV_NONE;
    }
    if (raw != in->candidate) {
        in->candidate       = raw;
        in->candidate_since = now;
        return EV_NONE;
    }
    if (raw == in->stable || (uint32_t)(now - in->candidate_since) < INPUT_DEBOUNCE_MS) {
        return EV_NONE;
    }

    in->stable = raw;
    if (raw == in->active) {
        in->pressed_since = now;
        in->pressed       = true;
        *held_ms          = 0;
        return EV_DOWN;
    }
    in->pressed       = false;
    *held_ms          = (in->pressed_since == 0) ? 0 : (uint32_t)(now - in->pressed_since);
    in->pressed_since = 0;
    return EV_UP;
}

/* Debounce one encoder phase; returns true if its stable level changed. */
static bool scan_phase(PHASE_STATE_T *phase, TUYA_GPIO_NUM_E pin, uint32_t now)
{
    TUYA_GPIO_LEVEL_E raw;
    if (tkl_gpio_read(pin, &raw) != OPRT_OK) {
        return false;
    }
    if (raw != phase->candidate) {
        phase->candidate       = raw;
        phase->candidate_since = now;
        return false;
    }
    if (raw == phase->stable || (uint32_t)(now - phase->candidate_since) < ENC_DEBOUNCE_MS) {
        return false;
    }
    phase->stable = raw;
    return true;
}

/* Decode EC11 rotation; returns +1 (cw) / -1 (ccw) per detent, else 0. */
static int scan_encoder(uint32_t now)
{
    bool changed = false;
    changed |= scan_phase(&s_phase_a, AK_EC11_A_PIN, now);
    changed |= scan_phase(&s_phase_b, AK_EC11_B_PIN, now);
    if (!changed) {
        return 0;
    }
    uint8_t new_code = (uint8_t)(((s_phase_a.stable == TUYA_GPIO_LEVEL_HIGH ? 1U : 0U) << 1) |
                                 (s_phase_b.stable == TUYA_GPIO_LEVEL_HIGH ? 1U : 0U));
    int8_t  delta    = s_quad_table[(s_enc_code << 2) | new_code];
    s_enc_code       = new_code;
    if (delta == 0) {
        return 0;
    }
    s_enc_accum += delta;
    if (s_enc_accum >= ENC_SUBSTEPS_PER_DETENT) {
        s_enc_accum -= ENC_SUBSTEPS_PER_DETENT;
        return +1;
    }
    if (s_enc_accum <= -ENC_SUBSTEPS_PER_DETENT) {
        s_enc_accum += ENC_SUBSTEPS_PER_DETENT;
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------ INPUT mode */

static void input_draw(void)
{
    static const uint16_t row_h  = 24;
    static const uint16_t gap    = 4;
    uint16_t              base_y = CONTENT_Y + 6;
    char                  encline[28];

    /* Left column: the 6 keys. Right column: encoder push + rotation. */
    for (uint32_t i = 0; i < CNTSOF(s_inputs); i++) {
        bool     is_sw = s_inputs[i].is_enc_sw;
        uint16_t col   = is_sw ? 166 : 10;
        uint16_t idx   = is_sw ? 0 : (uint16_t)i;
        uint16_t x     = col;
        uint16_t y     = (uint16_t)(base_y + idx * (row_h + gap));
        uint16_t w     = 144;
        uint16_t bg    = s_inputs[i].pressed ? AK_COL_GREEN : AK_RGB565(40, 40, 48);
        uint16_t fg    = s_inputs[i].pressed ? AK_COL_BLACK : AK_COL_WHITE;

        ak_gfx_fill_rect(x, y, w, row_h, bg);
        ak_gfx_text((uint16_t)(x + 6), (uint16_t)(y + 5), s_inputs[i].label, fg, bg, 2);
    }

    /* Encoder rotation readout under the ENC push row. */
    (void)snprintf(encline, sizeof(encline), "ROT %s %ld", s_enc_last_dir, (long)s_enc_position);
    uint16_t ey = (uint16_t)(base_y + (row_h + gap));
    ak_gfx_fill_rect(166, ey, 144, row_h, AK_RGB565(40, 40, 48));
    ak_gfx_text(172, (uint16_t)(ey + 5), encline, AK_COL_YELLOW, AK_RGB565(40, 40, 48), 2);

    lcd_flush();
}

static void input_report(INPUT_STATE_T *in, INPUT_EVENT_E ev, uint32_t held_ms)
{
    s_sequence++;
    const char *action = (ev == EV_DOWN) ? "down" : "up";
    emit("{\"v\":1,\"seq\":%u,\"type\":\"key\",\"payload\":{\"id\":\"%s\",\"action\":\"%s\","
         "\"held_ms\":%u}}\n",
         s_sequence, in->id, action, held_ms);
}

static void enc_report(int dir)
{
    s_sequence++;
    s_enc_last_dir = (dir > 0) ? "cw" : "ccw";
    s_enc_position += dir;
    emit("{\"v\":1,\"seq\":%u,\"type\":\"enc\",\"payload\":{\"dir\":\"%s\",\"steps\":1,\"pos\":%ld}}\n",
         s_sequence, s_enc_last_dir, (long)s_enc_position);
}

/* ------------------------------------------------------------ LCD mode */

typedef struct {
    const char *name;
    uint16_t    color; /* 0 = special color-bars pattern */
    bool        bars;
} LCD_PATTERN_T;

static const LCD_PATTERN_T s_lcd_patterns[] = {
    {"color_bars", 0, true},      {"red", AK_COL_RED, false},     {"green", AK_COL_GREEN, false},
    {"blue", AK_COL_BLUE, false}, {"white", AK_COL_WHITE, false}, {"black", AK_COL_BLACK, false},
};

static void lcd_draw_bars(void)
{
    static const uint16_t colors[] = {
        AK_COL_RED, AK_COL_YELLOW, AK_COL_GREEN, AK_COL_CYAN, AK_COL_BLUE, AK_COL_MAGENTA, AK_COL_WHITE, AK_COL_BLACK,
    };
    uint16_t bw = SCREEN_W / (uint16_t)CNTSOF(colors);
    for (uint32_t i = 0; i < CNTSOF(colors); i++) {
        uint16_t x = (uint16_t)(i * bw);
        uint16_t w = (i == CNTSOF(colors) - 1) ? (uint16_t)(SCREEN_W - x) : bw;
        ak_gfx_fill_rect(x, 0, w, SCREEN_H, colors[i]);
    }
}

static void lcd_show(uint32_t step)
{
    const LCD_PATTERN_T *p = &s_lcd_patterns[step % CNTSOF(s_lcd_patterns)];
    if (p->bars) {
        lcd_draw_bars();
    } else {
        ak_gfx_clear(p->color);
    }
    draw_header();
    /* name banner near the bottom */
    ak_gfx_fill_rect(0, (uint16_t)(SCREEN_H - 24), SCREEN_W, 24, AK_COL_BLACK);
    ak_gfx_text(6, (uint16_t)(SCREEN_H - 20), p->name, AK_COL_WHITE, AK_COL_BLACK, 2);
    lcd_flush();
    emit("{\"v\":1,\"type\":\"selftest\",\"payload\":{\"mode\":\"LCD\",\"pattern\":\"%s\"}}\n", p->name);
}

/* ------------------------------------------------------------ WS2812 mode */

#define WRGB(r, g, b)                                                                                                  \
    {                                                                                                                  \
        .red = (r), .green = (g), .blue = (b), .cold = 0, .warm = 0                                                    \
    }

typedef struct {
    const char   *name;
    PIXEL_COLOR_T colors[AK_WS2812_COUNT];
} WS_PATTERN_T;

static const WS_PATTERN_T s_ws_patterns[] = {
    {"all_off", {[0 ... AK_WS2812_COUNT - 1] = WRGB(0, 0, 0)}},
    {"all_red", {[0 ... AK_WS2812_COUNT - 1] = WRGB(WS_LEVEL, 0, 0)}},
    {"all_green", {[0 ... AK_WS2812_COUNT - 1] = WRGB(0, WS_LEVEL, 0)}},
    {"all_blue", {[0 ... AK_WS2812_COUNT - 1] = WRGB(0, 0, WS_LEVEL)}},
    {"all_white", {[0 ... AK_WS2812_COUNT - 1] = WRGB(WS_LEVEL, WS_LEVEL, WS_LEVEL)}},
};

static OPERATE_RET ws_send(const PIXEL_COLOR_T colors[AK_WS2812_COUNT])
{
    OPERATE_RET rt;
    for (uint32_t i = 0; i < AK_WS2812_COUNT; i++) {
        TUYA_CALL_ERR_RETURN(tdl_pixel_set_single_color(s_pixels, i, 1U, (PIXEL_COLOR_T *)&colors[i]));
    }
    return tdl_pixel_dev_refresh(s_pixels);
}

static void ws_show(uint32_t step)
{
    const WS_PATTERN_T *p  = &s_ws_patterns[step % CNTSOF(s_ws_patterns)];
    OPERATE_RET         rt = OPRT_OK;

    if (s_ws_ok) {
        rt = ws_send(p->colors);
    }

    clear_content();
    ak_gfx_text(8, CONTENT_Y + 10, "WS2812 x7 @ SPI1 P4", AK_COL_WHITE, AK_COL_BLACK, 1);
    ak_gfx_text(8, CONTENT_Y + 40, "Pattern:", AK_COL_GRAY, AK_COL_BLACK, 2);
    ak_gfx_text(8, CONTENT_Y + 70, p->name, AK_COL_YELLOW, AK_COL_BLACK, 2);
    if (!s_ws_ok || rt != OPRT_OK) {
        ak_gfx_text(8, CONTENT_Y + 110, "DRIVER ERROR", AK_COL_RED, AK_COL_BLACK, 2);
    }
    lcd_flush();
    emit("{\"v\":1,\"type\":\"selftest\",\"payload\":{\"mode\":\"WS2812\",\"pattern\":\"%s\",\"result\":%d}}\n",
         p->name, rt);
}

/* ------------------------------------------------------------ UART mode */

static void uart_draw(void)
{
    char line[40];
    clear_content();
    ak_gfx_text(8, CONTENT_Y + 10, "UART0 loopback P10/P11", AK_COL_WHITE, AK_COL_BLACK, 1);
    ak_gfx_text(8, CONTENT_Y + 30, "Send text; it echoes.", AK_COL_GRAY, AK_COL_BLACK, 1);

    (void)snprintf(line, sizeof(line), "RX bytes: %u", (unsigned)s_uart_rx);
    ak_gfx_text(8, CONTENT_Y + 60, line, AK_COL_CYAN, AK_COL_BLACK, 2);
    (void)snprintf(line, sizeof(line), "TX echo : %u", (unsigned)s_uart_echo);
    ak_gfx_text(8, CONTENT_Y + 90, line, AK_COL_CYAN, AK_COL_BLACK, 2);

    ak_gfx_text(8, CONTENT_Y + 130, "Last:", AK_COL_GRAY, AK_COL_BLACK, 1);
    ak_gfx_text(8, CONTENT_Y + 148, s_uart_last, AK_COL_GREEN, AK_COL_BLACK, 2);
    lcd_flush();
}

/* ------------------------------------------------------------ RADIO mode */

static uint8_t s_adv_data[] = {0x02, 0x01, 0x06};
static uint8_t s_scan_rsp[] = {0x0B, 0x09, 'A', 'K', '-', 'S', 'e', 'l', 'f', 't', 's', 't'};

static void ble_event_cb(TAL_BLE_EVT_PARAMS_T *event)
{
    if (event == NULL) {
        return;
    }
    switch (event->type) {
    case TAL_BLE_STACK_INIT:
        if (event->ble_event.init == 0) {
            TAL_BLE_DATA_T adv = {.len = sizeof(s_adv_data), .p_data = s_adv_data};
            TAL_BLE_DATA_T rsp = {.len = sizeof(s_scan_rsp), .p_data = s_scan_rsp};
            tal_ble_advertising_data_set(&adv, &rsp);
            tal_ble_advertising_start(TUYAOS_BLE_DEFAULT_ADV_PARAM);
            s_ble_state = "advertising";
        } else {
            s_ble_state = "init_error";
        }
        break;
    case TAL_BLE_EVT_PERIPHERAL_CONNECT:
        s_ble_state = "connected";
        break;
    case TAL_BLE_EVT_DISCONNECT:
        s_ble_state = "advertising";
        tal_ble_advertising_start(TUYAOS_BLE_DEFAULT_ADV_PARAM);
        break;
    default:
        break;
    }
}

static void radio_scan(void)
{
    AP_IF_S    *aps   = NULL;
    uint32_t    count = 0;
    int         best  = INT_MIN;
    OPERATE_RET rt    = tal_wifi_all_ap_scan(&aps, &count);

    if (rt != OPRT_OK) {
        s_ap_count  = 0;
        s_best_rssi = 0;
        emit("{\"v\":1,\"type\":\"selftest\",\"payload\":{\"mode\":\"RADIO\",\"wifi\":\"scan_error\","
             "\"result\":%d}}\n",
             rt);
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        if ((int)aps[i].rssi > best) {
            best = aps[i].rssi;
        }
    }
    s_ap_count  = count;
    s_best_rssi = (count == 0) ? 0 : best;
    tal_wifi_release_ap(aps);

    emit("{\"v\":1,\"type\":\"selftest\",\"payload\":{\"mode\":\"RADIO\",\"wifi\":\"scan_ok\",\"ap_count\":%u,"
         "\"best_rssi\":%d,\"ble\":\"%s\"}}\n",
         (unsigned)s_ap_count, s_best_rssi, s_ble_state);
}

static void radio_draw(bool scanning)
{
    char line[40];
    clear_content();
    ak_gfx_text(8, CONTENT_Y + 10, "Wi-Fi scan + BLE adv", AK_COL_WHITE, AK_COL_BLACK, 1);

    (void)snprintf(line, sizeof(line), "BLE: %s", s_ble_state);
    ak_gfx_text(8, CONTENT_Y + 40, line, AK_COL_YELLOW, AK_COL_BLACK, 2);

    if (scanning) {
        ak_gfx_text(8, CONTENT_Y + 80, "WiFi: scanning...", AK_COL_GRAY, AK_COL_BLACK, 2);
    } else {
        (void)snprintf(line, sizeof(line), "APs: %u", (unsigned)s_ap_count);
        ak_gfx_text(8, CONTENT_Y + 80, line, AK_COL_CYAN, AK_COL_BLACK, 2);
        (void)snprintf(line, sizeof(line), "Best RSSI: %d", s_best_rssi);
        ak_gfx_text(8, CONTENT_Y + 110, line, AK_COL_CYAN, AK_COL_BLACK, 2);
    }
    lcd_flush();
}

/* ------------------------------------------------------------ mode enter */

static void mode_enter(TEST_MODE_E mode)
{
    s_mode       = mode;
    s_mode_since = tal_system_get_millisecond();
    s_last_tick  = s_mode_since;
    s_step       = 0;

    draw_header();
    clear_content();

    switch (mode) {
    case TEST_INPUT:
        s_input_dirty = true;
        break;
    case TEST_LCD:
        lcd_show(0);
        break;
    case TEST_WS2812:
        ws_show(0);
        break;
    case TEST_UART:
        s_uart_rx      = 0;
        s_uart_echo    = 0;
        s_uart_last[0] = '\0';
        uart_draw();
        break;
    case TEST_WIRELESS:
        radio_draw(true);
        radio_scan();
        radio_draw(false);
        break;
    default:
        break;
    }
    emit_mode();
}

static void switch_mode(void)
{
    /* Turn LEDs off when leaving the WS2812 test so they don't stay lit. */
    if (s_mode == TEST_WS2812 && s_ws_ok) {
        (void)ws_send(s_ws_patterns[0].colors);
    }
    mode_enter((TEST_MODE_E)((s_mode + 1) % TEST_COUNT));
}

/* ------------------------------------------------------------ per-mode tick */

static void mode_tick(uint32_t now)
{
    switch (s_mode) {
    case TEST_INPUT:
        if (s_input_dirty) {
            s_input_dirty = false;
            input_draw();
        }
        break;

    case TEST_LCD:
        if ((uint32_t)(now - s_last_tick) >= LCD_PATTERN_MS) {
            s_last_tick = now;
            s_step++;
            lcd_show(s_step);
        }
        break;

    case TEST_WS2812:
        if ((uint32_t)(now - s_last_tick) >= WS_PATTERN_MS) {
            s_last_tick = now;
            s_step++;
            ws_show(s_step);
        }
        break;

    case TEST_UART: {
        uint8_t buf[64];
        int     n = bringup_uart_read(buf, sizeof(buf));
        if (n > 0) {
            bringup_uart_write(buf, (uint32_t)n);
            s_uart_rx += (uint32_t)n;
            s_uart_echo += (uint32_t)n;
            uint32_t copy = (n < (int)sizeof(s_uart_last) - 1) ? (uint32_t)n : sizeof(s_uart_last) - 1;
            for (uint32_t i = 0; i < copy; i++) {
                char c         = (char)buf[i];
                s_uart_last[i] = (c >= 0x20 && c < 0x7F) ? c : '.';
            }
            s_uart_last[copy] = '\0';
        }
        if ((uint32_t)(now - s_last_tick) >= UART_REFRESH_MS) {
            s_last_tick = now;
            uart_draw();
        }
        break;
    }

    case TEST_WIRELESS:
        if ((uint32_t)(now - s_last_tick) >= WIFI_SCAN_MS) {
            s_last_tick = now;
            radio_draw(true);
            radio_scan();
            radio_draw(false);
        }
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------ peripherals */

static const uint8_t s_st7789_init_seq_noinv[] = {
    1,
    100,
    ST7789_SWRESET,
    1,
    50,
    ST7789_SLPOUT,
    2,
    10,
    ST7789_COLMOD,
    0x55,
    2,
    0,
    ST7789_VCMOFSET,
    0x1a,
    6,
    0,
    ST7789_PORCTRL,
    0x0c,
    0x0c,
    0x00,
    0x33,
    0x33,
    1,
    0,
    ST7789_INVOFF,
    2,
    0,
    ST7789_GCTRL,
    0x56,
    2,
    0,
    ST7789_VCOMS,
    0x18,
    2,
    0,
    ST7789_LCMCTRL,
    0x2c,
    2,
    0,
    ST7789_VDVVRHEN,
    0x01,
    2,
    0,
    ST7789_VRHS,
    0x1f,
    2,
    0,
    ST7789_VDVSET,
    0x20,
    2,
    0,
    ST7789_FRCTR2,
    0x0f,
    3,
    0,
    ST7789_PWCTRL1,
    0xa6,
    0xa1,
    2,
    0,
    ST7789_PWCTRL2,
    0x03,
    2,
    0,
    ST7789_MADCTL,
    0x60, /* MV|MX: rotate 90 deg clockwise to landscape (320x240) */
    15,
    0,
    ST7789_PVGAMCTRL,
    0xd0,
    0x0d,
    0x14,
    0x0b,
    0x0b,
    0x07,
    0x3a,
    0x44,
    0x50,
    0x08,
    0x13,
    0x13,
    0x2d,
    0x32,
    15,
    0,
    ST7789_NVGAMCTRL,
    0xd0,
    0x0d,
    0x14,
    0x0b,
    0x0b,
    0x07,
    0x3a,
    0x44,
    0x50,
    0x08,
    0x13,
    0x13,
    0x2d,
    0x32,
    1,
    0,
    ST7789_SPI2EN,
    1,
    10,
    ST7789_INVOFF,
    1,
    10,
    ST7789_DISPON,
    0,
};

static OPERATE_RET display_init(void)
{
    DISP_SPI_DEVICE_CFG_T config;
    memset(&config, 0, sizeof(config));
    config.width     = SCREEN_W;
    config.height    = SCREEN_H;
    config.pixel_fmt = TUYA_PIXEL_FMT_RGB565;
    config.rotation  = TUYA_DISPLAY_ROTATION_0;
    config.port      = AK_LCD_SPI_PORT;
    config.spi_clk   = AK_LCD_SPI_CLOCK_HZ;
    config.cs_pin    = AK_LCD_SPI_CS_PIN;
    config.dc_pin    = AK_LCD_DC_PIN;
    config.rst_pin   = AK_LCD_RST_PIN;

    config.bl.type              = TUYA_DISP_BL_TP_GPIO;
    config.bl.gpio.pin          = AK_LCD_BACKLIGHT_PIN;
    config.bl.gpio.active_level = AK_LCD_CONTROL_ACTIVE;
    /* v0.3: no LCD_PWR line (P7 is now TE). Sentinel = "no power GPIO". */
    config.power.pin            = TUYA_GPIO_NUM_MAX;
    config.power.active_level   = TUYA_GPIO_LEVEL_LOW;

    tdd_disp_spi_st7789_set_init_seq(s_st7789_init_seq_noinv);

    OPERATE_RET rt = tdd_disp_spi_st7789_register(DISPLAY_NAME, &config);
    if (rt != OPRT_OK) {
        return rt;
    }

    s_display = tdl_disp_find_dev(DISPLAY_NAME);
    if (s_display == NULL) {
        return OPRT_NOT_FOUND;
    }
    TUYA_CALL_ERR_RETURN(tdl_disp_dev_get_info(s_display, &s_info));
    TUYA_CALL_ERR_RETURN(tdl_disp_dev_open(s_display));
    TUYA_CALL_ERR_RETURN(tdl_disp_set_brightness(s_display, 100));

    s_frame = tdl_disp_create_frame_buff(DISP_FB_TP_PSRAM, FRAME_BYTES);
    if (s_frame == NULL) {
        return OPRT_MALLOC_FAILED;
    }
    s_frame->x_start = 0;
    s_frame->y_start = 0;
    s_frame->fmt     = TUYA_PIXEL_FMT_RGB565;
    s_frame->width   = SCREEN_W;
    s_frame->height  = SCREEN_H;

    ak_gfx_bind(s_frame, s_info.is_swap);
    return OPRT_OK;
}

static OPERATE_RET ws2812_init(void)
{
    PIXEL_DRIVER_CONFIG_T cfg = {
        .port     = AK_WS2812_SPI_PORT,
        .line_seq = RGB_ORDER,
    };
    OPERATE_RET rt = tdd_ws2812_driver_register(WS_DEV_NAME, &cfg);
    if (rt != OPRT_OK) {
        return rt;
    }
    TUYA_CALL_ERR_RETURN(tdl_pixel_dev_find(WS_DEV_NAME, &s_pixels));

    PIXEL_DEV_CONFIG_T dev = {
        .pixel_num        = AK_WS2812_COUNT,
        .pixel_resolution = 255U,
    };
    return tdl_pixel_dev_open(s_pixels, &dev);
}

static void radio_init(void)
{
    tal_kv_init(&(tal_kv_cfg_t){.seed = "agentic-selftest", .key = "selftest"});
    tal_sw_timer_init();
    tal_workq_init();
    if (tal_ble_bt_init(TAL_BLE_ROLE_PERIPERAL, ble_event_cb) != OPRT_OK) {
        PR_ERR("BLE init failed");
        s_ble_state = "init_error";
    }
}

/* ------------------------------------------------------------ main */

static void user_main(void)
{
    OPERATE_RET rt;

    tal_log_init(TAL_LOG_LEVEL_DEBUG, 2048, (TAL_LOG_OUTPUT_CB)tkl_log_output);
    PR_NOTICE("======== Agentic Keyboard: full_selftest ========");
    PR_NOTICE("Built: %s %s", __DATE__, __TIME__);

    rt = bringup_uart_init();
    if (rt != OPRT_OK) {
        PR_ERR("UART0 init failed: %d", rt);
        return;
    }

    rt = display_init();
    if (rt != OPRT_OK) {
        PR_ERR("Display init failed: %d (continuing headless)", rt);
    } else {
        s_lcd_ok = true;
    }

    rt = ws2812_init();
    if (rt != OPRT_OK) {
        PR_ERR("WS2812 init failed: %d", rt);
    } else {
        s_ws_ok = true;
    }

    radio_init();

    rt = inputs_init();
    if (rt != OPRT_OK) {
        PR_ERR("Input init failed: %d", rt);
        return;
    }

    emit("{\"v\":1,\"type\":\"selftest\",\"payload\":{\"status\":\"ready\",\"lcd\":%s,\"ws2812\":%s,"
         "\"modes\":%u}}\n",
         s_lcd_ok ? "true" : "false", s_ws_ok ? "true" : "false", (unsigned)TEST_COUNT);

    mode_enter(TEST_INPUT);

    uint32_t last_heartbeat = tal_system_get_millisecond();

    while (1) {
        uint32_t now = tal_system_get_millisecond();

        /* Scan every key; report events only while in the INPUT test. */
        for (uint32_t i = 0; i < CNTSOF(s_inputs); i++) {
            uint32_t      held = 0;
            INPUT_EVENT_E ev   = scan_input(&s_inputs[i], now, &held);
            if (ev != EV_NONE) {
                if (s_mode == TEST_INPUT) {
                    input_report(&s_inputs[i], ev, held);
                    s_input_dirty = true;
                }
            }
        }

        /* Scan the EC11 rotation; report + redraw only in the INPUT test. */
        {
            int dir = scan_encoder(now);
            if (dir != 0 && s_mode == TEST_INPUT) {
                enc_report(dir);
                s_input_dirty = true;
            }
        }

        /* ENC push long-press switches test mode in any mode. */
        if (s_inputs[ENC_SW_INDEX].stable == s_inputs[ENC_SW_INDEX].active) {
            if (!s_center_active) {
                s_center_active   = true;
                s_center_since    = now;
                s_center_consumed = false;
            } else if (!s_center_consumed && (uint32_t)(now - s_center_since) >= LONG_PRESS_MS) {
                s_center_consumed = true;
                switch_mode();
            }
        } else {
            s_center_active = false;
        }

        mode_tick(now);

        if ((uint32_t)(now - last_heartbeat) >= HEARTBEAT_INTERVAL_MS) {
            last_heartbeat = now;
            emit("{\"v\":1,\"type\":\"selftest\",\"payload\":{\"status\":\"alive\",\"mode\":\"%s\","
                 "\"uptime_ms\":%u}}\n",
                 s_mode_names[s_mode], now);
        }

        tal_system_sleep(POLL_INTERVAL_MS);
    }
}

static void app_thread(void *arg)
{
    (void)arg;
    user_main();
    tal_thread_delete(s_app_thread);
    s_app_thread = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T thread_cfg = {
        .stackDepth = APP_TASK_STACK_SIZE,
        .priority   = THREAD_PRIO_1,
        .thrdname   = "full_selftest",
    };
    tal_thread_create_and_start(&s_app_thread, NULL, NULL, app_thread, NULL, &thread_cfg);
}
