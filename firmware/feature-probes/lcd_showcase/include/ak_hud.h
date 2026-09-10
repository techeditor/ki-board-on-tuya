/**
 * @file ak_hud.h
 * @brief Info-dense LVGL HUD for FP-lcd-showcase (320x240 landscape).
 *
 * Four zones (see design.md):
 *   - top status bar  : platform / mode / effort / connection / firmware
 *   - slots row       : K1..K3 agent slot cards (focused one highlighted)
 *   - context area    : focused-slot detail / last message / listening bar
 *   - bottom nav hints : 5-way direction meanings + K4/K5
 *
 * The corner mascot (ak_scene) and the notification bar (ak_notify_ui) are
 * separate LVGL objects owned by their own modules; ak_hud leaves the
 * bottom-right context corner and the very bottom strip free for them.
 *
 * ak_hud_init() builds the static skeleton once; ak_hud_render() refreshes the
 * dynamic text/colors from a showcase_view_t snapshot. All calls must hold the
 * lv_vendor display lock (callers take it).
 */

#ifndef AK_HUD_H
#define AK_HUD_H

#include "tuya_cloud_types.h"
#include "showcase_state.h"

/** Build the static HUD skeleton on the active screen. */
OPERATE_RET ak_hud_init(void);

/** Refresh all dynamic HUD content from the view snapshot. */
void ak_hud_render(const showcase_view_t *view);

#endif /* AK_HUD_H */
