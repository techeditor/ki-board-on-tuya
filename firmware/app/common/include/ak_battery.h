/**
 * @file ak_battery.h
 * @brief Battery fuel gauge: read the dev-board BAT divider on the ADC and
 *        convert to a 0..100% charge estimate for the LCD header.
 *
 * T5AI-Core V1.0.1 schematic (checked 2026-11-17): BAT -> R9 3M/1% -> tap ->
 * R11 1M/1% -> GND, C52 0.1 uF on the tap, R10 100R -> ADC_BAT = P28 = SARADC
 * channel 4. CHG_DET_N = P38: R8 100K pull-up to 3V3, pulled low through D5
 * by the ETA6003 STAT pin while charging. Neither is on the 44P header.
 * The maths lives in ak_bat_gauge.c (host-tested). Calibration: compare
 * "cell" on the hardware test page with a multimeter on BAT and set
 * AK_BAT_CAL_PERMILLE in ak_bat_gauge.h.
 */

#ifndef AK_BATTERY_H
#define AK_BATTERY_H

#include <stdbool.h>
#include <stdint.h>

#include "tuya_cloud_types.h"

/** Initialise the ADC and the charge-detect input. Safe to call once at
 *  startup. On failure ak_battery_available() stays false and the header
 *  simply omits the gauge. Touches no KV. */
OPERATE_RET ak_battery_init(void);

/** Whether the ADC opened and a reading is usable. */
bool ak_battery_available(void);

/** Current charge estimate in percent (0..100). Takes a new measurement at
 *  most every few seconds, so it is cheap to call every render. Returns the
 *  last good value when a measurement fails; 0 before the first one. */
uint8_t ak_battery_percent(void);

/** Last measurement, for the hardware test page and multimeter comparison. */
typedef struct {
    int32_t node_mv;  /* ADC tap voltage, trimmed mean (mV); <0 = no reading */
    int32_t cell_mv;  /* node_mv x divider x calibration (mV) */
    int32_t ema_mv;   /* filtered cell voltage the percent is computed from */
    bool    charging; /* CHG_DET_N low */
    bool    skipped;  /* last measurement not fed to the gauge (LED load) */
} ak_battery_info_t;

void ak_battery_get_info(ak_battery_info_t *info);

#endif /* AK_BATTERY_H */
