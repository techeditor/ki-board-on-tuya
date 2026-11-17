/**
 * @file ak_ble_sec.h
 * @brief Application-layer BLE secure channel (firmware-review FW-01 link
 *        part, FW-13): pairing key, handshake, per-frame AES-128-CCM.
 *
 * TuyaOpen's BLE stack has no SMP/bonding, so the AK BLE link is secured in
 * the application layer. All cryptography is mbedTLS (AES-CCM, HKDF-SHA256,
 * SHA-256, base64); nothing is hand-written. Pure apart from mbedTLS, so the
 * host tests link the same mbedTLS sources.
 *
 * Pairing key K (32 random bytes) is created on the device over USB inside
 * the pairing window and stored in KV; the Companion receives it once over
 * USB. key_id = hex of SHA-256(K)[0..3] names the key without revealing it.
 *
 * Handshake (plaintext lines, nothing else is accepted before it):
 *   C->B {"v":1,"type":"sec_hello","payload":{"key_id":"<8 hex>","nonce":"<32 hex>"}}
 *   B->C {"v":1,"type":"sec_hello_ack","payload":{"nonce":"<32 hex>"}}
 *   session keys = HKDF-SHA256(IKM=K, salt=Nc||Nb, info="AK-BLE-v1"), 32 bytes:
 *   [0..15] Companion->board key, [16..31] board->Companion key.
 *   The first sealed Companion frame (counter 0) proves the Companion holds
 *   K; until it verifies, the board sends nothing else.
 *
 * Sealed frame (one line): '~' base64( ctr[8 BE] || ciphertext || tag[16] ) '\n'
 *   nonce[13] = dir (0x01 C->B, 0x02 B->C) || 0x00 0x00 0x00 0x00 || ctr[8 BE]
 *   AAD = the 8 counter bytes. The receiver accepts only ctr == next expected
 *   (starting at 0), so replayed, reordered and dropped frames fail.
 * Any failure ends the session: the board then ignores the link until it
 * disconnects.
 */

#ifndef AK_BLE_SEC_H
#define AK_BLE_SEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AK_BLE_KEY_LEN      32U
#define AK_BLE_NONCE_LEN    16U
#define AK_BLE_KEY_ID_HEX   9U /* 8 hex + NUL */
#define AK_BLE_TAG_LEN      16U
#define AK_BLE_CTR_LEN      8U
#define AK_BLE_DIR_C2B      0x01U
#define AK_BLE_DIR_B2C      0x02U
#define AK_BLE_KEY_BLOB_LEN (1U + AK_BLE_KEY_LEN) /* version byte + key */
#define AK_BLE_KEY_BLOB_VER 1U

typedef enum {
    AK_BLE_SEC_IDLE = 0,     /* connected, waiting for sec_hello */
    AK_BLE_SEC_WAIT_CONFIRM, /* sec_hello_ack sent, waiting for the first sealed frame */
    AK_BLE_SEC_AUTHED,       /* frames flow both ways */
    AK_BLE_SEC_FAILED,       /* ignore everything until disconnect */
} ak_ble_sec_state_t;

typedef struct {
    ak_ble_sec_state_t state;
    uint8_t            k_c2b[16];
    uint8_t            k_b2c[16];
    uint64_t           rx_ctr; /* next expected Companion counter */
    uint64_t           tx_ctr; /* next board counter */
} ak_ble_sec_t;

/** Random byte source (device: CTR_DRBG over the TRNG). Returns 0 on success. */
typedef int (*ak_ble_rand_fn)(void *ctx, uint8_t *out, size_t len);

typedef enum {
    AK_BLE_SEC_DROP = 0, /* nothing to do; the line is ignored */
    AK_BLE_SEC_REPLY,    /* send `reply` (plaintext handshake line) on BLE */
    AK_BLE_SEC_DELIVER,  /* `plain` is a JSON line to process as a BLE frame */
    AK_BLE_SEC_AUTH_OK,  /* first sealed frame verified; also deliver `plain` */
} ak_ble_sec_action_t;

/** Wipe the session (disconnect, new key, cleared pairing). */
void ak_ble_sec_reset(ak_ble_sec_t *s);

bool ak_ble_sec_authed(const ak_ble_sec_t *s);

/** key_id = lowercase hex of SHA-256(key)[0..3]. */
void ak_ble_key_id(const uint8_t key[AK_BLE_KEY_LEN], char out[AK_BLE_KEY_ID_HEX]);

/** KV blob <-> key. from_blob rejects any other length or version. */
void ak_ble_key_to_blob(const uint8_t key[AK_BLE_KEY_LEN], uint8_t blob[AK_BLE_KEY_BLOB_LEN]);
bool ak_ble_key_from_blob(const uint8_t *blob, size_t len, uint8_t key[AK_BLE_KEY_LEN]);

/** Lowercase hex of `len` bytes into out (2*len+1). */
void ak_ble_hex(const uint8_t *in, size_t len, char *out);

/**
 * Handle one line received on BLE. `key` is NULL when the device is not
 * paired: then every line is dropped. Writes at most `plain_cap` bytes of
 * decrypted JSON (NUL-terminated) and at most `reply_cap` bytes of reply.
 */
ak_ble_sec_action_t ak_ble_sec_on_line(ak_ble_sec_t *s, const uint8_t *key, const char *line, ak_ble_rand_fn rnd,
                                       void *rnd_ctx, char *plain, size_t plain_cap, char *reply, size_t reply_cap);

/**
 * Seal one outgoing frame (JSON, trailing '\n' optional) into a '\n'
 * terminated line. Returns the line length, or -1 when not authenticated or
 * `out_cap` is too small. Advances the board counter.
 */
int ak_ble_sec_seal(ak_ble_sec_t *s, const uint8_t *pt, size_t len, char *out, size_t out_cap);

/** Bytes needed to seal `len` plaintext bytes (including '~' and '\n'). */
size_t ak_ble_sec_sealed_len(size_t len);

#endif /* AK_BLE_SEC_H */
