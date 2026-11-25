/**
 * @file ak_statuslcd.c
 * @brief Shared lightweight status LCD (ST7789 landscape 320x240 + ak_gfx).
 */

#include <string.h>
#include <stdio.h>

#include "ak_statuslcd.h"

#include "ak_board_io.h"
#include "ak_icons16.h"
#include "ak_palette.h"
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
static TDL_DISP_FRAME_BUFF_T *s_part  = NULL; /* ak_statuslcd_flush_rect copy */
static TDL_DISP_FRAME_BUFF_T *s_busy  = NULL; /* buffer of the DMA in flight */
static uint32_t               s_frame_gen;    /* full flushes so far */
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
 * v0.3 12P T280C7-C12-26 (ST7789P3) init: stock sequence with INVON kept (this
 * panel shows every pixel bit-inverted -- red<->cyan, white<->black -- under
 * INVOFF; INVON is correct, verified on hardware 2026-10-09, matching the
 * bringup lcd_st7789 probe). MADCTL set for landscape (see AK_STATUSLCD_MADCTL).
 * The old CL28CK230-18A board used INVOFF.
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
    ST7789_INVON,
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
    ST7789_INVON,
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
    ak_gfx_clear(AK_UI_BG);
    /* Small second buffer for ak_statuslcd_flush_rect (FR-14). Optional: without
     * it partial updates are skipped and the caller redraws the full frame. */
    s_part = tdl_disp_create_frame_buff(DISP_FB_TP_PSRAM, AK_STATUSLCD_RECT_MAX_PX * 2U);
    if (s_part == NULL) {
        PR_WARN("status LCD: no partial-refresh buffer; animations off");
    } else {
        s_part->fmt = TUYA_PIXEL_FMT_RGB565;
    }
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

bool ak_statuslcd_flush_busy(void)
{
    return s_ready && !s_flush_done;
}

/*
 * Block until the in-flight DMA (if any) completes, bounded by FLUSH_TIMEOUT_MS.
 * Returns true when the framebuffer is safe to reuse. Pulled out of
 * ak_statuslcd_flush() so the wait no longer sits on the caller's path right
 * after enqueue: the previous busy-wait kept the single-threaded app loop out
 * of ak_transport_read() for up to 1000ms per redraw, stalling PC->board UART.
 * Now the wait happens only when a NEW frame is about to overwrite PSRAM still
 * owned by SPI, and the app loop drains it between frames (see
 * ak_statuslcd_flush_poll).
 */
bool ak_statuslcd_flush_wait(void)
{
    if (!s_ready) {
        return false;
    }
    if (s_flush_done) {
        return true;
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
        return false;
    }
    if (s_busy != NULL) {
        s_busy->free_cb = NULL;
        s_busy = NULL;
    }
    return true;
}

/*
 * Non-blocking: if a prior DMA has already finished, release the free_cb so the
 * framebuffer is marked reusable without ever sleeping. The app loop calls this
 * each iteration (cheap) so a completed flush is reclaimed promptly without the
 * next render having to wait for it.
 */
void ak_statuslcd_flush_poll(void)
{
    if (s_ready && s_flush_done && s_busy != NULL) {
        s_busy->free_cb = NULL;
        s_busy = NULL;
    }
}

void ak_statuslcd_flush(void)
{
    if (!s_ready) {
        return;
    }

    /* The SPI TDD queues the framebuffer pointer and returns immediately. A new
     * frame must not overwrite PSRAM the previous DMA may still be reading, so
     * wait for the prior flush to complete BEFORE touching the buffer. This is
     * the only remaining blocking point, and it is almost always instant: the
     * CPU-side drawing done by ak_render_status() before calling us already
     * takes longer than the ~25ms full-frame DMA, so by the time we get here
     * the previous flush has finished and this returns without sleeping. */
    if (!ak_statuslcd_flush_wait()) {
        return;
    }

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
    s_busy           = s_frame;
    s_frame_gen++;
    rt = tdl_disp_dev_flush(s_disp, s_frame);
    if (rt != OPRT_OK) {
        s_frame->free_cb = NULL;
        s_busy           = NULL;
        s_flush_done = true;
        PR_ERR("status LCD flush enqueue failed: %d", rt);
        return;
    }
    /* Return immediately -- the DMA runs in the background. The app loop keeps
     * reading UART; the next ak_statuslcd_flush() waits at entry if needed. */
}

uint32_t ak_statuslcd_frame_gen(void)
{
    return s_frame_gen;
}

bool ak_statuslcd_flush_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    if (!s_ready || s_part == NULL || w == 0U || h == 0U || (uint32_t)x + w > AK_STATUSLCD_W ||
        (uint32_t)y + h > AK_STATUSLCD_H || (uint32_t)w * h > AK_STATUSLCD_RECT_MAX_PX) {
        return false;
    }
    /* The panel takes one transfer at a time; a partial waits like a full flush
     * (a 32 x 36 px area takes about 0.5 ms at 48 MHz). */
    if (!ak_statuslcd_flush_wait()) {
        return false;
    }
    /* Copy the rows out of the frame: the DMA needs one contiguous run. The
     * frame keeps the drawn pixels, so the next full flush shows the same. */
    for (uint16_t row = 0U; row < h; row++) {
        memcpy(s_part->frame + (uint32_t)row * w * 2U,
               s_frame->frame + ((uint32_t)(y + row) * AK_STATUSLCD_W + x) * 2U, (size_t)w * 2U);
    }
    s_part->x_start = x;
    s_part->y_start = y;
    s_part->width   = w;
    s_part->height  = h;
    s_part->len     = (uint32_t)w * h * 2U;
    SCB_CleanDCache_by_Addr((uint32_t *)s_part->frame, (int32_t)s_part->len);

    s_flush_done    = false;
    s_part->free_cb = ak_statuslcd_on_flush_done;
    s_busy          = s_part;
    /* The SDK keeps the last window it set and changes it only on a change,
     * so the next full frame (0,0,320,240) restores the full window itself. */
    OPERATE_RET rt = tdl_disp_dev_flush(s_disp, s_part);
    if (rt != OPRT_OK) {
        s_part->free_cb = NULL;
        s_busy          = NULL;
        s_flush_done    = true;
        PR_ERR("status LCD partial flush enqueue failed: %d", rt);
        return false;
    }
    return true;
}

void ak_statuslcd_title(const char *title, uint16_t bar_color, uint16_t fg)
{
    if (!s_ready) {
        return;
    }
    ak_gfx_fill_rect(0, 0, AK_STATUSLCD_W, TITLE_H, bar_color);
    (void)ak_gfx_text_box(LCD_M, 0U, TITLE_H, title == NULL ? "" : title, fg, bar_color);
    ak_gfx_fill_rect(0, TITLE_H, AK_STATUSLCD_W, (uint16_t)(AK_STATUSLCD_H - TITLE_H), AK_UI_BG);
}

/* Draw a segmented battery gauge with the percentage drawn over the body.
 * Geometry is in header (TITLE_H=22) coordinates. The far-right pixel of the
 * terminal sits at x_right. */
static void draw_battery(uint16_t x_right, uint8_t pct, uint16_t bar_color)
{
    if (pct > 100U) {
        pct = 100U;
    }
    const uint16_t body_w = 30U;  /* battery body width  */
    const uint16_t body_h = 14U;  /* battery body height */
    const uint16_t term_w = 3U;   /* positive terminal nub width  */
    const uint16_t term_h = 6U;   /* positive terminal nub height */
    const uint16_t body_y = (uint16_t)((TITLE_H - body_h) / 2U); /* =4 */
    const uint16_t term_x = (uint16_t)(x_right - term_w);
    const uint16_t body_x = (uint16_t)(term_x - body_w);

    /* Fill colour by charge level: White, functional red below 20 %. */
    bool     low     = pct < 20U;
    uint16_t fill    = low ? AK_UI_BAT_LOW : AK_UI_BAT_FILL;
    uint16_t fill_fg = low ? AK_UI_BAT_LOW_FG : AK_UI_BAT_FILL_FG;

    /* Outer shell: a 1 px border, black interior. */
    ak_gfx_fill_rect(body_x, body_y, body_w, body_h, AK_UI_BAT_SHELL);
    ak_gfx_fill_rect((uint16_t)(body_x + 1U), (uint16_t)(body_y + 1U), (uint16_t)(body_w - 2U), (uint16_t)(body_h - 2U),
                     AK_UI_BAT_EMPTY);
    /* Positive terminal nub on the right, vertically centred. */
    ak_gfx_fill_rect(term_x, (uint16_t)(body_y + (body_h - term_h) / 2U), term_w, term_h, AK_UI_BAT_SHELL);

    /* Interior fill proportional to charge (2 px inset inside the border). */
    const uint16_t in_x = (uint16_t)(body_x + 2U);
    const uint16_t in_y = (uint16_t)(body_y + 2U);
    const uint16_t in_w = (uint16_t)(body_w - 4U);
    const uint16_t in_h = (uint16_t)(body_h - 4U);
    uint16_t fill_w = (uint16_t)(((uint32_t)in_w * pct) / 100U);
    if (fill_w > 0U) {
        ak_gfx_fill_rect(in_x, in_y, fill_w, in_h, fill);
    }

    /* Percentage drawn over the body with the thin font. Right-justify so 100
     * and single digits both sit inside the body. Each thin glyph is 8 px wide;
     * use the interior fill colour as the glyph background where it overlaps so
     * the number reads as sitting "in" the battery. */
    char text[4];
    (void)snprintf(text, sizeof(text), "%u", (unsigned)pct);
    uint16_t digits = (uint16_t)strlen(text);
    uint16_t text_w = (uint16_t)(digits * 8U);
    uint16_t text_x = (uint16_t)(body_x + (body_w - text_w) / 2U);
    uint16_t text_y = (uint16_t)(body_y + (body_h - 8U) / 2U);
    for (uint16_t i = 0; i < digits; i++) {
        uint16_t gx = (uint16_t)(text_x + i * 8U);
        /* Background = the fill colour if this glyph cell is over the filled
         * part, else the empty interior. */
        bool over_fill = (uint16_t)(gx + 4U) < (uint16_t)(in_x + fill_w);
        uint16_t bg        = over_fill ? fill : AK_UI_BAT_EMPTY;
        uint16_t fg        = over_fill ? fill_fg : AK_UI_BAT_DIGIT;
        ak_gfx_char_thin(gx, text_y, text[i], fg, bg, 1);
    }
}

void ak_statuslcd_header(const char *title, uint16_t bar_color, uint16_t fg, bool wifi_connected, bool usb_connected,
                         bool ble_connected, uint8_t battery_pct, bool battery_valid)
{
    const uint16_t icon_y = (uint16_t)((TITLE_H - AK_ICON_H) / 2U);

    ak_statuslcd_title(title, bar_color, fg);
    if (!s_ready) {
        return;
    }

    if (battery_valid) {
        /* Gauge terminal at the far right edge; link icons shifted left of it. */
        draw_battery((uint16_t)(AK_STATUSLCD_W - 2U), battery_pct, bar_color);
        ak_gfx_icon(214U, icon_y, wifi_connected ? AK_ICON_WIFI : AK_ICON_NO_WIFI, AK_ICON_W, AK_ICON_H, fg, bar_color);
        ak_gfx_icon(234U, icon_y, usb_connected ? AK_ICON_USB : AK_ICON_NO_USB, AK_ICON_W, AK_ICON_H, fg, bar_color);
        ak_gfx_icon(254U, icon_y, ble_connected ? AK_ICON_BLUETOOTH : AK_ICON_NO_BLUETOOTH, AK_ICON_W, AK_ICON_H, fg,
                    bar_color);
    } else {
        /* No gauge: keep the original right-aligned icon layout. */
        ak_gfx_icon(256U, icon_y, wifi_connected ? AK_ICON_WIFI : AK_ICON_NO_WIFI, AK_ICON_W, AK_ICON_H, fg, bar_color);
        ak_gfx_icon(276U, icon_y, usb_connected ? AK_ICON_USB : AK_ICON_NO_USB, AK_ICON_W, AK_ICON_H, fg, bar_color);
        ak_gfx_icon(296U, icon_y, ble_connected ? AK_ICON_BLUETOOTH : AK_ICON_NO_BLUETOOTH, AK_ICON_W, AK_ICON_H, fg,
                    bar_color);
    }
}
