/**
 * @file ak_fonttest.h
 * @brief Hardware-test "Font" view: one sample text drawn with a candidate font.
 */

#ifndef AK_FONTTEST_H
#define AK_FONTTEST_H

/** @brief Number of candidate fonts (EC11 steps through them, wrapping). */
unsigned ak_fonttest_count(void);

/** @brief Name of candidate @p idx (wraps). */
const char *ak_fonttest_name(int idx);

/** @brief Redraw the whole font page for candidate @p idx and flush it. */
void ak_fonttest_draw(int idx);

#endif /* AK_FONTTEST_H */
