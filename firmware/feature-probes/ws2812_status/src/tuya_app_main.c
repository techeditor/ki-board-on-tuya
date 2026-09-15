/**
 * @file tuya_app_main.c
 * @brief FP-WS2812 · A1 — ak_leds driver bring-up self-test.
 *
 * This A1 milestone validates the ak_leds wrapper (spec FP-WS2812 R1) on the
 * v1.2 board: it walks a red/green/blue self-test across the 3 WS2812 LEDs to
 * confirm the register/open/set/show sequence and per-LED addressing behave the
 * same as the validated ws2812 bring-up app. The state machine (ak_led_state),
 * the tick-driven effects and the event sources arrive in A2..A4.
 */

#include "ak_board_io.h"
#include "ak_leds.h"
#include "bringup_uart.h"
#include "tal_api.h"
#include "tkl_output.h"

#define APP_TASK_STACK_SIZE (4U * 1024U)
#define SELFTEST_STEP_MS 600U

static THREAD_HANDLE s_app_thread = NULL;

/* One self-test step: which slot, and its R/G/B (already within max level). */
typedef struct {
  const char *name;
  uint8_t slot;
  uint8_t r, g, b;
} selftest_step_t;

static const selftest_step_t s_selftest[] = {
    {"all_off", 0xFF, 0, 0, 0}, /* slot 0xFF = clear all */
    {"led0_red", 0, AK_LEDS_MAX_LEVEL, 0, 0},
    {"led1_green", 1, 0, AK_LEDS_MAX_LEVEL, 0},
    {"led2_blue", 2, 0, 0, AK_LEDS_MAX_LEVEL},
    {"all_white", 0xFF, AK_LEDS_MAX_LEVEL, AK_LEDS_MAX_LEVEL,
     AK_LEDS_MAX_LEVEL},
};

static void clear_all(void) {
  for (uint8_t i = 0; i < AK_WS2812_COUNT; i++) {
    ak_leds_set(i, 0, 0, 0);
  }
}

static void apply_step(const selftest_step_t *step) {
  if (step->slot == 0xFF) {
    for (uint8_t i = 0; i < AK_WS2812_COUNT; i++) {
      ak_leds_set(i, step->r, step->g, step->b);
    }
  } else {
    clear_all();
    ak_leds_set(step->slot, step->r, step->g, step->b);
  }
  ak_leds_show();
}

static void user_main(void) {
  OPERATE_RET rt;

  tal_log_init(TAL_LOG_LEVEL_DEBUG, 2048, (TAL_LOG_OUTPUT_CB)tkl_log_output);
  PR_NOTICE(
      "======== Agentic Keyboard FP-WS2812: A1 ak_leds self-test ========");
  PR_NOTICE("Built: %s %s", __DATE__, __TIME__);

  rt = bringup_uart_init();
  if (rt != OPRT_OK) {
    PR_ERR("UART0 init failed: %d", rt);
    return;
  }

  rt = ak_leds_init();
  if (rt != OPRT_OK || !ak_leds_ready()) {
    /* R1.4: degrade gracefully — report and idle, do not reset. */
    PR_ERR("ak_leds init failed: %d; LED feature unavailable, continuing", rt);
    bringup_uart_printf(
        "{\"v\":1,\"type\":\"probe\",\"payload\":{\"name\":\"ws2812_status\","
        "\"status\":\"error\",\"stage\":\"leds_init\",\"code\":%d}}\n",
        rt);
    return;
  }

  bringup_uart_printf(
      "{\"v\":1,\"type\":\"probe\",\"payload\":{\"name\":\"ws2812_status\","
      "\"status\":\"ready\",\"spi_port\":%u,\"data_pin\":%u,\"count\":%u,\"max_"
      "level\":%u}}\n",
      (unsigned)AK_WS2812_SPI_PORT, (unsigned)AK_WS2812_DATA_PIN,
      (unsigned)AK_WS2812_COUNT, (unsigned)AK_LEDS_MAX_LEVEL);

  while (1) {
    for (uint32_t i = 0; i < CNTSOF(s_selftest); i++) {
      apply_step(&s_selftest[i]);
      PR_NOTICE("selftest[%u/%u]=%s", (unsigned)(i + 1U),
                (unsigned)CNTSOF(s_selftest), s_selftest[i].name);
      bringup_uart_printf(
          "{\"v\":1,\"type\":\"probe\",\"payload\":{\"name\":\"ws2812_status\","
          "\"selftest\":\"%s\"}}\n",
          s_selftest[i].name);
      tal_system_sleep(SELFTEST_STEP_MS);
    }
  }
}

static void app_thread(void *arg) {
  (void)arg;
  user_main();
  tal_thread_delete(s_app_thread);
  s_app_thread = NULL;
}

void tuya_app_main(void) {
  THREAD_CFG_T thread_cfg = {
      .stackDepth = APP_TASK_STACK_SIZE,
      .priority = THREAD_PRIO_1,
      .thrdname = "ws2812_status",
  };

  tal_thread_create_and_start(&s_app_thread, NULL, NULL, app_thread, NULL,
                              &thread_cfg);
}
