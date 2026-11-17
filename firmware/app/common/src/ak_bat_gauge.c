/**
 * @file ak_bat_gauge.c
 * @brief Battery fuel-gauge maths (see ak_bat_gauge.h).
 */

#include "ak_bat_gauge.h"

#include <stddef.h>

#define AK_BAT_CURVE_POINTS 11
/* Cell mV at 0,10,..,100 % (same breakpoints as the stock TuyaOpen T5 gauge).
 * Resting (open-circuit) voltages of a 4.2 V Li-ion/LiPo cell; under load
 * the cell reads lower, which is why heavy-load samples are skipped. */
static const int32_t s_curve[AK_BAT_CURVE_POINTS] = {
    3300, 3600, 3680, 3740, 3790, 3830, 3870, 3920, 3980, 4080, 4200,
};

int32_t ak_bat_trimmed_mean(const int32_t *samples, int n)
{
    int32_t v[AK_BAT_SAMPLES];
    int     k = 0;

    if (samples == NULL || n <= 0 || n > AK_BAT_SAMPLES) {
        return -1;
    }
    for (int i = 0; i < n; i++) {
        if (samples[i] >= AK_BAT_NODE_MIN_MV && samples[i] <= AK_BAT_NODE_MAX_MV) {
            v[k++] = samples[i];
        }
    }
    if (k < 2 * AK_BAT_TRIM + 1) {
        return -1;
    }
    for (int i = 1; i < k; i++) { /* insertion sort, k <= 8 */
        int32_t x = v[i];
        int     j = i - 1;
        while (j >= 0 && v[j] > x) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = x;
    }
    int32_t sum = 0;
    int     cnt = k - 2 * AK_BAT_TRIM;
    for (int i = AK_BAT_TRIM; i < k - AK_BAT_TRIM; i++) {
        sum += v[i];
    }
    return (sum + cnt / 2) / cnt;
}

int32_t ak_bat_node_to_cell_mv(int32_t node_mv, int32_t cal_permille)
{
    return (node_mv * AK_BAT_DIVIDER_NUM * cal_permille + 500) / 1000;
}

uint8_t ak_bat_curve_percent(int32_t cell_mv)
{
    if (cell_mv <= s_curve[0]) {
        return 0U;
    }
    if (cell_mv >= s_curve[AK_BAT_CURVE_POINTS - 1]) {
        return 100U;
    }
    for (int i = 1; i < AK_BAT_CURVE_POINTS; i++) {
        if (cell_mv < s_curve[i]) {
            int32_t lo   = s_curve[i - 1];
            int32_t hi   = s_curve[i];
            int32_t base = (int32_t)(i - 1) * 10;
            int32_t frac = ((cell_mv - lo) * 10 + (hi - lo) / 2) / (hi - lo);
            return (uint8_t)(base + frac);
        }
    }
    return 100U;
}

void ak_bat_gauge_init(ak_bat_gauge_t *g)
{
    g->seeded   = false;
    g->charging = false;
    g->ema_mv   = 0;
    g->percent  = 0U;
}

uint8_t ak_bat_gauge_update(ak_bat_gauge_t *g, int32_t cell_mv, bool charging)
{
    if (!g->seeded || charging != g->charging) {
        g->seeded   = true;
        g->charging = charging;
        g->ema_mv   = cell_mv;
        g->percent  = ak_bat_curve_percent(cell_mv);
        return g->percent;
    }

    /* A measurement far above the shown value is not load noise (load only
     * pulls the cell down): battery swapped, or the earlier seed was taken
     * under load. Restart from it instead of creeping up behind the EMA. */
    uint8_t now_p = ak_bat_curve_percent(cell_mv);
    if (!charging && now_p >= g->percent + AK_BAT_RISE_HYST) {
        g->ema_mv  = cell_mv;
        g->percent = now_p;
        return g->percent;
    }

    int32_t step = (cell_mv - g->ema_mv) / (1 << AK_BAT_EMA_SHIFT);
    if (step == 0) { /* close in fully instead of stalling up to 7 mV short */
        step = (cell_mv > g->ema_mv) - (cell_mv < g->ema_mv);
    }
    g->ema_mv += step;
    uint8_t p = ak_bat_curve_percent(g->ema_mv);

    if (charging || p < g->percent) {
        g->percent = p;
    }
    return g->percent;
}
