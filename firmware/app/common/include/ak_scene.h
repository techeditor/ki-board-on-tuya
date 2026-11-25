/**
 * @file ak_scene.h
 * @brief FR-03 application scenes: the board's 5 scene slots (slot 0 = the
 *        current scene pushed by the Companion, slots 1..4 = favourites).
 *        Pure logic, host-testable: slot validation, the compact `km` / `idle`
 *        hex encodings of scene_put / scene_data, the KV blob and the
 *        scene_put payload parser. The app owns KV I/O and applying a slot.
 *
 * The Companion is the authority for scene content; the board keeps copies
 * identified by (id, rev). Live settings stay in their own KV keys
 * (ak_keymap, ak_vmode, ak_ledpwr); switching copies a slot into them.
 * See shared/protocol/protocol.md "应用场景（FR-03）".
 */

#ifndef AK_SCENE_H
#define AK_SCENE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ak_keymap.h"
#include "ak_menu.h"

#define AK_SCENE_SLOTS    5U
#define AK_SCENE_ID_LEN   8U  /* exactly 8 of [a-z0-9] */
#define AK_SCENE_NAME_MAX 24U /* UTF-8 bytes, same as an LCD menu label */
#define AK_SCENE_KM_LEN   (AK_KEYMAP_BLOB_SIZE - 1U) /* 28: 7 keys x [kind, single, double, long] */
#define AK_SCENE_KM_HEX   (AK_SCENE_KM_LEN * 2U)     /* 56 lower-case hex chars */
#define AK_SCENE_IDLE_LEN 7U                         /* idle colour per k1..k6, enc */
#define AK_SCENE_IDLE_HEX (AK_SCENE_IDLE_LEN * 2U)

/* KV blob v1: ver, id[8], rev u32 LE, name_len, name[24], km[28], voice_mode,
 * led_mode, led_timeout_idx, idle[7], home, crc32 LE over all bytes before it. */
#define AK_SCENE_BLOB_VER 1U
#define AK_SCENE_BLOB_LEN (1U + AK_SCENE_ID_LEN + 4U + 1U + AK_SCENE_NAME_MAX + AK_SCENE_KM_LEN + 3U + \
                           AK_SCENE_IDLE_LEN + 1U + 4U) /* 81 */

/* LCD home layout. Stored and reported now; only SESSIONS is drawn until the
 * user decides FR-03 D2 (the `keys` page). */
typedef enum {
    AK_SCENE_HOME_SESSIONS = 0,
    AK_SCENE_HOME_KEYS     = 1,
    AK_SCENE_HOME_COUNT,
} ak_scene_home_t;

/* Idle colours (FR-03 D3): off / green / blue / purple only; red and yellow
 * keep their FR-09 meanings (cancel, attention). Byte = ak_light_t. */
bool ak_scene_idle_color_ok(uint8_t c);
/* Idle colours are drawn at this brightness (percent). */
#define AK_SCENE_IDLE_PCT 20U

typedef struct {
    char        id[AK_SCENE_ID_LEN + 1U];
    uint32_t    rev;
    char        name[AK_SCENE_NAME_MAX + 1U];
    ak_keymap_t keymap;
    uint8_t     voice_mode;      /* ak_voice_mode_t */
    uint8_t     led_mode;        /* ak_led_mode_t */
    uint8_t     led_timeout_idx; /* index into ak_led_policy timeouts */
    uint8_t     idle[AK_SCENE_IDLE_LEN];
    uint8_t     home; /* ak_scene_home_t */
} ak_scene_t;

/** Exactly AK_SCENE_ID_LEN characters of [a-z0-9]. */
bool ak_scene_id_ok(const char *id);

/** 1..AK_SCENE_NAME_MAX bytes of valid UTF-8 with no control or bidi
 *  override / isolate / mark characters. */
bool ak_scene_name_ok(const char *name);

/** Scene keymap rules on top of ak_keymap_from_blob: enc is NORMAL with no
 *  long binding (Settings), at most one VOICE key, and a VOICE key's gesture
 *  bytes are all NONE. */
bool ak_scene_keymap_ok(const ak_keymap_t *map);

/** Parse 56 lower-case hex chars into a keymap (all rules above). Returns
 *  false and leaves `out` untouched on any error. */
bool ak_scene_km_from_hex(const char *hex, ak_keymap_t *out);

/** Format a keymap as 56 lower-case hex chars + NUL. */
void ak_scene_km_to_hex(const ak_keymap_t *map, char out[AK_SCENE_KM_HEX + 1U]);

/** Parse 14 lower-case hex chars into idle colours (each <= AK_SCENE_IDLE_MAX). */
bool ak_scene_idle_from_hex(const char *hex, uint8_t out[AK_SCENE_IDLE_LEN]);

void ak_scene_idle_to_hex(const uint8_t idle[AK_SCENE_IDLE_LEN], char out[AK_SCENE_IDLE_HEX + 1U]);

/** Wire names of the home layout ("sessions" / "keys"). */
const char *ak_scene_home_wire(uint8_t home);
bool        ak_scene_home_from_wire(const char *s, uint8_t *out);

/** Every field valid (id, name, keymap rules, enum ranges). */
bool ak_scene_valid(const ak_scene_t *s);

/** Serialize a valid scene into the KV blob. Returns AK_SCENE_BLOB_LEN, or 0
 *  when the scene is invalid or the buffer is too small. */
unsigned ak_scene_to_blob(const ak_scene_t *s, uint8_t *buf, unsigned size);

/** Load a KV blob; false (out untouched) on a wrong length, version, CRC or
 *  any invalid field, so a damaged slot reads as empty. */
bool ak_scene_from_blob(ak_scene_t *out, const uint8_t *buf, unsigned len);

/** Parse a scene_put payload: slot 0..4 and every scene field are required.
 *  Returns false (outputs untouched) when anything is missing or invalid. */
bool ak_scene_parse_put(const char *payload, uint8_t *slot, ak_scene_t *out);

/** True when the live settings differ from the scene (scene_state `dirty`). */
bool ak_scene_differs(const ak_scene_t *s, const ak_keymap_t *live, uint8_t voice_mode, uint8_t led_mode,
                      uint8_t led_timeout_idx);

/* Scene picker (FR-03 D1): id of the keyboard-local menu, and at most two
 * pages (favourites 1..4 on K2..K5, then slot 0). */
#define AK_SCENE_PICKER_ID    "scene"
#define AK_SCENE_PICKER_PAGES 2U
#define AK_SCENE_PICKER_TIMEOUT_S 10U

/** Build the picker pages from the used slots: favourites 1..4 in order, then
 *  slot 0 unless it holds the same scene as a favourite. The active slot is
 *  ticked and is the default. `slot_of[i]` = slot of global item i. Returns
 *  the page count (0 = no scene stored). */
uint8_t ak_scene_picker_build(const ak_scene_t scenes[AK_SCENE_SLOTS], const bool used[AK_SCENE_SLOTS], int active,
                              const char *title, ak_menu_page_t pages[AK_SCENE_PICKER_PAGES],
                              uint8_t slot_of[AK_SCENE_SLOTS]);

/** Short LCD name of an action for the `keys` home page, in the current
 *  interface language (ak_lang_get). Writes into `buf` and returns it. */
const char *ak_scene_action_label(ak_action_t a, char *buf, size_t cap);

/** Reflected CRC-32 (zlib), used for the slot blob. */
uint32_t ak_scene_crc32(const uint8_t *buf, size_t len);

#endif /* AK_SCENE_H */
