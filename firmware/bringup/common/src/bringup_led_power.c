/**
 * @file bringup_led_power.c
 * @brief Shared WS2812 chain power switch sequence. See bringup_led_power.h.
 */

#include "bringup_led_power.h"

#include "agentic_board_io.h"
#include "bringup_uart.h"
#include "tal_api.h"
#include "tdl_pixel_color_manage.h"
#include "tkl_gpio.h"

static bool     s_on     = false;
static bool     s_was_on = false;
static uint32_t s_off_ms = 0U;

static void p18_write(bool high)
{
    (void)tkl_gpio_write(AK_LED_PWR_EN_PIN, high ? TUYA_GPIO_LEVEL_HIGH : TUYA_GPIO_LEVEL_LOW);
}

void bringup_led_power_init(void)
{
    TUYA_GPIO_BASE_CFG_T cfg = {
        .mode   = TUYA_GPIO_PUSH_PULL,
        .direct = TUYA_GPIO_OUTPUT,
        .level  = TUYA_GPIO_LEVEL_LOW,
    };
    (void)tkl_gpio_init(AK_LED_PWR_EN_PIN, &cfg);
    p18_write(false);
    s_on = false;
}

void bringup_led_power_on(uint32_t settle_ms, bool reset)
{
    if (s_was_on) {
        uint32_t since = tal_system_get_millisecond() - s_off_ms;
        if (since < BRINGUP_LED_PWR_OFF_HOLD_MS) {
            tal_system_sleep(BRINGUP_LED_PWR_OFF_HOLD_MS - since);
        }
    }
    /* Nothing is clocked out while the chain is off, so DIN (P4) idles low. */
    p18_write(true);
    s_on = true;
    if (settle_ms > 0U) {
        tal_system_sleep(settle_ms);
    }
    if (reset) {
        tal_system_sleep(BRINGUP_LED_PWR_RESET_MS);
    }
}

OPERATE_RET bringup_led_power_off(PIXEL_HANDLE_T pixels, uint32_t count)
{
    OPERATE_RET rt = OPRT_OK;
    if (s_on && pixels != NULL) {
        for (uint32_t i = 0; i < count && rt == OPRT_OK; i++) {
            PIXEL_COLOR_T dark = {0};
            rt                 = tdl_pixel_set_single_color(pixels, i, 1U, &dark);
        }
        if (rt == OPRT_OK) {
            rt = tdl_pixel_dev_refresh(pixels);
        }
        tal_system_sleep(BRINGUP_LED_PWR_RESET_MS); /* the frame (~0.2 ms) has left; DIN is low */
    }
    p18_write(false);
    s_on     = false;
    s_was_on = true;
    s_off_ms = tal_system_get_millisecond();
    return rt;
}

bool bringup_led_power_is_on(void)
{
    return s_on;
}

void bringup_led_power_print_option(const char *app, bool always)
{
    const char *state = always ? "ON (always, led_power)" : (BRINGUP_LED_PWR_AUTO ? "ON" : "OFF");
    PR_NOTICE("%s: LED_PWR_EN (P18) power-on option %s", app, state);
    bringup_uart_printf("{\"v\":1,\"type\":\"bringup\",\"payload\":{\"name\":\"%s\",\"led_pwr_option\":\"%s\","
                        "\"p18_pin\":%u}}\n",
                        app, always ? "always" : (BRINGUP_LED_PWR_AUTO ? "on" : "off"), (unsigned)AK_LED_PWR_EN_PIN);
}
