/**
 * @file ak_statuslcd.c
 * @brief Shared lightweight status LCD (ST7789 landscape 320x240 + ak_gfx).
 */

#include <string.h>

#include "ak_statuslcd.h"

#include "ak_board_io.h"
#include "tal_api.h"
#include "tdd_disp_st7789.h"
#include "tdl_display_draw.h"
#include "tdl_display_manage.h"

#define STATUSLCD_NAME "ak_statuslcd"
#define FRAME_BYTES    (AK_STATUSLCD_W * AK_STATUSLCD_H * 2U)
#define TITLE_H        22U

static TDL_DISP_HANDLE_T      s_disp = NULL;
static TDL_DISP_DEV_INFO_T    s_info;
static TDL_DISP_FRAME_BUFF_T *s_frame = NULL;
static bool                   s_ready = false;

/*
 * CL28CK230-18A ST7789 init: stock sequence with INVON replaced by INVOFF (this
 * panel is not inverted) and MADCTL set for landscape (see AK_STATUSLCD_MADCTL).
 */
static const uint8_t s_st7789_init_seq[] = {
    1,
    100,
    ST7789_SWRESET,
    1,
    50,
    ST7789_SLPOUT,
    2,
    10,
    ST7789_COLMOD,
    0x55,
    2,
    0,
    ST7789_VCMOFSET,
    0x1a,
    6,
    0,
    ST7789_PORCTRL,
    0x0c,
    0x0c,
    0x00,
    0x33,
    0x33,
    1,
    0,
    ST7789_INVOFF,
    2,
    0,
    ST7789_GCTRL,
    0x56,
    2,
    0,
    ST7789_VCOMS,
    0x18,
    2,
    0,
    ST7789_LCMCTRL,
    0x2c,
    2,
    0,
    ST7789_VDVVRHEN,
    0x01,
    2,
    0,
    ST7789_VRHS,
    0x1f,
    2,
    0,
    ST7789_VDVSET,
    0x20,
    2,
    0,
    ST7789_FRCTR2,
    0x0f,
    3,
    0,
    ST7789_PWCTRL1,
    0xa6,
    0xa1,
    2,
    0,
    ST7789_PWCTRL2,
    0x03,
    2,
    0,
    ST7789_MADCTL,
    AK_STATUSLCD_MADCTL,
    15,
    0,
    ST7789_PVGAMCTRL,
    0xd0,
    0x0d,
    0x14,
    0x0b,
    0x0b,
    0x07,
    0x3a,
    0x44,
    0x50,
    0x08,
    0x13,
    0x13,
    0x2d,
    0x32,
    15,
    0,
    ST7789_NVGAMCTRL,
    0xd0,
    0x0d,
    0x14,
    0x0b,
    0x0b,
    0x07,
    0x3a,
    0x44,
    0x50,
    0x08,
    0x13,
    0x13,
    0x2d,
    0x32,
    1,
    0,
    ST7789_SPI2EN,
    1,
    10,
    ST7789_INVOFF,
    1,
    10,
    ST7789_DISPON,
    0,
};

OPERATE_RET ak_statuslcd_init(void)
{
    OPERATE_RET           rt;
    DISP_SPI_DEVICE_CFG_T config;

    memset(&config, 0, sizeof(config));
    config.width     = AK_STATUSLCD_W;
    config.height    = AK_STATUSLCD_H;
    config.pixel_fmt = TUYA_PIXEL_FMT_RGB565;
    config.rotation  = TUYA_DISPLAY_ROTATION_0; /* orientation handled by MADCTL */
    config.port      = AK_LCD_SPI_PORT;
    config.spi_clk   = AK_LCD_SPI_CLOCK_HZ;
    config.cs_pin    = AK_LCD_SPI_CS_PIN;
    config.dc_pin    = AK_LCD_DC_PIN;
    config.rst_pin   = AK_LCD_RST_PIN;

    config.bl.type              = TUYA_DISP_BL_TP_GPIO;
    config.bl.gpio.pin          = AK_LCD_BACKLIGHT_PIN;
    config.bl.gpio.active_level = AK_LCD_CONTROL_ACTIVE;
    config.power.pin            = AK_LCD_POWER_PIN;
    config.power.active_level   = AK_LCD_CONTROL_ACTIVE;

    tdd_disp_spi_st7789_set_init_seq(s_st7789_init_seq);

    rt = tdd_disp_spi_st7789_register(STATUSLCD_NAME, &config);
    if (rt != OPRT_OK) {
        return rt;
    }

    s_disp = tdl_disp_find_dev(STATUSLCD_NAME);
    if (s_disp == NULL) {
        return OPRT_NOT_FOUND;
    }
    TUYA_CALL_ERR_RETURN(tdl_disp_dev_get_info(s_disp, &s_info));
    TUYA_CALL_ERR_RETURN(tdl_disp_dev_open(s_disp));
    TUYA_CALL_ERR_RETURN(tdl_disp_set_brightness(s_disp, 100));

    s_frame = tdl_disp_create_frame_buff(DISP_FB_TP_PSRAM, FRAME_BYTES);
    if (s_frame == NULL) {
        return OPRT_MALLOC_FAILED;
    }
    s_frame->x_start = 0;
    s_frame->y_start = 0;
    s_frame->fmt     = TUYA_PIXEL_FMT_RGB565;
    s_frame->width   = AK_STATUSLCD_W;
    s_frame->height  = AK_STATUSLCD_H;

    ak_gfx_bind(s_frame, s_info.is_swap);
    ak_gfx_clear(AK_COL_BLACK);
    s_ready = true;
    return OPRT_OK;
}

bool ak_statuslcd_ready(void)
{
    return s_ready;
}

void ak_statuslcd_flush(void)
{
    if (s_ready) {
        tdl_disp_dev_flush(s_disp, s_frame);
    }
}

void ak_statuslcd_title(const char *title, uint16_t bar_color)
{
    if (!s_ready) {
        return;
    }
    ak_gfx_fill_rect(0, 0, AK_STATUSLCD_W, TITLE_H, bar_color);
    ak_gfx_text(6, 7, title == NULL ? "" : title, AK_COL_WHITE, bar_color, 1);
    ak_gfx_fill_rect(0, TITLE_H, AK_STATUSLCD_W, (uint16_t)(AK_STATUSLCD_H - TITLE_H), AK_COL_BLACK);
}
