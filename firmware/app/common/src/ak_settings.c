#include "ak_settings.h"

#include <stdbool.h>
#include <stddef.h>

#include "ak_i18n.h"
#include "ak_led_policy.h"
#include "ak_text.h"

void ak_settings_init(ak_settings_model_t *model)
{
    if (model == NULL) {
        return;
    }
    model->selected   = AK_SETTINGS_ITEM_VOLUME;
    model->editing    = false;
    model->volume     = AK_SETTINGS_VOLUME_DEFAULT;
    model->brightness = AK_SETTINGS_BRIGHTNESS_DEFAULT;
    model->voice_chat = false;
    model->led_mode        = (uint8_t)AK_LED_MODE_DEFAULT;
    model->led_timeout_idx = AK_LED_TIMEOUT_IDX_DEFAULT;
    /* The language is restored from KV before the settings model exists (it is
     * needed for the first LCD draw), so start from the live value. */
    model->lang            = (uint8_t)ak_lang_get();
    model->font_size       = (uint8_t)ak_text_size_get(); /* restored from KV before this, like the language */
}

unsigned ak_settings_first_row(unsigned selected, unsigned rows)
{
    return rows == 0U || selected < rows ? 0U : selected + 1U - rows;
}

static uint8_t ak_settings_adjust_value(uint8_t value, uint8_t minimum, bool increase)
{
    if (increase) {
        return value > (uint8_t)(100U - AK_SETTINGS_VALUE_STEP)
                   ? 100U
                   : (uint8_t)(value + AK_SETTINGS_VALUE_STEP);
    }
    return value < (uint8_t)(minimum + AK_SETTINGS_VALUE_STEP)
               ? minimum
               : (uint8_t)(value - AK_SETTINGS_VALUE_STEP);
}

/* Two-level single-knob model:
 *   - not editing: rotate moves the selection (UP/DOWN or LEFT/RIGHT all step
 *     the highlight); a short press (CONFIRM) on a VALUE item enters edit mode,
 *     on TEST/EXIT it executes directly.
 *   - editing: rotate adjusts the value (LEFT/RIGHT or UP/DOWN); a short press
 *     (CONFIRM) commits and leaves edit mode. The selection cannot move while
 *     editing, so the knob only changes the value until the user confirms. */
static bool item_is_value(ak_settings_item_t item)
{
    return item == AK_SETTINGS_ITEM_VOLUME || item == AK_SETTINGS_ITEM_BRIGHTNESS ||
           item == AK_SETTINGS_ITEM_VOICE_MODE || item == AK_SETTINGS_ITEM_LED_POWER ||
           item == AK_SETTINGS_ITEM_LED_TIMEOUT || item == AK_SETTINGS_ITEM_LANGUAGE ||
           item == AK_SETTINGS_ITEM_FONT_SIZE;
}

static ak_settings_result_t ak_settings_move(ak_settings_model_t *model, bool forward)
{
    if (forward) {
        model->selected = (ak_settings_item_t)((model->selected + 1U) %
                                               AK_SETTINGS_ITEM_COUNT);
    } else {
        model->selected = model->selected == AK_SETTINGS_ITEM_VOLUME
                              ? AK_SETTINGS_ITEM_EXIT
                              : (ak_settings_item_t)(model->selected - 1);
    }
    return AK_SETTINGS_RESULT_NONE;
}

static ak_settings_result_t ak_settings_adjust(ak_settings_model_t *model, bool increase)
{
    uint8_t *value = NULL;
    uint8_t minimum = 0U;
    if (model->selected == AK_SETTINGS_ITEM_VOICE_MODE) {
        /* Two values: any rotation flips between ASR and Chat. */
        (void)increase;
        model->voice_chat = !model->voice_chat;
        return AK_SETTINGS_RESULT_CHANGED;
    }
    if (model->selected == AK_SETTINGS_ITEM_LANGUAGE) {
        /* Two values, like the voice mode: any rotation flips 中文 / English. */
        (void)increase;
        model->lang = model->lang == (uint8_t)AK_LANG_EN ? (uint8_t)AK_LANG_ZH : (uint8_t)AK_LANG_EN;
        return AK_SETTINGS_RESULT_CHANGED;
    }
    if (model->selected == AK_SETTINGS_ITEM_FONT_SIZE) {
        /* 12 px -> 16 px, wrapping both ways (ak_text.h lists the sizes). */
        model->font_size = (uint8_t)ak_text_size_next((ak_text_size_t)model->font_size, increase);
        return AK_SETTINGS_RESULT_CHANGED;
    }
    if (model->selected == AK_SETTINGS_ITEM_LED_POWER) {
        /* Off -> On -> Auto, wrapping both ways. */
        uint8_t n       = (uint8_t)AK_LED_MODE_COUNT;
        model->led_mode = (uint8_t)((model->led_mode + (increase ? 1U : n - 1U)) % n);
        return AK_SETTINGS_RESULT_CHANGED;
    }
    if (model->selected == AK_SETTINGS_ITEM_LED_TIMEOUT) {
        /* 30 s .. 15 min, clamped at both ends like the percentages. */
        if (increase ? model->led_timeout_idx + 1U >= AK_LED_TIMEOUT_COUNT : model->led_timeout_idx == 0U) {
            return AK_SETTINGS_RESULT_NONE;
        }
        model->led_timeout_idx = (uint8_t)(increase ? model->led_timeout_idx + 1U : model->led_timeout_idx - 1U);
        return AK_SETTINGS_RESULT_CHANGED;
    }
    if (model->selected == AK_SETTINGS_ITEM_VOLUME) {
        value = &model->volume;
    } else if (model->selected == AK_SETTINGS_ITEM_BRIGHTNESS) {
        value = &model->brightness;
        minimum = AK_SETTINGS_BRIGHTNESS_MIN;
    }
    if (value == NULL) {
        return AK_SETTINGS_RESULT_NONE;
    }
    uint8_t adjusted = ak_settings_adjust_value(*value, minimum, increase);
    if (adjusted == *value) {
        return AK_SETTINGS_RESULT_NONE;
    }
    *value = adjusted;
    return AK_SETTINGS_RESULT_CHANGED;
}

ak_settings_result_t ak_settings_handle(ak_settings_model_t *model,
                                        ak_settings_action_t action)
{
    if (model == NULL) {
        return AK_SETTINGS_RESULT_NONE;
    }

    switch (action) {
    /* Rotation: adjust when editing a value, otherwise move the selection. */
    case AK_SETTINGS_ACTION_UP:
        return model->editing ? ak_settings_adjust(model, false)
                              : ak_settings_move(model, false);
    case AK_SETTINGS_ACTION_LEFT:
        return model->editing ? ak_settings_adjust(model, false)
                              : ak_settings_move(model, false);
    case AK_SETTINGS_ACTION_DOWN:
        return model->editing ? ak_settings_adjust(model, true)
                              : ak_settings_move(model, true);
    case AK_SETTINGS_ACTION_RIGHT:
        return model->editing ? ak_settings_adjust(model, true)
                              : ak_settings_move(model, true);

    /* Short press. */
    case AK_SETTINGS_ACTION_CONFIRM:
        if (model->editing) {
            /* Second click: commit the value and leave edit mode. */
            model->editing = false;
            return AK_SETTINGS_RESULT_CONFIRMED;
        }
        if (model->selected == AK_SETTINGS_ITEM_EXIT) {
            return AK_SETTINGS_RESULT_EXIT;
        }
        if (model->selected == AK_SETTINGS_ITEM_TEST) {
            return AK_SETTINGS_RESULT_ENTER_TEST;
        }
        if (model->selected == AK_SETTINGS_ITEM_PAIRING) {
            return AK_SETTINGS_RESULT_PAIRING;
        }
        if (model->selected == AK_SETTINGS_ITEM_REBIND) {
            return AK_SETTINGS_RESULT_REBIND;
        }
        if (model->selected == AK_SETTINGS_ITEM_BIND_SESSION) {
            return AK_SETTINGS_RESULT_BIND_SESSION;
        }
        if (model->selected == AK_SETTINGS_ITEM_WIFI) {
            return AK_SETTINGS_RESULT_WIFI;
        }
        if (model->selected == AK_SETTINGS_ITEM_CREDENTIALS) {
            return AK_SETTINGS_RESULT_CREDENTIALS;
        }
        if (item_is_value(model->selected)) {
            /* First click on a value item: enter edit mode. */
            model->editing = true;
            return AK_SETTINGS_RESULT_ENTER_EDIT;
        }
        return AK_SETTINGS_RESULT_CONFIRMED;
    default:
        return AK_SETTINGS_RESULT_NONE;
    }
}
