#include "ak_settings.h"

#include <stdbool.h>
#include <stddef.h>

void ak_settings_init(ak_settings_model_t *model)
{
    if (model == NULL) {
        return;
    }
    model->selected   = AK_SETTINGS_ITEM_VOLUME;
    model->editing    = false;
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

/* Two-level single-knob model:
 *   - not editing: rotate moves the selection (UP/DOWN or LEFT/RIGHT all step
 *     the highlight); a short press (CONFIRM) on a VALUE item enters edit mode,
 *     on TEST/EXIT it executes directly.
 *   - editing: rotate adjusts the value (LEFT/RIGHT or UP/DOWN); a short press
 *     (CONFIRM) commits and leaves edit mode. The selection cannot move while
 *     editing, so the knob only changes the value until the user confirms. */
static bool item_is_value(ak_settings_item_t item)
{
    return item == AK_SETTINGS_ITEM_VOLUME || item == AK_SETTINGS_ITEM_BRIGHTNESS;
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
