/**
 * @file ak_panel.h
 * @brief Register the CL28CK230-18A (ST7789) panel for LVGL on v1.2 wiring.
 *
 * agent_display feature-probe (FP-1). Unlike firmware/app/ak_display.c (which opens the
 * panel and draws directly via TDL), this only REGISTERS the panel with the TDD
 * layer under AK_PANEL_NAME. The LVGL porting layer (lv_vendor_init) then finds
 * it by name and takes ownership of open + double draw buffers + rotation.
 *
 * Landscape 320x240 is achieved by registering with TUYA_DISPLAY_ROTATION_90;
 * LVGL reads the rotation from the TDD device info and rotates in software.
 */

#ifndef AK_PANEL_H
#define AK_PANEL_H

#include "tuya_cloud_types.h"

/* Must match CONFIG_DISPLAY_NAME in the .config so lv_vendor_init finds it. */
#define AK_PANEL_NAME "ak_lcd"

/** Register the ST7789 panel (INVOFF init seq, rotation 90). No open/flush. */
OPERATE_RET ak_panel_register(void);

#endif /* AK_PANEL_H */
