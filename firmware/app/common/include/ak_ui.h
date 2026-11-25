/**
 * @file ak_ui.h
 * @brief Keyboard work state machine (shared/protocol/protocol.md "工作状态机"),
 *        pure logic. Times are milliseconds from a free-running uint32 clock
 *        passed in by the caller (wrap-safe).
 *
 * Base states ①..⑤ change through the events below. ⑥ settings / test and
 * ⑦ system confirm are overlays fed in by ak_ui_set_modals(): the settings
 * page and ak_provision own them, this module only ranks them
 * (⑦ > ⑥ > base). A menu that arrives while the keyboard is not in ① is held
 * (menu_waiting) and shown once it is back in ①; it never takes the screen
 * from ②/③/⑥/⑦.
 */

#ifndef AK_UI_H
#define AK_UI_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    AK_UI_STANDBY = 0, /* ① keymap active */
    AK_UI_VOICE,       /* ② voice input (held / recognising; or a dictated answer) */
    AK_UI_CONFIRM,     /* ③ a dictated menu answer awaiting K6 (voice input text lives in the ① strip) */
    AK_UI_MENU,        /* ④ approval / question / reply options */
    AK_UI_MODEL_MENU,  /* ⑤ model / mode menu (opened by the user) */
    AK_UI_SETTINGS,    /* ⑥ settings / hardware test */
    AK_UI_SYSTEM,      /* ⑦ system confirm: pairing window, reset network, ... */
    AK_UI_STATE_COUNT,
} ak_ui_state_t;

/* ③ without an answer: an unsent dictated answer returns to its menu. */
#define AK_UI_CONFIRM_TIMEOUT_MS 20000U

typedef enum {
    AK_UI_VOICE_NONE = 0, /* no recognition (short press, PC voice input finished) */
    AK_UI_VOICE_TEXT,     /* text recognised */
    AK_UI_VOICE_ERROR,    /* nothing heard / failed / timed out */
} ak_ui_voice_t;

typedef struct {
    ak_ui_state_t base; /* ①..⑤ */
    bool          settings;
    bool          system;
    bool          menu_waiting; /* a menu arrived and is not on screen yet */
    bool          menu_model;   /* ... that menu is a model / mode menu */
    bool          dictating;    /* ②/③ answer the open menu's voice item */
    bool          pc_wait;      /* ② after the voice key: waiting for the PC ASR text */
    bool          bar;          /* ① the voice input strip holds text (or a PC fill awaits K6 / K4) */
    uint32_t      confirm_ms;   /* ③ entered at */
} ak_ui_t;

void ak_ui_init(ak_ui_t *ui);

/** Effective state: ⑦ > ⑥ > base. */
ak_ui_state_t ak_ui_state(const ak_ui_t *ui);
const char   *ak_ui_state_str(ak_ui_state_t s);

/** A menu is on screen (④ or ⑤). */
bool ak_ui_menu_visible(const ak_ui_t *ui);

/** Overlays from the settings page / provisioning. True when they changed. */
bool ak_ui_set_modals(ak_ui_t *ui, bool settings, bool system);

/** A new menu arrived (Companion ranks requests; the newest one wins). */
void ak_ui_menu_arrived(ak_ui_t *ui, bool model);

/** The menu is gone (answered, ESC, menu_close, timeout). */
void ak_ui_menu_gone(ak_ui_t *ui);

/** Show a held menu if the keyboard is back in ①. True when it appeared. */
bool ak_ui_pump(ak_ui_t *ui);

/** ④: the voice item was chosen -> ② for a dictated answer. */
void ak_ui_dictate(ak_ui_t *ui);

/** K1 during a dictated answer: back to the menu (④). */
void ak_ui_dictate_back(ak_ui_t *ui);

/** Voice key pressed. True when voice input may start (①, ② dictating, ③). */
bool ak_ui_voice_start(ak_ui_t *ui);

/** Voice input finished. `chat` = Chat voice mode (no input confirm). */
void ak_ui_voice_end(ak_ui_t *ui, ak_ui_voice_t result, bool chat, uint32_t now_ms);

/** System route, voice key released, and the Companion said it returns
 *  the PC ASR text (`hello` `pc_asr`): stay in ② until `input_restore`.
 *  True when now waiting (② and not a dictated answer). */
bool ak_ui_pc_wait(ak_ui_t *ui);

/** Companion `input_restore`: ends a PC ASR wait in ② (back to ①). True when
 *  the keyboard is in ① afterwards; the text itself goes to the strip in any
 *  state, it is the caller's buffer. */
bool ak_ui_input_restore(ak_ui_t *ui, uint32_t now_ms);

/** The input strip has text (K4 / K6 act on it; K4 gives up its recall hint). */
void ak_ui_set_bar(ak_ui_t *ui, bool open);

/** Delete the last UTF-8 character of `s` in place (K4 in ③). True when one
 *  was removed. */
bool ak_utf8_pop_last(char *s);

/** ③ finished (confirm, cancel all, dictated answer sent) -> ①. */
void ak_ui_confirm_done(ak_ui_t *ui);

/** Leave ②/③ without an answer (settings opened from ③, voice mode change). */
void ak_ui_voice_abort(ak_ui_t *ui);

/** ③ timeout. True when the state changed. */
bool ak_ui_tick(ak_ui_t *ui, uint32_t now_ms);

/** K4 breathes yellow: a request waits (held menu, or `parked` ESC'd ones),
 *  shown in ① and ② (③ keeps its own K4 hint). */
bool ak_ui_k4_waiting(const ak_ui_t *ui, uint8_t parked);

/** ①: K4 single recalls ESC'd requests. */
bool ak_ui_k4_recalls(const ak_ui_t *ui, uint8_t parked);

#endif /* AK_UI_H */
