/**
 * @file ak_menu.h
 * @brief LCD menu (menu / menu_close / notice / menu_parked -> menu_result)
 *        and the key lights that go with it, as pure logic. Spec:
 *        shared/protocol/protocol.md "LCD 菜单与按键灯光（v0.3）".
 *
 * Times are milliseconds from a free-running uint32 clock passed in by the
 * caller (wrap-safe), as in ak_led_policy. Keys are keymap slots: 0..5 =
 * K1..K6, 6 = the EC11 push (AK_MENU_KEY_ENC).
 *
 * The Companion owns content, queueing and priority; the keyboard owns the
 * key mapping, lights, double confirm and timeout:
 *
 *   IDLE --menu(new id)--> OPEN --key/timeout--> WAIT (result sent, keys back
 *        to the keymap, LCD shows "..." for up to 3 s) --menu_close/3 s--> IDLE
 *   OPEN --menu(other id)--> OPEN (replaced, no result for the old one)
 *   OPEN --menu_close(id)--> IDLE (no result)
 *
 * One layout for every menu: each page holds up to 4 items on K2..K5 (single
 * select: press = choose; multi: press = tick / untick). EC11 rotation moves
 * the highlight across pages, EC11 click acts on the highlighted item, K6
 * chooses the highlight (multi: submits), K1 = ESC. Approval menus ignore K6
 * and the EC11 click: only the item keys answer them.
 */

#ifndef AK_MENU_H
#define AK_MENU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Wire limits (bytes, excluding the terminator). */
#define AK_MENU_ID_MAX         15U
#define AK_MENU_TITLE_MAX      40U
#define AK_MENU_CTX_MAX        120U
#define AK_MENU_QUEUE_MAX      7U
#define AK_MENU_LABEL_MAX      24U
#define AK_MENU_CLOSE_MSG_MAX  32U
#define AK_MENU_NOTICE_MAX     48U
#define AK_MENU_PAGES_MAX      12U
#define AK_MENU_PAGE_ITEMS_MAX 4U /* one per K2..K5 */
#define AK_MENU_ITEMS_MAX      48U
#define AK_MENU_TIMEOUT_MAX_S  900U
#define AK_MENU_PARKED_MAX     99U
#define AK_MENU_TEXT_MAX       255U /* dictated answer in menu_result */

/* Timing from the spec. */
#define AK_MENU_GUARD_MS       300U  /* keys ignored right after a menu appears */
#define AK_MENU_CONFIRM2_MS    3000U /* double-confirm window */
#define AK_MENU_WAIT_MS        3000U /* "..." after a result, until menu_close */
#define AK_MENU_CLOSE_MSG_MS   2000U /* menu_close msg in the status bar */
#define AK_MENU_NOTICE_MS      3000U /* notice in the status bar */

/* User decision 2026-11-17: approval menus answer only through the key of
 * the item ("Approve once" ...); K6 / EC11 click do nothing there. */
#define AK_MENU_APPROVAL_ENTER 0

#define AK_MENU_KEY_COUNT 7
#define AK_MENU_KEY_ENC   6

typedef enum {
    AK_MENU_KIND_OPTIONS = 0, /* also every unknown kind */
    AK_MENU_KIND_APPROVAL,
    AK_MENU_KIND_QUESTION,
    AK_MENU_KIND_MODEL,
    AK_MENU_KIND_MODE,
    AK_MENU_KIND_INSTANCE, /* Bind session: pick the Crew instance (user-opened, like model) */
    AK_MENU_KIND_SESSION,  /* Bind session: pick the session (user-opened, like model) */
} ak_menu_kind_t;

typedef enum {
    AK_MENU_ROLE_NORMAL = 0, /* a selectable option; also every unknown role */
    AK_MENU_ROLE_CONFIRM,
    AK_MENU_ROLE_CANCEL,
    AK_MENU_ROLE_DANGER,
    AK_MENU_ROLE_ATTENTION,
    AK_MENU_ROLE_VOICE, /* "Dictate": answer by voice on the keyboard */
} ak_menu_role_t;

typedef struct {
    char    label[AK_MENU_LABEL_MAX + 1U];
    uint8_t role; /* ak_menu_role_t */
    bool    double_confirm;
    bool    checked;
} ak_menu_item_t;

/* One parsed `menu` frame (one page). */
typedef struct {
    char           id[AK_MENU_ID_MAX + 1U];
    ak_menu_kind_t kind;
    char           title[AK_MENU_TITLE_MAX + 1U];
    char           ctx[AK_MENU_CTX_MAX + 1U];
    bool           more; /* full content only on the PC screen */
    char           queue[AK_MENU_QUEUE_MAX + 1U];
    uint8_t        page;  /* 1-based */
    uint8_t        pages;
    bool           multi;
    uint8_t        def;   /* global default index */
    uint16_t       timeout_s;
    uint8_t        count;
    ak_menu_item_t items[AK_MENU_PAGE_ITEMS_MAX];
} ak_menu_page_t;

typedef enum {
    AK_NOTICE_INFO = 0,
    AK_NOTICE_WARN,
    AK_NOTICE_ERROR,
} ak_notice_level_t;

/** Parse a `menu` payload. False (frame must be dropped) on a bad id / kind /
 *  role / select, out-of-range numbers, missing fields or 0 / > 4 items.
 *  Display text is clipped at a UTF-8 boundary. */
bool ak_menu_parse(const char *payload, ak_menu_page_t *out);
/** Parse `menu_close` (`msg` optional, clipped). */
bool ak_menu_parse_close(const char *payload, char id[AK_MENU_ID_MAX + 1U], char msg[AK_MENU_CLOSE_MSG_MAX + 1U]);
/** Parse `notice` (`level` defaults to info). */
bool ak_menu_parse_notice(const char *payload, char text[AK_MENU_NOTICE_MAX + 1U], ak_notice_level_t *level);
/** Parse `menu_parked` (`count` 0..99). */
bool ak_menu_parse_parked(const char *payload, uint8_t *count);

typedef enum {
    AK_MENU_IDLE = 0,
    AK_MENU_OPEN,
    AK_MENU_WAIT,
} ak_menu_state_t;

typedef struct {
    ak_menu_kind_t kind;
    char           title[AK_MENU_TITLE_MAX + 1U];
    uint8_t        count; /* 0 = not arrived yet */
    ak_menu_item_t items[AK_MENU_PAGE_ITEMS_MAX];
} ak_menu_pg_t;

typedef struct {
    ak_menu_state_t state;
    char            id[AK_MENU_ID_MAX + 1U];
    char            ctx[AK_MENU_CTX_MAX + 1U];
    bool            more;
    char            queue[AK_MENU_QUEUE_MAX + 1U];
    uint8_t         pages;
    bool            multi;
    uint8_t         def;
    uint16_t        timeout_s;
    ak_menu_pg_t    pg[AK_MENU_PAGES_MAX];
    uint8_t         picked[AK_MENU_PAGES_MAX]; /* multi-select: item bits per page */
    uint8_t         cur_page; /* 0-based */
    uint8_t         cur_idx;
    bool            cursor_moved;
    uint32_t        opened_ms;
    uint32_t        guard_ms; /* when the menu last appeared */
    uint32_t        wait_ms;
    bool            hidden;   /* not on screen (queued behind another state) */
    uint8_t         pressed;  /* keys whose DOWN the menu accepted */
    uint8_t         consumed; /* ... whose release must do nothing */
    int8_t          armed_key; /* double confirm: key that armed it, -1 none */
    uint8_t         armed_page;
    uint8_t         armed_idx;
    uint32_t        armed_ms;
    bool            armed_again; /* the armed key is down a second time */
    int8_t          dictate_page; /* voice item chosen: its page, -1 none */
    uint8_t         dictate_idx;
} ak_menu_t;

typedef enum {
    AK_MENU_OUT_NONE = 0,
    AK_MENU_OUT_RESULT,  /* send menu_result selected[] (+ text) */
    AK_MENU_OUT_DISMISS, /* send menu_result dismissed + reason */
    AK_MENU_OUT_DICTATE, /* the voice item was chosen: start a dictated answer */
} ak_menu_out_kind_t;

typedef struct {
    ak_menu_out_kind_t kind;
    const char        *reason; /* DISMISS: "esc" / "timeout" */
    const char        *text;   /* RESULT of a dictated answer, else NULL */
    uint8_t            n;
    uint8_t            selected[AK_MENU_ITEMS_MAX];
    bool               redraw; /* LCD / lights changed */
} ak_menu_out_t;

typedef enum {
    AK_MENU_APPLY_REJECTED = 0, /* inconsistent with the open menu; dropped */
    AK_MENU_APPLY_OPENED,       /* a new menu is now open */
    AK_MENU_APPLY_UPDATED,      /* a page of the open menu arrived / changed */
    AK_MENU_APPLY_IGNORED,      /* page of a menu already answered */
} ak_menu_apply_t;

void ak_menu_init(ak_menu_t *m);

/** Apply one parsed page. */
ak_menu_apply_t ak_menu_on_page(ak_menu_t *m, const ak_menu_page_t *p, uint32_t now_ms);

/** menu_close: true when it closed the open menu or ended the WAIT for it. */
bool ak_menu_on_close(ak_menu_t *m, const char *id);

/** The menu owns K1..K6 + EC11 (open and on screen). */
bool ak_menu_owns_keys(const ak_menu_t *m);

/** Take the menu off / back on screen; showing again restarts the 300 ms guard. */
void ak_menu_set_hidden(ak_menu_t *m, bool hidden, uint32_t now_ms);

/** True for menus the user opened (state ⑤): model / mode, and the Bind
 *  session instance / session pickers. */
bool ak_menu_is_model(const ak_menu_t *m);

/** True for the Bind session pickers (kind instance / session). */
bool ak_menu_is_bind(const ak_menu_t *m);

/** Global index of (page, idx), or -1 while an earlier page is missing. */
int ak_menu_global_index(const ak_menu_t *m, uint8_t page, uint8_t idx);

/** Whether item (page, idx) shows a tick (multi: picked; single: `checked`). */
bool ak_menu_item_ticked(const ak_menu_t *m, uint8_t page, uint8_t idx);

/** Double confirm armed (LCD shows "Press again to confirm"). */
bool ak_menu_armed(const ak_menu_t *m);

/** K6 / EC11 click act on the highlight (false for approval menus). */
bool ak_menu_enter_allowed(const ak_menu_t *m);

/** Key edge (slot 0..6). */
void ak_menu_key(ak_menu_t *m, int key, bool down, uint32_t now_ms, ak_menu_out_t *out);

/** EC11 rotation (positive = clockwise = down), crossing pages. */
void ak_menu_encoder(ak_menu_t *m, int32_t detents, uint32_t now_ms, ak_menu_out_t *out);

/** Forget the presses of the keys in `mask` (bit = key index): their release
 *  selects nothing. Used when K1 + K6 become the recovery combo. */
void ak_menu_drop_keys(ak_menu_t *m, uint8_t mask);

/** Dictated answer confirmed with K6: menu_result for the voice item + text. */
void ak_menu_dictated(ak_menu_t *m, const char *text, uint32_t now_ms, ak_menu_out_t *out);

/** Dictation abandoned (K1): the menu is shown again. */
void ak_menu_dictate_cancel(ak_menu_t *m);

/** Timers: local timeout, double-confirm window, WAIT expiry. */
void ak_menu_tick(ak_menu_t *m, uint32_t now_ms, ak_menu_out_t *out);

/* ───────────────────────── Key lights (FR-09) ───────────────────────── */

#define AK_LIGHT_COUNT 7 /* LED1..LED6 under K1..K6, LED7 at the EC11 */

typedef enum {
    AK_LIGHT_OFF = 0,
    AK_LIGHT_RED,    /* cancel / delete / reject */
    AK_LIGHT_GREEN,  /* confirm */
    AK_LIGHT_BLUE,   /* selectable option / direction */
    AK_LIGHT_YELLOW, /* attention / double confirm / waiting */
    AK_LIGHT_PURPLE, /* voice / dictate */
} ak_light_t;

typedef struct {
    uint8_t r, g, b;
} ak_rgb_t;

#define AK_LIGHT_FULL_PCT    100U
#define AK_LIGHT_DIM_PCT     40U   /* items other than the highlight */
#define AK_LIGHT_FLASH_MS    250U  /* ~4 Hz: double confirm */
#define AK_LIGHT_BREATH_MS   2000U /* ~0.5 Hz: waiting */

ak_light_t ak_menu_role_light(uint8_t role);
ak_rgb_t   ak_light_rgb(ak_light_t light, uint8_t pct);

/** Lights while the menu owns the keys (only pressable keys lit). */
void ak_menu_lights(const ak_menu_t *m, uint32_t now_ms, ak_rgb_t out[AK_LIGHT_COUNT]);

/**
 * Lights outside a menu: K1..K3 breathe yellow for slots whose `display`
 * state is `waiting` (bit i = slot i); K4 breathes yellow while a request
 * waits for K4 (`k4_waiting`: ESC'd or queued); in the post-ASR input confirm
 * K4 breathes red and K6 blinks green instead (pc_ms = time since it began).
 */
void ak_session_lights(uint8_t waiting_mask, bool k4_waiting, bool input_confirm, uint32_t pc_ms, uint32_t now_ms,
                       ak_rgb_t out[AK_LIGHT_COUNT]);

#endif /* AK_MENU_H */
