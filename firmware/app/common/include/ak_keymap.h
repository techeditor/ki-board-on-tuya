/**
 * @file ak_keymap.h
 * @brief Runtime key-role table (v0.3). Replaces the old hard-coded
 *        `#define AK_PTT_KEY_ID "k4"` so key functions are DATA, not code.
 *
 * Each physical key (K1..K6 + the EC11 push "enc") carries a ROLE and a
 * gesture-DETECTION mode. The firmware acts on roles it owns locally (voice /
 * cancel / confirm) and reports the rest as protocol events for the Companion
 * to map (chat/instance focus). The table defaults to the confirmed v0.3
 * interaction model and can be overridden at runtime by a Companion `keymap`
 * frame (plan A: Companion is the config authority); overrides are cached in
 * TAL KV so a disconnected board keeps the last map.
 *
 * See docs/agentic-keyboard-interaction-design.md §0.
 */

#ifndef AK_KEYMAP_H
#define AK_KEYMAP_H

#include <stdbool.h>
#include <stdint.h>

/* Logical role a key plays in the base (idle) state. The firmware only ACTS on
 * the roles it owns locally; CHAT/NONE are reported to the Companion. */
typedef enum {
    AK_KEY_ROLE_NONE = 0,  /* report events only; no local behavior */
    AK_KEY_ROLE_CHAT,      /* K1-K3: single=focus chat, double=switch instance (Companion maps) */
    AK_KEY_ROLE_VOICE,     /* push-to-talk / trigger ASR (K5 by default) */
    AK_KEY_ROLE_CANCEL,    /* pending-confirm: click=del char, long=del all (K4 by default) */
    AK_KEY_ROLE_CONFIRM,   /* pending-confirm: click=confirm (K6 by default) */
} ak_key_role_t;

/* How the key's gestures are detected / reported. */
typedef enum {
    /* Immediate edges: DOWN/UP/LONG reported as they happen. Use for keys that
     * must react instantly (voice PTT needs DOWN to start recording now). */
    AK_KEY_GESTURE_EDGE = 0,
    /* Deferred single/double classification (scheme X): single is held back up
     * to the double-click window so single vs double can be told apart. Use for
     * keys whose single and double actions differ (K1-K3). */
    AK_KEY_GESTURE_SINGLE_DOUBLE,
} ak_key_gesture_mode_t;

/* Number of mappable inputs: K1..K6 plus the EC11 push "enc". */
#define AK_KEYMAP_SLOTS 7

typedef struct {
    ak_key_role_t         role;
    ak_key_gesture_mode_t gesture;
} ak_key_entry_t;

typedef struct {
    ak_key_entry_t entries[AK_KEYMAP_SLOTS]; /* index: k1..k6, then enc */
} ak_keymap_t;

/** Map a key id ("k1".."k6" / "enc") to its 0-based slot, or -1 if unknown. */
int ak_keymap_slot_for_id(const char *id);

/** Map a 0-based slot (0..AK_KEYMAP_SLOTS-1) back to its id string, or NULL. */
const char *ak_keymap_id_for_slot(int slot);

/** Fill `map` with the confirmed v0.3 defaults (K5=voice, K4=cancel, K6=confirm,
 *  K1-K3=chat with single/double detection, enc=none/edge). */
void ak_keymap_init_defaults(ak_keymap_t *map);

/** Role for a key id, or AK_KEY_ROLE_NONE for an unknown id. */
ak_key_role_t ak_keymap_role(const ak_keymap_t *map, const char *id);

/** Gesture-detection mode for a key id, or AK_KEY_GESTURE_EDGE for unknown. */
ak_key_gesture_mode_t ak_keymap_gesture(const ak_keymap_t *map, const char *id);

/** Parse a role string ("none"/"chat"/"voice"/"cancel"/"confirm") into a role.
 *  Returns false (and leaves *out untouched) for an unrecognized string, so a
 *  tolerant decoder can keep the key's current role. */
bool ak_keymap_role_from_str(const char *s, ak_key_role_t *out);

/** The gesture-detection mode a role implies: CHAT -> deferred single/double
 *  (K1-K3 need single vs double); every other role -> immediate edges. Keeps
 *  the downlink wire format to one field per key (role), gesture derived. */
ak_key_gesture_mode_t ak_keymap_gesture_for_role(ak_key_role_t role);

/** Apply one role (by id) to `map`, setting both the role and its implied
 *  gesture mode. No-op for an unknown id. Returns true if applied. */
bool ak_keymap_apply_role(ak_keymap_t *map, const char *id, ak_key_role_t role);

/* Compact KV blob: AK_KEYMAP_SLOTS bytes, one role per slot (k1..k6, enc).
 * A leading version byte guards format changes. */
#define AK_KEYMAP_BLOB_VERSION 1u
#define AK_KEYMAP_BLOB_SIZE    (1u + AK_KEYMAP_SLOTS) /* version + 7 roles */

/** Serialize `map` into a fixed AK_KEYMAP_BLOB_SIZE byte buffer for TAL KV.
 *  Returns the number of bytes written (AK_KEYMAP_BLOB_SIZE) or 0 on error. */
unsigned ak_keymap_to_blob(const ak_keymap_t *map, uint8_t *buf, unsigned buf_size);

/** Load `map` from a KV blob produced by ak_keymap_to_blob. Rejects a wrong
 *  version/size (returns false, leaving `map` untouched) so a stale/corrupt
 *  blob falls back to defaults rather than scrambling the table. */
bool ak_keymap_from_blob(ak_keymap_t *map, const uint8_t *buf, unsigned len);

#endif /* AK_KEYMAP_H */
