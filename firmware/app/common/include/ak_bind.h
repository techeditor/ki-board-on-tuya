/**
 * @file ak_bind.h
 * @brief Settings > Bind session flow (shared/protocol/protocol.md
 *        "绑定会话"), pure logic. Times are milliseconds from a free-running
 *        uint32 clock passed in by the caller (wrap-safe).
 *
 *   OFF --open--> PICK (slot page: K1-K3 blue)
 *   PICK --K1..K3--> WAIT (bind_session {slot} sent; the Companion answers
 *        with an instance / session menu or bind_done)
 *   PICK --EC11 click / 10 s idle--> OFF (back to the settings page)
 *   WAIT --bind menu opened--> MENU --menu_result sent--> WAIT
 *   WAIT/MENU --bind_done bound|unbound--> OFF + the slot key flashes green
 *   WAIT/MENU --bind_done back|error--> PICK;  --closed--> OFF
 *   WAIT --5 s without a menu or bind_done--> PICK (no reply)
 *
 * The Companion owns the lists and the binding; the keyboard owns the slot
 * page, its timeouts and the lights.
 */

#ifndef AK_BIND_H
#define AK_BIND_H

#include <stdbool.h>
#include <stdint.h>

#define AK_BIND_SLOTS    3U
#define AK_BIND_IDLE_MS  10000U /* slot page without input -> back to settings */
#define AK_BIND_REPLY_MS 5000U  /* no menu / bind_done after a request */
#define AK_BIND_FLASH_MS 1500U  /* the bound slot key flashes green */

typedef enum {
    AK_BIND_OFF = 0,
    AK_BIND_PICK, /* ① slot page on screen */
    AK_BIND_WAIT, /* waiting for the Companion (menu or bind_done) */
    AK_BIND_MENU, /* an instance / session menu is open */
} ak_bind_state_t;

typedef enum {
    AK_BIND_DO_NONE = 0,
    AK_BIND_DO_REQUEST,  /* send bind_session {slot} */
    AK_BIND_DO_SETTINGS, /* leave the flow: show the settings page */
    AK_BIND_DO_PICK,     /* show the slot page (again) */
    AK_BIND_DO_NO_REPLY, /* WAIT timed out: show the slot page + "no reply" */
} ak_bind_do_t;

typedef enum {
    AK_BIND_RESULT_BOUND = 0,
    AK_BIND_RESULT_UNBOUND,
    AK_BIND_RESULT_BACK,
    AK_BIND_RESULT_CLOSED,
    AK_BIND_RESULT_ERROR,
    AK_BIND_RESULT_INVALID, /* unknown wire value: the frame is dropped */
} ak_bind_result_t;

typedef struct {
    ak_bind_state_t state;
    uint8_t         slot;     /* 1..3 while WAIT / MENU */
    uint32_t        since_ms; /* PICK: last input; WAIT: request / last answer */
    uint8_t         flash_slot; /* 1..3 flashing green, 0 none */
    uint32_t        flash_ms;
} ak_bind_t;

void ak_bind_init(ak_bind_t *b);

/** The settings item was clicked: show the slot page. */
void ak_bind_open(ak_bind_t *b, uint32_t now_ms);

/** True while the flow owns the keys (slot page or waiting for the Companion). */
bool ak_bind_active(const ak_bind_t *b);

/** Key press on the slot page (slot 0..5 = K1..K6, 6 = EC11 click). */
ak_bind_do_t ak_bind_key(ak_bind_t *b, int key, uint32_t now_ms);

/** An instance / session menu opened while waiting. True when it belongs to the flow. */
bool ak_bind_menu_opened(ak_bind_t *b, uint32_t now_ms);

/** The flow's menu sent its menu_result (an answer, ESC or timeout). */
void ak_bind_menu_answered(ak_bind_t *b, uint32_t now_ms);

/** Wire value of bind_done.result. */
ak_bind_result_t ak_bind_result_from(const char *s);

/** bind_done for `slot` (1..3). Ignored (NONE) outside the flow or for another slot. */
ak_bind_do_t ak_bind_done(ak_bind_t *b, uint8_t slot, ak_bind_result_t result, uint32_t now_ms);

/** Idle / reply timeouts and the end of the green flash. */
ak_bind_do_t ak_bind_tick(ak_bind_t *b, uint32_t now_ms);

/** Slot (1..3) whose key flashes green now, 0 none. Off-phase of the flash also returns 0. */
uint8_t ak_bind_flash_on(const ak_bind_t *b, uint32_t now_ms);

/** The flash is still running (on or off phase). */
bool ak_bind_flashing(const ak_bind_t *b, uint32_t now_ms);

#endif /* AK_BIND_H */
