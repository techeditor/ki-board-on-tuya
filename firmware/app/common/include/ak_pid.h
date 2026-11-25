/**
 * @file ak_pid.h
 * @brief Runtime Tuya product ID (PID): which value is in effect and when a
 *        new one may be written. Pure logic, host-tested.
 *
 * The PID is not a secret, but it must not change under a bound device. A
 * public firmware is built with an empty CONFIG_TUYA_PRODUCT_ID; the owner
 * writes their own PID once over USB:
 *   - effective PID = the one in KV, else the compile-time one, else none;
 *   - a write is accepted only while no effective PID exists, only over
 *     UART0/USB, and only for 16 chars of [a-z0-9];
 *   - there is no command to clear or replace it, and unbinding / network
 *     resets never touch it; only Settings > Reset system deletes the KV
 *     value (ak_factory_reset.c), and the board restarts right after, so the
 *     PID in RAM is never used without it (back to the build PID, or none);
 *   - the written PID takes effect after a restart (tuya_iot_init keeps the
 *     pointer it is given, so the effective PID never changes while running).
 */

#ifndef AK_PID_H
#define AK_PID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ak_rx.h"

#define AK_PID_LEN    16U
#define AK_PID_KV_KEY "ak_pid"

typedef enum {
    AK_PID_SRC_NONE = 0,
    AK_PID_SRC_KV,
    AK_PID_SRC_BUILD,
} ak_pid_src_t;

typedef enum {
    AK_PID_WRITE_OK = 0,
    AK_PID_WRITE_SOURCE,  /* did not arrive over UART0/USB */
    AK_PID_WRITE_EXISTS,  /* a PID is already in effect (or stored, pending restart) */
    AK_PID_WRITE_INVALID, /* not 16 chars of [a-z0-9], or the template placeholder */
    AK_PID_WRITE_STORAGE, /* accepted, but the KV write failed (firmware only) */
} ak_pid_verdict_t;

/** 16 chars of [a-z0-9] that is not the "xxxxxxxxxxxxxxxx" template value. */
bool ak_pid_valid(const char *pid);

/**
 * Pick the effective PID. `kv` is the stored blob (exactly the 16 characters,
 * no terminator; anything else is ignored), `build` the compile-time value
 * (may be NULL, empty or invalid, which counts as none). Writes the result
 * and a terminator to `out` ("" for none).
 */
ak_pid_src_t ak_pid_resolve(const uint8_t *kv, size_t kv_len, const char *build,
                            char out[AK_PID_LEN + 1U]);

/** May `candidate` be written now? `have_pid`: an effective or stored PID exists. */
ak_pid_verdict_t ak_pid_check_write(ak_rx_source_t source, const char *candidate, bool have_pid);

/** Wire reason for a refusal: "source" / "exists" / "invalid" / "storage" ("" for OK). */
const char *ak_pid_verdict_str(ak_pid_verdict_t verdict);

/* ── State machine over a KV (pure: the KV is passed in, host-tested) ────────
 *
 * The write is the only way the device becomes "has a PID" at runtime, and it
 * locks only when the value is really stored:
 *   1. the request must pass ak_pid_check_write (USB, no PID yet, valid);
 *   2. the value is written to KV, read back and compared;
 *   3. only if that succeeded does the state become "pending" (locked).
 * Any failure (invalid, KV write error, read-back error or mismatch) leaves the
 * state unlocked so the PID can be written again; a read-back mismatch also
 * deletes the key so the next boot cannot pick up a wrong value. A half
 * written value (power loss, wrong length or characters) is "no PID" at boot. */

typedef struct {
    /* All return 0 on success. `get` copies at most `cap` bytes and sets *len. */
    int (*set)(const char *key, const uint8_t *value, size_t len, void *ctx);
    int (*get)(const char *key, uint8_t *out, size_t cap, size_t *len, void *ctx);
    int (*del)(const char *key, void *ctx);
    void *ctx;
} ak_pid_kv_ops_t;

typedef struct {
    char pid[AK_PID_LEN + 1U];     /* in effect this boot, "" when none */
    char pending[AK_PID_LEN + 1U]; /* written and verified this boot, effective after restart */
} ak_pid_state_t;

/** Boot: KV first (only a whole, valid value counts), else the build value. */
ak_pid_src_t ak_pid_state_load(ak_pid_state_t *st, const ak_pid_kv_ops_t *kv, const char *build);

/** A PID is in effect, or one was written and verified this boot. */
bool ak_pid_state_known(const ak_pid_state_t *st);

/** Check, store, read back, verify; lock only on success. */
ak_pid_verdict_t ak_pid_state_write(ak_pid_state_t *st, const ak_pid_kv_ops_t *kv, ak_rx_source_t source,
                                    const char *candidate);

/* ── Firmware side (ak_pid_store.c; needs KV, not built for the host) ─────── */

/** Read KV + build value once at boot (after ak_kv_init). Safe to call twice. */
void ak_pid_load(void);

/** The effective PID for this boot ("" when none). The pointer and contents
 *  stay valid and unchanged until the next restart. */
const char *ak_pid_get(void);

/** True when a PID is in effect or one was written and awaits the restart. */
bool ak_pid_known(void);

/** True when a PID was written this boot and only takes effect after restart. */
bool ak_pid_pending_restart(void);

/** Check and store `pid`. AK_PID_WRITE_OK means it is stored and takes effect
 *  after a restart. */
ak_pid_verdict_t ak_pid_write(ak_rx_source_t source, const char *candidate);

#endif /* AK_PID_H */
