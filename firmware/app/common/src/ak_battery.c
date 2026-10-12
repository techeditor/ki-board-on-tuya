/**
 * @file ak_battery.c
 * @brief Battery fuel gauge over the TKL ADC (see ak_battery.h).
 */

#include "ak_battery.h"

#include "tal_api.h"
#include "tkl_adc.h"

/* ---- Calibration constants (verify / correct on real hardware) ----------
 *
 * pinmap: "Battery ADC = P28 / ADC4", dev-board internal "BAT -> 3M/1M divider".
 * T5 exposes one ADC unit (TUYA_ADC_NUM_0) with per-channel selection; ADC4 is
 * taken as channel 4. The divider is 3M (high side) + 1M (low side), so the
 * node voltage is BAT * 1M/(3M+1M) = BAT/4; undo it by multiplying by 4.
 *
 * The ADC is opened in CONTINUOUS mode with INNER_SAMPLE_VOL, matching the two
 * stock T5 battery users in TuyaOpen (apps/.../app_system_info.c and
 * tdd_power_soc.c). SINGLE mode returned 0 mV on T5.
 *
 * A Li-ion cell's voltage->charge curve is non-linear, so a 11-point lookup
 * table (same breakpoints the stock T5 fuel gauge uses) is interpolated instead
 * of a flat 3300..4200 line, which badly misreads the mid-range. All of these
 * are tunable without touching the logic below.
 */
#define AK_BAT_ADC_PORT      TUYA_ADC_NUM_0
#define AK_BAT_ADC_CHANNEL   4U      /* "ADC4" per pinmap; confirm on board */
#define AK_BAT_DIVIDER_NUM   4       /* (3M + 1M) / 1M = 4 */
#define AK_BAT_SAMPLE_PERIOD 2000U   /* ms between real ADC reads */
#define AK_BAT_SMOOTH_SHIFT  2       /* EMA factor: new = old + (raw-old)>>2 */

/* Cell mV at 0,10,..,100% (same curve the stock T5 fuel gauge uses). */
#define AK_BAT_CURVE_POINTS  11
static const int32_t s_bvc_map[AK_BAT_CURVE_POINTS] = {
    3300, 3600, 3680, 3740, 3790, 3830, 3870, 3920, 3980, 4080, 4200,
};

static bool     s_available   = false;
static uint8_t  s_percent     = 0U;
static bool     s_have_sample = false;
static int32_t  s_cell_mv_ema = 0;      /* smoothed cell voltage (mV) */
static uint32_t s_last_read   = 0U;

OPERATE_RET ak_battery_init(void)
{
    TUYA_ADC_BASE_CFG_T cfg = {0};
    cfg.ch_list.data = 1U << AK_BAT_ADC_CHANNEL;
    cfg.ch_nums  = 1U;
    cfg.width    = 12U;           /* 12-bit; platform clamps if unsupported */
    cfg.freq     = 0U;            /* platform default */
    cfg.type     = TUYA_ADC_INNER_SAMPLE_VOL;
    cfg.mode     = TUYA_ADC_CONTINUOUS; /* SINGLE returned 0 mV on T5 */
    cfg.conv_cnt = 1U;
    cfg.ref_vol  = 0U; /* use the platform default reference */

    OPERATE_RET rt = tkl_adc_init(AK_BAT_ADC_PORT, &cfg);
    if (rt != OPRT_OK) {
        s_available = false;
        return rt;
    }
    s_available   = true;
    s_have_sample = false;
    s_last_read   = tal_system_get_millisecond() - AK_BAT_SAMPLE_PERIOD;
    return OPRT_OK;
}

bool ak_battery_available(void)
{
    return s_available;
}

int32_t ak_battery_read_mv(void)
{
    if (!s_available) {
        return -1;
    }
    int32_t node_mv = 0;
    OPERATE_RET rt = tkl_adc_read_voltage(AK_BAT_ADC_PORT, &node_mv, 1);
    if (rt != OPRT_OK) {
        return -1;
    }
    return node_mv;
}

/* Interpolate the 11-point curve: find the bracketing segment and lerp. */
static uint8_t cell_mv_to_percent(int32_t cell_mv)
{
    if (cell_mv <= s_bvc_map[0]) {
        return 0U;
    }
    if (cell_mv >= s_bvc_map[AK_BAT_CURVE_POINTS - 1]) {
        return 100U;
    }
    for (int i = 1; i < AK_BAT_CURVE_POINTS; i++) {
        if (cell_mv < s_bvc_map[i]) {
            int32_t lo   = s_bvc_map[i - 1];
            int32_t hi   = s_bvc_map[i];
            int32_t base = (int32_t)(i - 1) * 10;    /* 0,10,..,90 */
            int32_t frac = ((cell_mv - lo) * 10 + (hi - lo) / 2) / (hi - lo);
            return (uint8_t)(base + frac);
        }
    }
    return 100U;
}

uint8_t ak_battery_percent(void)
{
    if (!s_available) {
        return 0U;
    }
    uint32_t now = tal_system_get_millisecond();
    if (s_have_sample && (uint32_t)(now - s_last_read) < AK_BAT_SAMPLE_PERIOD) {
        return s_percent;
    }

    int32_t node_mv = ak_battery_read_mv();
    if (node_mv < 0) {
        return s_percent; /* keep last good value on a transient failure */
    }
    s_last_read = now;

    int32_t cell_mv = node_mv * AK_BAT_DIVIDER_NUM;
    if (!s_have_sample) {
        s_cell_mv_ema = cell_mv;
        s_have_sample = true;
    } else {
        s_cell_mv_ema += (cell_mv - s_cell_mv_ema) >> AK_BAT_SMOOTH_SHIFT;
    }
    s_percent = cell_mv_to_percent(s_cell_mv_ema);
    return s_percent;
}
