/**
 * @file ak_leds.c
 * @brief 7x WS2812 chain over the official SPI pixel driver (see ak_leds.h).
 */

#include "ak_leds.h"

#include <string.h>

#include "tal_api.h"
#include "tkl_gpio.h"

#include "ak_led_power.h"

#include "tdd_pixel_ws2812.h"
#include "tdd_pixel_type.h"
#include "tdl_pixel_dev_manage.h"
#include "tdl_pixel_color_manage.h"

#define AK_LEDS_DEVICE_NAME "ak_ws2812"
#define AK_LEDS_RESOLUTION  255U
/* Cap brightness: 7x full white would peak ~255 mA; the test page never needs
 * full intensity, so scale every channel down to keep current modest. */
#define AK_LEDS_MAX_LEVEL   48U

static bool           s_available = false;
static PIXEL_HANDLE_T s_pixels    = NULL;
static PIXEL_COLOR_T  s_buf[AK_LEDS_COUNT];
/* v0.3.2 chain power switch (FW-05). See ak_led_power.h for the sequence. */
static ak_led_power_t s_power;
static bool           s_power_pin_ok = false;
/* LED power setting: Off / On / Auto + idle timer (ak_led_policy.h). */
static ak_led_policy_t s_policy;

static void power_pin_apply(void)
{
    if (s_power_pin_ok) {
        (void)tkl_gpio_write(AK_PIN_LED_PWR_EN, s_power.enable ? TUYA_GPIO_LEVEL_HIGH : TUYA_GPIO_LEVEL_LOW);
    }
}

static bool frame_lit(void)
{
    for (uint32_t i = 0; i < AK_LEDS_COUNT; i++) {
        if (s_buf[i].red != 0U || s_buf[i].green != 0U || s_buf[i].blue != 0U) {
            return true;
        }
    }
    return false;
}

/* Clock the frame out on DIN: the effect buffer, or zeros while the policy
 * keeps the chain dark (the buffer itself is kept for the next wake). Only
 * called when the power state machine allows it. */
static OPERATE_RET transmit(uint32_t now_ms)
{
    OPERATE_RET               rt;
    static const PIXEL_COLOR_T dark = {0};
    bool                       off  = ak_led_policy_output(&s_policy) == AK_LED_OUT_DARK;
    for (uint32_t i = 0; i < AK_LEDS_COUNT; i++) {
        TUYA_CALL_ERR_RETURN(tdl_pixel_set_single_color(s_pixels, i, 1U, off ? (PIXEL_COLOR_T *)&dark : &s_buf[i]));
    }
    rt = tdl_pixel_dev_refresh(s_pixels);
    ak_led_power_sent(&s_power, now_ms);
    power_pin_apply();
    return rt;
}

static uint8_t cap(uint8_t v)
{
    return v > AK_LEDS_MAX_LEVEL ? AK_LEDS_MAX_LEVEL : v;
}

OPERATE_RET ak_leds_init(void)
{
    OPERATE_RET rt;

    /* LED_PWR_EN: push-pull, low = chain off (R11 already holds it low while
     * the pin floats during reset). Unconnected on lite / v0.3.1 boards. */
    ak_led_power_init(&s_power);
    ak_led_policy_init(&s_policy, AK_LED_MODE_DEFAULT, AK_LED_TIMEOUT_IDX_DEFAULT, tal_system_get_millisecond());
    TUYA_GPIO_BASE_CFG_T pwr_cfg = {
        .mode   = TUYA_GPIO_PUSH_PULL,
        .direct = TUYA_GPIO_OUTPUT,
        .level  = TUYA_GPIO_LEVEL_LOW,
    };
    s_power_pin_ok = (tkl_gpio_init(AK_PIN_LED_PWR_EN, &pwr_cfg) == OPRT_OK);
    power_pin_apply();

    PIXEL_DRIVER_CONFIG_T drv = {
        .port     = AK_WS2812_SPI_PORT,
        /* WS2812 wire protocol is GRB; RGB_ORDER swaps red<->green on real HW. */
        .line_seq = GRB_ORDER,
    };
    rt = tdd_ws2812_driver_register(AK_LEDS_DEVICE_NAME, &drv);
    if (rt != OPRT_OK) {
        s_available = false;
        return rt;
    }

    rt = tdl_pixel_dev_find(AK_LEDS_DEVICE_NAME, &s_pixels);
    if (rt != OPRT_OK) {
        s_available = false;
        return rt;
    }

    PIXEL_DEV_CONFIG_T dev = {
        .pixel_num        = AK_LEDS_COUNT,
        .pixel_resolution = AK_LEDS_RESOLUTION,
    };
    rt = tdl_pixel_dev_open(s_pixels, &dev);
    if (rt != OPRT_OK) {
        s_available = false;
        return rt;
    }

    memset(s_buf, 0, sizeof(s_buf));
    s_available = true;
    (void)ak_leds_show();
    return OPRT_OK;
}

bool ak_leds_available(void)
{
    return s_available;
}

void ak_leds_set(uint32_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (index >= AK_LEDS_COUNT) {
        return;
    }
    s_buf[index].red   = cap(r);
    s_buf[index].green = cap(g);
    s_buf[index].blue  = cap(b);
    s_buf[index].cold  = 0;
    s_buf[index].warm  = 0;
}

void ak_leds_set_hue(uint32_t index, int32_t hue, uint8_t level)
{
    /* HSV->RGB on a 6-sextant colour wheel at full saturation. */
    int32_t steps  = (int32_t)AK_LEDS_HUE_STEPS;
    int32_t seg    = ((hue % steps) + steps) % steps; /* 0..23 */
    int32_t region = seg / 4;                          /* 0..5 */
    int32_t frac   = (seg % 4) * (int32_t)level / 4;
    uint8_t up     = (uint8_t)frac;
    uint8_t dn     = (uint8_t)((int32_t)level - frac);
    uint8_t r = 0, g = 0, b = 0;
    switch (region) {
    case 0: r = level; g = up;    b = 0;     break; /* R->Y */
    case 1: r = dn;    g = level; b = 0;     break; /* Y->G */
    case 2: r = 0;     g = level; b = up;    break; /* G->C */
    case 3: r = 0;     g = dn;    b = level; break; /* C->B */
    case 4: r = up;    g = 0;     b = level; break; /* B->M */
    default:r = level; g = 0;     b = dn;    break; /* M->R */
    }
    ak_leds_set(index, r, g, b);
}

void ak_leds_set_all(uint8_t r, uint8_t g, uint8_t b)
{
    for (uint32_t i = 0; i < AK_LEDS_COUNT; i++) {
        ak_leds_set(i, r, g, b);
    }
}

void ak_leds_clear(void)
{
    memset(s_buf, 0, sizeof(s_buf));
}

OPERATE_RET ak_leds_show(void)
{
    if (!s_available) {
        return OPRT_COM_ERROR;
    }
    uint32_t now  = tal_system_get_millisecond();
    bool     lit  = ak_led_policy_frame_lit(ak_led_policy_output(&s_policy), frame_lit());
    bool     send = ak_led_power_frame(&s_power, lit, now);
    power_pin_apply();
    /* Not sent yet (powering up, or too soon after the last frame): the
     * frame stays in s_buf and ak_leds_tick() sends it. */
    return send ? transmit(now) : OPRT_OK;
}

void ak_leds_tick(uint32_t now_ms)
{
    if (!s_available) {
        return;
    }
    if (ak_led_policy_tick(&s_policy, now_ms)) {
        (void)ak_leds_show(); /* Auto idle: dark frame, then the switch drops */
        return;
    }
    bool send = ak_led_power_tick(&s_power, now_ms);
    power_pin_apply();
    if (send) {
        (void)transmit(now_ms);
    }
}

void ak_leds_set_policy(ak_led_mode_t mode, uint8_t timeout_idx)
{
    uint32_t now = tal_system_get_millisecond();
    ak_led_policy_set_timeout(&s_policy, timeout_idx, now);
    (void)ak_led_policy_set_mode(&s_policy, mode, now);
    (void)ak_leds_show(); /* On may need power with a dark buffer; Off drops it */
}

void ak_leds_activity(void)
{
    if (ak_led_policy_activity(&s_policy, tal_system_get_millisecond())) {
        (void)ak_leds_show(); /* restore the kept effect through the power-up sequence */
    }
}

void ak_leds_set_test_override(bool on)
{
    if (ak_led_policy_set_override(&s_policy, on, tal_system_get_millisecond())) {
        (void)ak_leds_show();
    }
}

bool ak_leds_power_enabled(void)
{
    return s_power.enable;
}

const char *ak_leds_power_state_str(void)
{
    switch (s_power.state) {
    case AK_LED_PWR_SETTLING:
        return "SETTLING";
    case AK_LED_PWR_ON:
        return "ON";
    case AK_LED_PWR_STOPPING:
        return "STOPPING";
    default:
        return "OFF";
    }
}
