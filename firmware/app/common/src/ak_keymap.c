/**
 * @file ak_keymap.c
 * @brief Runtime key-role table + v0.3 defaults (see ak_keymap.h). Pure logic,
 *        host-testable; no SDK deps.
 */

#include "ak_keymap.h"

#include <string.h>

/* Slot order matches the keymap entries[] layout: k1..k6 then enc. */
static const char *const s_slot_ids[AK_KEYMAP_SLOTS] = {
    "k1", "k2", "k3", "k4", "k5", "k6", "enc",
};

int ak_keymap_slot_for_id(const char *id)
{
    if (id == NULL) {
        return -1;
    }
    for (int i = 0; i < AK_KEYMAP_SLOTS; i++) {
        if (strcmp(id, s_slot_ids[i]) == 0) {
            return i;
        }
    }
    return -1;
}

const char *ak_keymap_id_for_slot(int slot)
{
    if (slot < 0 || slot >= AK_KEYMAP_SLOTS) {
        return NULL;
    }
    return s_slot_ids[slot];
}

void ak_keymap_init_defaults(ak_keymap_t *map)
{
    if (map == NULL) {
        return;
    }
    /* K1-K3: chat focus (single) / instance switch (double) -> deferred class. */
    map->entries[0] = (ak_key_entry_t){AK_KEY_ROLE_CHAT, AK_KEY_GESTURE_SINGLE_DOUBLE};
    map->entries[1] = (ak_key_entry_t){AK_KEY_ROLE_CHAT, AK_KEY_GESTURE_SINGLE_DOUBLE};
    map->entries[2] = (ak_key_entry_t){AK_KEY_ROLE_CHAT, AK_KEY_GESTURE_SINGLE_DOUBLE};
    /* K4: cancel (click=del char / long=del all) -> immediate edges. */
    map->entries[3] = (ak_key_entry_t){AK_KEY_ROLE_CANCEL, AK_KEY_GESTURE_EDGE};
    /* K5: voice push-to-talk -> immediate edges (DOWN starts recording). */
    map->entries[4] = (ak_key_entry_t){AK_KEY_ROLE_VOICE, AK_KEY_GESTURE_EDGE};
    /* K6: confirm -> immediate edges. */
    map->entries[5] = (ak_key_entry_t){AK_KEY_ROLE_CONFIRM, AK_KEY_GESTURE_EDGE};
    /* enc (EC11 push): local UI (settings/test) handled elsewhere; report only. */
    map->entries[6] = (ak_key_entry_t){AK_KEY_ROLE_NONE, AK_KEY_GESTURE_EDGE};
}

static const ak_key_entry_t *ak_keymap_entry(const ak_keymap_t *map, const char *id)
{
    int slot = ak_keymap_slot_for_id(id);
    if (map == NULL || slot < 0) {
        return NULL;
    }
    return &map->entries[slot];
}

ak_key_role_t ak_keymap_role(const ak_keymap_t *map, const char *id)
{
    const ak_key_entry_t *e = ak_keymap_entry(map, id);
    return e ? e->role : AK_KEY_ROLE_NONE;
}

ak_key_gesture_mode_t ak_keymap_gesture(const ak_keymap_t *map, const char *id)
{
    const ak_key_entry_t *e = ak_keymap_entry(map, id);
    return e ? e->gesture : AK_KEY_GESTURE_EDGE;
}

bool ak_keymap_role_from_str(const char *s, ak_key_role_t *out)
{
    if (s == NULL || out == NULL) {
        return false;
    }
    if (strcmp(s, "none") == 0) {
        *out = AK_KEY_ROLE_NONE;
    } else if (strcmp(s, "chat") == 0) {
        *out = AK_KEY_ROLE_CHAT;
    } else if (strcmp(s, "voice") == 0) {
        *out = AK_KEY_ROLE_VOICE;
    } else if (strcmp(s, "cancel") == 0) {
        *out = AK_KEY_ROLE_CANCEL;
    } else if (strcmp(s, "confirm") == 0) {
        *out = AK_KEY_ROLE_CONFIRM;
    } else {
        return false; /* unknown -> caller keeps current role */
    }
    return true;
}

ak_key_gesture_mode_t ak_keymap_gesture_for_role(ak_key_role_t role)
{
    /* CHAT is the only role whose single and double actions differ, so it needs
     * deferred classification; everything else reacts on immediate edges. */
    return (role == AK_KEY_ROLE_CHAT) ? AK_KEY_GESTURE_SINGLE_DOUBLE
                                      : AK_KEY_GESTURE_EDGE;
}

bool ak_keymap_apply_role(ak_keymap_t *map, const char *id, ak_key_role_t role)
{
    int slot = ak_keymap_slot_for_id(id);
    if (map == NULL || slot < 0) {
        return false;
    }
    map->entries[slot].role    = role;
    map->entries[slot].gesture = ak_keymap_gesture_for_role(role);
    return true;
}

unsigned ak_keymap_to_blob(const ak_keymap_t *map, uint8_t *buf, unsigned buf_size)
{
    if (map == NULL || buf == NULL || buf_size < AK_KEYMAP_BLOB_SIZE) {
        return 0;
    }
    buf[0] = (uint8_t)AK_KEYMAP_BLOB_VERSION;
    for (int i = 0; i < AK_KEYMAP_SLOTS; i++) {
        buf[1 + i] = (uint8_t)map->entries[i].role;
    }
    return AK_KEYMAP_BLOB_SIZE;
}

bool ak_keymap_from_blob(ak_keymap_t *map, const uint8_t *buf, unsigned len)
{
    if (map == NULL || buf == NULL || len != AK_KEYMAP_BLOB_SIZE) {
        return false;
    }
    if (buf[0] != (uint8_t)AK_KEYMAP_BLOB_VERSION) {
        return false; /* unknown version -> fall back to defaults */
    }
    /* Validate every role byte BEFORE mutating map, so a corrupt blob never
     * leaves a half-applied table. */
    ak_keymap_t tmp;
    for (int i = 0; i < AK_KEYMAP_SLOTS; i++) {
        ak_key_role_t role = (ak_key_role_t)buf[1 + i];
        if (role > AK_KEY_ROLE_CONFIRM) {
            return false; /* out-of-range role -> reject whole blob */
        }
        tmp.entries[i].role    = role;
        tmp.entries[i].gesture = ak_keymap_gesture_for_role(role);
    }
    *map = tmp;
    return true;
}
