/**
 * @file ak_factory_reset.c
 * @brief Settings > Reset system: two confirms and the KV clear list. See ak_factory_reset.h.
 */

#include "ak_factory_reset.h"

#include <string.h>

static bool before(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(deadline_ms - now_ms) > 0;
}

void ak_freset_init(ak_freset_t *f)
{
    f->stage       = AK_FRESET_IDLE;
    f->deadline_ms = 0U;
}

bool ak_freset_request(ak_freset_t *f, uint32_t now_ms)
{
    if (ak_freset_stage(f, now_ms) != AK_FRESET_IDLE) {
        return false;
    }
    f->stage       = AK_FRESET_ASK1;
    f->deadline_ms = now_ms + AK_FRESET_CONFIRM_MS;
    return true;
}

ak_freset_stage_t ak_freset_stage(ak_freset_t *f, uint32_t now_ms)
{
    if (f->stage != AK_FRESET_IDLE && !before(now_ms, f->deadline_ms)) {
        ak_freset_init(f); /* unanswered = cancel */
    }
    return f->stage;
}

ak_freset_result_t ak_freset_resolve(ak_freset_t *f, bool confirm, uint32_t now_ms)
{
    ak_freset_stage_t stage = ak_freset_stage(f, now_ms);
    if (stage == AK_FRESET_IDLE) {
        return AK_FRESET_NONE;
    }
    if (!confirm) {
        ak_freset_init(f);
        return AK_FRESET_CANCELLED;
    }
    if (stage == AK_FRESET_ASK1) {
        f->stage       = AK_FRESET_ASK2;
        f->deadline_ms = now_ms + AK_FRESET_CONFIRM_MS; /* the second dialog gets its own time */
        return AK_FRESET_NEXT;
    }
    ak_freset_init(f);
    return AK_FRESET_RUN;
}

void ak_freset_cancel(ak_freset_t *f)
{
    ak_freset_init(f);
}

/* Every KV key that matters for a reset, firmware and SDK. Order: user
 * settings, pairings, network, then markers. Keys on the keep list are listed
 * too, so the plan documents itself (docs: factory-reset report). */
static const ak_freset_key_t k_plan[] = {
    /* User settings: back to the built-in defaults on the next boot. */
    {"ak_vmode", true, "voice mode (ASR / Chat)"},
    {"ak_lang", true, "LCD language"},
    {"ak_fontsz", true, "LCD font size"},
    {"ak_ghost", true, "chat ghost animation"},
    {"ak_ledpwr", true, "LED power and auto-off delay"},
    {"ak_keymap", true, "key map (K1-K6 / knob bindings, session binds)"},
    {"ak_scnact", true, "active scene"},
    {"ak_scn0", true, "scene slot 0"},
    {"ak_scn1", true, "scene slot 1"},
    {"ak_scn2", true, "scene slot 2"},
    {"ak_scn3", true, "scene slot 3"},
    {"ak_scn4", true, "scene slot 4"},
    /* Pairings. */
    {"ak_ble_key", true, "AK BLE pairing key (Companion over Bluetooth)"},
    /* Network. */
    {"ak_wifis", true, "saved Wi-Fi networks (FR-15)"},
    {"netinfo", true, "Wi-Fi network the SDK connects to"},
    {"region", true, "Tuya cloud region (SDK endpoint)"},
    {"regist_key", true, "Tuya cloud register key (SDK endpoint)"},
    {"endpoint.cert", true, "Tuya cloud endpoint certificate (SDK)"},
    {"endpoint.domain", true, "Tuya cloud endpoint domains (SDK)"},
    {"rcs.active", true, "Tuya register-center config learned at activation (SDK reset deletes it too)"},
    /* SDK AI chat: volume and chat mode (JSON). */
    {"ty_ai_chat_par", true, "speaker volume and AI chat mode (SDK ai_chat_main)"},
    /* Tuya License: after the reset the board is a new board without one; it
     * is written again over USB (K6 overwrite) or by reflashing. */
    {"UUID_TUYAOPEN", true, "Tuya License UUID (write again to activate)"},
    {"AUTHKEY_TUYAOPEN", true, "Tuya License AuthKey (write again to activate)"},
    {"ak_lic_bak", true, "previous License kept by a confirmed overwrite"},
    /* Product ID written over USB: the next boot falls back to the build-time
     * PID; with none (public builds) the Tuya cloud stays off until the USB
     * `pid` command writes one again. The only way to clear it. */
    {"ak_pid", true, "Tuya product ID written over USB (falls back to the build-time PID)"},
    /* Markers: no pairing window and no fast-boot count carried over. */
    {"ak_prov_rb", true, "open-the-pairing-window-at-boot marker"},
    {"rst_cnt", true, "three-fast-boots counter"},
    /* Kept. */
    {"ak_ota_hi", false, "highest firmware version ever run: anti-downgrade guard"},
    {"ak_ota_pend", false, "OTA version waiting for its first boot (reset is refused during OTA)"},
    {"clt_cert_mf", false, "Tuya manufacturing client certificate (factory)"},
    {"clt_private_key_mf", false, "Tuya manufacturing client key (factory)"},
    {"rcs.mf", false, "Tuya register-center config from manufacturing (factory)"},
};

const ak_freset_key_t *ak_freset_plan(size_t *count)
{
    if (count != NULL) {
        *count = sizeof(k_plan) / sizeof(k_plan[0]);
    }
    return k_plan;
}

/* Never deleted, whatever the plan says: product and factory data that a
 * user cannot write back, and the anti-downgrade guard. */
static const char *const k_protected[] = {
    "clt_cert_mf", "clt_private_key_mf", "rcs.mf", "ak_ota_hi",
};

bool ak_freset_key_kept(const char *key)
{
    if (key == NULL) {
        return true;
    }
    for (size_t i = 0U; i < sizeof(k_protected) / sizeof(k_protected[0]); i++) {
        if (strcmp(k_protected[i], key) == 0) {
            return true;
        }
    }
    for (size_t i = 0U; i < sizeof(k_plan) / sizeof(k_plan[0]); i++) {
        if (!k_plan[i].clear && strcmp(k_plan[i].key, key) == 0) {
            return true;
        }
    }
    return false;
}

size_t ak_freset_wipe(ak_freset_del_fn del, void *ctx)
{
    size_t failed = 0U;
    if (del == NULL) {
        return sizeof(k_plan) / sizeof(k_plan[0]);
    }
    for (size_t i = 0U; i < sizeof(k_plan) / sizeof(k_plan[0]); i++) {
        /* The keep check runs on every key, so a factory key added to the
         * clear list by mistake is still never deleted. */
        if (!k_plan[i].clear || ak_freset_key_kept(k_plan[i].key)) {
            continue;
        }
        if (del(k_plan[i].key, ctx) != 0) {
            failed++;
        }
    }
    return failed;
}
