/**
 * @file ak_leds.h
 * @brief WS2812 driver wrapper for the Agentic Keyboard (3 LEDs on SPI1/P4).
 *
 * Wraps the official TuyaOpen SPI WS2812 driver (tdd_pixel_ws2812 +
 * tdl_pixel_*) behind a tiny init/set/show API, with a compile-time brightness
 * ceiling and a graceful "unavailable" degradation when the driver fails to
 * come up. The register/open/refresh sequence mirrors the validated bring-up
 * app (firmware/bringup/ws2812). Reusable: intended to move into
 * firmware/app/common/ during the merge phase (spec FP-WS2812, R1/R5).
 *
 * Pin/port/count come from the authoritative IO map (ak_board_io.h):
 *   AK_WS2812_SPI_PORT / AK_WS2812_DATA_PIN / AK_WS2812_COUNT.
 */

#ifndef AK_LEDS_H
#define AK_LEDS_H

#include <stdbool.h>
#include <stdint.h>

#include "tuya_cloud_types.h"

/*
 * Compile-time peak brightness ceiling per channel (0..255 scale of the WS2812
 * 8-bit resolution). Kept low because 3x full white peak current must stay
 * within the SK12D07VG5 battery switch 0.3A rating; 16 is the level validated
 * on the v1.2 board by the ws2812 bring-up app. Raising it needs a fresh
 * current check (hardware to-do). See spec FP-WS2812 D3.
 */
#ifndef AK_LEDS_MAX_LEVEL
#define AK_LEDS_MAX_LEVEL 16U
#endif

/**
 * @brief Register and open the WS2812 chain (AK_WS2812_COUNT LEDs on SPI1).
 * @return OPRT_OK on success. On failure the module stays in an "unavailable"
 *         state: ak_leds_ready() returns false and set/show become no-ops, so
 *         the caller can degrade gracefully instead of blocking (R1.4).
 */
OPERATE_RET ak_leds_init(void);

/**
 * @brief Buffer one LED's colour. r/g/b are clamped to AK_LEDS_MAX_LEVEL.
 *        Does not push to the chain until ak_leds_show().
 * @param slot LED index 0..AK_WS2812_COUNT-1. Out of range ->
 * OPRT_INVALID_PARM.
 * @return OPRT_OK, OPRT_INVALID_PARM (bad slot), or OPRT_COM_ERROR (not ready).
 */
OPERATE_RET ak_leds_set(uint8_t slot, uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Push the buffered colours to the physical chain (one SPI refresh).
 * @return OPRT_OK, or OPRT_COM_ERROR when the module is not ready.
 */
OPERATE_RET ak_leds_show(void);

/**
 * @brief Whether the driver came up. When false, set/show are no-ops (R1.4).
 */
bool ak_leds_ready(void);

#endif /* AK_LEDS_H */
