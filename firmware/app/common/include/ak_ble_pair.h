/**
 * @file ak_ble_pair.h
 * @brief Over-the-air BLE pairing with a keyboard confirmation (no USB, no
 *        Tuya cloud). Produces the same 32-byte pairing key K that USB
 *        `ble_pair` creates; the link then uses the ak_ble_sec channel.
 *
 * Modelled on BLE LE Secure Connections numeric comparison, in the
 * application layer (TuyaOpen's BLE stack has no SMP). All cryptography is
 * mbedTLS: P-256 (ECP), SHA-256, HKDF-SHA256.
 *
 * The keyboard opens a pairing window (Settings > BLE pairing, 120 s). Outside
 * it every sec_pair_* line is dropped without a reply or a dialog. One pairing
 * per window. Three failures that involve the user or prove a wrong peer close
 * it: a commitment that does not verify, K1 on the code dialog, or no answer
 * while the code is shown. Malformed or out-of-turn frames, a bad key, a
 * Companion cancel, a dropped link or a time-out before the code is shown only
 * end the attempt (ProtoShield R6a); the next sec_pair_req is then accepted
 * after AK_BLE_PAIR_RETRY_MS, so a flood cannot keep the CPU on P-256.
 *
 *   C->B sec_pair_req   {"pc":hex65, "cc":hex32}  cc = SHA-256("AKPAIR-C"||Pc||Nc)
 *   B->C sec_pair_rsp   {"pb":hex65, "cb":hex32}  cb = SHA-256("AKPAIR-B"||Pb||Pc||Nb)
 *   C->B sec_pair_nonce {"nc":hex16}              board checks cc
 *   B->C sec_pair_nonce {"nb":hex16}              Companion checks cb
 *   both: code = BE32(SHA-256("AKPAIR-V"||Pc||Pb||Nc||Nb)[0..3]) mod 10^6
 *         K    = HKDF-SHA256(salt = Nc||Nb, IKM = ECDH x, info = "AK-BLE-pair-v1")
 *   user compares the 6 digits on the LCD and in the Companion:
 *   C->B sec_pair_confirm {}   (Companion user clicked confirm)
 *   keyboard K6 + Companion confirm within 30 s: the keyboard stores K first,
 *   B->C sec_pair_done  {"key_id":hex4}           only after K is stored; Companion stores K
 *   any failure (including the keyboard's KV write, reason "storage"):
 *   B->C sec_pair_fail  {"reason":"..."}          (Companion may send sec_pair_cancel)
 *
 * Pure apart from mbedTLS; host-tested against shared/protocol/ble-pair-vectors.txt,
 * the same vectors the Companion tests use.
 */

#ifndef AK_BLE_PAIR_H
#define AK_BLE_PAIR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ak_ble_sec.h"

#define AK_BLE_PAIR_WINDOW_MS  120000U /* Settings > BLE pairing */
#define AK_BLE_PAIR_STEP_MS    30000U  /* request -> code shown -> both confirmed */
#define AK_BLE_PAIR_MAX_FAILS  3U      /* counted failures (see above) close the window */
#define AK_BLE_PAIR_RETRY_MS   1000U   /* after an uncounted failure, requests wait this long */
#define AK_BLE_PAIR_PUB_LEN    65U /* uncompressed P-256 point */
#define AK_BLE_PAIR_PRIV_LEN   32U
#define AK_BLE_PAIR_HASH_LEN   32U
#define AK_BLE_PAIR_NONCE_LEN  16U
#define AK_BLE_PAIR_CODE_MOD   1000000U

typedef enum {
    AK_BLE_PAIR_CLOSED = 0,  /* no window: every sec_pair_* line is dropped */
    AK_BLE_PAIR_WAITING,     /* window open, waiting for sec_pair_req */
    AK_BLE_PAIR_WAIT_NONCE,  /* sec_pair_rsp sent, waiting for the Companion nonce */
    AK_BLE_PAIR_COMPARE,     /* code shown: waiting for K6 and the Companion confirm */
    AK_BLE_PAIR_STORING,     /* both confirmed: the app stores K, then calls ak_ble_pair_stored */
} ak_ble_pair_state_t;

typedef struct {
    ak_ble_pair_state_t state;
    uint32_t            window_deadline_ms;
    uint32_t            step_deadline_ms;
    uint8_t             fails;
    bool                retry_wait;  /* an uncounted failure: sec_pair_req waits until retry_at_ms */
    uint32_t            retry_at_ms;
    bool                user_ok;
    bool                peer_ok;
    uint32_t            code;
    uint8_t             pc[AK_BLE_PAIR_PUB_LEN];
    uint8_t             pb[AK_BLE_PAIR_PUB_LEN];
    uint8_t             cc[AK_BLE_PAIR_HASH_LEN];
    uint8_t             shared[AK_BLE_PAIR_HASH_LEN];
    uint8_t             nb[AK_BLE_PAIR_NONCE_LEN];
    uint8_t             key[AK_BLE_KEY_LEN];
    char                key_id[AK_BLE_KEY_ID_HEX]; /* of K, for sec_pair_done after the KV write */
} ak_ble_pair_t;

typedef enum {
    AK_BLE_PAIR_ACT_NONE = 0, /* nothing to send or show */
    AK_BLE_PAIR_ACT_REPLY,    /* send `reply` on BLE */
    AK_BLE_PAIR_ACT_SHOW,     /* send `reply`, then show the code dialog (ak_ble_pair_code) */
    AK_BLE_PAIR_ACT_STORE,    /* nothing sent yet: store K (ak_ble_pair_take_key), then ak_ble_pair_stored */
    AK_BLE_PAIR_ACT_DONE,     /* send `reply` (sec_pair_done): K is stored; window closed */
    AK_BLE_PAIR_ACT_FAILED,   /* send `reply` (may be empty); the attempt ended */
} ak_ble_pair_act_t;

/* ── Primitives (exposed for the test vectors) ─────────────────────────── */

/** Public key of `priv`. 0 on success. `rnd` blinds the multiplication. */
int ak_ble_pair_pub(const uint8_t priv[AK_BLE_PAIR_PRIV_LEN], uint8_t pub[AK_BLE_PAIR_PUB_LEN], ak_ble_rand_fn rnd,
                    void *rnd_ctx);
/** ECDH x coordinate of priv * peer. 0 on success; a point not on the curve fails. */
int ak_ble_pair_ecdh(const uint8_t priv[AK_BLE_PAIR_PRIV_LEN], const uint8_t peer[AK_BLE_PAIR_PUB_LEN],
                     uint8_t shared[AK_BLE_PAIR_HASH_LEN], ak_ble_rand_fn rnd, void *rnd_ctx);
void     ak_ble_pair_commit_c(const uint8_t pc[AK_BLE_PAIR_PUB_LEN], const uint8_t nc[AK_BLE_PAIR_NONCE_LEN],
                              uint8_t out[AK_BLE_PAIR_HASH_LEN]);
void     ak_ble_pair_commit_b(const uint8_t pb[AK_BLE_PAIR_PUB_LEN], const uint8_t pc[AK_BLE_PAIR_PUB_LEN],
                              const uint8_t nb[AK_BLE_PAIR_NONCE_LEN], uint8_t out[AK_BLE_PAIR_HASH_LEN]);
uint32_t ak_ble_pair_compute_code(const uint8_t pc[AK_BLE_PAIR_PUB_LEN], const uint8_t pb[AK_BLE_PAIR_PUB_LEN],
                                  const uint8_t nc[AK_BLE_PAIR_NONCE_LEN], const uint8_t nb[AK_BLE_PAIR_NONCE_LEN]);
int      ak_ble_pair_derive(const uint8_t shared[AK_BLE_PAIR_HASH_LEN], const uint8_t nc[AK_BLE_PAIR_NONCE_LEN],
                            const uint8_t nb[AK_BLE_PAIR_NONCE_LEN], uint8_t key[AK_BLE_KEY_LEN]);

/* ── Window and exchange ───────────────────────────────────────────────── */

void ak_ble_pair_init(ak_ble_pair_t *p);
/** Settings > BLE pairing: open (or restart) the 120 s window. */
void ak_ble_pair_open(ak_ble_pair_t *p, uint32_t now_ms);
/** Close the window and wipe the session. */
void ak_ble_pair_close(ak_ble_pair_t *p);
bool ak_ble_pair_is_open(const ak_ble_pair_t *p, uint32_t now_ms);
/** Seconds left in the window (0 when closed). */
uint32_t ak_ble_pair_remaining_s(const ak_ble_pair_t *p, uint32_t now_ms);

/** True for the frame types this module owns (sec_pair_*). */
bool ak_ble_pair_owns_line(const char *line);

/** One BLE line of type sec_pair_*. Outside the window: NONE, no reply. */
ak_ble_pair_act_t ak_ble_pair_on_line(ak_ble_pair_t *p, const char *line, uint32_t now_ms, ak_ble_rand_fn rnd,
                                      void *rnd_ctx, char *reply, size_t reply_cap);
/** K6 (confirm) or K1 (cancel) on the code dialog. */
ak_ble_pair_act_t ak_ble_pair_user(ak_ble_pair_t *p, bool confirm, uint32_t now_ms, char *reply, size_t reply_cap);
/** Once per loop: step and window time-outs. */
ak_ble_pair_act_t ak_ble_pair_tick(ak_ble_pair_t *p, uint32_t now_ms, char *reply, size_t reply_cap);
/** The BLE central went away: an exchange in progress fails (no reply). */
ak_ble_pair_act_t ak_ble_pair_link_down(ak_ble_pair_t *p);

/** The 6-digit code while the dialog is up. */
uint32_t ak_ble_pair_code(const ak_ble_pair_t *p);
/** After ACT_STORE: copy K out once and wipe it here. False when there is none. */
bool ak_ble_pair_take_key(ak_ble_pair_t *p, uint8_t key[AK_BLE_KEY_LEN]);

/** After ACT_STORE: the KV write of K succeeded (`ok`) or failed. Success
 *  replies sec_pair_done and closes the window (ACT_DONE). Failure replies
 *  sec_pair_fail "storage", so the Companion never stores K, and keeps the
 *  window open for another attempt (ACT_FAILED, not counted). */
ak_ble_pair_act_t ak_ble_pair_stored(ak_ble_pair_t *p, bool ok, uint32_t now_ms, char *reply, size_t reply_cap);

#endif /* AK_BLE_PAIR_H */
