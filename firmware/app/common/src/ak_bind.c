/**
 * @file ak_bind.c
 * @brief Settings > Bind session flow (pure). See ak_bind.h.
 */

#include "ak_bind.h"

#include <stddef.h>
#include <string.h>

#define AK_BIND_FLASH_HALF_MS 250U /* ~2 Hz blink: 250 ms on, 250 ms off */

void ak_bind_init(ak_bind_t *b)
{
    memset(b, 0, sizeof(*b));
}

void ak_bind_open(ak_bind_t *b, uint32_t now_ms)
{
    b->state    = AK_BIND_PICK;
    b->slot     = 0U;
    b->since_ms = now_ms;
}

bool ak_bind_active(const ak_bind_t *b)
{
    return b->state != AK_BIND_OFF;
}

ak_bind_do_t ak_bind_key(ak_bind_t *b, int key, uint32_t now_ms)
{
    if (b->state != AK_BIND_PICK) {
        return AK_BIND_DO_NONE;
    }
    b->since_ms = now_ms;
    if (key >= 0 && key < (int)AK_BIND_SLOTS) {
        b->state = AK_BIND_WAIT;
        b->slot  = (uint8_t)(key + 1);
        return AK_BIND_DO_REQUEST;
    }
    if (key == 6) { /* EC11 click: back to the settings page */
        b->state = AK_BIND_OFF;
        return AK_BIND_DO_SETTINGS;
    }
    return AK_BIND_DO_NONE; /* K4-K6 do nothing but count as activity */
}

bool ak_bind_menu_opened(ak_bind_t *b, uint32_t now_ms)
{
    if (b->state != AK_BIND_WAIT && b->state != AK_BIND_MENU) {
        return false;
    }
    b->state    = AK_BIND_MENU;
    b->since_ms = now_ms;
    return true;
}

void ak_bind_menu_answered(ak_bind_t *b, uint32_t now_ms)
{
    if (b->state == AK_BIND_MENU) {
        b->state    = AK_BIND_WAIT;
        b->since_ms = now_ms;
    }
}

ak_bind_result_t ak_bind_result_from(const char *s)
{
    static const char *const names[] = {"bound", "unbound", "back", "closed", "error"};
    for (size_t i = 0; s != NULL && i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(s, names[i]) == 0) {
            return (ak_bind_result_t)i;
        }
    }
    return AK_BIND_RESULT_INVALID;
}

ak_bind_do_t ak_bind_done(ak_bind_t *b, uint8_t slot, ak_bind_result_t result, uint32_t now_ms)
{
    if ((b->state != AK_BIND_WAIT && b->state != AK_BIND_MENU) || slot != b->slot) {
        return AK_BIND_DO_NONE;
    }
    switch (result) {
    case AK_BIND_RESULT_BOUND:
    case AK_BIND_RESULT_UNBOUND:
        b->flash_slot = slot;
        b->flash_ms   = now_ms;
        b->state      = AK_BIND_OFF;
        return AK_BIND_DO_SETTINGS;
    case AK_BIND_RESULT_CLOSED:
        b->state = AK_BIND_OFF;
        return AK_BIND_DO_SETTINGS;
    case AK_BIND_RESULT_BACK:
    case AK_BIND_RESULT_ERROR:
        ak_bind_open(b, now_ms);
        return AK_BIND_DO_PICK;
    default:
        return AK_BIND_DO_NONE;
    }
}

ak_bind_do_t ak_bind_tick(ak_bind_t *b, uint32_t now_ms)
{
    if (b->flash_slot != 0U && (uint32_t)(now_ms - b->flash_ms) >= AK_BIND_FLASH_MS) {
        b->flash_slot = 0U;
    }
    uint32_t idle = (uint32_t)(now_ms - b->since_ms);
    if (b->state == AK_BIND_PICK && idle >= AK_BIND_IDLE_MS) {
        b->state = AK_BIND_OFF;
        return AK_BIND_DO_SETTINGS;
    }
    if (b->state == AK_BIND_WAIT && idle >= AK_BIND_REPLY_MS) {
        ak_bind_open(b, now_ms);
        return AK_BIND_DO_NO_REPLY;
    }
    return AK_BIND_DO_NONE;
}

bool ak_bind_flashing(const ak_bind_t *b, uint32_t now_ms)
{
    return b->flash_slot != 0U && (uint32_t)(now_ms - b->flash_ms) < AK_BIND_FLASH_MS;
}

uint8_t ak_bind_flash_on(const ak_bind_t *b, uint32_t now_ms)
{
    if (!ak_bind_flashing(b, now_ms)) {
        return 0U;
    }
    return (((uint32_t)(now_ms - b->flash_ms) / AK_BIND_FLASH_HALF_MS) & 1U) == 0U ? b->flash_slot : 0U;
}
