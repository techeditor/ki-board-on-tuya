#include "ak_settings.h"

#include <stdbool.h>
#include <stddef.h>

void ak_settings_init(ak_settings_model_t *model)
{
    if (model == NULL) {
        return;
    }
    model->selected   = AK_SETTINGS_ITEM_VOLUME;
    model->volume     = AK_SETTINGS_VOLUME_DEFAULT;
    model->brightness = AK_SETTINGS_BRIGHTNESS_DEFAULT;
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

ak_settings_result_t ak_settings_handle(ak_settings_model_t *model,
                                        ak_settings_action_t action)
{
    if (model == NULL) {
        return AK_SETTINGS_RESULT_NONE;
    }

    switch (action) {
    case AK_SETTINGS_ACTION_UP:
        model->selected = model->selected == AK_SETTINGS_ITEM_VOLUME
                              ? AK_SETTINGS_ITEM_EXIT
                              : (ak_settings_item_t)(model->selected - 1);
        return AK_SETTINGS_RESULT_NONE;
    case AK_SETTINGS_ACTION_DOWN:
        model->selected = (ak_settings_item_t)((model->selected + 1U) %
                                               AK_SETTINGS_ITEM_COUNT);
        return AK_SETTINGS_RESULT_NONE;
    case AK_SETTINGS_ACTION_LEFT:
    case AK_SETTINGS_ACTION_RIGHT: {
        bool increase = action == AK_SETTINGS_ACTION_RIGHT;
        uint8_t *value = NULL;
        uint8_t minimum = 0U;
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
    case AK_SETTINGS_ACTION_CONFIRM:
        return model->selected == AK_SETTINGS_ITEM_EXIT
                   ? AK_SETTINGS_RESULT_EXIT
                   : AK_SETTINGS_RESULT_CONFIRMED;
    default:
        return AK_SETTINGS_RESULT_NONE;
    }
}
