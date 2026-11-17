/**
 * @file ak_ota.c
 * @brief Local OTA package checks and receive state machine (see ak_ota.h).
 */

#include "ak_ota.h"

#include <stdio.h>
#include <string.h>

#include "mbedtls/bignum.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"

#define UG_MAGIC          0x004D4D4DUL
#define UG_TYPE_LZMA_FULL 0x4C5A4350UL

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

const char *ak_ota_err_str(ak_ota_err_t err)
{
    switch (err) {
    case AK_OTA_OK:           return "ok";
    case AK_OTA_E_FORMAT:     return "format";
    case AK_OTA_E_PRODUCT:    return "product";
    case AK_OTA_E_NO_KEY:     return "no_key";
    case AK_OTA_E_KEY:        return "key";
    case AK_OTA_E_SIG:        return "bad_sig";
    case AK_OTA_E_PROFILE:    return "profile";
    case AK_OTA_E_DOWNGRADE:  return "downgrade";
    case AK_OTA_E_SIZE:       return "too_large";
    case AK_OTA_E_UG:         return "image";
    case AK_OTA_E_BUSY:       return "busy";
    case AK_OTA_E_POWER:      return "power";
    case AK_OTA_E_CANCELLED:  return "cancelled";
    case AK_OTA_E_TIMEOUT:    return "timeout";
    case AK_OTA_E_SEQUENCE:   return "sequence";
    case AK_OTA_E_HASH:       return "hash";
    case AK_OTA_E_FLASH:      return "flash";
    case AK_OTA_E_INCOMPLETE: return "incomplete";
    case AK_OTA_E_ABORTED:    return "aborted";
    case AK_OTA_E_STATE:      return "state";
    default:                  return "unknown";
    }
}

const char *ak_ota_state_str(ak_ota_state_t state)
{
    switch (state) {
    case AK_OTA_CONFIRM:   return "confirm";
    case AK_OTA_ERASING:   return "erasing";
    case AK_OTA_RECEIVING: return "receiving";
    case AK_OTA_VERIFIED:  return "verified";
    case AK_OTA_IDLE:
    default:               return "idle";
    }
}

bool ak_ota_version_parse(const char *text, ak_ota_version_t *out)
{
    unsigned part = 0U;
    uint32_t value = 0U;
    bool     digit = false;
    if (text == NULL || out == NULL) {
        return false;
    }
    for (const char *p = text;; p++) {
        if (*p >= '0' && *p <= '9') {
            value = value * 10U + (uint32_t)(*p - '0');
            if (value > 0xFFFFU) {
                return false;
            }
            digit = true;
        } else if ((*p == '.' || *p == '\0') && digit && part < 3U) {
            out->part[part++] = (uint16_t)value;
            value             = 0U;
            digit             = false;
            if (*p == '\0') {
                return part == 3U;
            }
        } else {
            return false;
        }
    }
}

int ak_ota_version_cmp(const ak_ota_version_t *a, const ak_ota_version_t *b)
{
    for (unsigned i = 0U; i < 3U; i++) {
        if (a->part[i] != b->part[i]) {
            return a->part[i] < b->part[i] ? -1 : 1;
        }
    }
    return 0;
}

void ak_ota_version_str(const ak_ota_version_t *v, char *buf, size_t size)
{
    (void)snprintf(buf, size, "%u.%u.%u", (unsigned)v->part[0], (unsigned)v->part[1], (unsigned)v->part[2]);
}

ak_ota_err_t ak_ota_check_ug_header(const uint8_t ug[AK_OTA_UG_HEADER_LEN], uint32_t image_size)
{
    if (rd32(ug) != UG_MAGIC || rd32(ug + 4) != UG_TYPE_LZMA_FULL) {
        return AK_OTA_E_UG;
    }
    /* Field 8 is the compressed payload length after the 64-byte header. */
    if (image_size < AK_OTA_UG_HEADER_LEN || rd32(ug + 32) != image_size - AK_OTA_UG_HEADER_LEN) {
        return AK_OTA_E_UG;
    }
    return AK_OTA_OK;
}

static bool verify_signature(const uint8_t point[AK_OTA_KEY_POINT_LEN], const uint8_t hash[32],
                             const uint8_t sig[AK_OTA_SIG_LEN])
{
    mbedtls_ecp_group grp;
    mbedtls_ecp_point q;
    mbedtls_mpi       r;
    mbedtls_mpi       s;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
              mbedtls_ecp_point_read_binary(&grp, &q, point, AK_OTA_KEY_POINT_LEN) == 0 &&
              mbedtls_ecp_check_pubkey(&grp, &q) == 0 &&
              mbedtls_mpi_read_binary(&r, sig, 32U) == 0 &&
              mbedtls_mpi_read_binary(&s, sig + 32U, 32U) == 0 &&
              mbedtls_ecdsa_verify(&grp, hash, 32U, &q, &r, &s) == 0;
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

ak_ota_err_t ak_ota_check_header(const uint8_t header[AK_OTA_HEADER_LEN], const ak_ota_key_t *key,
                                 const ak_ota_version_t *running, const ak_ota_version_t *highest,
                                 ak_ota_manifest_t *out)
{
    static const char product[16] = "ki-board";
    static const char hw[8]       = "v0.3";
    const uint8_t    *m           = header;
    uint8_t           digest[32];

    if (memcmp(m, "AKOT", 4) != 0 || rd16(m + 4) != 1U || rd16(m + 6) != AK_OTA_MANIFEST_LEN) {
        return AK_OTA_E_FORMAT;
    }
    if (memcmp(m + 8, product, sizeof(product)) != 0 || memcmp(m + 24, hw, sizeof(hw)) != 0) {
        return AK_OTA_E_PRODUCT;
    }
    if (key == NULL || key->placeholder || key->point == NULL) {
        return AK_OTA_E_NO_KEY;
    }
    if (mbedtls_sha256(key->point, AK_OTA_KEY_POINT_LEN, digest, 0) != 0 ||
        memcmp(digest, m + 80, AK_OTA_KEY_ID_LEN) != 0) {
        return AK_OTA_E_KEY;
    }
    if (mbedtls_sha256(m, AK_OTA_MANIFEST_LEN, digest, 0) != 0 ||
        !verify_signature(key->point, digest, header + AK_OTA_MANIFEST_LEN)) {
        return AK_OTA_E_SIG;
    }
    /* Signed content from here on. */
    ak_ota_manifest_t man;
    man.profile = m[32];
    for (unsigned i = 0U; i < 3U; i++) {
        man.version.part[i] = rd16(m + 36 + 2U * i);
    }
    man.image_size = rd32(m + 44);
    memcpy(man.image_sha256, m + 48, sizeof(man.image_sha256));
    memcpy(man.key_id, m + 80, sizeof(man.key_id));
    memcpy(man.ug_header, m + 128, sizeof(man.ug_header));

    if (man.profile != AK_OTA_PROFILE_RELEASE && man.profile != AK_OTA_PROFILE_DEBUG) {
        return AK_OTA_E_FORMAT;
    }
    if (key->release && man.profile != AK_OTA_PROFILE_RELEASE) {
        return AK_OTA_E_PROFILE;
    }
    if (ak_ota_version_cmp(&man.version, running) <= 0 ||
        (highest != NULL && ak_ota_version_cmp(&man.version, highest) <= 0)) {
        return AK_OTA_E_DOWNGRADE;
    }
    if (man.image_size == 0U || man.image_size > AK_OTA_IMAGE_MAX) {
        return AK_OTA_E_SIZE;
    }
    ak_ota_err_t ug = ak_ota_check_ug_header(man.ug_header, man.image_size);
    if (ug != AK_OTA_OK) {
        return ug;
    }
    if (out != NULL) {
        *out = man;
    }
    return AK_OTA_OK;
}

bool ak_ota_power_allows(bool usb_power, bool reading_trusted, uint8_t percent)
{
    return usb_power || (reading_trusted && percent >= AK_OTA_MIN_BATTERY_PERCENT);
}

uint32_t ak_ota_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0U; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320UL & (uint32_t)(-(int32_t)(crc & 1U)));
        }
    }
    return ~crc;
}

/* ── session ─────────────────────────────────────────────────────────────── */

void ak_ota_init(ak_ota_session_t *s, const ak_ota_flash_ops_t *ops, void *ctx)
{
    memset(s, 0, sizeof(*s));
    s->ops = ops;
    s->ctx = ctx;
}

void ak_ota_fail(ak_ota_session_t *s, ak_ota_err_t err)
{
    if (s->state == AK_OTA_ERASING || s->state == AK_OTA_RECEIVING) {
        mbedtls_sha256_free(&s->sha);
    }
    s->state      = AK_OTA_IDLE;
    s->last_error = err;
    s->next       = 0U;
}

ak_ota_err_t ak_ota_begin(ak_ota_session_t *s, const uint8_t header[AK_OTA_HEADER_LEN], const ak_ota_key_t *key,
                          const ak_ota_version_t *running, const ak_ota_version_t *highest, bool power_ok,
                          uint32_t now_ms)
{
    if (s->state != AK_OTA_IDLE) {
        return AK_OTA_E_BUSY;
    }
    ak_ota_manifest_t man;
    ak_ota_err_t      err = ak_ota_check_header(header, key, running, highest, &man);
    if (err == AK_OTA_OK && !power_ok) {
        err = AK_OTA_E_POWER;
    }
    if (err != AK_OTA_OK) {
        s->last_error = err;
        return err;
    }
    s->manifest    = man;
    s->next        = 0U;
    s->state       = AK_OTA_CONFIRM;
    s->deadline_ms = now_ms + AK_OTA_CONFIRM_MS;
    s->last_error  = AK_OTA_OK;
    return AK_OTA_OK;
}

ak_ota_err_t ak_ota_confirm(ak_ota_session_t *s, bool accept, uint32_t now_ms)
{
    if (s->state != AK_OTA_CONFIRM) {
        return AK_OTA_E_STATE;
    }
    if (!accept) {
        ak_ota_fail(s, AK_OTA_E_CANCELLED);
        return AK_OTA_E_CANCELLED;
    }
    if (s->ops->prepare(s->ctx, s->manifest.ug_header, s->manifest.image_size) != 0) {
        ak_ota_fail(s, AK_OTA_E_FLASH);
        return AK_OTA_E_FLASH;
    }
    mbedtls_sha256_init(&s->sha);
    (void)mbedtls_sha256_starts(&s->sha, 0);
    s->state       = AK_OTA_ERASING;
    s->deadline_ms = now_ms + AK_OTA_IDLE_MS;
    return AK_OTA_OK;
}

bool ak_ota_tick(ak_ota_session_t *s, uint32_t now_ms)
{
    switch (s->state) {
    case AK_OTA_CONFIRM:
        if ((int32_t)(now_ms - s->deadline_ms) >= 0) {
            ak_ota_fail(s, AK_OTA_E_CANCELLED); /* no answer counts as Cancel */
            return true;
        }
        return false;
    case AK_OTA_ERASING: {
        bool done = false;
        if (s->ops->erase_step(s->ctx, &done) != 0) {
            ak_ota_fail(s, AK_OTA_E_FLASH);
            return true;
        }
        if (done) {
            s->state       = AK_OTA_RECEIVING;
            s->deadline_ms = now_ms + AK_OTA_IDLE_MS;
            return true;
        }
        return false;
    }
    case AK_OTA_RECEIVING:
        if ((int32_t)(now_ms - s->deadline_ms) >= 0) {
            ak_ota_fail(s, AK_OTA_E_TIMEOUT);
            return true;
        }
        return false;
    case AK_OTA_VERIFIED:
    case AK_OTA_IDLE:
    default:
        return false;
    }
}

ak_ota_err_t ak_ota_data(ak_ota_session_t *s, uint32_t offset, const uint8_t *data, uint32_t len,
                         uint32_t now_ms, uint32_t *next_out)
{
    if (s->state != AK_OTA_RECEIVING) {
        return AK_OTA_E_STATE;
    }
    if (offset != s->next) {
        /* Out of order (a resend after a lost ack, or a window ahead of a
         * dropped line): keep what we have; the ack tells where to resume. */
        *next_out = s->next;
        return AK_OTA_OK;
    }
    if (len == 0U || len > AK_OTA_CHUNK_MAX || len > s->manifest.image_size - s->next) {
        ak_ota_fail(s, AK_OTA_E_SEQUENCE);
        return AK_OTA_E_SEQUENCE;
    }
    /* The first 64 bytes must be the UG header the signed manifest names. */
    if (offset < AK_OTA_UG_HEADER_LEN) {
        uint32_t n = AK_OTA_UG_HEADER_LEN - offset;
        if (n > len) {
            n = len;
        }
        if (memcmp(data, s->manifest.ug_header + offset, n) != 0) {
            ak_ota_fail(s, AK_OTA_E_HASH);
            return AK_OTA_E_HASH;
        }
    }
    if (s->ops->write(s->ctx, offset, data, len) != 0) {
        ak_ota_fail(s, AK_OTA_E_FLASH);
        return AK_OTA_E_FLASH;
    }
    (void)mbedtls_sha256_update(&s->sha, data, len);
    s->next += len;
    s->deadline_ms = now_ms + AK_OTA_IDLE_MS;
    *next_out      = s->next;
    return AK_OTA_OK;
}

ak_ota_err_t ak_ota_end(ak_ota_session_t *s)
{
    uint8_t digest[32];
    if (s->state != AK_OTA_RECEIVING) {
        return AK_OTA_E_STATE;
    }
    if (s->next != s->manifest.image_size) {
        ak_ota_fail(s, AK_OTA_E_INCOMPLETE);
        return AK_OTA_E_INCOMPLETE;
    }
    (void)mbedtls_sha256_finish(&s->sha, digest);
    mbedtls_sha256_free(&s->sha);
    s->state = AK_OTA_VERIFIED; /* sha context released */
    if (memcmp(digest, s->manifest.image_sha256, sizeof(digest)) != 0) {
        ak_ota_fail(s, AK_OTA_E_HASH);
        return AK_OTA_E_HASH;
    }
    /* Read the image back from flash: catches a write that did not stick. */
    if (s->ops->readback_sha256(s->ctx, s->manifest.image_size, digest) != 0) {
        ak_ota_fail(s, AK_OTA_E_FLASH);
        return AK_OTA_E_FLASH;
    }
    if (memcmp(digest, s->manifest.image_sha256, sizeof(digest)) != 0) {
        ak_ota_fail(s, AK_OTA_E_HASH);
        return AK_OTA_E_HASH;
    }
    return AK_OTA_OK;
}
