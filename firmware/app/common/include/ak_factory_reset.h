/**
 * @file ak_factory_reset.h
 * @brief Settings > Reset system: two confirms, then every user setting back
 *        to default, every pairing and the Tuya License (UUID/AuthKey) gone:
 *        the board boots as a new board without a License.
 *
 * Pure logic (time and KV access come from the caller), host-tested.
 *
 * Confirm flow: ak_freset_request opens dialog 1 ("Reset system?"). K6 moves
 * to dialog 2 (the list of what is cleared), K6 again runs the reset. K1, an
 * unanswered dialog (AK_FRESET_CONFIRM_MS each) or any other cancel at either
 * step changes nothing.
 *
 * What is cleared is a fixed list of KV key names, deleted one by one. Nothing
 * here erases a KV partition or any raw flash (the boot splash at
 * 0x7ce000-0x7d8000 is never touched). The USB-written product ID (ak_pid)
 * is cleared too, so the board falls back to the build-time PID. The manufacturing
 * certificate and the anti-downgrade guard are on a keep list that
 * ak_freset_wipe refuses to delete even if a name is added to the clear list
 * by mistake.
 */

#ifndef AK_FACTORY_RESET_H
#define AK_FACTORY_RESET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AK_FRESET_CONFIRM_MS 10000U /* each dialog: unanswered = cancel (same as the other ⑦ dialogs) */

typedef enum {
    AK_FRESET_IDLE = 0,
    AK_FRESET_ASK1, /* "Reset system?" */
    AK_FRESET_ASK2, /* list of what is cleared, last chance */
} ak_freset_stage_t;

typedef struct {
    ak_freset_stage_t stage;
    uint32_t          deadline_ms;
} ak_freset_t;

typedef enum {
    AK_FRESET_NONE = 0,  /* nothing to do (no dialog up, or it ran out) */
    AK_FRESET_NEXT,      /* dialog 1 confirmed: dialog 2 is up */
    AK_FRESET_RUN,       /* dialog 2 confirmed: run the reset now */
    AK_FRESET_CANCELLED, /* K1 at either step: nothing changed */
} ak_freset_result_t;

void ak_freset_init(ak_freset_t *f);
/** Open dialog 1. False when one is already up. */
bool ak_freset_request(ak_freset_t *f, uint32_t now_ms);
/** The dialog up now (IDLE once it ran out; the call also clears an expired one). */
ak_freset_stage_t ak_freset_stage(ak_freset_t *f, uint32_t now_ms);
/** K6 (`confirm`) or K1 on the dialog that is up. */
ak_freset_result_t ak_freset_resolve(ak_freset_t *f, bool confirm, uint32_t now_ms);
/** Drop any dialog (e.g. another ⑦ took over). */
void ak_freset_cancel(ak_freset_t *f);

/* ── KV plan ──────────────────────────────────────────────────────────── */

typedef struct {
    const char *key;
    bool        clear;  /* true: deleted by the reset; false: kept */
    const char *reason; /* for docs and logs */
} ak_freset_key_t;

/** The full plan, every key the firmware or the SDK keeps that matters here. */
const ak_freset_key_t *ak_freset_plan(size_t *count);

/** True when `key` is on the keep list (factory certificate, OTA rollback guard). */
bool ak_freset_key_kept(const char *key);

/** Delete callback: returns 0 when the key is gone (deleting a missing key counts as gone). */
typedef int (*ak_freset_del_fn)(const char *key, void *ctx);

/**
 * Delete every key on the clear list through `del`, in plan order. Keys on
 * the keep list are never passed to `del`. Returns the number of keys whose
 * delete failed (0 = all cleared).
 */
size_t ak_freset_wipe(ak_freset_del_fn del, void *ctx);

#endif /* AK_FACTORY_RESET_H */
