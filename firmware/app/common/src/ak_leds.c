/**
 * @file ak_leds.c
 * @brief 7x WS2812 chain over the official SPI pixel driver (see ak_leds.h).
 */

#include "ak_leds.h"

#include <string.h>

#include "tal_api.h"

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

static uint8_t cap(uint8_t v)
{
    return v > AK_LEDS_MAX_LEVEL ? AK_LEDS_MAX_LEVEL : v;
}

OPERATE_RET ak_leds_init(void)
{
    OPERATE_RET rt;

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
    OPERATE_RET rt;
    for (uint32_t i = 0; i < AK_LEDS_COUNT; i++) {
        TUYA_CALL_ERR_RETURN(tdl_pixel_set_single_color(s_pixels, i, 1U, &s_buf[i]));
    }
    return tdl_pixel_dev_refresh(s_pixels);
}
