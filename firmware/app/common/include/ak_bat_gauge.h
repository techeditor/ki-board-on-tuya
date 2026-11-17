/**
 * @file ak_bat_gauge.h
 * @brief Battery fuel-gauge maths as pure logic (host-testable; no TKL).
 *
 * T5AI-Core V1.0.1 schematic (docs.tuyaopen.ai T5AI-Core_V101-SCH): BAT ->
 * R9 3M/1% -> tap -> R11 1M/1% -> GND, C52 0.1 uF on the tap, R10 100R ->
 * ADC_BAT = module pin 11 = P28 = SARADC channel 4 (tkl_adc.c ADC_4 ->
 * GPIO_28). Tap = BAT / 4. ak_battery.c reads the tap with
 * tkl_adc_read_voltage (mV) and feeds this module.
 *
 * Pipeline per measurement:
 *   1. ak_bat_trimmed_mean(): drop out-of-range samples, sort, drop the
 *      AK_BAT_TRIM lowest and highest, average the rest.
 *   2. ak_bat_node_to_cell_mv(): x4 divider, x calibration (permille).
 *   3. ak_bat_gauge_update(): EMA of the cell voltage, OCV curve -> percent,
 *      then the display hysteresis (only falls while discharging).
 */

#ifndef AK_BAT_GAUGE_H
#define AK_BAT_GAUGE_H

#include <stdbool.h>
#include <stdint.h>

#define AK_BAT_DIVIDER_NUM   4      /* (R9 3M + R11 1M) / R11 1M */
#define AK_BAT_SAMPLES       8      /* ADC reads per measurement */
#define AK_BAT_TRIM          2      /* dropped at each end before averaging */
/* Plausible tap voltage: cell 2.4..4.8 V. Anything outside is a failed read
 * (T5 SINGLE mode returns 0) or a glitch, not a battery voltage. */
#define AK_BAT_NODE_MIN_MV   600
#define AK_BAT_NODE_MAX_MV   1200
#define AK_BAT_EMA_SHIFT     3      /* new = old + (x - old) / 8 */
/* While discharging the shown percent only falls, except when the curve
 * reads this many points higher (battery swapped, or the first sample was
 * taken under a load step). Load relaxation after a burst stays below it. */
#define AK_BAT_RISE_HYST     15
/* Calibration multiplier in permille (1000 = none). shortcut: compile-time
 * constant, no KV/USB setting yet; upgrade to a USB-set KV value once real
 * boards show a consistent per-board error. */
#define AK_BAT_CAL_PERMILLE  1000

typedef struct {
    bool    seeded;
    bool    charging;  /* charging state of the last accepted sample */
    int32_t ema_mv;    /* smoothed cell voltage */
    uint8_t percent;   /* shown percent */
} ak_bat_gauge_t;

/** Trimmed mean of the plausible samples in mV, or -1 when fewer than
 *  2 * AK_BAT_TRIM + 1 samples are plausible. n must be <= AK_BAT_SAMPLES. */
int32_t ak_bat_trimmed_mean(const int32_t *samples, int n);

/** Tap mV -> cell mV: x AK_BAT_DIVIDER_NUM x cal_permille / 1000 (rounded). */
int32_t ak_bat_node_to_cell_mv(int32_t node_mv, int32_t cal_permille);

/** Cell mV -> 0..100 % on the 11-point OCV curve (linear between points). */
uint8_t ak_bat_curve_percent(int32_t cell_mv);

void ak_bat_gauge_init(ak_bat_gauge_t *g);

/** Feed one measured cell voltage. A change of charging state reseeds the
 *  filter and the shown percent (the cell voltage steps on plug/unplug).
 *  Returns the shown percent. */
uint8_t ak_bat_gauge_update(ak_bat_gauge_t *g, int32_t cell_mv, bool charging);

#endif /* AK_BAT_GAUGE_H */
