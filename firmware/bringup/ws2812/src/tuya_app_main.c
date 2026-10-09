/**
 * @file tuya_app_main.c
 * @brief Validate seven WS2812 LEDs on the v0.3 SPI1_MOSI (P4) data net.
 *
 * v0.3 has 7 LEDs (one per key K1-K6 + LED7 near the EC11 encoder) in a single
 * daisy chain. The waveform is produced by the official TuyaOpen SPI WS2812
 * driver (tdd_pixel_ws2812 + tdl_pixel_*): each WS2812 bit is encoded as 8 SPI
 * bits @6.5 MHz (0=0xC0/1=0xF0), so it is immune to the CPU/interrupt jitter
 * that made the v1.0 P13 GPIO bit-bang driver unreliable. SPI1 initialises the
 * whole P2/P3/P4/P5 group; only MOSI=P4 carries data.
 */

#include <string.h>

#include "agentic_board_io.h"
#include "bringup_uart.h"
#include "tal_api.h"
#include "tkl_output.h"

#include "tdd_pixel_ws2812.h"
#include "tdd_pixel_type.h"
#include "tdl_pixel_dev_manage.h"
#include "tdl_pixel_color_manage.h"

#define APP_TASK_STACK_SIZE (4U * 1024U)
#define PATTERN_DELAY_MS    700U
#define WS2812_DRIVER_REV   "spi1-official-v7-7led"
#define WS2812_DEVICE_NAME  "bringup_ws2812"
#define WS2812_RESOLUTION   255U
#define WS2812_LEVEL        16U /* keep brightness low: 7x white peak ~255mA */

typedef struct {
    const char   *name;
    PIXEL_COLOR_T colors[AK_WS2812_COUNT];
} WS_PATTERN_T;

#define RGB(r, g, b)                                                                                                   \
    {                                                                                                                  \
        .red = (r), .green = (g), .blue = (b), .cold = 0, .warm = 0                                                    \
    }

static THREAD_HANDLE  s_app_thread = NULL;
static PIXEL_HANDLE_T s_pixels     = NULL;

/* Fixed patterns run after the per-LED chain walk: RGB sweep then all white. */
static const WS_PATTERN_T s_fixed_patterns[] = {
    {"all_red", {[0 ... AK_WS2812_COUNT - 1] = RGB(WS2812_LEVEL, 0, 0)}},
    {"all_green", {[0 ... AK_WS2812_COUNT - 1] = RGB(0, WS2812_LEVEL, 0)}},
    {"all_blue", {[0 ... AK_WS2812_COUNT - 1] = RGB(0, 0, WS2812_LEVEL)}},
    {"all_white", {[0 ... AK_WS2812_COUNT - 1] = RGB(WS2812_LEVEL, WS2812_LEVEL, WS2812_LEVEL)}},
    {"all_off", {[0 ... AK_WS2812_COUNT - 1] = RGB(0, 0, 0)}},
};

static OPERATE_RET ws2812_register(void)
{
    PIXEL_DRIVER_CONFIG_T cfg = {
        .port     = AK_WS2812_SPI_PORT,
        .line_seq = RGB_ORDER,
    };
    return tdd_ws2812_driver_register(WS2812_DEVICE_NAME, &cfg);
}

static OPERATE_RET ws2812_open(void)
{
    OPERATE_RET rt;

    TUYA_CALL_ERR_RETURN(tdl_pixel_dev_find(WS2812_DEVICE_NAME, &s_pixels));

    PIXEL_DEV_CONFIG_T dev_cfg = {
        .pixel_num        = AK_WS2812_COUNT,
        .pixel_resolution = WS2812_RESOLUTION,
    };
    return tdl_pixel_dev_open(s_pixels, &dev_cfg);
}

static OPERATE_RET ws2812_send(const PIXEL_COLOR_T colors[AK_WS2812_COUNT])
{
    OPERATE_RET rt;

    for (uint32_t index = 0; index < AK_WS2812_COUNT; index++) {
        /* set_single_color writes one contiguous run starting at index. */
        TUYA_CALL_ERR_RETURN(tdl_pixel_set_single_color(s_pixels, index, 1U, (PIXEL_COLOR_T *)&colors[index]));
    }
    return tdl_pixel_dev_refresh(s_pixels);
}

/* Light exactly one LED white, all others off, to verify daisy-chain order. */
static OPERATE_RET ws2812_light_single(uint32_t which)
{
    PIXEL_COLOR_T colors[AK_WS2812_COUNT];
    memset(colors, 0, sizeof(colors));
    colors[which].red   = WS2812_LEVEL;
    colors[which].green = WS2812_LEVEL;
    colors[which].blue  = WS2812_LEVEL;
    return ws2812_send(colors);
}

static void user_main(void)
{
    OPERATE_RET rt;

    tal_log_init(TAL_LOG_LEVEL_DEBUG, 2048, (TAL_LOG_OUTPUT_CB)tkl_log_output);
    PR_NOTICE("======== Agentic Keyboard bring-up: ws2812 ========");
    PR_NOTICE("Firmware: %s v%s, driver=%s", PROJECT_NAME, PROJECT_VERSION, WS2812_DRIVER_REV);
    PR_NOTICE("Built: %s %s", __DATE__, __TIME__);

    rt = bringup_uart_init();
    if (rt != OPRT_OK) {
        PR_ERR("UART0 init failed: %d", rt);
        return;
    }
    rt = ws2812_register();
    if (rt != OPRT_OK) {
        PR_ERR("WS2812 driver register failed: %d", rt);
        return;
    }
    rt = ws2812_open();
    if (rt != OPRT_OK) {
        PR_ERR("WS2812 open failed: %d", rt);
        return;
    }

    bringup_uart_printf("{\"v\":1,\"type\":\"bringup\",\"payload\":{\"name\":\"ws2812\","
                        "\"version\":\"%s\",\"driver\":\"%s\",\"status\":\"ready\",\"spi_port\":%u,"
                        "\"data_pin\":%u,\"count\":%u}}\n",
                        PROJECT_VERSION, WS2812_DRIVER_REV, (unsigned)AK_WS2812_SPI_PORT, (unsigned)AK_WS2812_DATA_PIN,
                        AK_WS2812_COUNT);

    while (1) {
        /* Phase 1: walk the chain LED1..LED7, one at a time, to confirm order. */
        for (uint32_t which = 0; which < AK_WS2812_COUNT; which++) {
            rt = ws2812_light_single(which);
            if (rt != OPRT_OK) {
                bringup_uart_printf("{\"v\":1,\"type\":\"bringup\",\"payload\":{\"name\":\"ws2812\","
                                    "\"status\":\"error\",\"stage\":\"walk\",\"led\":%u,\"code\":%d}}\n",
                                    (unsigned)(which + 1U), rt);
                return;
            }
            PR_NOTICE("WS2812 walk LED[%u/%u]", (unsigned)(which + 1U), (unsigned)AK_WS2812_COUNT);
            bringup_uart_printf("{\"v\":1,\"type\":\"bringup\",\"payload\":{\"name\":\"ws2812\","
                                "\"version\":\"%s\",\"walk_led\":%u,\"of\":%u}}\n",
                                PROJECT_VERSION, (unsigned)(which + 1U), (unsigned)AK_WS2812_COUNT);
            tal_system_sleep(PATTERN_DELAY_MS);
        }

        /* Phase 2: fixed full-chain patterns (RGB sweep, all white, off). */
        for (uint32_t index = 0; index < CNTSOF(s_fixed_patterns); index++) {
            rt = ws2812_send(s_fixed_patterns[index].colors);
            if (rt != OPRT_OK) {
                bringup_uart_printf("{\"v\":1,\"type\":\"bringup\",\"payload\":{\"name\":\"ws2812\","
                                    "\"status\":\"error\",\"stage\":\"send\",\"code\":%d}}\n",
                                    rt);
                return;
            }
            PR_NOTICE("WS2812 pattern[%u/%u]=%s", (unsigned)(index + 1U), (unsigned)CNTSOF(s_fixed_patterns),
                      s_fixed_patterns[index].name);
            bringup_uart_printf("{\"v\":1,\"type\":\"bringup\",\"payload\":{\"name\":\"ws2812\","
                                "\"version\":\"%s\",\"pattern\":\"%s\"}}\n",
                                PROJECT_VERSION, s_fixed_patterns[index].name);
            tal_system_sleep(PATTERN_DELAY_MS);
        }
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
        .thrdname   = "ws2812",
    };

    tal_thread_create_and_start(&s_app_thread, NULL, NULL, app_thread, NULL, &thread_cfg);
}
