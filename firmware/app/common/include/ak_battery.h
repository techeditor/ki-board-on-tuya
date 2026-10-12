/**
 * @file ak_battery.h
 * @brief Battery fuel gauge: read the dev-board internal BAT divider on the
 *        ADC and convert to a 0..100% charge estimate for the LCD header.
 *
 * v0.3 hardware: the battery is monitored on the dev board internally via
 * BAT -> 3M/1M divider -> ADC (pinmap: "P28 / ADC4"). The expansion board does
 * not route this pin to the 44P header. Charge-detect (P38, CHG_DET_N, active
 * low) is also internal and NOT read here (left for a later revision).
 *
 * IMPORTANT (needs real-hardware calibration): the ADC channel number, the
 * divider ratio, and the empty/full voltage endpoints are best-effort defaults
 * taken from the pinmap. They are exposed as tunable constants in ak_battery.c
 * so they can be corrected against a real board without touching logic. Until
 * calibrated, treat the percentage as approximate.
 */

#ifndef AK_BATTERY_H
#define AK_BATTERY_H

#include <stdbool.h>
#include <stdint.h>

#include "tuya_cloud_types.h"

/** Initialise the ADC used for battery sensing. Safe to call once at startup.
 *  Returns OPRT_OK when the ADC opened; on failure ak_battery_available()
 *  stays false and the header simply omits the gauge. */
OPERATE_RET ak_battery_init(void);

/** Whether the ADC opened and a reading is usable. */
bool ak_battery_available(void);

/** Take one battery reading. Returns the raw divider-node voltage in millivolts
 *  (before the divider ratio is undone), or <0 on error. Mainly for diagnostics
 *  / the test page. */
int32_t ak_battery_read_mv(void);

/** Current charge estimate in percent (0..100), clamped. Returns the last good
 *  value when a single read fails; 0 before the first successful read.
 *  Internally rate-limited, so it is cheap to call every render. */
uint8_t ak_battery_percent(void);

#endif /* AK_BATTERY_H */
