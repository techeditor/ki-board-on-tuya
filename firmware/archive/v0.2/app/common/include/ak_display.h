/**
 * @file ak_display.h
 * @brief ST7789 LCD bring-up: register the panel on the v1.2 wiring, open it,
 *        turn on the backlight and show a bring-up pattern.
 *
 * This is the decision-independent slice of the display work (Task 9): it proves
 * the SPI/pins/backlight/INVOFF path on real hardware. The LVGL 3-zone product
 * UI (status bar / Agent zone / key hints) is deferred until the interaction
 * design is finalised and the v1.2 board is in hand.
 */

#ifndef AK_DISPLAY_H
#define AK_DISPLAY_H

#include "tuya_cloud_types.h"

/** Register + open the ST7789, enable backlight, and draw the bring-up bars. */
OPERATE_RET ak_display_init(void);

/** Redraw the colour-bar bring-up pattern (SPI/orientation/INVOFF check). */
OPERATE_RET ak_display_show_bars(void);

/** Fill the whole screen with one RGB565 colour. */
OPERATE_RET ak_display_fill(uint16_t rgb565);

#endif /* AK_DISPLAY_H */
