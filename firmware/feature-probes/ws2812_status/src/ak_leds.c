/**
 * @file ak_leds.c
 * @brief WS2812 driver wrapper (see ak_leds.h). Mirrors the validated bring-up
 *        register/open/refresh sequence (firmware/bringup/ws2812) and adds a
 *        brightness ceiling + graceful degradation.
 */

#include "ak_leds.h"

#include "ak_board_io.h"
#include "tal_api.h"

#include "tdd_pixel_type.h"
#include "tdd_pixel_ws2812.h"
#include "tdl_pixel_color_manage.h"
#include "tdl_pixel_dev_manage.h"

#define AK_LEDS_DEVICE_NAME "ak_ws2812"
#define AK_LEDS_RESOLUTION 255U

static PIXEL_HANDLE_T s_pixels = NULL;
static bool s_ready = false;

/* Buffered colours; flushed to the chain on ak_leds_show(). */
static PIXEL_COLOR_T s_buf[AK_WS2812_COUNT];

static uint8_t clamp_level(uint8_t v) {
  return (v > AK_LEDS_MAX_LEVEL) ? AK_LEDS_MAX_LEVEL : v;
}

OPERATE_RET ak_leds_init(void) {
  OPERATE_RET rt;

  if (s_ready) {
    return OPRT_OK;
  }

  PIXEL_DRIVER_CONFIG_T drv_cfg = {
      .port = AK_WS2812_SPI_PORT,
      .line_seq = RGB_ORDER,
  };
  rt = tdd_ws2812_driver_register(AK_LEDS_DEVICE_NAME, &drv_cfg);
  if (rt != OPRT_OK) {
    PR_ERR("ak_leds: driver register failed: %d (LEDs unavailable)", rt);
    return rt;
  }

  rt = tdl_pixel_dev_find(AK_LEDS_DEVICE_NAME, &s_pixels);
  if (rt != OPRT_OK) {
    PR_ERR("ak_leds: dev find failed: %d (LEDs unavailable)", rt);
    return rt;
  }

  PIXEL_DEV_CONFIG_T dev_cfg = {
      .pixel_num = AK_WS2812_COUNT,
      .pixel_resolution = AK_LEDS_RESOLUTION,
  };
  rt = tdl_pixel_dev_open(s_pixels, &dev_cfg);
  if (rt != OPRT_OK) {
    PR_ERR("ak_leds: dev open failed: %d (LEDs unavailable)", rt);
    s_pixels = NULL;
    return rt;
  }

  memset(s_buf, 0, sizeof(s_buf));
  s_ready = true;
  PR_NOTICE("ak_leds: %u WS2812 ready on SPI%u (P%u), max_level=%u",
            (unsigned)AK_WS2812_COUNT, (unsigned)AK_WS2812_SPI_PORT,
            (unsigned)AK_WS2812_DATA_PIN, (unsigned)AK_LEDS_MAX_LEVEL);
  return OPRT_OK;
}

OPERATE_RET ak_leds_set(uint8_t slot, uint8_t r, uint8_t g, uint8_t b) {
  if (slot >= AK_WS2812_COUNT) {
    return OPRT_INVALID_PARM;
  }
  if (!s_ready) {
    return OPRT_COM_ERROR;
  }

  s_buf[slot].red = clamp_level(r);
  s_buf[slot].green = clamp_level(g);
  s_buf[slot].blue = clamp_level(b);
  s_buf[slot].cold = 0;
  s_buf[slot].warm = 0;
  return OPRT_OK;
}

OPERATE_RET ak_leds_show(void) {
  OPERATE_RET rt;

  if (!s_ready) {
    return OPRT_COM_ERROR;
  }

  for (uint32_t i = 0; i < AK_WS2812_COUNT; i++) {
    /* set_single_color writes one contiguous run of length 1 at index i. */
    TUYA_CALL_ERR_RETURN(
        tdl_pixel_set_single_color(s_pixels, i, 1U, &s_buf[i]));
  }
  return tdl_pixel_dev_refresh(s_pixels);
}

bool ak_leds_ready(void) { return s_ready; }
