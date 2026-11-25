/**
 * @file ak_keymap.h
 * @brief Runtime per-gesture key binding table (v0.3, stage 乙). Replaces the
 *        earlier role-only table: key functions are DATA, freely bound per
 *        gesture, not fixed by a role.
 *
 * Model (design §5.5):
 *   - Each physical input (K1..K6 + the EC11 push "enc") has a KIND:
 *       NORMAL — runs the single/double/long classifier; carries three gesture
 *                SLOTS (single/double/long), each bound to one ACTION.
 *       VOICE  — hold-to-talk; immediate-edge PTT, no gesture slots.
 *   - An ACTION is what a gesture does. The firmware OWNS cancel/confirm
 *     (pending-confirm edits) and voice locally. Every gesture is reported as a
 *     key frame (action = gesture); a bound action rides along as `bind`.
 *
 * Defaults match the confirmed v0.3 model and are overridable at runtime by a
 * Companion `keymap` frame (Companion is the config authority); the resolved
 * table is cached in TAL KV so a disconnected board keeps the last map.
 */

#ifndef AK_KEYMAP_H
#define AK_KEYMAP_H

#include <stdbool.h>
#include <stdint.h>

/* Number of mappable inputs: K1..K6 plus the EC11 push "enc". */
#define AK_KEYMAP_SLOTS 7

/* A key's kind. NORMAL keys carry gesture slots; VOICE is special hold-to-talk. */
typedef enum {
    AK_KEY_KIND_NORMAL = 0,
    AK_KEY_KIND_VOICE  = 1,
} ak_key_kind_t;

/* The three gesture slots a NORMAL key exposes. Index into entry.actions[]. */
typedef enum {
    AK_GESTURE_SLOT_SINGLE = 0,
    AK_GESTURE_SLOT_DOUBLE = 1,
    AK_GESTURE_SLOT_LONG = 2,
    AK_GESTURE_SLOT_COUNT = 3,
} ak_gesture_slot_t;

/* An action bound to a gesture slot, encoded as one byte for the KV blob:
 *   0x00            = NONE (unbound)
 *   0x01            = CANCEL_CHAR   (pending-confirm: delete one char)
 *   0x02            = CANCEL_ALL    (pending-confirm: delete all + exit)
 *   0x03            = CONFIRM       (pending-confirm: confirm + exit)
 *   0x04            = MODEL_MENU    (Companion opens the LCD model menu)
 *   0x05            = MODEL_TOGGLE  (Companion toggles Auto / last model)
 *   0x06            = SCENE_MENU    (FR-03: open the scene picker, board-local)
 *   0x10 + N (1..9) = CHAT N        (jump to chat N -> Alt+N, Companion-mapped)
 *   0x20 + N (1..9) = INSTANCE N    (switch instance N -> Ctrl+N, Companion-mapped)
 *   0x30 + N (1..14)= SHORTCUT N    (FR-03 limited shortcut "key_*", Companion-injected;
 *                                    table in ak_keymap.c, same order as protocol.md)
 * The high-nibble scheme keeps the parameterised chat/instance actions in one
 * byte and leaves room for future action families. */
typedef uint8_t ak_action_t;

#define AK_ACTION_NONE        0x00u
#define AK_ACTION_CANCEL_CHAR 0x01u
#define AK_ACTION_CANCEL_ALL  0x02u
#define AK_ACTION_CONFIRM     0x03u
#define AK_ACTION_MODEL_MENU   0x04u /* open the LCD model/mode menu (Companion-run) */
#define AK_ACTION_MODEL_TOGGLE 0x05u /* Auto <-> last picked model (Companion-run) */
#define AK_ACTION_CHAT_BASE   0x10u /* + N (1..9) */
#define AK_ACTION_INSTANCE_BASE 0x20u /* + N (1..9) */
#define AK_ACTION_SCENE_MENU   0x06u /* FR-03: scene picker (board-local) */
#define AK_ACTION_SHORTCUT_BASE 0x30u /* + N (1..AK_SHORTCUT_COUNT), FR-03 */
#define AK_SHORTCUT_COUNT      14u

/** If `a` is SHORTCUT N return N (1..AK_SHORTCUT_COUNT) else 0. */
uint8_t ak_action_shortcut_n(ak_action_t a);

/** Build a CHAT N action (N 1..9), or AK_ACTION_NONE if out of range. */
ak_action_t ak_action_chat(uint8_t n);
/** Build an INSTANCE N action (N 1..9), or AK_ACTION_NONE if out of range. */
ak_action_t ak_action_instance(uint8_t n);
/** If `a` is CHAT N return N (1..9) else 0. */
uint8_t ak_action_chat_n(ak_action_t a);
/** If `a` is INSTANCE N return N (1..9) else 0. */
uint8_t ak_action_instance_n(ak_action_t a);
/** True if `a` is a recognised action encoding (incl. NONE). */
bool ak_action_valid(ak_action_t a);

typedef struct {
    ak_key_kind_t kind;
    /* Actions per gesture slot (single/double/long). Unused for VOICE kind. */
    ak_action_t   actions[AK_GESTURE_SLOT_COUNT];
} ak_key_entry_t;

typedef struct {
    ak_key_entry_t entries[AK_KEYMAP_SLOTS]; /* index: k1..k6, then enc */
} ak_keymap_t;

/** Map a key id ("k1".."k6" / "enc") to its 0-based slot, or -1 if unknown. */
int ak_keymap_slot_for_id(const char *id);

/** Map a 0-based slot (0..AK_KEYMAP_SLOTS-1) back to its id string, or NULL. */
const char *ak_keymap_id_for_slot(int slot);

/** Map a gesture-slot name ("single"/"double"/"long") to its enum, or -1. */
int ak_keymap_slot_for_gesture(const char *g);

/** Fill `map` with the confirmed v0.3 defaults:
 *   K1-K3 NORMAL: single=chatN, long=instanceN (N=1,2,3), double=none
 *   K4   NORMAL: single=cancel_char, long=cancel_all
 *   K5   VOICE
 *   K6   NORMAL: single=confirm, double=model_toggle, long=model_menu
 *   enc  NORMAL: all none (local nav handled outside the keymap) */
void ak_keymap_init_defaults(ak_keymap_t *map);

/** Key kind for an id, or AK_KEY_KIND_NORMAL for an unknown id. */
ak_key_kind_t ak_keymap_kind(const ak_keymap_t *map, const char *id);

/** The action bound to (id, slot), or AK_ACTION_NONE if unknown/out of range. */
ak_action_t ak_keymap_action(const ak_keymap_t *map, const char *id, ak_gesture_slot_t slot);

/** True if the key id is the VOICE (hold-to-talk) key. */
bool ak_keymap_is_voice(const ak_keymap_t *map, const char *id);

/** Parse an action WIRE name into an encoded action. Recognises "none",
 *  "cancel_char", "cancel_all", "confirm", "model_menu", "model_toggle",
 *  "scene_menu", "key_*" shortcuts, "chat1".."chat9", "instance1".."instance9".
 *  Returns false (out untouched) for anything else. */
bool ak_keymap_action_from_str(const char *s, ak_action_t *out);

/** Wire name of a fixed (non-parameterised) action or a "key_*" shortcut, or
 *  NULL for NONE, chatN / instanceN and unknown bytes. */
const char *ak_keymap_action_str(ak_action_t a);

/** Set key `id`'s kind to VOICE. Returns true if applied (known id). */
bool ak_keymap_set_voice(ak_keymap_t *map, const char *id);

/** Bind an action to (id, slot), forcing the key to NORMAL kind. Returns true
 *  if applied (known id). */
bool ak_keymap_bind(ak_keymap_t *map, const char *id, ak_gesture_slot_t slot, ak_action_t action);

/* Compact KV blob: version byte, then per slot: 1 kind byte + 3 action bytes. */
#define AK_KEYMAP_BLOB_VERSION 3u    /* v3: K1-K3 default instanceN moved from double to long */
#define AK_KEYMAP_BLOB_VERSION_V2 2u /* same layout; from_blob migrates the old K1-K3 default */
#define AK_KEYMAP_BLOB_SIZE    (1u + AK_KEYMAP_SLOTS * (1u + AK_GESTURE_SLOT_COUNT)) /* 1 + 7*4 = 29 */

/** Serialize `map` into a fixed AK_KEYMAP_BLOB_SIZE byte buffer for TAL KV.
 *  Returns bytes written (AK_KEYMAP_BLOB_SIZE) or 0 on error. */
unsigned ak_keymap_to_blob(const ak_keymap_t *map, uint8_t *buf, unsigned buf_size);

/** Load `map` from a KV blob (current or v2; a v2 blob still holding the old
 *  K1-K3 default double=instanceN is migrated to long=instanceN). Rejects a wrong version/size or an invalid kind/
 *  action byte (returns false, leaving `map` untouched) so a stale/corrupt blob
 *  falls back to defaults rather than scrambling the table. */
bool ak_keymap_from_blob(ak_keymap_t *map, const uint8_t *buf, unsigned len);

#endif /* AK_KEYMAP_H */
