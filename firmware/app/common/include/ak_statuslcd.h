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
 * Orientation: MADCTL defaults to 0xA0 (landscape matching the rotation-270 that
 * was verified upright on the v1.2 expansion board). If the screen shows upside
 * down, flip AK_STATUSLCD_MADCTL to 0x60.
 */

#ifndef AK_STATUSLCD_H_
#define AK_STATUSLCD_H_

#include "ak_gfx.h"
#include "tuya_cloud_types.h"

#define AK_STATUSLCD_W 320U /* logical landscape width  */
#define AK_STATUSLCD_H 240U /* logical landscape height */

#ifndef AK_STATUSLCD_MADCTL
#define AK_STATUSLCD_MADCTL 0x60U /* MV|MX landscape (verified upright); 0xA0 = 180-flipped */
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

/** @brief Set the PWM backlight brightness, clamped to 0..100 percent. */
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

/** @brief Y coordinate where the content area starts (below the title bar). */
#define AK_STATUSLCD_CONTENT_Y 26U

#endif /* AK_STATUSLCD_H_ */
