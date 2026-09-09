/**
 * @file ak_notify_ui.h
 * @brief Lightweight LVGL notification bar for FP-1 (T4).
 *
 * Renders the newest notification from a notification_store_t as a bottom bar
 * (project + message + a "(N)" count when more than one is queued), and hides
 * itself when the store is empty.
 *
 * NOTE: This is a deliberately light LVGL-native bar, NOT a port of CodeTank's
 * canvas + pixel-font app_notification_ui.c (which is laid out for 480x320 and
 * is much heavier). It validates the same notification data -> display path
 * (R3.2/R3.3) with far less code and adapts cleanly to 320x240.
 *
 * All calls must hold the lv_vendor display lock (callers take it).
 */

#ifndef AK_NOTIFY_UI_H
#define AK_NOTIFY_UI_H

#include "tuya_cloud_types.h"
#include "app_notification.h"

/** Create the (initially hidden) notification bar on the active screen. */
OPERATE_RET ak_notify_ui_init(void);

/** Refresh the bar from the store: show newest + count, or hide if empty. */
void ak_notify_ui_update(const notification_store_t *store);

#endif /* AK_NOTIFY_UI_H */
