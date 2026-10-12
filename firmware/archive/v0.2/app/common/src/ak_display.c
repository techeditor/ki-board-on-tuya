/**
 * @file ak_display.c
 * @brief ST7789 LCD bring-up on the expansion baseboard v1.2 wiring.
 *
 * Reuses the CL28CK230-18A init sequence validated during hardware bring-up
 * (firmware/bringup/lcd_st7789): a copy of the stock TuyaOpen ST7789 sequence
 * with the trailing INVON replaced by INVOFF, which this panel needs. v1.2 only
 * moves the backlight to P8 (AK_LCD_BACKLIGHT_PIN); SPI0 + DC/RST are unchanged.
 */

#include "ak_display.h"

#include "ak_board_io.h"
#include "tal_api.h"
#include "tdd_disp_st7789.h"
#include "tdl_display_draw.h"
#include "tdl_display_manage.h"

#define AK_DISPLAY_NAME "ak_lcd"
#define AK_LCD_FB_BYTES (AK_LCD_NATIVE_WIDTH * AK_LCD_NATIVE_HEIGHT * 2U)

static TDL_DISP_HANDLE_T      s_disp  = NULL;
static TDL_DISP_DEV_INFO_T    s_info  = {0};
static TDL_DISP_FRAME_BUFF_T *s_frame = NULL;

/*
 * CL28CK230-18A init sequence (validated in bring-up). The stock TuyaOpen
 * ST7789 sequence ends with INVON (0x21), which inverts every pixel on this
 * panel; this copy ends with INVOFF (0x20). Entries: {len, delay_ms, cmd,
 * data...}, terminated by 0.
 */
static const uint8_t s_st7789_init_seq_noinv[] = {
    1,    100, ST7789_SWRESET,                                 // Software reset
    1,    50,  ST7789_SLPOUT,                                  // Exit sleep mode
    2,    10,  ST7789_COLMOD,    0x55,                         // 16-bit colour
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

static OPERATE_RET register_panel(void)
{
    DISP_SPI_DEVICE_CFG_T cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.width     = AK_LCD_NATIVE_WIDTH;
    cfg.height    = AK_LCD_NATIVE_HEIGHT;
    cfg.pixel_fmt = TUYA_PIXEL_FMT_RGB565;
    cfg.rotation  = TUYA_DISPLAY_ROTATION_0;
    cfg.port      = AK_LCD_SPI_PORT;
    cfg.spi_clk   = AK_LCD_SPI_CLOCK_HZ;
    cfg.cs_pin    = AK_LCD_SPI_CS_PIN;
    cfg.dc_pin    = AK_LCD_DC_PIN;
    cfg.rst_pin   = AK_LCD_RST_PIN;

    cfg.bl.type              = TUYA_DISP_BL_TP_GPIO;
    cfg.bl.gpio.pin          = AK_LCD_BACKLIGHT_PIN; /* v1.2: P8 */
    cfg.bl.gpio.active_level = AK_LCD_CONTROL_ACTIVE;
    cfg.power.pin            = AK_LCD_POWER_PIN;
    cfg.power.active_level   = AK_LCD_CONTROL_ACTIVE;

    tdd_disp_spi_st7789_set_init_seq(s_st7789_init_seq_noinv);
    return tdd_disp_spi_st7789_register(AK_DISPLAY_NAME, &cfg);
}

OPERATE_RET ak_display_init(void)
{
    OPERATE_RET rt;

    TUYA_CALL_ERR_RETURN(register_panel());

    s_disp = tdl_disp_find_dev(AK_DISPLAY_NAME);
    if (s_disp == NULL) {
        return OPRT_NOT_FOUND;
    }
    TUYA_CALL_ERR_RETURN(tdl_disp_dev_get_info(s_disp, &s_info));
    TUYA_CALL_ERR_RETURN(tdl_disp_dev_open(s_disp));
    TUYA_CALL_ERR_RETURN(tdl_disp_set_brightness(s_disp, 100));

    s_frame = tdl_disp_create_frame_buff(DISP_FB_TP_PSRAM, AK_LCD_FB_BYTES);
    if (s_frame == NULL) {
        return OPRT_MALLOC_FAILED;
    }
    s_frame->x_start = 0;
    s_frame->y_start = 0;
    s_frame->fmt     = TUYA_PIXEL_FMT_RGB565;
    s_frame->width   = AK_LCD_NATIVE_WIDTH;
    s_frame->height  = AK_LCD_NATIVE_HEIGHT;

    return ak_display_show_bars();
}

OPERATE_RET ak_display_show_bars(void)
{
    static const uint16_t colors[] = {
        0xF800, 0xFFE0, 0x07E0, 0x07FF, 0x001F, 0xF81F, 0xFFFF, 0x0000,
    };
    uint16_t bar_w = AK_LCD_NATIVE_WIDTH / CNTSOF(colors);

    if (s_disp == NULL || s_frame == NULL) {
        return OPRT_RESOURCE_NOT_READY;
    }

    for (uint32_t i = 0; i < CNTSOF(colors); i++) {
        TDL_DISP_RECT_T rect = {
            .x0 = (uint16_t)(i * bar_w),
            .y0 = 0,
            .x1 = (i == CNTSOF(colors) - 1) ? (AK_LCD_NATIVE_WIDTH - 1) : (uint16_t)((i + 1) * bar_w - 1),
            .y1 = AK_LCD_NATIVE_HEIGHT - 1,
        };
        OPERATE_RET rt = tdl_disp_draw_fill(s_frame, &rect, colors[i], s_info.is_swap);
        if (rt != OPRT_OK) {
            return rt;
        }
    }
    return tdl_disp_dev_flush(s_disp, s_frame);
}

OPERATE_RET ak_display_fill(uint16_t rgb565)
{
    if (s_disp == NULL || s_frame == NULL) {
        return OPRT_RESOURCE_NOT_READY;
    }
    OPERATE_RET rt = tdl_disp_draw_fill_full(s_frame, rgb565, s_info.is_swap);
    if (rt != OPRT_OK) {
        return rt;
    }
    return tdl_disp_dev_flush(s_disp, s_frame);
}
