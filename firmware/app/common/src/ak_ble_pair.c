/**
 * @file ak_ble_pair.c
 * @brief Over-the-air BLE pairing with a keyboard confirmation. See ak_ble_pair.h.
 */

#include "ak_ble_pair.h"

#include <stdio.h>
#include <string.h>

#include "ak_rx.h"
#include "mbedtls/bignum.h"
#include "mbedtls/ecp.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"

#define PAIR_INFO   "AK-BLE-pair-v1"
#define KEYGEN_TRIES 8

static bool before(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(deadline_ms - now_ms) > 0;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1; /* lowercase only, as ak_ble_sec */
}

static bool unhex(const char *s, uint8_t *out, size_t len)
{
    if (strlen(s) != 2U * len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        int hi = hex_val(s[2 * i]);
        int lo = hex_val(s[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/* ── Primitives ─────────────────────────────────────────────────────────── */

int ak_ble_pair_pub(const uint8_t priv[AK_BLE_PAIR_PRIV_LEN], uint8_t pub[AK_BLE_PAIR_PUB_LEN], ak_ble_rand_fn rnd,
                    void *rnd_ctx)
{
    mbedtls_ecp_group grp;
    mbedtls_mpi       d;
    mbedtls_ecp_point q;
    size_t            olen = 0;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&q);
    int rc = (rnd == NULL) ? -1 : 0;
    if (rc == 0) {
        rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    }
    if (rc == 0) {
        rc = mbedtls_mpi_read_binary(&d, priv, AK_BLE_PAIR_PRIV_LEN);
    }
    if (rc == 0) {
        rc = mbedtls_ecp_check_privkey(&grp, &d);
    }
    if (rc == 0) {
        rc = mbedtls_ecp_mul(&grp, &q, &d, &grp.G, (int (*)(void *, unsigned char *, size_t))rnd, rnd_ctx);
    }
    if (rc == 0) {
        rc = mbedtls_ecp_point_write_binary(&grp, &q, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, pub, AK_BLE_PAIR_PUB_LEN);
    }
    if (rc == 0 && olen != AK_BLE_PAIR_PUB_LEN) {
        rc = -1;
    }
    mbedtls_ecp_point_free(&q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return rc;
}

int ak_ble_pair_ecdh(const uint8_t priv[AK_BLE_PAIR_PRIV_LEN], const uint8_t peer[AK_BLE_PAIR_PUB_LEN],
                     uint8_t shared[AK_BLE_PAIR_HASH_LEN], ak_ble_rand_fn rnd, void *rnd_ctx)
{
    mbedtls_ecp_group grp;
    mbedtls_mpi       d;
    mbedtls_ecp_point pt;
    mbedtls_ecp_point r;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&pt);
    mbedtls_ecp_point_init(&r);
    int rc = (rnd == NULL || peer[0] != 0x04U) ? -1 : 0; /* uncompressed points only */
    if (rc == 0) {
        rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    }
    if (rc == 0) {
        rc = mbedtls_mpi_read_binary(&d, priv, AK_BLE_PAIR_PRIV_LEN);
    }
    if (rc == 0) {
        rc = mbedtls_ecp_point_read_binary(&grp, &pt, peer, AK_BLE_PAIR_PUB_LEN);
    }
    if (rc == 0) {
        rc = mbedtls_ecp_check_pubkey(&grp, &pt); /* on the curve, not infinity */
    }
    if (rc == 0) {
        rc = mbedtls_ecp_mul(&grp, &r, &d, &pt, (int (*)(void *, unsigned char *, size_t))rnd, rnd_ctx);
    }
    uint8_t point[AK_BLE_PAIR_PUB_LEN];
    size_t  olen = 0;
    if (rc == 0) {
        rc = mbedtls_ecp_point_write_binary(&grp, &r, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, point, sizeof(point));
    }
    if (rc == 0 && olen == AK_BLE_PAIR_PUB_LEN) {
        memcpy(shared, &point[1], AK_BLE_PAIR_HASH_LEN); /* x coordinate */
    } else if (rc == 0) {
        rc = -1;
    }
    mbedtls_platform_zeroize(point, sizeof(point));
    mbedtls_ecp_point_free(&r);
    mbedtls_ecp_point_free(&pt);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return rc;
}

static void sha256_parts(const void *const parts[], const size_t lens[], size_t n, uint8_t out[AK_BLE_PAIR_HASH_LEN])
{
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    (void)mbedtls_sha256_starts(&c, 0);
    for (size_t i = 0; i < n; i++) {
        (void)mbedtls_sha256_update(&c, (const unsigned char *)parts[i], lens[i]);
    }
    (void)mbedtls_sha256_finish(&c, out);
    mbedtls_sha256_free(&c);
}

void ak_ble_pair_commit_c(const uint8_t pc[AK_BLE_PAIR_PUB_LEN], const uint8_t nc[AK_BLE_PAIR_NONCE_LEN],
                          uint8_t out[AK_BLE_PAIR_HASH_LEN])
{
    const void  *parts[] = {"AKPAIR-C", pc, nc};
    const size_t lens[]  = {8U, AK_BLE_PAIR_PUB_LEN, AK_BLE_PAIR_NONCE_LEN};
    sha256_parts(parts, lens, 3U, out);
}

void ak_ble_pair_commit_b(const uint8_t pb[AK_BLE_PAIR_PUB_LEN], const uint8_t pc[AK_BLE_PAIR_PUB_LEN],
                          const uint8_t nb[AK_BLE_PAIR_NONCE_LEN], uint8_t out[AK_BLE_PAIR_HASH_LEN])
{
    const void  *parts[] = {"AKPAIR-B", pb, pc, nb};
    const size_t lens[]  = {8U, AK_BLE_PAIR_PUB_LEN, AK_BLE_PAIR_PUB_LEN, AK_BLE_PAIR_NONCE_LEN};
    sha256_parts(parts, lens, 4U, out);
}

uint32_t ak_ble_pair_compute_code(const uint8_t pc[AK_BLE_PAIR_PUB_LEN], const uint8_t pb[AK_BLE_PAIR_PUB_LEN],
                                  const uint8_t nc[AK_BLE_PAIR_NONCE_LEN], const uint8_t nb[AK_BLE_PAIR_NONCE_LEN])
{
    uint8_t      h[AK_BLE_PAIR_HASH_LEN];
    const void  *parts[] = {"AKPAIR-V", pc, pb, nc, nb};
    const size_t lens[]  = {8U, AK_BLE_PAIR_PUB_LEN, AK_BLE_PAIR_PUB_LEN, AK_BLE_PAIR_NONCE_LEN, AK_BLE_PAIR_NONCE_LEN};
    sha256_parts(parts, lens, 5U, h);
    uint32_t v = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | (uint32_t)h[3];
    return v % AK_BLE_PAIR_CODE_MOD;
}

int ak_ble_pair_derive(const uint8_t shared[AK_BLE_PAIR_HASH_LEN], const uint8_t nc[AK_BLE_PAIR_NONCE_LEN],
                       const uint8_t nb[AK_BLE_PAIR_NONCE_LEN], uint8_t key[AK_BLE_KEY_LEN])
{
    uint8_t salt[2U * AK_BLE_PAIR_NONCE_LEN];
    memcpy(salt, nc, AK_BLE_PAIR_NONCE_LEN);
    memcpy(salt + AK_BLE_PAIR_NONCE_LEN, nb, AK_BLE_PAIR_NONCE_LEN);
    return mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), salt, sizeof(salt), shared,
                        AK_BLE_PAIR_HASH_LEN, (const unsigned char *)PAIR_INFO, strlen(PAIR_INFO), key,
                        AK_BLE_KEY_LEN);
}

/* ── Window and exchange ───────────────────────────────────────────────── */

static void wipe_session(ak_ble_pair_t *p)
{
    p->user_ok = false;
    p->peer_ok = false;
    p->code    = 0U;
    mbedtls_platform_zeroize(p->pc, sizeof(p->pc));
    mbedtls_platform_zeroize(p->pb, sizeof(p->pb));
    mbedtls_platform_zeroize(p->cc, sizeof(p->cc));
    mbedtls_platform_zeroize(p->shared, sizeof(p->shared));
    mbedtls_platform_zeroize(p->nb, sizeof(p->nb));
}

void ak_ble_pair_init(ak_ble_pair_t *p)
{
    mbedtls_platform_zeroize(p, sizeof(*p));
    p->state = AK_BLE_PAIR_CLOSED;
}

void ak_ble_pair_open(ak_ble_pair_t *p, uint32_t now_ms)
{
    ak_ble_pair_init(p);
    p->state              = AK_BLE_PAIR_WAITING;
    p->window_deadline_ms = now_ms + AK_BLE_PAIR_WINDOW_MS;
}

void ak_ble_pair_close(ak_ble_pair_t *p)
{
    ak_ble_pair_init(p);
}

bool ak_ble_pair_is_open(const ak_ble_pair_t *p, uint32_t now_ms)
{
    if (p->state == AK_BLE_PAIR_CLOSED) {
        return false;
    }
    /* An exchange already under way may finish past the window's end. */
    return p->state != AK_BLE_PAIR_WAITING || before(now_ms, p->window_deadline_ms);
}

uint32_t ak_ble_pair_remaining_s(const ak_ble_pair_t *p, uint32_t now_ms)
{
    if (p->state == AK_BLE_PAIR_CLOSED || !before(now_ms, p->window_deadline_ms)) {
        return 0U;
    }
    return (p->window_deadline_ms - now_ms + 999U) / 1000U;
}

bool ak_ble_pair_owns_line(const char *line)
{
    ak_rx_frame_t f;
    return line != NULL && ak_rx_parse_frame(line, &f) && strncmp(f.type, "sec_pair_", 9) == 0;
}

uint32_t ak_ble_pair_code(const ak_ble_pair_t *p)
{
    return p->state == AK_BLE_PAIR_COMPARE ? p->code : 0U;
}

bool ak_ble_pair_take_key(ak_ble_pair_t *p, uint8_t key[AK_BLE_KEY_LEN])
{
    static const uint8_t zero[AK_BLE_KEY_LEN] = {0};
    if (memcmp(p->key, zero, sizeof(zero)) == 0) {
        return false;
    }
    memcpy(key, p->key, AK_BLE_KEY_LEN);
    mbedtls_platform_zeroize(p->key, sizeof(p->key));
    return true;
}

static int put_reply(char *reply, size_t cap, const char *type, const char *payload)
{
    if (reply == NULL || cap == 0U) {
        return -1;
    }
    int n = snprintf(reply, cap, "{\"v\":1,\"type\":\"%s\",\"payload\":%s}\n", type, payload);
    return (n > 0 && (size_t)n < cap) ? n : -1;
}

/* End this attempt. `counted` failures (wrong commitment, K1, no answer to
 * the code) close the window at the third. Any other failure only ends the
 * attempt and holds the next request back for AK_BLE_PAIR_RETRY_MS
 * (ProtoShield R6a). `reason` NULL: no reply (the link is gone). */
static ak_ble_pair_act_t fail_attempt(ak_ble_pair_t *p, uint32_t now_ms, bool counted, const char *reason,
                                      char *reply, size_t cap)
{
    wipe_session(p);
    mbedtls_platform_zeroize(p->key, sizeof(p->key));
    if (counted) {
        p->fails++;
    } else {
        p->retry_wait  = true;
        p->retry_at_ms = now_ms + AK_BLE_PAIR_RETRY_MS;
    }
    p->state = p->fails >= AK_BLE_PAIR_MAX_FAILS ? AK_BLE_PAIR_CLOSED : AK_BLE_PAIR_WAITING;
    if (reply != NULL && cap > 0U) {
        reply[0] = '\0';
    }
    if (reason != NULL) {
        char payload[48];
        (void)snprintf(payload, sizeof(payload), "{\"reason\":\"%s\"}", reason);
        (void)put_reply(reply, cap, "sec_pair_fail", payload);
    }
    return AK_BLE_PAIR_ACT_FAILED;
}

/* A failure that ends the attempt but does not count towards the three. */
static ak_ble_pair_act_t fail(ak_ble_pair_t *p, uint32_t now_ms, const char *reason, char *reply, size_t cap)
{
    return fail_attempt(p, now_ms, false, reason, reply, cap);
}

/* Both sides confirmed: hand K to the app to store. sec_pair_done is sent
 * only by ak_ble_pair_stored, after the KV write succeeded (ProtoShield R6b). */
static ak_ble_pair_act_t finish(ak_ble_pair_t *p, char *reply, size_t cap)
{
    if (reply != NULL && cap > 0U) {
        reply[0] = '\0';
    }
    ak_ble_key_id(p->key, p->key_id);
    wipe_session(p);
    p->state = AK_BLE_PAIR_STORING;
    return AK_BLE_PAIR_ACT_STORE;
}

ak_ble_pair_act_t ak_ble_pair_stored(ak_ble_pair_t *p, bool ok, uint32_t now_ms, char *reply, size_t reply_cap)
{
    if (reply != NULL && reply_cap > 0U) {
        reply[0] = '\0';
    }
    if (p->state != AK_BLE_PAIR_STORING) {
        return AK_BLE_PAIR_ACT_NONE;
    }
    if (!ok) {
        return fail(p, now_ms, "storage", reply, reply_cap);
    }
    char payload[40];
    (void)snprintf(payload, sizeof(payload), "{\"key_id\":\"%s\"}", p->key_id);
    int n = put_reply(reply, reply_cap, "sec_pair_done", payload);
    ak_ble_pair_init(p); /* one pairing per window */
    return n < 0 ? AK_BLE_PAIR_ACT_FAILED : AK_BLE_PAIR_ACT_DONE;
}

static ak_ble_pair_act_t on_req(ak_ble_pair_t *p, const char *payload, uint32_t now_ms, ak_ble_rand_fn rnd,
                                void *rnd_ctx, char *reply, size_t cap)
{
    char    pc_hex[2U * AK_BLE_PAIR_PUB_LEN + 1U];
    char    cc_hex[2U * AK_BLE_PAIR_HASH_LEN + 1U];
    uint8_t priv[AK_BLE_PAIR_PRIV_LEN];
    if (ak_json_get_str(payload, "pc", pc_hex, sizeof(pc_hex)) != AK_JSON_OK ||
        ak_json_get_str(payload, "cc", cc_hex, sizeof(cc_hex)) != AK_JSON_OK || !unhex(pc_hex, p->pc, sizeof(p->pc)) ||
        !unhex(cc_hex, p->cc, sizeof(p->cc)) || rnd == NULL) {
        return fail(p, now_ms, "bad_request", reply, cap);
    }
    int rc = -1;
    for (int i = 0; i < KEYGEN_TRIES && rc != 0; i++) {
        rc = rnd(rnd_ctx, priv, sizeof(priv));
        if (rc == 0) {
            rc = ak_ble_pair_pub(priv, p->pb, rnd, rnd_ctx); /* fails for an out-of-range scalar: retry */
        }
    }
    if (rc == 0) {
        rc = ak_ble_pair_ecdh(priv, p->pc, p->shared, rnd, rnd_ctx);
    }
    mbedtls_platform_zeroize(priv, sizeof(priv));
    if (rc == 0) {
        rc = rnd(rnd_ctx, p->nb, sizeof(p->nb));
    }
    if (rc != 0) {
        return fail(p, now_ms, "bad_key", reply, cap); /* also: the Companion key is not a curve point */
    }
    uint8_t cb[AK_BLE_PAIR_HASH_LEN];
    char    pb_hex[2U * AK_BLE_PAIR_PUB_LEN + 1U];
    char    cb_hex[2U * AK_BLE_PAIR_HASH_LEN + 1U];
    char    payload_out[2U * AK_BLE_PAIR_PUB_LEN + 2U * AK_BLE_PAIR_HASH_LEN + 32U];
    ak_ble_pair_commit_b(p->pb, p->pc, p->nb, cb);
    ak_ble_hex(p->pb, sizeof(p->pb), pb_hex);
    ak_ble_hex(cb, sizeof(cb), cb_hex);
    (void)snprintf(payload_out, sizeof(payload_out), "{\"pb\":\"%s\",\"cb\":\"%s\"}", pb_hex, cb_hex);
    if (put_reply(reply, cap, "sec_pair_rsp", payload_out) < 0) {
        return fail(p, now_ms, "internal", reply, cap);
    }
    p->state            = AK_BLE_PAIR_WAIT_NONCE;
    p->step_deadline_ms = now_ms + AK_BLE_PAIR_STEP_MS;
    return AK_BLE_PAIR_ACT_REPLY;
}

static ak_ble_pair_act_t on_nonce(ak_ble_pair_t *p, const char *payload, uint32_t now_ms, char *reply, size_t cap)
{
    char    nc_hex[2U * AK_BLE_PAIR_NONCE_LEN + 1U];
    uint8_t nc[AK_BLE_PAIR_NONCE_LEN];
    uint8_t cc[AK_BLE_PAIR_HASH_LEN];
    if (ak_json_get_str(payload, "nc", nc_hex, sizeof(nc_hex)) != AK_JSON_OK || !unhex(nc_hex, nc, sizeof(nc))) {
        return fail(p, now_ms, "bad_request", reply, cap);
    }
    ak_ble_pair_commit_c(p->pc, nc, cc);
    /* Not secret-dependent timing: both values are public once nc is sent. */
    if (memcmp(cc, p->cc, sizeof(cc)) != 0) {
        return fail_attempt(p, now_ms, true, "bad_commitment", reply, cap); /* wrong peer or attack: counted */
    }
    if (ak_ble_pair_derive(p->shared, nc, p->nb, p->key) != 0) {
        return fail(p, now_ms, "internal", reply, cap);
    }
    p->code = ak_ble_pair_compute_code(p->pc, p->pb, nc, p->nb);
    mbedtls_platform_zeroize(p->shared, sizeof(p->shared));
    char nb_hex[2U * AK_BLE_PAIR_NONCE_LEN + 1U];
    char payload_out[64];
    ak_ble_hex(p->nb, sizeof(p->nb), nb_hex);
    (void)snprintf(payload_out, sizeof(payload_out), "{\"nb\":\"%s\"}", nb_hex);
    if (put_reply(reply, cap, "sec_pair_nonce", payload_out) < 0) {
        mbedtls_platform_zeroize(p->key, sizeof(p->key));
        return fail(p, now_ms, "internal", reply, cap);
    }
    p->state            = AK_BLE_PAIR_COMPARE;
    p->step_deadline_ms = now_ms + AK_BLE_PAIR_STEP_MS;
    return AK_BLE_PAIR_ACT_SHOW;
}

ak_ble_pair_act_t ak_ble_pair_on_line(ak_ble_pair_t *p, const char *line, uint32_t now_ms, ak_ble_rand_fn rnd,
                                      void *rnd_ctx, char *reply, size_t reply_cap)
{
    ak_rx_frame_t f;
    if (reply != NULL && reply_cap > 0U) {
        reply[0] = '\0';
    }
    if (p == NULL || line == NULL || !ak_ble_pair_is_open(p, now_ms)) {
        return AK_BLE_PAIR_ACT_NONE; /* outside the window: silent, no dialog */
    }
    if (!ak_rx_parse_frame(line, &f)) {
        return fail(p, now_ms, "bad_request", reply, reply_cap);
    }
    if (strcmp(f.type, "sec_pair_cancel") == 0) {
        return p->state == AK_BLE_PAIR_WAITING ? AK_BLE_PAIR_ACT_NONE : fail(p, now_ms, "cancelled", reply, reply_cap);
    }
    switch (p->state) {
    case AK_BLE_PAIR_WAITING:
        if (strcmp(f.type, "sec_pair_req") == 0) {
            if (p->retry_wait && before(now_ms, p->retry_at_ms)) {
                return AK_BLE_PAIR_ACT_NONE; /* after an uncounted failure: wait, no P-256 work */
            }
            p->retry_wait = false;
            return on_req(p, f.payload, now_ms, rnd, rnd_ctx, reply, reply_cap);
        }
        break;
    case AK_BLE_PAIR_WAIT_NONCE:
        if (strcmp(f.type, "sec_pair_nonce") == 0) {
            return on_nonce(p, f.payload, now_ms, reply, reply_cap);
        }
        break;
    case AK_BLE_PAIR_COMPARE:
        if (strcmp(f.type, "sec_pair_confirm") == 0) {
            p->peer_ok = true;
            return p->user_ok ? finish(p, reply, reply_cap) : AK_BLE_PAIR_ACT_NONE;
        }
        break;
    case AK_BLE_PAIR_STORING:
        return AK_BLE_PAIR_ACT_NONE; /* the KV write is under way */
    case AK_BLE_PAIR_CLOSED:
    default:
        return AK_BLE_PAIR_ACT_NONE;
    }
    return fail(p, now_ms, "unexpected", reply, reply_cap);
}

ak_ble_pair_act_t ak_ble_pair_user(ak_ble_pair_t *p, bool confirm, uint32_t now_ms, char *reply, size_t reply_cap)
{
    if (reply != NULL && reply_cap > 0U) {
        reply[0] = '\0';
    }
    if (p->state != AK_BLE_PAIR_COMPARE) {
        return AK_BLE_PAIR_ACT_NONE;
    }
    if (!confirm) {
        return fail_attempt(p, now_ms, true, "cancelled", reply, reply_cap); /* K1: counted */
    }
    p->user_ok = true;
    return p->peer_ok ? finish(p, reply, reply_cap) : AK_BLE_PAIR_ACT_NONE;
}

ak_ble_pair_act_t ak_ble_pair_tick(ak_ble_pair_t *p, uint32_t now_ms, char *reply, size_t reply_cap)
{
    if (reply != NULL && reply_cap > 0U) {
        reply[0] = '\0';
    }
    switch (p->state) {
    case AK_BLE_PAIR_WAITING:
        if (!before(now_ms, p->window_deadline_ms)) {
            ak_ble_pair_close(p);
        }
        return AK_BLE_PAIR_ACT_NONE;
    case AK_BLE_PAIR_WAIT_NONCE:
    case AK_BLE_PAIR_COMPARE:
        if (!before(now_ms, p->step_deadline_ms)) {
            /* Only an unanswered code counts: a peer can stall before it for free. */
            ak_ble_pair_act_t act =
                fail_attempt(p, now_ms, p->state == AK_BLE_PAIR_COMPARE, "timeout", reply, reply_cap);
            if (p->state == AK_BLE_PAIR_WAITING && !before(now_ms, p->window_deadline_ms)) {
                p->state = AK_BLE_PAIR_CLOSED; /* the window ran out meanwhile */
            }
            return act;
        }
        return AK_BLE_PAIR_ACT_NONE;
    case AK_BLE_PAIR_CLOSED:
    default:
        return AK_BLE_PAIR_ACT_NONE;
    }
}

ak_ble_pair_act_t ak_ble_pair_link_down(ak_ble_pair_t *p)
{
    if (p->state != AK_BLE_PAIR_WAIT_NONCE && p->state != AK_BLE_PAIR_COMPARE) {
        return AK_BLE_PAIR_ACT_NONE;
    }
    ak_ble_pair_act_t act = fail(p, 0U, NULL, NULL, 0U);
    p->retry_wait         = false; /* no clock here; the next link may try at once */
    return act;
}
