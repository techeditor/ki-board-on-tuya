/**
 * @file bringup_led_power.h
 * @brief Shared v0.3.2 WS2812 chain power switch (LED_PWR_EN = P18) sequence
 *        for the bring-up apps that drive the LED chain.
 *
 * Order:
 *   on : DIN (P4) idle low -> P18 high -> settle -> reset (DIN low) -> data
 *   off: all-zero frame -> DIN idle low -> P18 low; the next power-on waits
 *        BRINGUP_LED_PWR_OFF_HOLD_MS (Q2 turns off in ~160 ms via R9/C15).
 * The caller opens the pixel device first, so DIN is the idle SPI1 MOSI and
 * nothing is clocked out before bringup_led_power_on() returns.
 */

#ifndef BRINGUP_LED_POWER_H
#define BRINGUP_LED_POWER_H

#include <stdbool.h>
#include <stdint.h>

#include "tdl_pixel_dev_manage.h"
#include "tuya_cloud_types.h"

/*
 * Build option for ws2812 / full_selftest (led_power always drives P18):
 *   1 = power the chain on at boot through P18, so the LEDs light on v0.3.2;
 *   0 = leave P18 alone (chain stays off on v0.3.2, as before this option).
 * Default 1: on lite / v0.3.1 P18 has no connection on the expansion board
 * (netlist-v0.3.md, netlist-v0.3.1-low-profile.md) and the T5AI-Core board
 * support does not use it, so driving it high there does nothing.
 * Change it here (one place) or pass -DBRINGUP_LED_PWR_AUTO=0.
 */
#ifndef BRINGUP_LED_PWR_AUTO
#define BRINGUP_LED_PWR_AUTO 1
#endif

#define BRINGUP_LED_PWR_SETTLE_MS   10U  /* VDD_LED 90 % at ~3.3 ms; the study asks >= 5 ms */
#define BRINGUP_LED_PWR_RESET_MS    1U   /* WS2812 reset: DIN low >= 280 us */
#define BRINGUP_LED_PWR_OFF_HOLD_MS 250U /* >= 200 ms before re-powering */

/** P18 as a push-pull output, low (chain off). Call before anything else. */
void bringup_led_power_init(void);

/** Wait out the off-hold if needed, P18 high, wait settle_ms, then the reset
 *  gap when `reset`. The caller sends the first frame afterwards. */
void bringup_led_power_on(uint32_t settle_ms, bool reset);

/** All-zero frame on `pixels` (when powered), DIN back to idle low, then P18
 *  low. Returns the driver result of the zero frame (OPRT_OK when not sent). */
OPERATE_RET bringup_led_power_off(PIXEL_HANDLE_T pixels, uint32_t count);

bool bringup_led_power_is_on(void);

/** UART0 line naming the build option, so the flashed image is identifiable. */
void bringup_led_power_print_option(const char *app, bool always);

#endif /* BRINGUP_LED_POWER_H */
