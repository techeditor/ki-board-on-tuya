/**
 * @file ak_font_data.h
 * @brief Subset fonts for the on-device font test (generated in ak_font_data.c).
 */

#ifndef AK_FONT_DATA_H
#define AK_FONT_DATA_H

#include "ak_font.h"

#define AK_FONT_TEST_COUNT   4U
#define AK_FONT_SAMPLE_LINES 5U

/** Candidates in test order, all 1bpp: Fusion Pixel 12 (current), Unifont 16, Chill Bitmap 16, ISAS Song 16. */
extern const ak_font_t *const ak_font_test_set[AK_FONT_TEST_COUNT];

/** Same sample text for every candidate (UTF-8). */
extern const char *const ak_font_sample[AK_FONT_SAMPLE_LINES];

#endif /* AK_FONT_DATA_H */
