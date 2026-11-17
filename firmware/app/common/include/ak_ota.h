/**
 * @file ak_ota.h
 * @brief Local OTA over USB (docs/ota.md): package checks and the receive
 *        state machine. Pure apart from mbedTLS; flash access goes through
 *        ak_ota_flash_ops_t so the host tests run it against RAM.
 *
 * Flow: begin (signed 256-byte header) -> CONFIRM (K6 / K1 on the device,
 * 10 s) -> ERASING (step by step from the app loop) -> RECEIVING (data in
 * order, sliding window on the Companion side) -> end -> read back and hash
 * the written image -> VERIFIED. The platform layer then writes the TuyaOS
 * manage block and reboots; tuyaboot installs the image.
 */

#ifndef AK_OTA_H
#define AK_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mbedtls/sha256.h"

#define AK_OTA_MANIFEST_LEN  192U
#define AK_OTA_SIG_LEN       64U
#define AK_OTA_HEADER_LEN    (AK_OTA_MANIFEST_LEN + AK_OTA_SIG_LEN)
#define AK_OTA_UG_HEADER_LEN 64U
#define AK_OTA_KEY_POINT_LEN 65U
#define AK_OTA_KEY_ID_LEN    8U
#define AK_OTA_CHUNK_MAX     256U
/* ota partition is 2940K; refuse images above 85 % like the platform packer. */
#define AK_OTA_IMAGE_MAX     (2940U * 1024U * 85U / 100U)

#define AK_OTA_CONFIRM_MS 10000U
#define AK_OTA_IDLE_MS    30000U /* no data / no end for this long: give up */

typedef enum {
    AK_OTA_PROFILE_RELEASE = 1,
    AK_OTA_PROFILE_DEBUG   = 2,
} ak_ota_profile_t;

typedef struct {
    uint16_t part[3];
} ak_ota_version_t;

typedef struct {
    uint8_t          profile;
    ak_ota_version_t version;
    uint32_t         image_size;
    uint8_t          image_sha256[32];
    uint8_t          key_id[AK_OTA_KEY_ID_LEN];
    uint8_t          ug_header[AK_OTA_UG_HEADER_LEN];
} ak_ota_manifest_t;

/* The public key this firmware trusts. release = true: only profile=release
 * packages (debug firmware never reaches a release device). */
typedef struct {
    const uint8_t *point; /* 65-byte uncompressed P-256 point */
    bool           placeholder;
    bool           release;
} ak_ota_key_t;

typedef enum {
    AK_OTA_OK = 0,
    AK_OTA_E_FORMAT,    /* not an AKOT v1 header */
    AK_OTA_E_PRODUCT,   /* other product / hardware */
    AK_OTA_E_NO_KEY,    /* release key is still the placeholder */
    AK_OTA_E_KEY,       /* signed by another key */
    AK_OTA_E_SIG,       /* bad signature */
    AK_OTA_E_PROFILE,   /* debug package on a release device */
    AK_OTA_E_DOWNGRADE, /* not newer than running / highest installed */
    AK_OTA_E_SIZE,      /* image too large or empty */
    AK_OTA_E_UG,        /* not a full LZMA UG image */
    AK_OTA_E_BUSY,      /* another update is in progress */
    AK_OTA_E_POWER,     /* power rule not met */
    AK_OTA_E_CANCELLED, /* K1 or no answer on the device */
    AK_OTA_E_TIMEOUT,   /* Companion stopped sending */
    AK_OTA_E_SEQUENCE,  /* data outside the image / bad chunk */
    AK_OTA_E_HASH,      /* received or read-back image does not match */
    AK_OTA_E_FLASH,     /* erase / write / read failed */
    AK_OTA_E_INCOMPLETE,/* end before all bytes arrived */
    AK_OTA_E_ABORTED,   /* Companion sent ota_abort */
    AK_OTA_E_STATE,     /* command not valid in this state */
} ak_ota_err_t;

const char *ak_ota_err_str(ak_ota_err_t err);

bool ak_ota_version_parse(const char *text, ak_ota_version_t *out);
int  ak_ota_version_cmp(const ak_ota_version_t *a, const ak_ota_version_t *b);
void ak_ota_version_str(const ak_ota_version_t *v, char *buf, size_t size);

/** Check the UG header against the image size: full LZMA package, patch
 *  length consistent. */
ak_ota_err_t ak_ota_check_ug_header(const uint8_t ug[AK_OTA_UG_HEADER_LEN], uint32_t image_size);

/** Parse and verify the 256-byte package header (manifest + signature).
 *  `highest` may be NULL. */
ak_ota_err_t ak_ota_check_header(const uint8_t header[AK_OTA_HEADER_LEN], const ak_ota_key_t *key,
                                 const ak_ota_version_t *running, const ak_ota_version_t *highest,
                                 ak_ota_manifest_t *out);

/** Power rule: USB power always allows; on battery only a reading the gauge
 *  marks trusted and >= 30 %. An untrusted reading never allows. */
#define AK_OTA_MIN_BATTERY_PERCENT 30U
bool ak_ota_power_allows(bool usb_power, bool reading_trusted, uint8_t percent);

/** CRC-32 (IEEE, reflected) for the debug UART baud probe. */
uint32_t ak_ota_crc32(const uint8_t *data, size_t len);

typedef struct {
    /* Validate the layout for this image and remember where it goes. */
    int (*prepare)(void *ctx, const uint8_t ug_header[AK_OTA_UG_HEADER_LEN], uint32_t image_size);
    /* Erase the next block; set *done when the whole area is erased. */
    int (*erase_step)(void *ctx, bool *done);
    int (*write)(void *ctx, uint32_t offset, const uint8_t *data, uint32_t len);
    /* SHA-256 of the first len bytes as stored in flash. */
    int (*readback_sha256)(void *ctx, uint32_t len, uint8_t out[32]);
} ak_ota_flash_ops_t;

typedef enum {
    AK_OTA_IDLE = 0,
    AK_OTA_CONFIRM,
    AK_OTA_ERASING,
    AK_OTA_RECEIVING,
    AK_OTA_VERIFIED,
} ak_ota_state_t;

const char *ak_ota_state_str(ak_ota_state_t state);

typedef struct {
    ak_ota_state_t            state;
    ak_ota_manifest_t         manifest;
    uint32_t                  next;        /* bytes written so far */
    uint32_t                  deadline_ms; /* CONFIRM / RECEIVING timeout */
    ak_ota_err_t              last_error;  /* why the last session ended */
    mbedtls_sha256_context    sha;
    const ak_ota_flash_ops_t *ops;
    void                     *ctx;
} ak_ota_session_t;

void ak_ota_init(ak_ota_session_t *s, const ak_ota_flash_ops_t *ops, void *ctx);

/** A verified header arrived: enter CONFIRM. Fails with AK_OTA_E_BUSY when a
 *  session is open; other errors leave the session idle. */
ak_ota_err_t ak_ota_begin(ak_ota_session_t *s, const uint8_t header[AK_OTA_HEADER_LEN], const ak_ota_key_t *key,
                          const ak_ota_version_t *running, const ak_ota_version_t *highest, bool power_ok,
                          uint32_t now_ms);

/** Device answer in CONFIRM. accept: prepare the flash and start erasing. */
ak_ota_err_t ak_ota_confirm(ak_ota_session_t *s, bool accept, uint32_t now_ms);

/** App-loop step: erase one block, enforce timeouts. Returns true when the
 *  state changed (the caller reports it). */
bool ak_ota_tick(ak_ota_session_t *s, uint32_t now_ms);

/** One data chunk at `offset`. Chunks must arrive in order: a chunk that is
 *  not at `next` is ignored (the Companion resends from the acked offset).
 *  Returns AK_OTA_OK also for an ignored duplicate; *next_out = bytes held. */
ak_ota_err_t ak_ota_data(ak_ota_session_t *s, uint32_t offset, const uint8_t *data, uint32_t len,
                         uint32_t now_ms, uint32_t *next_out);

/** All data sent: check the running hash and the read-back hash. */
ak_ota_err_t ak_ota_end(ak_ota_session_t *s);

/** End the session (abort / error); records the reason. */
void ak_ota_fail(ak_ota_session_t *s, ak_ota_err_t err);

#endif /* AK_OTA_H */
