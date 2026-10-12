/**
 * @file ak_statuslcd.h
 * @brief Shared lightweight status LCD for test firmware (feature-probes + bringup).
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
#include "tuya_cloud_types.h"

#define AK_STATUSLCD_W 320U /* logical landscape width  */
#define AK_STATUSLCD_H 240U /* logical landscape height */

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

/** @brief Push the bound frame buffer to the panel. */
void ak_statuslcd_flush(void);

/**
 * @brief Draw a top title bar (full width, height 22) with 1x text, and clear
 *        the content area below it to black. Does NOT flush.
 * @param title     Bar text (drawn black).
 * @param bar_color Bar background color.
 */
void ak_statuslcd_title(const char *title, uint16_t bar_color);

/**
 * @brief Draw the title bar plus 16x16 WiFi, USB and BLE link icons at right.
 *
 * Connected links use their normal icon; disconnected links use the matching
 * slashed icon. All link icons use a black foreground for contrast on the
 * colored header. Icon order is WiFi, USB, BLE. Does NOT flush.
 */
void ak_statuslcd_title_links(const char *title, uint16_t bar_color, bool wifi_connected,
                              bool usb_connected, bool ble_connected);

/**
 * @brief Draw the title bar, the WiFi/USB/BLE link icons, AND a segmented
 *        battery gauge at the far right with the percentage drawn over it.
 *
 * Same as ak_statuslcd_title_links() but the three link icons are shifted left
 * to make room for the gauge. Pass battery_valid=false (e.g. ADC unavailable)
 * to omit the gauge and fall back to the plain icon layout. Does NOT flush.
 *
 * @param battery_pct   0..100 charge estimate (clamped).
 * @param battery_valid Whether to draw the gauge at all.
 */
void ak_statuslcd_header(const char *title, uint16_t bar_color, bool wifi_connected,
                         bool usb_connected, bool ble_connected,
                         uint8_t battery_pct, bool battery_valid);

/** @brief Y coordinate where the content area starts (below the title bar). */
#define AK_STATUSLCD_CONTENT_Y 26U

#endif /* AK_STATUSLCD_H_ */
