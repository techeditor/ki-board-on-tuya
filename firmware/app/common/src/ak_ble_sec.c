/**
 * @file ak_ble_sec.c
 * @brief BLE secure channel: handshake + AES-128-CCM framing (mbedTLS).
 */

#include "ak_ble_sec.h"

#include <stdio.h>
#include <string.h>

#include "ak_rx.h"
#include "mbedtls/base64.h"
#include "mbedtls/ccm.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"

#define CCM_NONCE_LEN 13U
#define SESSION_INFO  "AK-BLE-v1"
/* Largest decoded sealed frame: a BLE ak_rx line is < AK_RX_SEALED_LINE_MAX chars. */
#define RAW_MAX       ((AK_RX_SEALED_LINE_MAX / 4U) * 3U)

void ak_ble_sec_reset(ak_ble_sec_t *s)
{
    mbedtls_platform_zeroize(s, sizeof(*s));
    s->state = AK_BLE_SEC_IDLE;
}

bool ak_ble_sec_authed(const ak_ble_sec_t *s)
{
    return s->state == AK_BLE_SEC_AUTHED;
}

void ak_ble_hex(const uint8_t *in, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i]     = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0x0FU];
    }
    out[2 * len] = '\0';
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1; /* lowercase only: one spelling per value */
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

void ak_ble_key_id(const uint8_t key[AK_BLE_KEY_LEN], char out[AK_BLE_KEY_ID_HEX])
{
    uint8_t digest[32];
    (void)mbedtls_sha256(key, AK_BLE_KEY_LEN, digest, 0);
    ak_ble_hex(digest, 4U, out);
}

void ak_ble_key_to_blob(const uint8_t key[AK_BLE_KEY_LEN], uint8_t blob[AK_BLE_KEY_BLOB_LEN])
{
    blob[0] = AK_BLE_KEY_BLOB_VER;
    memcpy(&blob[1], key, AK_BLE_KEY_LEN);
}

bool ak_ble_key_from_blob(const uint8_t *blob, size_t len, uint8_t key[AK_BLE_KEY_LEN])
{
    if (blob == NULL || len != AK_BLE_KEY_BLOB_LEN || blob[0] != AK_BLE_KEY_BLOB_VER) {
        return false;
    }
    memcpy(key, &blob[1], AK_BLE_KEY_LEN);
    return true;
}

static void make_nonce(uint8_t dir, const uint8_t ctr[AK_BLE_CTR_LEN], uint8_t nonce[CCM_NONCE_LEN])
{
    memset(nonce, 0, CCM_NONCE_LEN);
    nonce[0] = dir;
    memcpy(&nonce[5], ctr, AK_BLE_CTR_LEN);
}

static void put_ctr(uint64_t v, uint8_t out[AK_BLE_CTR_LEN])
{
    for (int i = 7; i >= 0; i--) {
        out[i] = (uint8_t)(v & 0xFFU);
        v >>= 8;
    }
}

static uint64_t get_ctr(const uint8_t in[AK_BLE_CTR_LEN])
{
    uint64_t v = 0;
    for (size_t i = 0; i < AK_BLE_CTR_LEN; i++) {
        v = (v << 8) | in[i];
    }
    return v;
}

/* Verify and decrypt one sealed line from the Companion. */
static bool open_line(ak_ble_sec_t *s, const char *line, char *plain, size_t plain_cap)
{
    uint8_t raw[RAW_MAX];
    size_t  raw_len = 0;
    if (line[0] != '~' ||
        mbedtls_base64_decode(raw, sizeof(raw), &raw_len, (const unsigned char *)line + 1, strlen(line + 1)) != 0 ||
        raw_len < AK_BLE_CTR_LEN + AK_BLE_TAG_LEN) {
        return false;
    }
    size_t ct_len = raw_len - AK_BLE_CTR_LEN - AK_BLE_TAG_LEN;
    if (ct_len + 1U > plain_cap) {
        return false;
    }
    if (get_ctr(raw) != s->rx_ctr) {
        return false; /* replay, reorder or gap */
    }
    uint8_t nonce[CCM_NONCE_LEN];
    make_nonce(AK_BLE_DIR_C2B, raw, nonce);

    mbedtls_ccm_context ccm;
    mbedtls_ccm_init(&ccm);
    bool ok = mbedtls_ccm_setkey(&ccm, MBEDTLS_CIPHER_ID_AES, s->k_c2b, 128) == 0 &&
              mbedtls_ccm_auth_decrypt(&ccm, ct_len, nonce, sizeof(nonce), raw, AK_BLE_CTR_LEN,
                                       raw + AK_BLE_CTR_LEN, (unsigned char *)plain,
                                       raw + AK_BLE_CTR_LEN + ct_len, AK_BLE_TAG_LEN) == 0;
    mbedtls_ccm_free(&ccm);
    if (!ok) {
        mbedtls_platform_zeroize(plain, ct_len);
        return false;
    }
    plain[ct_len] = '\0';
    if (memchr(plain, '\0', ct_len) != NULL) {
        return false; /* a NUL would hide the rest of the frame */
    }
    s->rx_ctr++;
    return true;
}

static int random_nonce(ak_ble_rand_fn rnd, void *ctx, uint8_t out[AK_BLE_NONCE_LEN])
{
    return rnd == NULL ? -1 : rnd(ctx, out, AK_BLE_NONCE_LEN);
}

static ak_ble_sec_action_t fail(ak_ble_sec_t *s)
{
    ak_ble_sec_reset(s);
    s->state = AK_BLE_SEC_FAILED;
    return AK_BLE_SEC_DROP;
}

ak_ble_sec_action_t ak_ble_sec_on_line(ak_ble_sec_t *s, const uint8_t *key, const char *line, ak_ble_rand_fn rnd,
                                       void *rnd_ctx, char *plain, size_t plain_cap, char *reply, size_t reply_cap)
{
    if (s == NULL || line == NULL) {
        return AK_BLE_SEC_DROP;
    }
    if (key == NULL) {
        return AK_BLE_SEC_DROP; /* not paired: the link stays silent */
    }
    switch (s->state) {
    case AK_BLE_SEC_IDLE: {
        ak_rx_frame_t f;
        char          key_id[AK_BLE_KEY_ID_HEX];
        char          want_id[AK_BLE_KEY_ID_HEX];
        char          nonce_hex[2U * AK_BLE_NONCE_LEN + 1U];
        uint8_t       nc[AK_BLE_NONCE_LEN];
        uint8_t       nb[AK_BLE_NONCE_LEN];
        if (!ak_rx_parse_frame(line, &f) || strcmp(f.type, "sec_hello") != 0 ||
            ak_json_get_str(f.payload, "key_id", key_id, sizeof(key_id)) != AK_JSON_OK ||
            ak_json_get_str(f.payload, "nonce", nonce_hex, sizeof(nonce_hex)) != AK_JSON_OK ||
            !unhex(nonce_hex, nc, sizeof(nc))) {
            return fail(s);
        }
        ak_ble_key_id(key, want_id);
        if (strcmp(key_id, want_id) != 0 || random_nonce(rnd, rnd_ctx, nb) != 0) {
            return fail(s);
        }
        uint8_t salt[2U * AK_BLE_NONCE_LEN];
        uint8_t okm[32];
        memcpy(salt, nc, AK_BLE_NONCE_LEN);
        memcpy(salt + AK_BLE_NONCE_LEN, nb, AK_BLE_NONCE_LEN);
        if (mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), salt, sizeof(salt), key, AK_BLE_KEY_LEN,
                         (const unsigned char *)SESSION_INFO, strlen(SESSION_INFO), okm, sizeof(okm)) != 0) {
            return fail(s);
        }
        memcpy(s->k_c2b, okm, 16);
        memcpy(s->k_b2c, okm + 16, 16);
        mbedtls_platform_zeroize(okm, sizeof(okm));
        s->rx_ctr = 0;
        s->tx_ctr = 0;
        char nb_hex[2U * AK_BLE_NONCE_LEN + 1U];
        ak_ble_hex(nb, sizeof(nb), nb_hex);
        int n = 0;
        if (reply != NULL && reply_cap > 0) {
            n = snprintf(reply, reply_cap, "{\"v\":1,\"type\":\"sec_hello_ack\",\"payload\":{\"nonce\":\"%s\"}}\n",
                         nb_hex);
        }
        if (n <= 0 || (size_t)n >= reply_cap) {
            return fail(s);
        }
        s->state = AK_BLE_SEC_WAIT_CONFIRM;
        return AK_BLE_SEC_REPLY;
    }
    case AK_BLE_SEC_WAIT_CONFIRM:
        if (!open_line(s, line, plain, plain_cap)) {
            return fail(s);
        }
        s->state = AK_BLE_SEC_AUTHED;
        return AK_BLE_SEC_AUTH_OK;
    case AK_BLE_SEC_AUTHED:
        if (!open_line(s, line, plain, plain_cap)) {
            return fail(s);
        }
        return AK_BLE_SEC_DELIVER;
    case AK_BLE_SEC_FAILED:
    default:
        return AK_BLE_SEC_DROP;
    }
}

size_t ak_ble_sec_sealed_len(size_t len)
{
    size_t raw = AK_BLE_CTR_LEN + len + AK_BLE_TAG_LEN;
    return 1U + 4U * ((raw + 2U) / 3U) + 1U; /* '~' + base64 + '\n' */
}

int ak_ble_sec_seal(ak_ble_sec_t *s, const uint8_t *pt, size_t len, char *out, size_t out_cap)
{
    if (s == NULL || !ak_ble_sec_authed(s) || pt == NULL || out == NULL) {
        return -1;
    }
    if (len > 0 && pt[len - 1] == '\n') {
        len--; /* the sealed line carries its own terminator */
    }
    /* +1 for mbedtls_base64_encode's NUL */
    if (ak_ble_sec_sealed_len(len) + 1U > out_cap) {
        return -1;
    }
    static uint8_t raw[AK_BLE_CTR_LEN + 2048U + AK_BLE_TAG_LEN];
    if (len > 2048U) {
        return -1;
    }
    uint8_t nonce[CCM_NONCE_LEN];
    put_ctr(s->tx_ctr, raw);
    make_nonce(AK_BLE_DIR_B2C, raw, nonce);

    mbedtls_ccm_context ccm;
    mbedtls_ccm_init(&ccm);
    bool ok = mbedtls_ccm_setkey(&ccm, MBEDTLS_CIPHER_ID_AES, s->k_b2c, 128) == 0 &&
              mbedtls_ccm_encrypt_and_tag(&ccm, len, nonce, sizeof(nonce), raw, AK_BLE_CTR_LEN, pt,
                                          raw + AK_BLE_CTR_LEN, raw + AK_BLE_CTR_LEN + len, AK_BLE_TAG_LEN) == 0;
    mbedtls_ccm_free(&ccm);
    if (!ok) {
        return -1;
    }
    size_t olen = 0;
    out[0]      = '~';
    if (mbedtls_base64_encode((unsigned char *)out + 1, out_cap - 2U, &olen, raw,
                              AK_BLE_CTR_LEN + len + AK_BLE_TAG_LEN) != 0) {
        return -1;
    }
    out[1 + olen] = '\n';
    out[2 + olen] = '\0';
    s->tx_ctr++;
    return (int)(2U + olen);
}
