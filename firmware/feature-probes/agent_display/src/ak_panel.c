/**
 * @file ak_panel.c
 * @brief Register the CL28CK230-18A (ST7789) panel for LVGL (v1.2 wiring).
 *
 * The init sequence is the no-inversion (INVOFF) copy validated during hardware
 * bring-up (firmware/bringup/lcd_st7789 + firmware/app/ak_display.c). We only
 * register here; lv_vendor_init() opens the device and owns the draw buffers.
 */

#include "ak_panel.h"

#include <string.h>

#include "ak_board_io.h"
#include "tal_api.h"
#include "tdd_disp_st7789.h"

/*
 * Stock TuyaOpen ST7789 sequence with the trailing INVON (0x21) replaced by
 * INVOFF (0x20): the CL28CK230-18A shows every pixel bit-inverted with INVON.
 * Entries: {len, delay_ms, cmd, data...}, list terminated by 0.
 */
/* clang-format off */
static const uint8_t s_st7789_init_seq_noinv[] = {
    1,  100, ST7789_SWRESET,
    1,  50,  ST7789_SLPOUT,
    2,  10,  ST7789_COLMOD,    0x55,
    2,  0,   ST7789_VCMOFSET,  0x1a,
    6,  0,   ST7789_PORCTRL,   0x0c, 0x0c, 0x00, 0x33, 0x33,
    1,  0,   ST7789_INVOFF,
    2,  0,   ST7789_GCTRL,     0x56,
    2,  0,   ST7789_VCOMS,     0x18,
    2,  0,   ST7789_LCMCTRL,   0x2c,
    2,  0,   ST7789_VDVVRHEN,  0x01,
    2,  0,   ST7789_VRHS,      0x1f,
    2,  0,   ST7789_VDVSET,    0x20,
    2,  0,   ST7789_FRCTR2,    0x0f,
    3,  0,   ST7789_PWCTRL1,   0xa6, 0xa1,
    2,  0,   ST7789_PWCTRL2,   0x03,
    2,  0,   ST7789_MADCTL,    0x00,
    15, 0,   ST7789_PVGAMCTRL, 0xd0, 0x0d, 0x14, 0x0b, 0x0b, 0x07, 0x3a, 0x44, 0x50, 0x08, 0x13, 0x13, 0x2d, 0x32,
    15, 0,   ST7789_NVGAMCTRL, 0xd0, 0x0d, 0x14, 0x0b, 0x0b, 0x07, 0x3a, 0x44, 0x50, 0x08, 0x13, 0x13, 0x2d, 0x32,
    1,  0,   ST7789_SPI2EN,
    1,  10,  ST7789_INVOFF,
    1,  10,  ST7789_DISPON,
    0
};
/* clang-format on */

OPERATE_RET ak_panel_register(void)
{
    DISP_SPI_DEVICE_CFG_T cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.width     = AK_LCD_NATIVE_WIDTH;  /* 240 (native short side) */
    cfg.height    = AK_LCD_NATIVE_HEIGHT; /* 320 (native long side)  */
    cfg.pixel_fmt = TUYA_PIXEL_FMT_RGB565;
    cfg.rotation  = TUYA_DISPLAY_ROTATION_270; /* 320x240 landscape, flipped 180 from ROT_90 (panel mount) */
    cfg.port      = AK_LCD_SPI_PORT;
    cfg.spi_clk   = AK_LCD_SPI_CLOCK_HZ;
    cfg.cs_pin    = AK_LCD_SPI_CS_PIN;
    cfg.dc_pin    = AK_LCD_DC_PIN;
    cfg.rst_pin   = AK_LCD_RST_PIN;

    cfg.bl.type              = TUYA_DISP_BL_TP_GPIO;
    cfg.bl.gpio.pin          = AK_LCD_BACKLIGHT_PIN; /* v1.2: P8, high-active */
    cfg.bl.gpio.active_level = AK_LCD_CONTROL_ACTIVE;
    cfg.power.pin            = AK_LCD_POWER_PIN;
    cfg.power.active_level   = AK_LCD_CONTROL_ACTIVE;

    tdd_disp_spi_st7789_set_init_seq(s_st7789_init_seq_noinv);
    return tdd_disp_spi_st7789_register(AK_PANEL_NAME, &cfg);
}
