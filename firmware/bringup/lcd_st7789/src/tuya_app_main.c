/**
 * @file tuya_app_main.c
 * @brief Validate the CL28CK230-18A ST7789 display and project SPI wiring.
 */

#include <string.h>

#include "agentic_board_io.h"
#include "bringup_uart.h"
#include "tal_api.h"
#include "tdd_disp_st7789.h"
#include "tdl_display_draw.h"
#include "tdl_display_manage.h"
#include "tkl_output.h"

#define APP_TASK_STACK_SIZE (6U * 1024U)
#define DISPLAY_NAME        "bringup_lcd"
#define FRAME_BYTES         (AK_LCD_NATIVE_WIDTH * AK_LCD_NATIVE_HEIGHT * 2U)
#define PATTERN_DELAY_MS    2000U

typedef struct {
    const char *name;
    uint16_t    color;
} SOLID_PATTERN_T;

static THREAD_HANDLE          s_app_thread = NULL;
static TDL_DISP_HANDLE_T      s_display    = NULL;
static TDL_DISP_DEV_INFO_T    s_info;
static TDL_DISP_FRAME_BUFF_T *s_frame = NULL;

static const SOLID_PATTERN_T s_solid_patterns[] = {
    {"red", 0xF800}, {"green", 0x07E0}, {"blue", 0x001F}, {"white", 0xFFFF}, {"black", 0x0000},
};

/*
 * The stock TuyaOpen ST7789 init sequence ends with INVON (0x21, display
 * inversion on), which some IPS panels require but the CL28CK230-18A does not.
 * With INVON this panel shows every pixel bit-inverted (red<->cyan, white<->
 * black). This is a copy of the stock sequence with the final INVON replaced by
 * INVOFF (0x20) so colors display correctly on this panel. Injected via
 * tdd_disp_spi_st7789_set_init_seq() before register.
 */
static const uint8_t s_st7789_init_seq_noinv[] = {
    1,    100, ST7789_SWRESET,                                 // Software reset
    1,    50,  ST7789_SLPOUT,                                  // Exit sleep mode
    2,    10,  ST7789_COLMOD,    0x55,                         // 16-bit color
    2,    0,   ST7789_VCMOFSET,  0x1a,                         // VCOM
    6,    0,   ST7789_PORCTRL,   0x0c, 0x0c, 0x00, 0x33, 0x33, // Porch
    1,    0,   ST7789_INVOFF,                                  //
    2,    0,   ST7789_GCTRL,     0x56,                         // Gate control
    2,    0,   ST7789_VCOMS,     0x18,                         // VCOMS
    2,    0,   ST7789_LCMCTRL,   0x2c,                         // LCM control
    2,    0,   ST7789_VDVVRHEN,  0x01,                         // VDV/VRH enable
    2,    0,   ST7789_VRHS,      0x1f,                         // VRH set
    2,    0,   ST7789_VDVSET,    0x20,                         // VDV setting
    2,    0,   ST7789_FRCTR2,    0x0f,                         // FR control 2
    3,    0,   ST7789_PWCTRL1,   0xa6, 0xa1,                   // Power control 1
    2,    0,   ST7789_PWCTRL2,   0x03,                         // Power control 2
    2,    0,   ST7789_MADCTL,    0x00,                         // MADCTL
    15,   0,   ST7789_PVGAMCTRL, 0xd0, 0x0d, 0x14, 0x0b, 0x0b, 0x07, 0x3a, 0x44, 0x50, 0x08, 0x13, 0x13, 0x2d,
    0x32, // Positive gamma
    15,   0,   ST7789_NVGAMCTRL, 0xd0, 0x0d, 0x14, 0x0b, 0x0b, 0x07, 0x3a, 0x44, 0x50, 0x08, 0x13, 0x13, 0x2d,
    0x32,                     // Negative gamma
    1,    0,   ST7789_SPI2EN, //
    1,    10,  ST7789_INVOFF, // No inversion for this panel (stock uses INVON)
    1,    10,  ST7789_DISPON, // Display on
    0                         // Terminate list
};

static OPERATE_RET display_register(void)
{
    DISP_SPI_DEVICE_CFG_T config;

    memset(&config, 0, sizeof(config));
    config.width     = AK_LCD_NATIVE_WIDTH;
    config.height    = AK_LCD_NATIVE_HEIGHT;
    config.pixel_fmt = TUYA_PIXEL_FMT_RGB565;
    config.rotation  = TUYA_DISPLAY_ROTATION_0;
    config.port      = AK_LCD_SPI_PORT;
    config.spi_clk   = AK_LCD_SPI_CLOCK_HZ;
    config.cs_pin    = AK_LCD_SPI_CS_PIN;
    config.dc_pin    = AK_LCD_DC_PIN;
    config.rst_pin   = AK_LCD_RST_PIN;

    config.bl.type              = TUYA_DISP_BL_TP_GPIO;
    config.bl.gpio.pin          = AK_LCD_BACKLIGHT_PIN;
    config.bl.gpio.active_level = AK_LCD_CONTROL_ACTIVE;
    /*
     * v0.3: the 12P T280C7-C12-26 (ST7789P3) panel wires VDD (FPC pin 9) straight
     * to 3V3 -- there is no LCD_PWR enable line any more. The old P7 power pin is
     * repurposed as the TE input (see agentic_board_io.h). The driver treats
     * power.pin >= TUYA_GPIO_NUM_MAX as "no power GPIO" (tdl_display_manage.c),
     * so set the sentinel explicitly -- memset left it at 0 (= GPIO0), which the
     * driver would otherwise init and drive.
     */
    config.power.pin          = TUYA_GPIO_NUM_MAX;
    config.power.active_level = TUYA_GPIO_LEVEL_LOW;

    /* Use the no-inversion init sequence for the CL28CK230-18A panel. */
    tdd_disp_spi_st7789_set_init_seq(s_st7789_init_seq_noinv);

    return tdd_disp_spi_st7789_register(DISPLAY_NAME, &config);
}

static OPERATE_RET display_open(void)
{
    OPERATE_RET rt;

    s_display = tdl_disp_find_dev(DISPLAY_NAME);
    if (s_display == NULL) {
        return OPRT_NOT_FOUND;
    }
    TUYA_CALL_ERR_RETURN(tdl_disp_dev_get_info(s_display, &s_info));
    TUYA_CALL_ERR_RETURN(tdl_disp_dev_open(s_display));
    TUYA_CALL_ERR_RETURN(tdl_disp_set_brightness(s_display, 100));

    s_frame = tdl_disp_create_frame_buff(DISP_FB_TP_PSRAM, FRAME_BYTES);
    if (s_frame == NULL) {
        return OPRT_MALLOC_FAILED;
    }
    s_frame->x_start = 0;
    s_frame->y_start = 0;
    s_frame->fmt     = TUYA_PIXEL_FMT_RGB565;
    s_frame->width   = AK_LCD_NATIVE_WIDTH;
    s_frame->height  = AK_LCD_NATIVE_HEIGHT;
    return OPRT_OK;
}

static OPERATE_RET draw_color_bars(void)
{
    static const uint16_t colors[] = {
        0xF800, 0xFFE0, 0x07E0, 0x07FF, 0x001F, 0xF81F, 0xFFFF, 0x0000,
    };
    uint16_t bar_width = AK_LCD_NATIVE_WIDTH / CNTSOF(colors);

    for (uint32_t index = 0; index < CNTSOF(colors); index++) {
        TDL_DISP_RECT_T rect = {
            .x0 = (uint16_t)(index * bar_width),
            .y0 = 0,
            .x1 = index == CNTSOF(colors) - 1 ? AK_LCD_NATIVE_WIDTH - 1 : (uint16_t)((index + 1) * bar_width - 1),
            .y1 = AK_LCD_NATIVE_HEIGHT - 1,
        };
        OPERATE_RET rt = tdl_disp_draw_fill(s_frame, &rect, colors[index], s_info.is_swap);
        if (rt != OPRT_OK) {
            return rt;
        }
    }
    return tdl_disp_dev_flush(s_display, s_frame);
}

static OPERATE_RET draw_solid(uint16_t color)
{
    OPERATE_RET rt = tdl_disp_draw_fill_full(s_frame, color, s_info.is_swap);
    if (rt != OPRT_OK) {
        return rt;
    }
    return tdl_disp_dev_flush(s_display, s_frame);
}

static void user_main(void)
{
    OPERATE_RET rt;

    tal_log_init(TAL_LOG_LEVEL_DEBUG, 2048, (TAL_LOG_OUTPUT_CB)tkl_log_output);
    PR_NOTICE("======== Agentic Keyboard bring-up: lcd_st7789 ========");

    rt = bringup_uart_init();
    if (rt != OPRT_OK) {
        PR_ERR("UART0 init failed: %d", rt);
        return;
    }
    rt = display_register();
    if (rt != OPRT_OK) {
        PR_ERR("Display register failed: %d", rt);
        return;
    }
    rt = display_open();
    if (rt != OPRT_OK) {
        PR_ERR("Display open failed: %d", rt);
        return;
    }

    bringup_uart_printf("{\"v\":1,\"type\":\"bringup\",\"payload\":{\"name\":\"lcd_st7789\","
                        "\"status\":\"ready\",\"width\":%u,\"height\":%u,\"spi_hz\":%u}}\n",
                        AK_LCD_NATIVE_WIDTH, AK_LCD_NATIVE_HEIGHT, AK_LCD_SPI_CLOCK_HZ);

    while (1) {
        rt = draw_color_bars();
        bringup_uart_printf("{\"v\":1,\"type\":\"bringup\",\"payload\":{\"name\":\"lcd_st7789\","
                            "\"pattern\":\"color_bars\",\"result\":%d}}\n",
                            rt);
        tal_system_sleep(PATTERN_DELAY_MS);

        for (uint32_t index = 0; index < CNTSOF(s_solid_patterns); index++) {
            rt = draw_solid(s_solid_patterns[index].color);
            bringup_uart_printf("{\"v\":1,\"type\":\"bringup\",\"payload\":{\"name\":\"lcd_st7789\","
                                "\"pattern\":\"%s\",\"result\":%d}}\n",
                                s_solid_patterns[index].name, rt);
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
        .thrdname   = "lcd_st7789",
    };

    tal_thread_create_and_start(&s_app_thread, NULL, NULL, app_thread, NULL, &thread_cfg);
}
