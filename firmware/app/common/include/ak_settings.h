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
    AK_SETTINGS_RESULT_EXIT,
} ak_settings_result_t;

typedef struct {
    ak_settings_item_t selected;
    bool               editing;   /* true while a value item is being adjusted */
    uint8_t volume;
    uint8_t brightness;
} ak_settings_model_t;

void ak_settings_init(ak_settings_model_t *model);
ak_settings_result_t ak_settings_handle(ak_settings_model_t *model,
                                        ak_settings_action_t action);

#endif /* AK_SETTINGS_H */
