/**
 * @file ak_keymap.c
 * @brief Per-gesture key binding table + v0.3 defaults (see ak_keymap.h, stage
 *        乙). Pure logic, host-testable; no SDK deps.
 */

#include "ak_keymap.h"

#include <string.h>

/* Slot order matches the keymap entries[] layout: k1..k6 then enc. */
static const char *const s_slot_ids[AK_KEYMAP_SLOTS] = {
    "k1", "k2", "k3", "k4", "k5", "k6", "enc",
};

static const char *const s_gesture_names[AK_GESTURE_SLOT_COUNT] = {
    "click", "double", "long",
};

/* ---- action encode / decode ---- */

ak_action_t ak_action_chat(uint8_t n)
{
    return (n >= 1u && n <= 9u) ? (ak_action_t)(AK_ACTION_CHAT_BASE + n) : AK_ACTION_NONE;
}

ak_action_t ak_action_instance(uint8_t n)
{
    return (n >= 1u && n <= 9u) ? (ak_action_t)(AK_ACTION_INSTANCE_BASE + n) : AK_ACTION_NONE;
}

uint8_t ak_action_chat_n(ak_action_t a)
{
    uint8_t n = (uint8_t)(a - AK_ACTION_CHAT_BASE);
    return (a >= AK_ACTION_CHAT_BASE + 1u && a <= AK_ACTION_CHAT_BASE + 9u) ? n : 0u;
}

uint8_t ak_action_instance_n(ak_action_t a)
{
    uint8_t n = (uint8_t)(a - AK_ACTION_INSTANCE_BASE);
    return (a >= AK_ACTION_INSTANCE_BASE + 1u && a <= AK_ACTION_INSTANCE_BASE + 9u) ? n : 0u;
}

bool ak_action_valid(ak_action_t a)
{
    switch (a) {
    case AK_ACTION_NONE:
    case AK_ACTION_CANCEL_CHAR:
    case AK_ACTION_CANCEL_ALL:
    case AK_ACTION_CONFIRM:
        return true;
    default:
        return ak_action_chat_n(a) != 0u || ak_action_instance_n(a) != 0u;
    }
}

/* ---- id / slot helpers ---- */

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

int ak_keymap_slot_for_gesture(const char *g)
{
    if (g == NULL) {
        return -1;
    }
    for (int i = 0; i < AK_GESTURE_SLOT_COUNT; i++) {
        if (strcmp(g, s_gesture_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

/* ---- defaults ---- */

void ak_keymap_init_defaults(ak_keymap_t *map)
{
    if (map == NULL) {
        return;
    }
    memset(map, 0, sizeof(*map)); /* all NORMAL, all NONE */

    /* K1-K3: click=chatN, double=instanceN (N=1,2,3). */
    for (int i = 0; i < 3; i++) {
        map->entries[i].kind = AK_KEY_KIND_NORMAL;
        map->entries[i].actions[AK_GESTURE_SLOT_CLICK] = ak_action_chat((uint8_t)(i + 1));
        map->entries[i].actions[AK_GESTURE_SLOT_DOUBLE] = ak_action_instance((uint8_t)(i + 1));
    }
    /* K4: click=cancel_char, long=cancel_all. */
    map->entries[3].kind = AK_KEY_KIND_NORMAL;
    map->entries[3].actions[AK_GESTURE_SLOT_CLICK] = AK_ACTION_CANCEL_CHAR;
    map->entries[3].actions[AK_GESTURE_SLOT_LONG] = AK_ACTION_CANCEL_ALL;
    /* K5: VOICE. */
    map->entries[4].kind = AK_KEY_KIND_VOICE;
    /* K6: click=confirm. */
    map->entries[5].kind = AK_KEY_KIND_NORMAL;
    map->entries[5].actions[AK_GESTURE_SLOT_CLICK] = AK_ACTION_CONFIRM;
    /* enc: NORMAL, all none (local nav handled outside the keymap). */
    map->entries[6].kind = AK_KEY_KIND_NORMAL;
}

/* ---- lookups ---- */

static const ak_key_entry_t *entry_for(const ak_keymap_t *map, const char *id)
{
    int slot = ak_keymap_slot_for_id(id);
    if (map == NULL || slot < 0) {
        return NULL;
    }
    return &map->entries[slot];
}

ak_key_kind_t ak_keymap_kind(const ak_keymap_t *map, const char *id)
{
    const ak_key_entry_t *e = entry_for(map, id);
    return e ? e->kind : AK_KEY_KIND_NORMAL;
}

ak_action_t ak_keymap_action(const ak_keymap_t *map, const char *id, ak_gesture_slot_t slot)
{
    const ak_key_entry_t *e = entry_for(map, id);
    if (e == NULL || slot < 0 || slot >= AK_GESTURE_SLOT_COUNT) {
        return AK_ACTION_NONE;
    }
    return e->actions[slot];
}

bool ak_keymap_is_voice(const ak_keymap_t *map, const char *id)
{
    return ak_keymap_kind(map, id) == AK_KEY_KIND_VOICE;
}

/* ---- wire parse ---- */

bool ak_keymap_action_from_str(const char *s, ak_action_t *out)
{
    if (s == NULL || out == NULL) {
        return false;
    }
    if (strcmp(s, "none") == 0) {
        *out = AK_ACTION_NONE;
        return true;
    }
    if (strcmp(s, "cancel_char") == 0) {
        *out = AK_ACTION_CANCEL_CHAR;
        return true;
    }
    if (strcmp(s, "cancel_all") == 0) {
        *out = AK_ACTION_CANCEL_ALL;
        return true;
    }
    if (strcmp(s, "confirm") == 0) {
        *out = AK_ACTION_CONFIRM;
        return true;
    }
    /* chatN / instanceN with a single digit 1..9 */
    if (strncmp(s, "chat", 4) == 0 && s[4] >= '1' && s[4] <= '9' && s[5] == '\0') {
        *out = ak_action_chat((uint8_t)(s[4] - '0'));
        return *out != AK_ACTION_NONE;
    }
    if (strncmp(s, "instance", 8) == 0 && s[8] >= '1' && s[8] <= '9' && s[9] == '\0') {
        *out = ak_action_instance((uint8_t)(s[8] - '0'));
        return *out != AK_ACTION_NONE;
    }
    return false;
}

/* ---- mutate ---- */

bool ak_keymap_set_voice(ak_keymap_t *map, const char *id)
{
    int slot = ak_keymap_slot_for_id(id);
    if (map == NULL || slot < 0) {
        return false;
    }
    map->entries[slot].kind = AK_KEY_KIND_VOICE;
    return true;
}

bool ak_keymap_bind(ak_keymap_t *map, const char *id, ak_gesture_slot_t slot, ak_action_t action)
{
    int s = ak_keymap_slot_for_id(id);
    if (map == NULL || s < 0 || slot < 0 || slot >= AK_GESTURE_SLOT_COUNT || !ak_action_valid(action)) {
        return false;
    }
    map->entries[s].kind = AK_KEY_KIND_NORMAL;
    map->entries[s].actions[slot] = action;
    return true;
}

/* ---- KV blob ---- */

unsigned ak_keymap_to_blob(const ak_keymap_t *map, uint8_t *buf, unsigned buf_size)
{
    if (map == NULL || buf == NULL || buf_size < AK_KEYMAP_BLOB_SIZE) {
        return 0;
    }
    unsigned p = 0;
    buf[p++] = (uint8_t)AK_KEYMAP_BLOB_VERSION;
    for (int i = 0; i < AK_KEYMAP_SLOTS; i++) {
        buf[p++] = (uint8_t)map->entries[i].kind;
        for (int g = 0; g < AK_GESTURE_SLOT_COUNT; g++) {
            buf[p++] = (uint8_t)map->entries[i].actions[g];
        }
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
    /* Validate everything into a temp before mutating, so a corrupt blob never
     * leaves a half-applied table. */
    ak_keymap_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    unsigned p = 1;
    for (int i = 0; i < AK_KEYMAP_SLOTS; i++) {
        uint8_t kind = buf[p++];
        if (kind != AK_KEY_KIND_NORMAL && kind != AK_KEY_KIND_VOICE) {
            return false;
        }
        tmp.entries[i].kind = (ak_key_kind_t)kind;
        for (int g = 0; g < AK_GESTURE_SLOT_COUNT; g++) {
            ak_action_t a = buf[p++];
            if (!ak_action_valid(a)) {
                return false;
            }
            tmp.entries[i].actions[g] = a;
        }
    }
    *map = tmp;
    return true;
}
