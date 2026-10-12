/**
 * @file ak_statuslcd.c
 * @brief Shared lightweight status LCD (ST7789 landscape 320x240 + ak_gfx).
 */

#include <string.h>

#include "ak_statuslcd.h"

#include "ak_board_io.h"
#include "ak_icons16.h"
#include "armstar.h"
#include "tal_api.h"
#include "tdd_disp_st7789.h"
#include "tdd_display_spi.h"
#include "tdl_display_draw.h"
#include "tdl_display_manage.h"
#include "tkl_gpio.h"

#define STATUSLCD_NAME   "ak_statuslcd"
#define FRAME_BYTES      (AK_STATUSLCD_W * AK_STATUSLCD_H * 2U)
#define TITLE_H          22U
#define FLUSH_TIMEOUT_MS 1000U

/* T5AI's SPI DMA adapter does not populate the actual source range used by its
 * generic cache-maintenance path. Explicitly synchronize the PSRAM framebuffer
 * before queueing it, otherwise Wi-Fi/cloud activity can evict dirty cache
 * lines while DMA is reading stale pixels. */

static TDL_DISP_HANDLE_T      s_disp = NULL;
static TDL_DISP_DEV_INFO_T    s_info;
static TDL_DISP_FRAME_BUFF_T *s_frame = NULL;
static bool                   s_ready = false;
static volatile bool          s_flush_done = true;
static DISP_SPI_BASE_CFG_T    s_spi_cfg;

static void ak_statuslcd_on_flush_done(TDL_DISP_FRAME_BUFF_T *frame)
{
    (void)frame;
    s_flush_done = true;
}

static OPERATE_RET ak_statuslcd_restore_window(void)
{
    OPERATE_RET rt;
    uint16_t x_end = (uint16_t)(AK_STATUSLCD_W - 1U);
    uint16_t y_end = (uint16_t)(AK_STATUSLCD_H - 1U);
    uint8_t columns[4] = {0U, 0U, (uint8_t)(x_end >> 8), (uint8_t)x_end};
    uint8_t rows[4] = {0U, 0U, (uint8_t)(y_end >> 8), (uint8_t)y_end};

    TUYA_CALL_ERR_RETURN(tdd_disp_spi_send_cmd(&s_spi_cfg, ST7789_CASET));
    TUYA_CALL_ERR_RETURN(tdd_disp_spi_send_data(&s_spi_cfg, columns,
                                                 sizeof(columns)));
    TUYA_CALL_ERR_RETURN(tdd_disp_spi_send_cmd(&s_spi_cfg, ST7789_RASET));
    TUYA_CALL_ERR_RETURN(tdd_disp_spi_send_data(&s_spi_cfg, rows, sizeof(rows)));
    return OPRT_OK;
}

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
    /* v0.3: no LCD power-enable pin (panel VDD tied to 3V3). */
    config.power.pin            = AK_LCD_POWER_PIN; /* TUYA_GPIO_NUM_MAX */
    config.power.active_level   = AK_LCD_CONTROL_ACTIVE;

    /* v0.3: FPC pin7/TE is wired to P7 through a 10k series resistor. The panel
     * drives it; the firmware must keep P7 a high-impedance INPUT so it never
     * fights the panel's TE output. Configure it before registering the panel. */
    {
        TUYA_GPIO_BASE_CFG_T te_cfg = {
            .mode   = TUYA_GPIO_FLOATING,
            .direct = TUYA_GPIO_INPUT,
            .level  = TUYA_GPIO_LEVEL_HIGH,
        };
        (void)tkl_gpio_init(AK_LCD_TE_PIN, &te_cfg);
    }

    memset(&s_spi_cfg, 0, sizeof(s_spi_cfg));
    s_spi_cfg.width     = config.width;
    s_spi_cfg.height    = config.height;
    s_spi_cfg.pixel_fmt = config.pixel_fmt;
    s_spi_cfg.cs_pin    = config.cs_pin;
    s_spi_cfg.dc_pin    = config.dc_pin;
    s_spi_cfg.rst_pin   = config.rst_pin;
    s_spi_cfg.port      = config.port;
    s_spi_cfg.spi_clk   = config.spi_clk;
    s_spi_cfg.cmd_caset = ST7789_CASET;
    s_spi_cfg.cmd_raset = ST7789_RASET;
    s_spi_cfg.cmd_ramwr = ST7789_RAMWR;

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

OPERATE_RET ak_statuslcd_set_brightness(uint8_t brightness)
{
    if (!s_ready || s_disp == NULL) {
        return OPRT_NOT_FOUND;
    }
    ak_gfx_set_brightness(brightness);
    return OPRT_OK;
}

void ak_statuslcd_flush(void)
{
    if (!s_ready) {
        return;
    }

    /* The SPI TDD queues the framebuffer pointer and returns immediately. Do
     * not let the renderer overwrite that PSRAM until the display task has
     * completed every DMA chunk and invokes free_cb. */
    OPERATE_RET rt = ak_statuslcd_restore_window();
    if (rt != OPRT_OK) {
        PR_ERR("status LCD window restore failed: %d", rt);
        return;
    }

    /* The frame lives in cacheable PSRAM. The T5 SPI DMA path does not clean
     * this actual address range, so make every completed software render
     * visible to DMA before it starts reading the frame. */
    SCB_CleanDCache_by_Addr((uint32_t *)s_frame->frame, (int32_t)s_frame->len);

    s_flush_done = false;
    s_frame->free_cb = ak_statuslcd_on_flush_done;
    rt = tdl_disp_dev_flush(s_disp, s_frame);
    if (rt != OPRT_OK) {
        s_frame->free_cb = NULL;
        s_flush_done = true;
        PR_ERR("status LCD flush enqueue failed: %d", rt);
        return;
    }

    uint32_t started_ms = tal_system_get_millisecond();
    while (!s_flush_done &&
           (uint32_t)(tal_system_get_millisecond() - started_ms) <
               FLUSH_TIMEOUT_MS) {
        tal_system_sleep(1U);
    }
    if (!s_flush_done) {
        /* Reusing a buffer still owned by SPI corrupts the panel. Stop future
         * drawing instead of returning a writable in-flight framebuffer. */
        PR_ERR("status LCD flush timed out; disabling redraw");
        s_ready = false;
        return;
    }
    s_frame->free_cb = NULL;
}

void ak_statuslcd_title(const char *title, uint16_t bar_color)
{
    if (!s_ready) {
        return;
    }
    ak_gfx_fill_rect(0, 0, AK_STATUSLCD_W, TITLE_H, bar_color);
    ak_gfx_text(6, 7, title == NULL ? "" : title, AK_COL_BLACK, bar_color, 1);
    ak_gfx_fill_rect(0, TITLE_H, AK_STATUSLCD_W, (uint16_t)(AK_STATUSLCD_H - TITLE_H), AK_COL_BLACK);
}

void ak_statuslcd_title_links(const char *title, uint16_t bar_color, bool wifi_connected,
                              bool usb_connected, bool ble_connected)
{
    const uint16_t icon_y = (uint16_t)((TITLE_H - AK_ICON_H) / 2U);

    ak_statuslcd_title(title, bar_color);
    if (!s_ready) {
        return;
    }

    /* Right aligned: WiFi, USB, BLE. 16 px icons with 4 px gaps. */
    ak_gfx_icon(256U, icon_y, wifi_connected ? AK_ICON_WIFI : AK_ICON_NO_WIFI,
                AK_ICON_W, AK_ICON_H, AK_COL_BLACK, bar_color);
    ak_gfx_icon(276U, icon_y, usb_connected ? AK_ICON_USB : AK_ICON_NO_USB,
                AK_ICON_W, AK_ICON_H, AK_COL_BLACK, bar_color);
    ak_gfx_icon(296U, icon_y, ble_connected ? AK_ICON_BLUETOOTH : AK_ICON_NO_BLUETOOTH,
                AK_ICON_W, AK_ICON_H, AK_COL_BLACK, bar_color);
}
