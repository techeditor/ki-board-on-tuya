/**
 * @file ak_font_ui.h
 * @brief LCD interface fonts, one per font size (Settings > Font size).
 *
 * Replaceable data: a host-side generator rebuilds each table from any pixel
 * font; nothing else changes. ak_text_font() (ak_text.h) picks the one in use.
 *   ak_font_ui12  Fusion Pixel 12px 1bpp (6x12 ASCII, 12x12 CJK), Tongyong
 *                 level 1 + GB2312 level 1 + UI strings (ak_font_ui12_data.c)
 *   ak_font_ui14  Ki-board Pixel 14px 1bpp (7x14 ASCII, 14x14 CJK), same set as
 *                 12 px (ak_font_ui14_data.c; third_party/fonts/ki-board-pixel-14)
 *   ak_font_ui16  GNU Unifont 16px 1bpp (8x16 ASCII, 16x16 CJK), Tongyong
 *                 level 1 + CJK punctuation + UI strings (ak_font_ui16_data.c)
 * Characters outside a set draw as the 0x7F box.
 */

#ifndef AK_FONT_UI_H
#define AK_FONT_UI_H

#include "ak_font.h"

extern const ak_font_t ak_font_ui12;
extern const ak_font_t ak_font_ui14;
extern const ak_font_t ak_font_ui16;

#endif /* AK_FONT_UI_H */
