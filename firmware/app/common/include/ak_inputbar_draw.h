/**
 * @file ak_inputbar_draw.h
 * @brief Draws the voice input strip (ak_inputbar.h) on the home page.
 */

#ifndef AK_INPUTBAR_DRAW_H
#define AK_INPUTBAR_DRAW_H

#include "ak_scroll.h"

/**
 * Draw the strip for `text` at ak_inputbar_top(): a rounded frame, 2 or 3
 * text rows (the end of the text, or the view `scroll` holds), a scroll bar
 * when the text is longer. Updates `scroll` for the new text length.
 */
void ak_inputbar_draw(const char *text, ak_scroll_t *scroll);

#endif /* AK_INPUTBAR_DRAW_H */
