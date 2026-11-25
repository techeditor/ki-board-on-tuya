/**
 * @file ak_ui.c
 * @brief Keyboard work state machine (pure). See ak_ui.h.
 */

#include "ak_ui.h"

#include <string.h>

void ak_ui_init(ak_ui_t *ui)
{
    memset(ui, 0, sizeof(*ui));
    ui->base = AK_UI_STANDBY;
}

ak_ui_state_t ak_ui_state(const ak_ui_t *ui)
{
    if (ui->system) {
        return AK_UI_SYSTEM;
    }
    if (ui->settings) {
        return AK_UI_SETTINGS;
    }
    return ui->base;
}

const char *ak_ui_state_str(ak_ui_state_t s)
{
    static const char *const names[AK_UI_STATE_COUNT] = {
        "standby", "voice", "confirm", "menu", "model_menu", "settings", "system",
    };
    return (unsigned)s < AK_UI_STATE_COUNT ? names[s] : "?";
}

bool ak_ui_menu_visible(const ak_ui_t *ui)
{
    ak_ui_state_t s = ak_ui_state(ui);
    return s == AK_UI_MENU || s == AK_UI_MODEL_MENU;
}

bool ak_ui_pump(ak_ui_t *ui)
{
    if (!ui->menu_waiting || ui->base != AK_UI_STANDBY) {
        return false;
    }
    ui->menu_waiting = false;
    ui->base         = ui->menu_model ? AK_UI_MODEL_MENU : AK_UI_MENU;
    return !ui->settings && !ui->system;
}

bool ak_ui_set_modals(ak_ui_t *ui, bool settings, bool system)
{
    bool changed = ui->settings != settings || ui->system != system;
    ui->settings = settings;
    ui->system   = system;
    return changed;
}

void ak_ui_menu_arrived(ak_ui_t *ui, bool model)
{
    ui->menu_model = model;
    if (ui->base == AK_UI_MENU || ui->base == AK_UI_MODEL_MENU || ui->dictating) {
        /* Replaces the open one (also the one being answered by voice). */
        ui->dictating = false;
        ui->base      = model ? AK_UI_MODEL_MENU : AK_UI_MENU;
        return;
    }
    ui->menu_waiting = true;
    (void)ak_ui_pump(ui);
}

void ak_ui_menu_gone(ak_ui_t *ui)
{
    ui->menu_waiting = false;
    if (ui->base == AK_UI_MENU || ui->base == AK_UI_MODEL_MENU) {
        ui->base = AK_UI_STANDBY;
    } else if (ui->dictating) {
        ui->dictating = false; /* answered elsewhere while dictating */
        ui->base      = AK_UI_STANDBY;
    }
}

void ak_ui_dictate(ak_ui_t *ui)
{
    if (ui->base == AK_UI_MENU) {
        ui->base      = AK_UI_VOICE;
        ui->dictating = true;
    }
}

void ak_ui_dictate_back(ak_ui_t *ui)
{
    if (ui->dictating && (ui->base == AK_UI_VOICE || ui->base == AK_UI_CONFIRM)) {
        ui->dictating = false;
        ui->base      = AK_UI_MENU;
    }
}

bool ak_ui_voice_start(ak_ui_t *ui)
{
    switch (ui->base) {
    case AK_UI_STANDBY:
    case AK_UI_CONFIRM: /* speaking again replaces the text */
        ui->base = AK_UI_VOICE;
        return true;
    case AK_UI_VOICE:
        if (ui->pc_wait) {
            ui->pc_wait = false; /* speaking again while the PC recognises */
            return true;
        }
        return ui->dictating; /* waiting for the dictated answer */
    default:
        return false; /* in a menu K5 is an item key */
    }
}

void ak_ui_voice_end(ak_ui_t *ui, ak_ui_voice_t result, bool chat, uint32_t now_ms)
{
    if (ui->base != AK_UI_VOICE) {
        return;
    }
    ui->pc_wait = false;
    (void)chat; /* the strip is the firmware's choice: Chat mode never sends text here */
    if (result == AK_UI_VOICE_TEXT && ui->dictating) {
        ui->base       = AK_UI_CONFIRM; /* a dictated answer is confirmed on its own page */
        ui->confirm_ms = now_ms;
        return;
    }
    if (!ui->dictating) {
        ui->base = AK_UI_STANDBY; /* a dictated answer stays in ② to retry */
        (void)ak_ui_pump(ui);
    }
}

bool ak_ui_pc_wait(ak_ui_t *ui)
{
    if (ui->base != AK_UI_VOICE || ui->dictating) {
        return false;
    }
    ui->pc_wait = true;
    return true;
}

bool ak_ui_input_restore(ak_ui_t *ui, uint32_t now_ms)
{
    (void)now_ms;
    if (ui->dictating) {
        return false;
    }
    if (ui->base == AK_UI_VOICE && ui->pc_wait) {
        ui->pc_wait = false;
        ui->base    = AK_UI_STANDBY;
        (void)ak_ui_pump(ui);
        return true;
    }
    return ui->base == AK_UI_STANDBY;
}

void ak_ui_set_bar(ak_ui_t *ui, bool open)
{
    ui->bar = open;
}

bool ak_utf8_pop_last(char *s)
{
    size_t n = strlen(s);
    if (n == 0U) {
        return false;
    }
    /* Step back over continuation bytes (10xxxxxx) to the lead byte. */
    do {
        n--;
    } while (n > 0U && ((unsigned char)s[n] & 0xC0U) == 0x80U);
    s[n] = '\0';
    return true;
}

void ak_ui_confirm_done(ak_ui_t *ui)
{
    if (ui->base == AK_UI_CONFIRM) {
        ui->base      = AK_UI_STANDBY;
        ui->dictating = false;
        (void)ak_ui_pump(ui);
    }
}

void ak_ui_voice_abort(ak_ui_t *ui)
{
    ui->pc_wait = false;
    if (ui->dictating) {
        ak_ui_dictate_back(ui);
    } else if (ui->base == AK_UI_VOICE || ui->base == AK_UI_CONFIRM) {
        ui->base = AK_UI_STANDBY;
        (void)ak_ui_pump(ui);
    }
}

bool ak_ui_tick(ak_ui_t *ui, uint32_t now_ms)
{
    if (ui->base != AK_UI_CONFIRM || (uint32_t)(now_ms - ui->confirm_ms) < AK_UI_CONFIRM_TIMEOUT_MS) {
        return false;
    }
    if (ui->dictating) {
        ak_ui_dictate_back(ui); /* an unsent dictated answer returns to the menu */
    } else {
        ak_ui_confirm_done(ui);
    }
    return true;
}

bool ak_ui_k4_waiting(const ak_ui_t *ui, uint8_t parked)
{
    ak_ui_state_t s = ak_ui_state(ui);
    return (s == AK_UI_STANDBY || s == AK_UI_VOICE) && !ui->bar && (ui->menu_waiting || parked > 0U);
}

bool ak_ui_k4_recalls(const ak_ui_t *ui, uint8_t parked)
{
    return ak_ui_state(ui) == AK_UI_STANDBY && !ui->bar && !ui->menu_waiting && parked > 0U;
}
