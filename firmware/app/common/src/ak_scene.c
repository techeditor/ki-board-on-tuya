/**
 * @file ak_scene.c
 * @brief FR-03 scene slots (see ak_scene.h). Pure logic, host-testable.
 */

#include "ak_scene.h"

#include <stdio.h>
#include <string.h>

#include "ak_i18n.h"
#include "ak_led_policy.h"
#include "ak_rx.h"
#include "ak_voice_mode.h"

#define AK_SCENE_ENC_SLOT (AK_KEYMAP_SLOTS - 1)

uint32_t ak_scene_crc32(const uint8_t *buf, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

bool ak_scene_idle_color_ok(uint8_t c)
{
    return c == AK_LIGHT_OFF || c == AK_LIGHT_GREEN || c == AK_LIGHT_BLUE || c == AK_LIGHT_PURPLE;
}

bool ak_scene_id_ok(const char *id)
{
    if (id == NULL || strlen(id) != AK_SCENE_ID_LEN) {
        return false;
    }
    for (size_t i = 0; i < AK_SCENE_ID_LEN; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
            return false;
        }
    }
    return true;
}

/* Decode one UTF-8 sequence at s (n bytes left); returns its length (0 = invalid). */
static size_t utf8_seq(const uint8_t *s, size_t n, uint32_t *cp)
{
    uint8_t c = s[0];
    size_t  len;
    uint32_t v;
    if (c < 0x80u) {
        *cp = c;
        return 1;
    } else if (c >= 0xC2u && c <= 0xDFu) {
        len = 2;
        v   = c & 0x1Fu;
    } else if (c >= 0xE0u && c <= 0xEFu) {
        len = 3;
        v   = c & 0x0Fu;
    } else if (c >= 0xF0u && c <= 0xF4u) {
        len = 4;
        v   = c & 0x07u;
    } else {
        return 0;
    }
    if (len > n) {
        return 0;
    }
    for (size_t i = 1; i < len; i++) {
        if ((s[i] & 0xC0u) != 0x80u) {
            return 0;
        }
        v = (v << 6) | (s[i] & 0x3Fu);
    }
    /* overlong, surrogate and out-of-range forms */
    if ((len == 3 && v < 0x800u) || (len == 4 && (v < 0x10000u || v > 0x10FFFFu)) ||
        (v >= 0xD800u && v <= 0xDFFFu)) {
        return 0;
    }
    *cp = v;
    return len;
}

bool ak_scene_name_ok(const char *name)
{
    if (name == NULL) {
        return false;
    }
    size_t n = strlen(name);
    if (n == 0 || n > AK_SCENE_NAME_MAX) {
        return false;
    }
    const uint8_t *p = (const uint8_t *)name;
    while (n > 0) {
        uint32_t cp;
        size_t   len = utf8_seq(p, n, &cp);
        if (len == 0 || cp < 0x20u || (cp >= 0x7Fu && cp <= 0x9Fu) || cp == 0x200Eu || cp == 0x200Fu ||
            (cp >= 0x202Au && cp <= 0x202Eu) || (cp >= 0x2066u && cp <= 0x2069u) || cp == 0x2028u ||
            cp == 0x2029u) {
            return false;
        }
        p += len;
        n -= len;
    }
    return true;
}

bool ak_scene_keymap_ok(const ak_keymap_t *map)
{
    if (map == NULL) {
        return false;
    }
    const ak_key_entry_t *enc = &map->entries[AK_SCENE_ENC_SLOT];
    if (enc->kind != AK_KEY_KIND_NORMAL || enc->actions[AK_GESTURE_SLOT_LONG] != AK_ACTION_NONE) {
        return false;
    }
    unsigned voice = 0;
    for (int i = 0; i < AK_KEYMAP_SLOTS; i++) {
        const ak_key_entry_t *e = &map->entries[i];
        if (e->kind != AK_KEY_KIND_VOICE) {
            continue;
        }
        voice++;
        for (int g = 0; g < AK_GESTURE_SLOT_COUNT; g++) {
            if (e->actions[g] != AK_ACTION_NONE) {
                return false;
            }
        }
    }
    return voice <= 1u;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1; /* upper case is rejected: the wire form is lower case */
}

static bool hex_decode(const char *hex, uint8_t *out, size_t n)
{
    if (hex == NULL || strlen(hex) != n * 2U) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static void hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0x0Fu];
    }
    out[2 * n] = '\0';
}

bool ak_scene_km_from_hex(const char *hex, ak_keymap_t *out)
{
    uint8_t blob[AK_KEYMAP_BLOB_SIZE];
    blob[0] = (uint8_t)AK_KEYMAP_BLOB_VERSION;
    if (out == NULL || !hex_decode(hex, &blob[1], AK_SCENE_KM_LEN)) {
        return false;
    }
    ak_keymap_t tmp;
    if (!ak_keymap_from_blob(&tmp, blob, sizeof(blob)) || !ak_scene_keymap_ok(&tmp)) {
        return false;
    }
    *out = tmp;
    return true;
}

void ak_scene_km_to_hex(const ak_keymap_t *map, char out[AK_SCENE_KM_HEX + 1U])
{
    uint8_t blob[AK_KEYMAP_BLOB_SIZE];
    if (map == NULL || ak_keymap_to_blob(map, blob, sizeof(blob)) != AK_KEYMAP_BLOB_SIZE) {
        out[0] = '\0';
        return;
    }
    hex_encode(&blob[1], AK_SCENE_KM_LEN, out);
}

bool ak_scene_idle_from_hex(const char *hex, uint8_t out[AK_SCENE_IDLE_LEN])
{
    uint8_t tmp[AK_SCENE_IDLE_LEN];
    if (out == NULL || !hex_decode(hex, tmp, AK_SCENE_IDLE_LEN)) {
        return false;
    }
    for (size_t i = 0; i < AK_SCENE_IDLE_LEN; i++) {
        if (!ak_scene_idle_color_ok(tmp[i])) {
            return false;
        }
    }
    memcpy(out, tmp, sizeof(tmp));
    return true;
}

void ak_scene_idle_to_hex(const uint8_t idle[AK_SCENE_IDLE_LEN], char out[AK_SCENE_IDLE_HEX + 1U])
{
    hex_encode(idle, AK_SCENE_IDLE_LEN, out);
}

static const char *const s_home_wire[AK_SCENE_HOME_COUNT] = {"sessions", "keys"};

const char *ak_scene_home_wire(uint8_t home)
{
    return home < AK_SCENE_HOME_COUNT ? s_home_wire[home] : NULL;
}

bool ak_scene_home_from_wire(const char *s, uint8_t *out)
{
    for (uint8_t i = 0; s != NULL && i < AK_SCENE_HOME_COUNT; i++) {
        if (strcmp(s, s_home_wire[i]) == 0) {
            *out = i;
            return true;
        }
    }
    return false;
}

bool ak_scene_valid(const ak_scene_t *s)
{
    if (s == NULL || !ak_scene_id_ok(s->id) || !ak_scene_name_ok(s->name) || !ak_scene_keymap_ok(&s->keymap) ||
        s->voice_mode > AK_VOICE_MODE_CHAT || s->led_mode > AK_LED_MODE_AUTO ||
        s->led_timeout_idx >= AK_LED_TIMEOUT_COUNT || s->home >= AK_SCENE_HOME_COUNT) {
        return false;
    }
    for (size_t i = 0; i < AK_SCENE_IDLE_LEN; i++) {
        if (!ak_scene_idle_color_ok(s->idle[i])) {
            return false;
        }
    }
    /* ak_keymap_from_blob already checks kinds and action bytes; re-check for
     * a scene built in RAM. */
    for (int i = 0; i < AK_KEYMAP_SLOTS; i++) {
        if (s->keymap.entries[i].kind != AK_KEY_KIND_NORMAL && s->keymap.entries[i].kind != AK_KEY_KIND_VOICE) {
            return false;
        }
        for (int g = 0; g < AK_GESTURE_SLOT_COUNT; g++) {
            if (!ak_action_valid(s->keymap.entries[i].actions[g])) {
                return false;
            }
        }
    }
    return true;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

unsigned ak_scene_to_blob(const ak_scene_t *s, uint8_t *buf, unsigned size)
{
    uint8_t km[AK_KEYMAP_BLOB_SIZE];
    if (buf == NULL || size < AK_SCENE_BLOB_LEN || !ak_scene_valid(s) ||
        ak_keymap_to_blob(&s->keymap, km, sizeof(km)) != AK_KEYMAP_BLOB_SIZE) {
        return 0;
    }
    memset(buf, 0, AK_SCENE_BLOB_LEN);
    unsigned p = 0;
    buf[p++] = (uint8_t)AK_SCENE_BLOB_VER;
    memcpy(&buf[p], s->id, AK_SCENE_ID_LEN);
    p += AK_SCENE_ID_LEN;
    put_u32(&buf[p], s->rev);
    p += 4;
    size_t name_len = strlen(s->name);
    buf[p++]        = (uint8_t)name_len;
    memcpy(&buf[p], s->name, name_len);
    p += AK_SCENE_NAME_MAX;
    memcpy(&buf[p], &km[1], AK_SCENE_KM_LEN);
    p += AK_SCENE_KM_LEN;
    buf[p++] = s->voice_mode;
    buf[p++] = s->led_mode;
    buf[p++] = s->led_timeout_idx;
    memcpy(&buf[p], s->idle, AK_SCENE_IDLE_LEN);
    p += AK_SCENE_IDLE_LEN;
    buf[p++] = s->home;
    put_u32(&buf[p], ak_scene_crc32(buf, p));
    return AK_SCENE_BLOB_LEN;
}

bool ak_scene_from_blob(ak_scene_t *out, const uint8_t *buf, unsigned len)
{
    if (out == NULL || buf == NULL || len != AK_SCENE_BLOB_LEN || buf[0] != AK_SCENE_BLOB_VER ||
        get_u32(&buf[AK_SCENE_BLOB_LEN - 4U]) != ak_scene_crc32(buf, AK_SCENE_BLOB_LEN - 4U)) {
        return false;
    }
    ak_scene_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    unsigned p = 1;
    memcpy(tmp.id, &buf[p], AK_SCENE_ID_LEN);
    p += AK_SCENE_ID_LEN;
    tmp.rev = get_u32(&buf[p]);
    p += 4;
    uint8_t name_len = buf[p++];
    if (name_len > AK_SCENE_NAME_MAX) {
        return false;
    }
    memcpy(tmp.name, &buf[p], name_len);
    p += AK_SCENE_NAME_MAX;
    char km_hex[AK_SCENE_KM_HEX + 1U];
    hex_encode(&buf[p], AK_SCENE_KM_LEN, km_hex);
    if (!ak_scene_km_from_hex(km_hex, &tmp.keymap)) {
        return false;
    }
    p += AK_SCENE_KM_LEN;
    tmp.voice_mode      = buf[p++];
    tmp.led_mode        = buf[p++];
    tmp.led_timeout_idx = buf[p++];
    memcpy(tmp.idle, &buf[p], AK_SCENE_IDLE_LEN);
    p += AK_SCENE_IDLE_LEN;
    tmp.home = buf[p];
    if (!ak_scene_valid(&tmp)) {
        return false;
    }
    *out = tmp;
    return true;
}

bool ak_scene_parse_put(const char *payload, uint8_t *slot, ak_scene_t *out)
{
    if (payload == NULL || slot == NULL || out == NULL) {
        return false;
    }
    ak_scene_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    uint32_t slot_v = 0;
    uint32_t rev    = 0;
    uint32_t off_s  = 0;
    char     km[AK_SCENE_KM_HEX + 2U];
    char     idle[AK_SCENE_IDLE_HEX + 2U];
    char     voice[8];
    char     led[8];
    char     home[12];
    ak_voice_mode_t vm;
    ak_led_mode_t   lm;
    uint8_t         idx;
    if (ak_json_get_uint(payload, "slot", AK_SCENE_SLOTS - 1U, &slot_v) != AK_JSON_OK ||
        ak_json_get_str(payload, "id", tmp.id, sizeof(tmp.id)) != AK_JSON_OK ||
        ak_json_get_uint(payload, "rev", 0xFFFFFFFFu, &rev) != AK_JSON_OK ||
        ak_json_get_str(payload, "name", tmp.name, sizeof(tmp.name)) != AK_JSON_OK ||
        ak_json_get_str(payload, "km", km, sizeof(km)) != AK_JSON_OK ||
        ak_json_get_str(payload, "voice", voice, sizeof(voice)) != AK_JSON_OK ||
        ak_json_get_str(payload, "led", led, sizeof(led)) != AK_JSON_OK ||
        ak_json_get_uint(payload, "led_off_s", 3600U, &off_s) != AK_JSON_OK ||
        ak_json_get_str(payload, "idle", idle, sizeof(idle)) != AK_JSON_OK ||
        ak_json_get_str(payload, "home", home, sizeof(home)) != AK_JSON_OK) {
        return false;
    }
    if (!ak_scene_km_from_hex(km, &tmp.keymap) || !ak_scene_idle_from_hex(idle, tmp.idle) ||
        !ak_voice_mode_from_wire(voice, &vm) || !ak_led_mode_from_wire(led, &lm) ||
        !ak_led_policy_timeout_from_s(off_s, &idx) || !ak_scene_home_from_wire(home, &tmp.home)) {
        return false;
    }
    tmp.rev             = rev;
    tmp.voice_mode      = (uint8_t)vm;
    tmp.led_mode        = (uint8_t)lm;
    tmp.led_timeout_idx = idx;
    if (!ak_scene_valid(&tmp)) {
        return false;
    }
    *slot = (uint8_t)slot_v;
    *out  = tmp;
    return true;
}

bool ak_scene_differs(const ak_scene_t *s, const ak_keymap_t *live, uint8_t voice_mode, uint8_t led_mode,
                      uint8_t led_timeout_idx)
{
    if (s == NULL || live == NULL) {
        return true;
    }
    uint8_t a[AK_KEYMAP_BLOB_SIZE];
    uint8_t b[AK_KEYMAP_BLOB_SIZE];
    if (ak_keymap_to_blob(&s->keymap, a, sizeof(a)) != AK_KEYMAP_BLOB_SIZE ||
        ak_keymap_to_blob(live, b, sizeof(b)) != AK_KEYMAP_BLOB_SIZE) {
        return true;
    }
    return memcmp(a, b, sizeof(a)) != 0 || s->voice_mode != voice_mode || s->led_mode != led_mode ||
           s->led_timeout_idx != led_timeout_idx;
}

uint8_t ak_scene_picker_build(const ak_scene_t scenes[AK_SCENE_SLOTS], const bool used[AK_SCENE_SLOTS], int active,
                              const char *title, ak_menu_page_t pages[AK_SCENE_PICKER_PAGES],
                              uint8_t slot_of[AK_SCENE_SLOTS])
{
    uint8_t order[AK_SCENE_SLOTS];
    uint8_t n = 0;
    for (uint8_t s = 1; s < AK_SCENE_SLOTS; s++) {
        if (used[s]) {
            order[n++] = s;
        }
    }
    bool dup = false;
    for (uint8_t i = 0; i < n; i++) {
        dup |= strcmp(scenes[order[i]].id, scenes[0].id) == 0;
    }
    if (used[0] && (!dup || active == 0)) {
        order[n++] = 0;
    }
    if (n == 0) {
        return 0;
    }
    uint8_t npages = (uint8_t)((n + AK_MENU_PAGE_ITEMS_MAX - 1U) / AK_MENU_PAGE_ITEMS_MAX);
    uint8_t def    = 0;
    for (uint8_t i = 0; i < n; i++) {
        if (order[i] == active) {
            def = i;
        }
    }
    for (uint8_t p = 0; p < npages; p++) {
        ak_menu_page_t *pg = &pages[p];
        memset(pg, 0, sizeof(*pg));
        (void)snprintf(pg->id, sizeof(pg->id), "%s", AK_SCENE_PICKER_ID);
        pg->kind      = AK_MENU_KIND_SCENE;
        (void)snprintf(pg->title, sizeof(pg->title), "%s", title != NULL ? title : "");
        pg->page      = (uint8_t)(p + 1U);
        pg->pages     = npages;
        pg->def       = def;
        pg->timeout_s = AK_SCENE_PICKER_TIMEOUT_S;
        for (uint8_t k = 0; k < AK_MENU_PAGE_ITEMS_MAX && p * AK_MENU_PAGE_ITEMS_MAX + k < n; k++) {
            uint8_t slot = order[p * AK_MENU_PAGE_ITEMS_MAX + k];
            ak_menu_item_t *it = &pg->items[k];
            (void)snprintf(it->label, sizeof(it->label), "%s", scenes[slot].name);
            it->role    = AK_MENU_ROLE_NORMAL;
            it->checked = (int)slot == active;
            pg->count++;
        }
    }
    for (uint8_t i = 0; i < n; i++) {
        slot_of[i] = order[i];
    }
    return npages;
}

/* keys home page labels: {zh, en}. Fixed actions by byte, shortcuts by n. */
static const char *const s_fixed_labels[][2] = {
    {"-", "-"},          {"删字", "Del char"}, {"全删", "Clear"},     {"确认", "Confirm"},
    {"模型菜单", "Models"}, {"切换模型", "Auto/model"}, {"场景", "Scenes"},
};
static const char *const s_shortcut_labels[AK_SHORTCUT_COUNT][2] = {
    {"Enter", "Enter"},     {"Esc", "Esc"},          {"上", "Up"},          {"下", "Down"},
    {"左", "Left"},         {"右", "Right"},         {"退格", "Bksp"},      {"删词", "Del word"},
    {"清空输入", "Clear in"}, {"命令栏", "Ctrl+K"},    {"上个对话", "Prev chat"}, {"下个对话", "Next chat"},
    {"输入框", "Focus"},    {"新对话", "New chat"},
};

const char *ak_scene_action_label(ak_action_t a, char *buf, size_t cap)
{
    int     zh = ak_lang_get() == AK_LANG_ZH ? 0 : 1;
    uint8_t n;
    if (buf == NULL || cap == 0U) {
        return "";
    }
    if (a < sizeof(s_fixed_labels) / sizeof(s_fixed_labels[0])) {
        (void)snprintf(buf, cap, "%s", s_fixed_labels[a][zh]);
    } else if ((n = ak_action_chat_n(a)) != 0U) {
        (void)snprintf(buf, cap, zh == 0 ? "对话%u" : "Chat %u", (unsigned)n);
    } else if ((n = ak_action_instance_n(a)) != 0U) {
        (void)snprintf(buf, cap, zh == 0 ? "实例%u" : "Inst %u", (unsigned)n);
    } else if ((n = ak_action_shortcut_n(a)) != 0U) {
        (void)snprintf(buf, cap, "%s", s_shortcut_labels[n - 1U][zh]);
    } else {
        (void)snprintf(buf, cap, "?");
    }
    return buf;
}
