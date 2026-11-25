/**
 * @file ak_statuslcd.h
 * @brief Shared lightweight status LCD for test firmware (bring-up and earlier internal prototypes).
 *
 * Opens the CL28CK230-18A (ST7789) panel in landscape 320x240 via the raw TDD/TDL
 * path (no LVGL) and binds ak_gfx (8x8 font) to a PSRAM frame buffer. Every test
 * probe uses this to show operation prompts + live feedback on screen, so the
 * board can be judged directly without a serial monitor. UART JSONL stays as a
 * secondary channel.
 *
 * Orientation: MADCTL defaults to 0xA0 for the v0.3 12P T280C7-C12-26 (ST7789P3)
 * panel, verified upright on hardware 2026-10-09 (same value as the bringup
 * lcd_st7789 probe). The old v1.2/CL28CK230-18A board used 0x60; on the v0.3
 * panel 0x60 shows the screen 180-flipped. If upside down, flip to 0x60.
 */

#ifndef AK_STATUSLCD_H_
#define AK_STATUSLCD_H_

#include "ak_gfx.h"
#include "ak_layout.h"
#include "tuya_cloud_types.h"

#define AK_STATUSLCD_W AK_LAYOUT_W /* logical landscape width, 320 */
#define AK_STATUSLCD_H AK_LAYOUT_H /* logical landscape height, 240 */

#ifndef AK_STATUSLCD_MADCTL
#define AK_STATUSLCD_MADCTL 0xA0U /* v0.3 ST7789P3 landscape (verified upright 2026-10-09); 0x60 = 180-flipped */
#endif

/**
 * @brief Register + open the ST7789 panel (landscape 320x240), allocate the
 *        frame buffer, and bind ak_gfx to it. Call once at startup.
 * @return OPRT_OK on success; on failure ak_statuslcd_ready() stays false and
 *         the caller should fall back to UART-only output.
 */
OPERATE_RET ak_statuslcd_init(void);

/** @brief Whether the panel opened successfully and drawing is usable. */
bool ak_statuslcd_ready(void);

/** @brief Set safe software brightness while keeping the GPIO backlight on. */
OPERATE_RET ak_statuslcd_set_brightness(uint8_t brightness);

/**
 * @brief Enqueue the bound frame buffer to the panel and return immediately.
 *
 * The SPI DMA runs in the background. A subsequent ak_statuslcd_flush() waits
 * at entry for the prior DMA to finish before reusing the framebuffer, so the
 * app loop is never blocked right after a redraw (it keeps reading UART).
 */
void ak_statuslcd_flush(void);

/** @brief True while a flush DMA is still in flight (framebuffer owned by SPI). */
bool ak_statuslcd_flush_busy(void);

/**
 * @brief Block until the in-flight flush completes (bounded by the internal
 *        timeout). Returns true when the framebuffer is safe to reuse.
 */
bool ak_statuslcd_flush_wait(void);

/**
 * @brief Non-blocking reclaim of a completed flush. Call once per app-loop
 *        iteration so a finished DMA is released without the next render
 *        having to wait for it. Never sleeps.
 */
void ak_statuslcd_flush_poll(void);

/** Largest area ak_statuslcd_flush_rect sends (px); its buffer is 2 bytes per px. */
#define AK_STATUSLCD_RECT_MAX_PX 2048U

/**
 * @brief Send only the area (x, y, w, h) of the frame buffer to the panel (FR-14).
 *
 * Draw into the frame as usual first (ak_gfx_*), then call this instead of
 * ak_statuslcd_flush(). Waits for a DMA still in flight, copies the area to a
 * small buffer and returns while it transfers. Does not count as a full frame.
 * @return false when the area is too large / outside the panel, the partial
 *         buffer is missing or the panel is not usable (nothing sent).
 */
bool ak_statuslcd_flush_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/**
 * @brief Number of full-frame flushes so far. A screen that records it after
 *        its own flush can tell later whether anything else has been drawn
 *        over it (the value changed).
 */
uint32_t ak_statuslcd_frame_gen(void);

/**
 * @brief Draw a top title bar (full width, height 22) with 1x text, and clear
 *        the content area below it. Does NOT flush.
 */
void ak_statuslcd_title(const char *title, uint16_t bar_color, uint16_t fg);

/**
 * @brief Draw the title bar, the WiFi/USB/BLE link icons (connected: normal
 *        icon, else the slashed one) and, when battery_valid, a battery gauge
 *        at the far right with the percentage drawn over it. Title and icons
 *        use @p fg on @p bar_color (ak_palette.h). Does NOT flush.
 *
 * @param battery_pct   0..100 charge estimate (clamped).
 * @param battery_valid Whether to draw the gauge at all.
 */
void ak_statuslcd_header(const char *title, uint16_t bar_color, uint16_t fg, bool wifi_connected, bool usb_connected,
                         bool ble_connected, uint8_t battery_pct, bool battery_valid);

/** @brief Y coordinate where the content area starts (below the title bar). */
#define AK_STATUSLCD_CONTENT_Y 26U

#endif /* AK_STATUSLCD_H_ */
