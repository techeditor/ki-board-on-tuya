/**
 * @file tuya_app_main.c
 * @brief v0.3.2 WS2812 chain power switch (LED_PWR_EN = P18) bring-up.
 *
 * Power-on order (hardware/eda/ws2812-power-reduction-study.md §3, §6.3):
 *   DIN (P4) idle low -> P18 high -> wait for VDD_LED to settle -> reset
 *   (DIN low >= 280 us) -> first colour frame.
 * Power-off order: all-zero frame -> DIN idle low -> P18 low; wait >= 200 ms
 * before the next power-on (Q2 turn-off through R9/C15 takes ~160 ms).
 *
 * Steps: a boot-time back-feed check (P4 driven high with the chain off),
 * then commands from UART0 or keys: on, off, on-but-dark, an on/off stress
 * loop, and a settle-time sweep for first-frame colour errors. Every step and
 * result is printed on UART0 (115200 8N1) together with what to measure.
 *
 * Only v0.3.2 boards have the switch. On lite / v0.3.1 P18 is not connected,
 * the chain is always powered and the P18 steps have no effect.
 */

#include <stdbool.h>
#include <string.h>

#include "agentic_board_io.h"
#include "bringup_led_power.h"
#include "bringup_uart.h"
#include "tal_api.h"
#include "tkl_gpio.h"
#include "tkl_output.h"

#include "tdd_pixel_ws2812.h"
#include "tdd_pixel_type.h"
#include "tdl_pixel_dev_manage.h"
#include "tdl_pixel_color_manage.h"

#define APP_TASK_STACK_SIZE (4U * 1024U)
#define WS2812_DEVICE_NAME  "bringup_ledpwr"
#define WS2812_RESOLUTION   255U
#define WS2812_LEVEL        24U /* low brightness: 7x white at full is ~255 mA */
#define SETTLE_DEFAULT_MS   BRINGUP_LED_PWR_SETTLE_MS
#define OFF_HOLD_MS         BRINGUP_LED_PWR_OFF_HOLD_MS
#define BACKFEED_WINDOW_MS  10000U /* time to measure VDD_LED with P4 high */
#define STRESS_ON_MS        200U
#define STRESS_CYCLES_SHORT 50U
#define STRESS_CYCLES_LONG  500U
#define SWEEP_HOLD_MS       1500U
#define KEY_DEBOUNCE_MS     30U
#define POLL_MS             10U

static THREAD_HANDLE  s_app_thread = NULL;
static PIXEL_HANDLE_T s_pixels     = NULL;

/* Test pattern: LED1..LED7 = R G B W R G B, so a swapped or shifted first
 * frame is easy to spot. */
static const uint8_t k_pattern[AK_WS2812_COUNT][3] = {
    {WS2812_LEVEL, 0, 0}, {0, WS2812_LEVEL, 0}, {0, 0, WS2812_LEVEL}, {WS2812_LEVEL, WS2812_LEVEL, WS2812_LEVEL},
    {WS2812_LEVEL, 0, 0}, {0, WS2812_LEVEL, 0}, {0, 0, WS2812_LEVEL},
};

typedef struct {
    TUYA_GPIO_NUM_E   pin;
    const char       *id;
    TUYA_GPIO_LEVEL_E stable;
    TUYA_GPIO_LEVEL_E candidate;
    uint32_t          since;
} KEY_T;

static KEY_T s_keys[] = {
    {AK_KEY1_PIN, "k1", AK_KEY_INACTIVE_LEVEL, AK_KEY_INACTIVE_LEVEL, 0U},
    {AK_KEY2_PIN, "k2", AK_KEY_INACTIVE_LEVEL, AK_KEY_INACTIVE_LEVEL, 0U},
    {AK_KEY3_PIN, "k3", AK_KEY_INACTIVE_LEVEL, AK_KEY_INACTIVE_LEVEL, 0U},
    {AK_KEY4_PIN, "k4", AK_KEY_INACTIVE_LEVEL, AK_KEY_INACTIVE_LEVEL, 0U},
    {AK_KEY5_PIN, "k5", AK_KEY_INACTIVE_LEVEL, AK_KEY_INACTIVE_LEVEL, 0U},
};

/* One result line, JSON like the other bring-up apps, stamped with t_ms (ms
 * since boot) so step timing can be read off the serial log. */
#define REPORT(fmt, ...)                                                                                               \
    bringup_uart_printf("{\"v\":1,\"type\":\"bringup\",\"payload\":{\"name\":\"led_power\",\"t_ms\":%lu," fmt "}}\n",  \
                        (unsigned long)tal_system_get_millisecond(), ##__VA_ARGS__)
/* Human instruction / measurement hint. */
#define HINT(text) bringup_uart_printf("# %s\n", text)

static OPERATE_RET frame_send(bool lit)
{
    OPERATE_RET rt;
    for (uint32_t i = 0; i < AK_WS2812_COUNT; i++) {
        PIXEL_COLOR_T c = {0};
        if (lit) {
            c.red   = k_pattern[i][0];
            c.green = k_pattern[i][1];
            c.blue  = k_pattern[i][2];
        }
        TUYA_CALL_ERR_RETURN(tdl_pixel_set_single_color(s_pixels, i, 1U, &c));
    }
    return tdl_pixel_dev_refresh(s_pixels);
}

/* DIN idle low -> P18 high -> settle -> reset -> first frame (shared sequence). */
static OPERATE_RET power_on(uint32_t settle_ms, bool lit, bool reset)
{
    bringup_led_power_on(settle_ms, reset);
    return frame_send(lit);
}

/* All-zero frame -> DIN idle low -> P18 low (shared sequence). */
static OPERATE_RET power_off(void)
{
    return bringup_led_power_off(s_pixels, AK_WS2812_COUNT);
}

static void cmd_on(void)
{
    OPERATE_RET rt = power_on(SETTLE_DEFAULT_MS, true, true);
    REPORT("\"step\":\"on\",\"settle_ms\":%u,\"p18\":1,\"rc\":%d", (unsigned)SETTLE_DEFAULT_MS, rt);
    HINT("Expect LED1..LED7 = R G B W R G B. Wrong first colours: see the sweep (k4 / w).");
    HINT("Scope: trigger on P18 (J1.11) rising; VDD_LED (Q2.D / C1) should reach 90 % in ~3.3 ms,");
    HINT("       LED1 DIN quiet until after the settle time. Inrush: current on VDD_5V, expect ~0.5 A peak.");
}

static void cmd_dark_on(void)
{
    OPERATE_RET rt = power_on(SETTLE_DEFAULT_MS, false, true);
    REPORT("\"step\":\"on_dark\",\"p18\":1,\"rc\":%d", rt);
    HINT("Chain powered, all LEDs off. Measure 5 V input current now (standby with the chain on),");
    HINT("then k2 / 0 and measure again (chain off). Expected difference ~4 mA (7 WS2812 idle).");
}

static void cmd_off(void)
{
    OPERATE_RET rt = power_off();
    REPORT("\"step\":\"off\",\"p18\":0,\"rc\":%d", rt);
    HINT("Check: all 7 LEDs dark. VDD_LED falls to ~0 V within ~200 ms (Q2 turn-off ~160 ms).");
    HINT("Measure 5 V input current: this is the chain-off standby value.");
}

/* A byte on UART0 or K2 aborts a long run. */
static bool abort_requested(void)
{
    uint8_t           b;
    TUYA_GPIO_LEVEL_E level;
    if (bringup_uart_read(&b, 1U) > 0) {
        return true;
    }
    return tkl_gpio_read(AK_KEY2_PIN, &level) == OPRT_OK && level == AK_KEY_ACTIVE_LEVEL;
}

static void cmd_stress(uint32_t cycles)
{
    uint32_t errors = 0U;
    uint32_t done   = 0U;
    REPORT("\"step\":\"stress\",\"status\":\"start\",\"cycles\":%u", (unsigned)cycles);
    HINT("Watch for a cycle with wrong colours, a stuck LED or a flicker while off. Any key/byte stops.");
    for (done = 0U; done < cycles; done++) {
        if (power_on(SETTLE_DEFAULT_MS, true, true) != OPRT_OK) {
            errors++;
        }
        tal_system_sleep(STRESS_ON_MS);
        if (power_off() != OPRT_OK) {
            errors++;
        }
        tal_system_sleep(OFF_HOLD_MS);
        if ((done + 1U) % 10U == 0U) {
            REPORT("\"step\":\"stress\",\"cycle\":%u,\"of\":%u,\"driver_errors\":%u", (unsigned)(done + 1U),
                   (unsigned)cycles, (unsigned)errors);
        }
        if (abort_requested()) {
            done++;
            break;
        }
    }
    REPORT("\"step\":\"stress\",\"status\":\"%s\",\"cycles\":%u,\"driver_errors\":%u",
           done == cycles ? "done" : "aborted", (unsigned)done, (unsigned)errors);
    HINT("driver_errors only counts SPI/driver failures. Record what you SAW per cycle.");
}

static void cmd_sweep(void)
{
    static const uint32_t settle[] = {0U, 1U, 3U, 5U, 10U, 20U};
    REPORT("\"step\":\"sweep\",\"status\":\"start\",\"count\":%u", (unsigned)CNTSOF(settle));
    HINT("Each run starts from a fully off chain. Note whether the FIRST frame is R G B W R G B.");
    for (uint32_t i = 0; i < CNTSOF(settle); i++) {
        (void)power_off();
        tal_system_sleep(OFF_HOLD_MS);
        /* 0 ms: no settle and no reset gap, the worst case. */
        bool        reset = settle[i] > 0U;
        OPERATE_RET rt    = power_on(settle[i], true, reset);
        REPORT("\"step\":\"sweep\",\"settle_ms\":%u,\"reset\":%s,\"rc\":%d", (unsigned)settle[i],
               reset ? "true" : "false", rt);
        tal_system_sleep(SWEEP_HOLD_MS);
    }
    (void)power_off();
    REPORT("\"step\":\"sweep\",\"status\":\"done\"");
    HINT("Firmware uses 10 ms. Record the shortest wait that is always correct.");
}

static void print_help(void)
{
    HINT("led_power: v0.3.2 WS2812 power switch (LED_PWR_EN = P18). lite / v0.3.1: P18 not connected,");
    HINT("the chain is always powered there and the P18 steps have no effect.");
    HINT("Commands (UART0 byte or key):");
    HINT("  1 / k1  power on: P18 high, settle 10 ms, reset, R G B W R G B");
    HINT("  0 / k2  power off: dark frame, DIN low, P18 low");
    HINT("  d / k5  power on, all LEDs dark (standby current with the chain on)");
    HINT("  s / k3  stress: 50 on/off cycles      S  500 cycles");
    HINT("  w / k4  settle sweep 0/1/3/5/10/20 ms, first-frame colours");
    HINT("  p       print state      h  this help");
    HINT("  Back-feed check runs once at boot (reboot to repeat).");
}

static void run_command(char c)
{
    switch (c) {
    case '1':
        cmd_on();
        break;
    case '0':
        cmd_off();
        break;
    case 'd':
        cmd_dark_on();
        break;
    case 's':
        cmd_stress(STRESS_CYCLES_SHORT);
        break;
    case 'S':
        cmd_stress(STRESS_CYCLES_LONG);
        break;
    case 'w':
        cmd_sweep();
        break;
    case 'p':
        REPORT("\"step\":\"state\",\"p18\":%u", bringup_led_power_is_on() ? 1U : 0U);
        break;
    case 'h':
    case '?':
        print_help();
        break;
    default:
        break;
    }
}

/* Chain off, P4 driven high as a plain GPIO: VDD_LED should stay ~0 V. Runs
 * before the pixel driver opens SPI1, which re-muxes P4 afterwards. */
static void backfeed_check(void)
{
    TUYA_GPIO_BASE_CFG_T out = {
        .mode   = TUYA_GPIO_PUSH_PULL,
        .direct = TUYA_GPIO_OUTPUT,
        .level  = TUYA_GPIO_LEVEL_HIGH,
    };
    if (tkl_gpio_init(AK_WS2812_DATA_PIN, &out) != OPRT_OK) {
        REPORT("\"step\":\"backfeed\",\"status\":\"skipped\",\"reason\":\"p4_gpio\"");
        return;
    }
    REPORT("\"step\":\"backfeed\",\"status\":\"start\",\"p18\":0,\"p4\":1,\"window_ms\":%u",
           (unsigned)BACKFEED_WINDOW_MS);
    HINT("Back-feed: P18 low, P4 high for 10 s. Measure VDD_LED (Q2.D / C1+): expect < 0.3 V;");
    HINT("clearly higher means a back-feed path through U2. No LED may glow.");
    tal_system_sleep(BACKFEED_WINDOW_MS);
    (void)tkl_gpio_write(AK_WS2812_DATA_PIN, TUYA_GPIO_LEVEL_LOW);
    (void)tkl_gpio_deinit(AK_WS2812_DATA_PIN);
    REPORT("\"step\":\"backfeed\",\"status\":\"done\"");
}

static void keys_init(void)
{
    TUYA_GPIO_BASE_CFG_T in = {
        .mode   = TUYA_GPIO_PULLUP,
        .direct = TUYA_GPIO_INPUT,
        .level  = AK_KEY_INACTIVE_LEVEL,
    };
    for (uint32_t i = 0; i < CNTSOF(s_keys); i++) {
        (void)tkl_gpio_init(s_keys[i].pin, &in);
    }
}

/* Debounced key press -> the same command as its UART byte. */
static void keys_poll(uint32_t now)
{
    static const char cmd[] = {'1', '0', 's', 'w', 'd'};
    for (uint32_t i = 0; i < CNTSOF(s_keys); i++) {
        TUYA_GPIO_LEVEL_E raw;
        KEY_T            *k = &s_keys[i];
        if (tkl_gpio_read(k->pin, &raw) != OPRT_OK) {
            continue;
        }
        if (raw != k->candidate) {
            k->candidate = raw;
            k->since     = now;
            continue;
        }
        if (raw == k->stable || (uint32_t)(now - k->since) < KEY_DEBOUNCE_MS) {
            continue;
        }
        k->stable = raw;
        if (raw == AK_KEY_ACTIVE_LEVEL) {
            REPORT("\"key\":\"%s\"", k->id);
            run_command(cmd[i]);
        }
    }
}

static void user_main(void)
{
    OPERATE_RET rt;

    /* P18 first: keep the chain off from the very start (R9 already holds it). */
    bringup_led_power_init();

    tal_log_init(TAL_LOG_LEVEL_DEBUG, 2048, (TAL_LOG_OUTPUT_CB)tkl_log_output);
    PR_NOTICE("======== Agentic Keyboard bring-up: led_power ========");
    PR_NOTICE("Firmware: %s v%s, built %s %s", PROJECT_NAME, PROJECT_VERSION, __DATE__, __TIME__);

    rt = bringup_uart_init();
    if (rt != OPRT_OK) {
        PR_ERR("UART0 init failed: %d", rt);
        return;
    }
    keys_init();
    REPORT("\"version\":\"%s\",\"status\":\"boot\",\"p18_pin\":%u,\"data_pin\":%u,\"count\":%u", PROJECT_VERSION,
           (unsigned)AK_LED_PWR_EN_PIN, (unsigned)AK_WS2812_DATA_PIN, (unsigned)AK_WS2812_COUNT);
    bringup_led_power_print_option("led_power", true);
    print_help();

    backfeed_check();

    PIXEL_DRIVER_CONFIG_T drv = {
        .port     = AK_WS2812_SPI_PORT,
        .line_seq = GRB_ORDER, /* as firmware/app: RGB_ORDER swaps red and green on the real chain */
    };
    rt = tdd_ws2812_driver_register(WS2812_DEVICE_NAME, &drv);
    if (rt == OPRT_OK) {
        rt = tdl_pixel_dev_find(WS2812_DEVICE_NAME, &s_pixels);
    }
    if (rt == OPRT_OK) {
        PIXEL_DEV_CONFIG_T dev = {
            .pixel_num        = AK_WS2812_COUNT,
            .pixel_resolution = WS2812_RESOLUTION,
        };
        rt = tdl_pixel_dev_open(s_pixels, &dev);
    }
    if (rt != OPRT_OK) {
        REPORT("\"status\":\"error\",\"stage\":\"ws2812_open\",\"code\":%d", rt);
        return;
    }
    REPORT("\"status\":\"ready\",\"p18\":0");
    HINT("Ready. Chain is off. Press k1 or send 1 to power on.");

    while (1) {
        uint8_t c;
        while (bringup_uart_read(&c, 1U) > 0) {
            run_command((char)c);
        }
        keys_poll(tal_system_get_millisecond());
        tal_system_sleep(POLL_MS);
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
        .thrdname   = "led_power",
    };

    tal_thread_create_and_start(&s_app_thread, NULL, NULL, app_thread, NULL, &thread_cfg);
}
