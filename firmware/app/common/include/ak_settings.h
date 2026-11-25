#ifndef AK_SETTINGS_H
#define AK_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#define AK_SETTINGS_VOLUME_DEFAULT     70U
#define AK_SETTINGS_BRIGHTNESS_DEFAULT 100U
#define AK_SETTINGS_BRIGHTNESS_MIN     10U
#define AK_SETTINGS_VALUE_STEP         10U

typedef enum {
    AK_SETTINGS_ITEM_VOLUME = 0,
    AK_SETTINGS_ITEM_BRIGHTNESS,
    AK_SETTINGS_ITEM_VOICE_MODE,  /* value: ASR / Chat (applied on commit) */
    AK_SETTINGS_ITEM_LED_POWER,   /* value: Off / On / Auto (applied on commit) */
    AK_SETTINGS_ITEM_LED_TIMEOUT, /* value: Auto power-off delay (applied on commit) */
    AK_SETTINGS_ITEM_LANGUAGE,    /* value: 中文 / English (applied on commit, ak_i18n.h) */
    AK_SETTINGS_ITEM_FONT_SIZE,   /* value: 12 / 14 / 16 px (applied on commit, ak_text.h) */
    AK_SETTINGS_ITEM_ANIMATION,   /* value: chat ghost animation on / off (FR-14, applied on commit) */
    AK_SETTINGS_ITEM_SCENE,       /* FR-03: open the scene picker (shows the current scene) */
    AK_SETTINGS_ITEM_BIND_SESSION, /* sub flow: bind a Crew session to K1-K3 (ak_bind.h) */
    AK_SETTINGS_ITEM_WIFI,        /* sub page: remembered Wi-Fi networks (FR-15) */
    AK_SETTINGS_ITEM_DEVICE_INFO, /* read-only sub page: connection, Wi-Fi, Bluetooth, device (and the credentials SET / NOT SET) */
    AK_SETTINGS_ITEM_BLE_PAIR,    /* action: open / close the over-the-air BLE pairing window */
    AK_SETTINGS_ITEM_PAIRING, /* action: open / close the pairing window */
    AK_SETTINGS_ITEM_REBIND,  /* action: unbind from Tuya and re-pair (activated devices) */
    AK_SETTINGS_ITEM_FACTORY_RESET, /* action: Reset system (two confirms, ak_factory_reset.h) */
    AK_SETTINGS_ITEM_TEST,
    AK_SETTINGS_ITEM_EXIT,
    AK_SETTINGS_ITEM_COUNT,
} ak_settings_item_t;

typedef enum {
    AK_SETTINGS_ACTION_UP = 0,
    AK_SETTINGS_ACTION_DOWN,
    AK_SETTINGS_ACTION_LEFT,
    AK_SETTINGS_ACTION_RIGHT,
    AK_SETTINGS_ACTION_CONFIRM,
} ak_settings_action_t;

typedef enum {
    AK_SETTINGS_RESULT_NONE = 0,
    AK_SETTINGS_RESULT_CHANGED,
    AK_SETTINGS_RESULT_ENTER_EDIT,  /* a value item was selected for adjusting */
    AK_SETTINGS_RESULT_CONFIRMED,   /* edit finished / value committed */
    AK_SETTINGS_RESULT_ENTER_TEST,
    AK_SETTINGS_RESULT_PAIRING,     /* Pairing mode clicked */
    AK_SETTINGS_RESULT_REBIND,      /* Unbind & re-pair clicked */
    AK_SETTINGS_RESULT_FACTORY_RESET, /* Reset system clicked */
    AK_SETTINGS_RESULT_DEVICE_INFO, /* open the Device info page */
    AK_SETTINGS_RESULT_BLE_PAIR,    /* BLE pairing clicked */
    AK_SETTINGS_RESULT_WIFI,        /* open the Saved Wi-Fi page */
    AK_SETTINGS_RESULT_BIND_SESSION, /* open the Bind session slot page */
    AK_SETTINGS_RESULT_SCENE,        /* FR-03: open the scene picker */
    AK_SETTINGS_RESULT_EXIT,
} ak_settings_result_t;

typedef struct {
    ak_settings_item_t selected;
    bool               editing;   /* true while a value item is being adjusted */
    uint8_t volume;
    uint8_t brightness;
    bool    voice_chat; /* Voice mode row: false = ASR, true = Chat */
    uint8_t led_mode;        /* ak_led_mode_t: 0 Off, 1 On, 2 Auto */
    uint8_t led_timeout_idx; /* index into the Auto timeouts (ak_led_policy.h) */
    uint8_t lang;            /* ak_lang_t: 0 中文, 1 English; ak_settings_init copies ak_lang_get() */
    uint8_t font_size;       /* ak_text_size_t: 0 12 px, 1 14 px, 2 16 px; ak_settings_init copies ak_text_size_get() */
    bool    animation;       /* chat ghost animation (FR-14), default on; the app loads KV after init */
    bool    rebind_hidden;   /* "Unbind device" is not listed: nothing is bound yet (set from the cloud state) */
} ak_settings_model_t;

void ak_settings_init(ak_settings_model_t *model);
ak_settings_result_t ak_settings_handle(ak_settings_model_t *model,
                                        ak_settings_action_t action);

/** @brief Whether `item` is listed. Only Unbind device can be hidden (`rebind_hidden`). */
bool ak_settings_item_visible(const ak_settings_model_t *model, ak_settings_item_t item);

/** @brief Number of listed items. */
unsigned ak_settings_visible_count(const ak_settings_model_t *model);

/** @brief Row position of `item` among the listed items (a hidden item has none: returns
 *         ak_settings_visible_count()). Use it, not the enum value, to place rows. */
unsigned ak_settings_visible_pos(const ak_settings_model_t *model, ak_settings_item_t item);

/**
 * @brief Show or hide "Unbind device". Hiding while it is selected moves the cursor to the next
 *        listed item (Hardware test) and ends any editing. Returns true when the list changed.
 */
bool ak_settings_set_rebind_hidden(ak_settings_model_t *model, bool hidden);

/** @brief First entry shown when only `rows` fit: a list scrolls just enough to keep `selected` visible
 *         (the settings page and its Saved Wi-Fi page). */
unsigned ak_settings_first_row(unsigned selected, unsigned rows);

#endif /* AK_SETTINGS_H */
