/**
 * @file ak_scroll.h
 * @brief Line scrolling for LCD text longer than one screen (FR-17, pure C, host-tested).
 *
 * The view is the index of its first line. max_first is the first line of the
 * bottom-most full view (0 when everything fits). A view at the bottom
 * follows new content; a view scrolled up keeps its lines and flags `news`
 * until the user scrolls back down.
 */

#ifndef AK_SCROLL_H
#define AK_SCROLL_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t first;  /**< first visible line */
    uint16_t max;    /**< max_first of the last update */
    bool     follow; /**< at the bottom: new content scrolls into view */
    bool     news;   /**< content grew below while scrolled up */
} ak_scroll_t;

typedef enum {
    AK_SCROLL_NONE = 0, /**< nothing to scroll (the text fits) */
    AK_SCROLL_MOVED,
    AK_SCROLL_AT_TOP,    /**< turned up at the first line */
    AK_SCROLL_AT_BOTTOM, /**< turned down at the last line */
} ak_scroll_result_t;

/** @brief New text: start at the top, or at the bottom when `at_bottom` (a growing conversation). */
void ak_scroll_reset(ak_scroll_t *s, uint16_t max_first, bool at_bottom);

/** @brief The same text changed length: follow at the bottom, else keep the view (clamped) and flag growth. */
void ak_scroll_update(ak_scroll_t *s, uint16_t max_first);

/** @brief EC11 rotation; positive detents move towards the end of the text. */
ak_scroll_result_t ak_scroll_step(ak_scroll_t *s, int32_t detents);

/**
 * @brief Scroll bar thumb inside a track of `track` px: `shown` of `total`
 *        lines visible from `first`. The thumb is at least `min_h` px.
 * @return false (no bar) when everything fits.
 */
bool ak_scroll_bar(uint16_t total, uint16_t shown, uint16_t first, uint16_t track, uint16_t min_h, uint16_t *top,
                   uint16_t *h);

#endif /* AK_SCROLL_H */
