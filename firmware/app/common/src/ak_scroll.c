/**
 * @file ak_scroll.c
 * @brief Line scrolling for long LCD text (FR-17). See ak_scroll.h.
 */

#include "ak_scroll.h"

#include <stddef.h>

void ak_scroll_reset(ak_scroll_t *s, uint16_t max_first, bool at_bottom)
{
    if (s == NULL) {
        return;
    }
    s->max    = max_first;
    s->first  = at_bottom ? max_first : 0U;
    s->follow = s->first == max_first;
    s->news   = false;
}

void ak_scroll_update(ak_scroll_t *s, uint16_t max_first)
{
    if (s == NULL) {
        return;
    }
    bool grew = max_first > s->max;
    s->max    = max_first;
    if (s->follow || s->first >= max_first) {
        s->first  = max_first;
        s->follow = true;
        s->news   = false;
        return;
    }
    if (grew) {
        s->news = true;
    }
}

ak_scroll_result_t ak_scroll_step(ak_scroll_t *s, int32_t detents)
{
    if (s == NULL || s->max == 0U || detents == 0) {
        return AK_SCROLL_NONE;
    }
    int32_t next = (int32_t)s->first + detents;
    if (next < 0) {
        next = 0;
    }
    if (next > (int32_t)s->max) {
        next = (int32_t)s->max;
    }
    if ((uint16_t)next == s->first) {
        return detents < 0 ? AK_SCROLL_AT_TOP : AK_SCROLL_AT_BOTTOM;
    }
    s->first  = (uint16_t)next;
    s->follow = s->first == s->max;
    if (s->follow) {
        s->news = false;
    }
    return AK_SCROLL_MOVED;
}

bool ak_scroll_bar(uint16_t total, uint16_t shown, uint16_t first, uint16_t track, uint16_t min_h, uint16_t *top,
                   uint16_t *h)
{
    if (top == NULL || h == NULL || total == 0U || shown >= total || track == 0U) {
        return false;
    }
    uint32_t thumb = (uint32_t)track * shown / total;
    if (thumb < min_h) {
        thumb = min_h;
    }
    if (thumb > track) {
        thumb = track;
    }
    uint32_t range = (uint32_t)(total - shown);
    uint32_t at    = first > range ? range : first;
    *h             = (uint16_t)thumb;
    *top           = (uint16_t)((track - thumb) * at / range);
    return true;
}
