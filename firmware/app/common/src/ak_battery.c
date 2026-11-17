/**
 * @file ak_battery.c
 * @brief Battery fuel gauge over the TKL ADC (see ak_battery.h).
 *
 * Hardware facts are in ak_battery.h (T5AI-Core V1.0.1 schematic). ADC
 * notes from the T5 adapter (platform/T5AI/.../tkl_adc.c):
 *  - channel 4 is P28 and is opened with the SARADC internal attenuator at
 *    1/1 (g_channel_id_div[ADC_4] = 1, ADC_VOL_DIV_1);
 *  - tkl_adc_read_voltage() returns mV: the CP side converts the raw code
 *    with the chip's 1 V / 2 V calibration points and the attenuation.
 * CONTINUOUS mode + INNER_SAMPLE_VOL as in the stock T5 users; SINGLE mode
 * returned 0 mV on T5.
 *
 * Each measurement takes AK_BAT_SAMPLES separate reads and keeps the trimmed
 * mean. While the WS2812 chain is powered (up to several hundred mA from the
 * cell through the 5 V boost) the cell sags, so such measurements are shown
 * on the test page but not fed to the gauge, unless no light-load sample has
 * been seen for AK_BAT_MAX_DEFER_MS.
 */

#include "ak_battery.h"

#include "ak_bat_gauge.h"
#include "ak_leds.h"
#include "tal_api.h"
#include "tkl_adc.h"
#include "tkl_gpio.h"

#define AK_BAT_ADC_PORT      TUYA_ADC_NUM_0
#define AK_BAT_ADC_CHANNEL   4U                 /* P28, schematic ADC_BAT */
#define AK_BAT_CHG_PIN       TUYA_GPIO_NUM_38   /* CHG_DET_N, active low */
#define AK_BAT_SAMPLE_PERIOD 5000U              /* ms between measurements */
#define AK_BAT_MAX_DEFER_MS  60000U             /* feed a loaded sample after this */
#define AK_BAT_LOG_EVERY     6U                 /* log every 6th measurement (~30 s) */

static bool              s_available = false;
static bool              s_chg_ok    = false;
static bool              s_measured  = false;
static uint32_t          s_last_read = 0U;
static uint32_t          s_last_fed  = 0U;
static uint32_t          s_count     = 0U;
static ak_bat_gauge_t    s_gauge;
static ak_battery_info_t s_info = {.node_mv = -1};

OPERATE_RET ak_battery_init(void)
{
    TUYA_ADC_BASE_CFG_T cfg = {0};
    cfg.ch_list.data = 1U << AK_BAT_ADC_CHANNEL;
    cfg.ch_nums      = 1U;
    cfg.width        = 12U;
    cfg.freq         = 0U;
    cfg.type         = TUYA_ADC_INNER_SAMPLE_VOL;
    cfg.mode         = TUYA_ADC_CONTINUOUS; /* SINGLE returned 0 mV on T5 */
    cfg.conv_cnt     = 1U;
    cfg.ref_vol      = 0U;

    ak_bat_gauge_init(&s_gauge);
    OPERATE_RET rt = tkl_adc_init(AK_BAT_ADC_PORT, &cfg);
    if (rt != OPRT_OK) {
        s_available = false;
        return rt;
    }

    /* CHG_DET_N already has R8 100K to 3V3; the internal pull-up only keeps
     * it defined if R8 is not fitted. Optional: without it the gauge simply
     * treats the battery as discharging. */
    TUYA_GPIO_BASE_CFG_T gc = {
        .mode   = TUYA_GPIO_PULLUP,
        .direct = TUYA_GPIO_INPUT,
        .level  = TUYA_GPIO_LEVEL_HIGH,
    };
    s_chg_ok = (tkl_gpio_init(AK_BAT_CHG_PIN, &gc) == OPRT_OK);

    s_available = true;
    s_measured  = false;
    return OPRT_OK;
}

bool ak_battery_available(void)
{
    return s_available;
}

static bool battery_charging(void)
{
    TUYA_GPIO_LEVEL_E lv = TUYA_GPIO_LEVEL_HIGH;
    if (!s_chg_ok || tkl_gpio_read(AK_BAT_CHG_PIN, &lv) != OPRT_OK) {
        return false;
    }
    return lv == TUYA_GPIO_LEVEL_LOW;
}

static int32_t battery_measure_node_mv(void)
{
    int32_t samples[AK_BAT_SAMPLES];
    for (int i = 0; i < AK_BAT_SAMPLES; i++) {
        int32_t mv = -1;
        if (tkl_adc_read_voltage(AK_BAT_ADC_PORT, &mv, 1) != OPRT_OK) {
            mv = -1;
        }
        samples[i] = mv;
    }
    return ak_bat_trimmed_mean(samples, AK_BAT_SAMPLES);
}

uint8_t ak_battery_percent(void)
{
    if (!s_available) {
        return 0U;
    }
    uint32_t now = tal_system_get_millisecond();
    if (s_measured && (uint32_t)(now - s_last_read) < AK_BAT_SAMPLE_PERIOD) {
        return s_gauge.percent;
    }
    s_measured  = true;
    s_last_read = now;

    int32_t node_mv = battery_measure_node_mv();
    s_info.node_mv  = node_mv;
    s_info.charging = battery_charging();
    if (node_mv < 0) {
        s_info.cell_mv = -1;
        s_info.skipped = true;
        PR_WARN("battery: measurement failed, keeping %u%%", (unsigned)s_gauge.percent);
        return s_gauge.percent; /* keep last good value */
    }
    s_info.cell_mv = ak_bat_node_to_cell_mv(node_mv, AK_BAT_CAL_PERMILLE);

    bool loaded    = ak_leds_power_enabled();
    bool overdue   = !s_gauge.seeded || (uint32_t)(now - s_last_fed) >= AK_BAT_MAX_DEFER_MS;
    s_info.skipped = loaded && !overdue && !s_info.charging;
    if (!s_info.skipped) {
        (void)ak_bat_gauge_update(&s_gauge, s_info.cell_mv, s_info.charging);
        s_last_fed = now;
    }
    s_info.ema_mv = s_gauge.ema_mv;

    if ((s_count++ % AK_BAT_LOG_EVERY) == 0U) {
        PR_INFO("battery: adc=%ld mV cell=%ld mV ema=%ld mV pct=%u chg=%d led=%d%s",
                (long)node_mv, (long)s_info.cell_mv, (long)s_gauge.ema_mv,
                (unsigned)s_gauge.percent, s_info.charging ? 1 : 0, loaded ? 1 : 0,
                s_info.skipped ? " (skipped)" : "");
    }
    return s_gauge.percent;
}

void ak_battery_get_info(ak_battery_info_t *info)
{
    if (info != NULL) {
        *info = s_info;
    }
}
