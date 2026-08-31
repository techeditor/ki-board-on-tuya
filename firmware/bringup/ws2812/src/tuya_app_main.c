/**
 * @file tuya_app_main.c
 * @brief Validate three WS2812 LEDs on the project-specific P13 data net.
 */

#include <string.h>

#include "agentic_board_io.h"
#include "bringup_uart.h"
#include "dwt.h"
#include "tal_api.h"
#include "tkl_gpio.h"
#include "tkl_output.h"

#define APP_TASK_STACK_SIZE (4U * 1024U)
#define PATTERN_DELAY_MS    1000U
#define T5_PM_DEV_ID_CPU1   38
#define T5_PM_CPU_FRQ_120M  3
#define T5_WS_CPU_VOTE_HZ   120000000U
#define WS2812_DRIVER_REV   "gpio-dwt-autoclock-v5"
#define WS2812_RESET_MS     4U
#define CPU_MEASURE_US      10000U
#define CPU_FREQ_MIN_HZ     20000000U
#define CPU_FREQ_MAX_HZ     400000000U

/*
 * TuyaOpen's official WS2812 driver encodes each data bit into eight SPI bits
 * at 6.5 MHz: 0=0xC0 and 1=0xF0. These are the equivalent nominal timings.
 */
#define WS2812_BIT_NS 1231U
#define WS2812_T0H_NS 308U
#define WS2812_T1H_NS 615U

/*
 * modules/pm.h pulls in a platform-generated sdkconfig.h that is unavailable
 * while TuyaOpen compiles the application library. Keep this platform-specific
 * declaration local; the values match pm_dev_id_e and pm_cpu_freq_e on T5AI.
 */
extern int      bk_pm_module_vote_cpu_freq(int module, int cpu_freq);
extern uint64_t bk_aon_rtc_get_us(void);

/*
 * T5AI (bk7258) AON GPIO: one 32-bit config register per pin, matching the SDK
 * GPIO_UP/GPIO_DOWN debug macros (soc_debug.h). Base is SOC_AON_GPIO_REG_BASE =
 * 0x44000400 + SOC_ADDR_OFFSET. The application core is built with CONFIG_SPE=1
 * (secure), so SOC_ADDR_OFFSET = 0 and the correct base is 0x44000400. Writing
 * the non-secure alias 0x54000400 from this core faults the bus (precise data
 * access violation) - that was the original crash. Per the SDK macros, writing
 * value 2 drives the pin high and 0 drives it low.
 */
#define T5_AON_GPIO_BASE   0x44000400U
#define T5_GPIO_REG_STRIDE 4U
#define T5_GPIO_LEVEL_LOW  0U
#define T5_GPIO_LEVEL_HIGH (1U << 1)

typedef struct {
    const char *name;
    uint8_t     rgb[AK_WS2812_COUNT][3];
} WS_PATTERN_T;

static THREAD_HANDLE      s_app_thread = NULL;
static volatile uint32_t *s_gpio_reg;
static uint32_t           s_cpu_hz;
static uint32_t           s_bit_cycles;
static uint32_t           s_t0h_cycles;
static uint32_t           s_t1h_cycles;

static const WS_PATTERN_T s_patterns[] = {
    {"all_off", {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}},     {"led1_red", {{16, 0, 0}, {0, 0, 0}, {0, 0, 0}}},
    {"led2_green", {{0, 0, 0}, {0, 16, 0}, {0, 0, 0}}}, {"led3_blue", {{0, 0, 0}, {0, 0, 0}, {0, 0, 16}}},
    {"all_white", {{8, 8, 8}, {8, 8, 8}, {8, 8, 8}}},
};

static inline __attribute__((always_inline)) void wait_cycles(uint32_t start, uint32_t cycles)
{
    while ((uint32_t)(DWT->CYCCNT - start) < cycles) {
        __asm volatile("nop");
    }
}

static inline __attribute__((always_inline)) void send_bit(bool one)
{
    uint32_t bit_start = DWT->CYCCNT;

    /*
     * Match the T5AI SDK's GPIO_UP/GPIO_DOWN macros: a single full-register
     * store gives deterministic edges. gpio_output_en is active-low, so values
     * 2 and 0 both keep the pin in output mode.
     */
    *s_gpio_reg         = T5_GPIO_LEVEL_HIGH;
    uint32_t high_start = DWT->CYCCNT;
    wait_cycles(high_start, one ? s_t1h_cycles : s_t0h_cycles);
    *s_gpio_reg = T5_GPIO_LEVEL_LOW;
    wait_cycles(bit_start, s_bit_cycles);
}

static void send_byte(uint8_t value)
{
    for (uint8_t mask = 0x80; mask != 0; mask >>= 1) {
        send_bit((value & mask) != 0);
    }
}

static OPERATE_RET ws2812_start_cycle_counter(void)
{
    dwt_init_cycle_counter();

    uint32_t start = DWT->CYCCNT;
    for (volatile uint32_t i = 0; i < 32U; i++) {
        __asm volatile("nop");
    }

    if (DWT->CYCCNT == start) {
        PR_ERR("WS2812 DWT cycle counter is not running");
        return OPRT_COM_ERROR;
    }
    return OPRT_OK;
}

static OPERATE_RET ws2812_measure_cpu_clock(void)
{
    OPERATE_RET rt = ws2812_start_cycle_counter();
    if (rt != OPRT_OK) {
        return rt;
    }

    uint64_t start_us = bk_aon_rtc_get_us();
    uint64_t end_us;
    uint32_t start_cycles = DWT->CYCCNT;

    do {
        end_us = bk_aon_rtc_get_us();
    } while ((end_us - start_us) < CPU_MEASURE_US);

    uint32_t elapsed_cycles = DWT->CYCCNT - start_cycles;
    uint64_t elapsed_us     = end_us - start_us;
    uint64_t measured_hz    = ((uint64_t)elapsed_cycles * 1000000ULL + (elapsed_us / 2ULL)) / elapsed_us;

    if (measured_hz < CPU_FREQ_MIN_HZ || measured_hz > CPU_FREQ_MAX_HZ) {
        PR_ERR("WS2812 invalid measured CPU frequency: %lluHz", measured_hz);
        return OPRT_COM_ERROR;
    }

    s_cpu_hz = (uint32_t)measured_hz;
    return OPRT_OK;
}

static uint32_t ns_to_cpu_cycles(uint32_t ns)
{
    return (uint32_t)(((uint64_t)s_cpu_hz * ns + 500000000ULL) / 1000000000ULL);
}

static OPERATE_RET ws2812_send(const uint8_t rgb[AK_WS2812_COUNT][3])
{
    OPERATE_RET rt = ws2812_start_cycle_counter();
    if (rt != OPRT_OK) {
        return rt;
    }

    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    for (uint32_t index = 0; index < AK_WS2812_COUNT; index++) {
        send_byte(rgb[index][1]);
        send_byte(rgb[index][0]);
        send_byte(rgb[index][2]);
    }
    __set_PRIMASK(primask);

    *s_gpio_reg = T5_GPIO_LEVEL_LOW;
    tal_system_sleep(WS2812_RESET_MS);
    return OPRT_OK;
}

static OPERATE_RET ws2812_init(void)
{
    /*
     * The CPU0 platform configuration drops the shared CPU clock to 60 MHz
     * after boot. Hold a CPU1 vote at 120 MHz so the DWT cycle counts below
     * continue to represent the intended WS2812 timings.
     */
    int pm_rt = bk_pm_module_vote_cpu_freq(T5_PM_DEV_ID_CPU1, T5_PM_CPU_FRQ_120M);
    if (pm_rt != OPRT_OK) {
        PR_ERR("WS2812 CPU frequency vote failed: %d", pm_rt);
        return OPRT_COM_ERROR;
    }

    TUYA_GPIO_BASE_CFG_T gpio_cfg = {
        .mode   = TUYA_GPIO_PUSH_PULL,
        .direct = TUYA_GPIO_OUTPUT,
        .level  = TUYA_GPIO_LEVEL_LOW,
    };
    OPERATE_RET rt = tkl_gpio_init(AK_WS2812_DATA_PIN, &gpio_cfg);
    if (rt != OPRT_OK) {
        return rt;
    }

    /*
     * T5AI GPIO config registers are one 32-bit word per pin. The application
     * core is secure (CONFIG_SPE=1), so the correct AON GPIO base is the secure
     * alias 0x44000400 (SOC_ADDR_OFFSET=0). Writing value 2 drives the pin high
     * and 0 low, matching the SDK GPIO_UP/GPIO_DOWN macros. Direct stores are
     * required because an 800 kHz WS2812 waveform cannot tolerate the TAL
     * function-call overhead.
     */
    s_gpio_reg  = (volatile uint32_t *)(T5_AON_GPIO_BASE + ((uint32_t)AK_WS2812_DATA_PIN * T5_GPIO_REG_STRIDE));
    *s_gpio_reg = T5_GPIO_LEVEL_LOW;
    tal_system_sleep(WS2812_RESET_MS);

    rt = ws2812_measure_cpu_clock();
    if (rt != OPRT_OK) {
        return rt;
    }

    s_bit_cycles = ns_to_cpu_cycles(WS2812_BIT_NS);
    s_t0h_cycles = ns_to_cpu_cycles(WS2812_T0H_NS);
    s_t1h_cycles = ns_to_cpu_cycles(WS2812_T1H_NS);

    PR_NOTICE("WS2812 driver=%s cpu_vote=%uHz cpu_measured=%uHz cycles(bit/t0h/t1h)=%u/%u/%u", WS2812_DRIVER_REV,
              T5_WS_CPU_VOTE_HZ, s_cpu_hz, s_bit_cycles, s_t0h_cycles, s_t1h_cycles);
    return OPRT_OK;
}

static void user_main(void)
{
    OPERATE_RET rt;

    tal_log_init(TAL_LOG_LEVEL_DEBUG, 2048, (TAL_LOG_OUTPUT_CB)tkl_log_output);
    PR_NOTICE("======== Agentic Keyboard bring-up: ws2812 ========");
    PR_NOTICE("Firmware: %s v%s, driver=%s", PROJECT_NAME, PROJECT_VERSION, WS2812_DRIVER_REV);
    PR_NOTICE("Built: %s %s", __DATE__, __TIME__);
    PR_NOTICE("TuyaOpen commit: %.12s, platform=%s/%s commit=%.12s", OPEN_COMMIT, PLATFORM_CHIP, PLATFORM_BOARD,
              PLATFORM_COMMIT);

    rt = bringup_uart_init();
    if (rt != OPRT_OK) {
        PR_ERR("UART0 init failed: %d", rt);
        return;
    }
    rt = ws2812_init();
    if (rt != OPRT_OK) {
        PR_ERR("WS2812 init failed: %d", rt);
        return;
    }

    bringup_uart_printf("{\"v\":1,\"type\":\"bringup\",\"payload\":{\"name\":\"ws2812\","
                        "\"version\":\"%s\",\"driver\":\"%s\",\"status\":\"ready\",\"gpio\":%u,\"count\":%u}}\n",
                        PROJECT_VERSION, WS2812_DRIVER_REV, (unsigned)AK_WS2812_DATA_PIN, AK_WS2812_COUNT);

    while (1) {
        for (uint32_t index = 0; index < CNTSOF(s_patterns); index++) {
            rt = ws2812_send(s_patterns[index].rgb);
            if (rt != OPRT_OK) {
                bringup_uart_printf("{\"v\":1,\"type\":\"bringup\",\"payload\":{\"name\":\"ws2812\","
                                    "\"status\":\"error\",\"stage\":\"send\",\"code\":%d}}\n",
                                    rt);
                return;
            }
            PR_NOTICE("WS2812 pattern[%u/%u]=%s", (unsigned)(index + 1U), (unsigned)CNTSOF(s_patterns),
                      s_patterns[index].name);
            bringup_uart_printf("{\"v\":1,\"type\":\"bringup\",\"payload\":{\"name\":\"ws2812\","
                                "\"version\":\"%s\",\"pattern\":\"%s\"}}\n",
                                PROJECT_VERSION, s_patterns[index].name);
            tal_system_sleep(PATTERN_DELAY_MS);
        }
    }
}

static void app_thread(void *arg)
{
    (void)arg;
    user_main();
    tal_thread_delete(s_app_thread);
    s_app_thread = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T thread_cfg = {
        .stackDepth = APP_TASK_STACK_SIZE,
        .priority   = THREAD_PRIO_1,
        .thrdname   = "ws2812",
    };

    tal_thread_create_and_start(&s_app_thread, NULL, NULL, app_thread, NULL, &thread_cfg);
}
