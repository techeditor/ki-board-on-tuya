/**
 * @file ak_i18n.h
 * @brief Interface language (中文 / English) and the LCD string table (pure C).
 *
 * Every LCD text the firmware writes itself is an ak_str_id_t; ak_tr() returns
 * it in the current language. The language is one device-wide setting: the
 * Settings page, the Companion (device_settings "language") and the Chat-mode
 * agent hint all read it from here. Persisted by the caller (KV "ak_lang").
 *
 * Strings are UTF-8. Every non-ASCII character used here is pulled into the
 * UI font by tools/gen_font_ui.py, and the host tests check the coverage.
 */

#ifndef AK_I18N_H
#define AK_I18N_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    AK_LANG_ZH = 0,
    AK_LANG_EN,
    AK_LANG_COUNT,
} ak_lang_t;

#define AK_LANG_DEFAULT AK_LANG_ZH

typedef enum {
    /* Settings page */
    AK_STR_SET_TITLE = 0,
    AK_STR_SET_VOLUME,
    AK_STR_SET_BRIGHTNESS,
    AK_STR_SET_VOICE_MODE,
    AK_STR_SET_LED_POWER,
    AK_STR_SET_LED_TIMEOUT,
    AK_STR_SET_LANGUAGE,
    AK_STR_SET_CREDENTIALS,
    AK_STR_SET_PAIRING,
    AK_STR_SET_TEST,
    AK_STR_SET_EXIT,
    AK_STR_SET_APPLIED,
    AK_STR_SET_OPEN, /* pairing row value prefix: "OPEN 4:59" */
    AK_STR_SET_HINT_ADJUST,
    AK_STR_SET_HINT_COMMIT,
    AK_STR_SET_HINT_PAIRING,
    AK_STR_SET_HINT_PAIRING2,
    AK_STR_SET_HINT_SELECT,
    AK_STR_SET_HINT_CLICK,
    AK_STR_VOICE_ASR,
    AK_STR_VOICE_CHAT,
    AK_STR_LED_OFF,
    AK_STR_LED_ON,
    AK_STR_LED_AUTO,
    AK_STR_TIME_30S, /* LED auto-off values, same order as ak_led_policy */
    AK_STR_TIME_60S,
    AK_STR_TIME_5MIN,
    AK_STR_TIME_15MIN,
    /* Status bar notes after a settings commit */
    AK_STR_NOTE_CHAT,
    AK_STR_NOTE_ASR,
    AK_STR_NOTE_LEDS_OFF,
    AK_STR_NOTE_LEDS_ON,
    AK_STR_NOTE_LEDS_AUTO,
    AK_STR_NOTE_LANGUAGE, /* "语言：中文" / "Language: English" */
    /* Status bar, pages, dialogs and the LCD menu (FR-01 phase 2) */
    AK_STR_ST_HOLD_TO_SPEAK,
    AK_STR_ST_DICTATION_NEEDS_CLOUD,
    AK_STR_ST_READY,
    AK_STR_ST_INPUT_CANCELLED,
    AK_STR_ST_INPUT_CLEARED,
    AK_STR_ST_INPUT_NOT_SENT,
    AK_STR_ST_INPUT_CONFIRMED,
    AK_STR_ST_CHAT_NEEDS_CLOUD,
    AK_STR_ST_WIFI_CONNECTING,
    AK_STR_ST_WIFI_CONNECTED,
    AK_STR_ST_WIFI_FAILED,
    AK_STR_ST_WIFI_RESTORED,
    AK_STR_ST_WIFI_DISCONNECTED,
    AK_STR_ST_PAIR_WINDOW,
    AK_STR_ST_PAIR_WRITE_LICENSE,
    AK_STR_ST_REQUEST_WAITING,
    AK_STR_ST_PAIRING_TUYA_APP,
    AK_STR_ST_ASR_FALLBACK,
    AK_STR_ST_ASR_RESTORED,
    AK_STR_ST_ASR_READY,
    AK_STR_ST_PC_ASR,
    AK_STR_ST_SESSION,
    AK_STR_ST_STATUS,
    AK_STR_ST_SELECTED_SESSION,
    AK_STR_ST_LISTENING,
    AK_STR_ST_RECOGNIZING,
    AK_STR_ST_DICTATED_HINT,
    AK_STR_ST_CONFIRM_HINT,
    AK_STR_ST_NO_SPEECH,
    AK_STR_ST_ASR_FAILED,
    AK_STR_ST_ASR_TIMEOUT,
    AK_STR_ST_PAIRING_STORAGE,
    AK_STR_ST_PAIRING_RESTART,
    AK_STR_ST_BLE_PAIRED,
    AK_STR_ST_BLE_CLEARED,
    AK_STR_ST_LICENSE_EXISTS,
    AK_STR_ST_PAIRING_FIRST,
    AK_STR_ST_USB_CONNECTED,
    AK_STR_ST_USB_DISCONNECTED,
    AK_STR_ST_USB_TIMEOUT,
    AK_STR_ST_BLE_INIT_FAILED,
    AK_STR_ST_BLE_CONNECTED,
    AK_STR_ST_BLE_SUBSCRIBING,
    AK_STR_ST_BLE_ADVERTISING,
    AK_STR_ST_BLE_DISCONNECTED,
    AK_STR_ST_LICENSE_SAVED,
    AK_STR_ST_LICENSE_WRITE_FAILED,
    AK_STR_ST_LCD_READY,
    AK_STR_ST_CLOUD_STARTING,
    AK_STR_ST_CLOUD_OK,
    AK_STR_ST_CLOUD_FAILED,
    AK_STR_ST_ASR_STARTING,
    AK_STR_ST_ASR_OK,
    AK_STR_ST_ASR_INIT_FAILED,
    AK_STR_ST_WIFI_STARTING,
    AK_STR_ST_WIFI_INIT_FAILED,
    AK_STR_ST_BOOT_CHAT,
    AK_STR_ST_BOOT_ASR,
    AK_STR_ST_SCAN_QR,
    AK_STR_ST_ACTIVATING,
    AK_STR_ST_ACTIVATED,
    AK_STR_ST_CLOUD_READY,
    AK_STR_ST_CLOUD_DISCONNECTED,
    AK_STR_ST_RESETTING,
    AK_STR_ST_PROVISIONED,
    AK_STR_CHAT_YOU,
    AK_STR_CHAT_AI,
    AK_STR_CHAT_IDLE,
    AK_STR_CHAT_THINKING,
    AK_STR_CHAT_REPLYING,
    AK_STR_CHAT_TITLE,
    AK_STR_RST_TITLE,
    AK_STR_RST_L1,
    AK_STR_RST_L2,
    AK_STR_RST_L3,
    AK_STR_RST_L4,
    AK_STR_RST_KEYS,
    AK_STR_RST_TIMEOUT,
    AK_STR_CRED_PID,
    AK_STR_CRED_SET,
    AK_STR_CRED_NOT_SET,
    AK_STR_CRED_SOURCE,
    AK_STR_CRED_OVERWRITE_ON,
    AK_STR_CRED_OVERWRITE_K6,
    AK_STR_CRED_NEED_USB,
    AK_STR_CRED_NEED_USB2,
    AK_STR_CRED_BLE,
    AK_STR_CRED_BACK,
    AK_STR_CRED_UNPAIR_ARMED,
    AK_STR_CRED_UNPAIR,
    AK_STR_PAIR_TITLE,
    AK_STR_PAIR_L1,
    AK_STR_PAIR_L2,
    AK_STR_PAIR_L3,
    AK_STR_PAIR_L4,
    AK_STR_PAIR_CANCEL,
    AK_STR_PAIR_CONFIRM,
    AK_STR_PAIR_AUTO_CANCEL,
    AK_STR_PAIR_KEYS,
    AK_STR_PAIR_HINT,
    AK_STR_TEST_KEYS,
    AK_STR_TEST_BATTERY_WAIT,
    AK_STR_TEST_WIFI_WAIT,
    AK_STR_TEST_BLE_WAIT,
    AK_STR_TEST_HINT,
    AK_STR_TEST_EXIT,
    AK_STR_TEST_BATTERY_NA,
    AK_STR_TEST_BLE_NA,
    AK_STR_TEST_NO_SWITCH1,
    AK_STR_TEST_NO_SWITCH2,
    AK_STR_TEST_LP_KEYS,
    AK_STR_TEST_LP_HINT,
    AK_STR_FONT_TITLE,
    AK_STR_FONT_HINT,
    AK_STR_MENU_OPTIONS,
    AK_STR_MENU_APPROVE,
    AK_STR_MENU_QUESTION,
    AK_STR_MENU_MODEL,
    AK_STR_MENU_MODE,
    AK_STR_MENU_SEE_SCREEN,
    AK_STR_MENU_ARMED,
    AK_STR_MENU_HINT_ANSWER,
    AK_STR_MENU_HINT_MULTI,
    AK_STR_MENU_HINT_SINGLE,
    AK_STR_DICT_TITLE,
    AK_STR_DICT_HOLD,
    AK_STR_DICT_SPEAK,
    AK_STR_DICT_CONFIRM_HINT,
    AK_STR_DICT_BACK_HINT,
    AK_STR_INPUT_TITLE,
    AK_STR_INPUT_HINT,
    AK_STR_TEST_TITLE,
    /* Firmware update (system confirm ⑦, docs/ota.md) */
    AK_STR_OTA_TITLE,
    AK_STR_OTA_INSTALL,
    AK_STR_OTA_RUNNING,
    AK_STR_OTA_KEEP_USB,
    AK_STR_OTA_KEPT,
    AK_STR_OTA_KEYS,
    AK_STR_OTA_PREPARING,
    AK_STR_OTA_RECEIVING,
    AK_STR_OTA_INSTALLING,
    AK_STR_OTA_NO_UNPLUG,
    AK_STR_OTA_RESTART,
    AK_STR_OTA_FAILED,
    AK_STR_OTA_UPDATED,
    AK_STR_COUNT,
} ak_str_id_t;

ak_lang_t ak_lang_get(void);
void      ak_lang_set(ak_lang_t lang); /* out-of-range values are ignored */

/** @brief String in the current language; "" for an unknown id (never NULL). */
const char *ak_tr(ak_str_id_t id);
/** @brief String in a given language; "" for an unknown id or language. */
const char *ak_tr_lang(ak_str_id_t id, ak_lang_t lang);

/** @brief The language's own name, for the Settings value ("中文", "English"). */
const char *ak_lang_name(ak_lang_t lang);

/** @brief Wire / KV form: "zh" / "en". */
const char *ak_lang_wire_str(ak_lang_t lang);
/** @brief Parse "zh" / "en"; false (out untouched) for anything else. */
bool ak_lang_from_wire(const char *s, ak_lang_t *out);

/** @brief KV blob: one byte, 0 = zh, 1 = en. */
uint8_t ak_lang_to_byte(ak_lang_t lang);
bool    ak_lang_from_blob(const uint8_t *blob, size_t len, ak_lang_t *out);

/**
 * @brief JSON object for the Tuya agent's sessionAttributes "custom.param"
 *        (tuya_ai_agent_set_event_param), telling the cloud agent the reply
 *        language: {"lang":"zh","lang_name":"Simplified Chinese"}.
 */
const char *ak_lang_agent_param(ak_lang_t lang);

#endif /* AK_I18N_H */
