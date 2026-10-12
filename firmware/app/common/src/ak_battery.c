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
 * Li-ion single cell maps 3300 mV (empty) .. 4200 mV (full) to 0..100%. All of
 * these are tunable without touching logic below.
 */
#define AK_BAT_ADC_PORT      TUYA_ADC_NUM_0
#define AK_BAT_ADC_CHANNEL   4U      /* "ADC4" per pinmap; confirm on board */
#define AK_BAT_DIVIDER_NUM   4       /* (3M + 1M) / 1M = 4 */
#define AK_BAT_EMPTY_MV      3300    /* 0%  cell voltage */
#define AK_BAT_FULL_MV       4200    /* 100% cell voltage */
#define AK_BAT_SAMPLE_PERIOD 2000U   /* ms between real ADC reads */
#define AK_BAT_SMOOTH_SHIFT  2       /* EMA factor: new = old + (raw-old)>>2 */

static bool     s_available   = false;
static uint8_t  s_percent     = 0U;
static bool     s_have_sample = false;
static int32_t  s_cell_mv_ema = 0;      /* smoothed cell voltage (mV) */
static uint32_t s_last_read   = 0U;

OPERATE_RET ak_battery_init(void)
{
    TUYA_ADC_BASE_CFG_T cfg = {0};
    cfg.ch_list.data = 0U;
    cfg.ch_list.bits.ch_4 = 1U;   /* AK_BAT_ADC_CHANNEL */
    cfg.ch_nums  = 1U;
    cfg.width    = 12U;           /* 12-bit; platform clamps if unsupported */
    cfg.freq     = 0U;            /* platform default */
    cfg.type     = TUYA_ADC_INNER_SAMPLE_VOL;
    cfg.mode     = TUYA_ADC_SINGLE;
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

static uint8_t cell_mv_to_percent(int32_t cell_mv)
{
    if (cell_mv <= AK_BAT_EMPTY_MV) {
        return 0U;
    }
    if (cell_mv >= AK_BAT_FULL_MV) {
        return 100U;
    }
    int32_t span = AK_BAT_FULL_MV - AK_BAT_EMPTY_MV;
    return (uint8_t)(((cell_mv - AK_BAT_EMPTY_MV) * 100 + span / 2) / span);
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
