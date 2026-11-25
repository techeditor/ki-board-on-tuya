/**
 * @file ak_splash.h
 * @brief Boot splash image: pure format parser (host-testable) plus the
 *        device glue that reads it from flash and draws it on the status LCD.
 *
 * The image lives in the TUYA_FLASH_TYPE_USER0 area. Layout,
 * all integers little-endian:
 *   0       magic        "AKSP"
 *   4       version      u16, must be 1
 *   6       format       u16, must be 1 (4 bpp, 16-entry RGB565 palette)
 *   8       width        u16, 1..320
 *   10      height       u16, 1..240
 *   12      data_len     u32, must equal ((width + 1) / 2) * height
 *   16      palette      16 x u16 RGB565 (native, as ak_gfx takes them)
 *   48      pixel data   data_len bytes; high nibble = left pixel of a pair
 *   48+N    crc32        u32 over bytes [0, 48+N), reflected CRC-32 (zlib)
 *
 * A blob that fails any check is ignored and the built-in default is shown.
 */

#ifndef AK_SPLASH_H
#define AK_SPLASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AK_SPLASH_HDR_LEN   48U
#define AK_SPLASH_CRC_LEN   4U
#define AK_SPLASH_MAX_W     320U
#define AK_SPLASH_MAX_H     240U
/* USER0 area size on the board (tkl_flash.c TUYA_USER1_SIZE). */
#define AK_SPLASH_MAX_BYTES (60U * 1024U)

typedef struct {
    uint16_t        width;
    uint16_t        height;
    uint16_t        palette[16];
    const uint8_t  *data; /* points into the parsed buffer */
} ak_splash_t;

/** Reflected CRC-32 (polynomial 0xEDB88320), same result as zlib.crc32. */
uint32_t ak_splash_crc32(const uint8_t *buf, size_t len);

/** True when every byte is 0xFF (erased flash, no image written). */
bool ak_splash_is_erased(const uint8_t *buf, size_t len);

/**
 * Check the first AK_SPLASH_HDR_LEN bytes and fill @p out; @p total_len is
 * the full blob size (header + data + CRC). Does not check the CRC.
 */
bool ak_splash_parse_header(const uint8_t *buf, size_t len, ak_splash_t *out, size_t *total_len);

/** Full check: header, length and CRC. On success @p out->data is in @p buf. */
bool ak_splash_parse(const uint8_t *buf, size_t len, ak_splash_t *out);

/* ---- Hold time and key skip (pure, host-tested) ----
 * The built-in default stays AK_SPLASH_HOLD_DEFAULT_MS, a custom image
 * AK_SPLASH_HOLD_CUSTOM_MS (long enough to read a dedication). Any key
 * (K1-K6, EC11 push) or an EC11 turn ends it early. The key that skips is
 * swallowed: the hold keeps polling (and dropping events) until every key
 * pressed during the splash is released, so its UP never reaches the main
 * loop. That wait is capped at AK_SPLASH_RELEASE_MAX_MS after the hold would
 * have ended; a key still held then is released into the main loop. */
#define AK_SPLASH_HOLD_DEFAULT_MS 1500U
#define AK_SPLASH_HOLD_CUSTOM_MS  4000U
#define AK_SPLASH_RELEASE_MAX_MS  3000U
#define AK_SPLASH_POLL_MS         10U

typedef struct {
    uint32_t start_ms;
    uint32_t hold_ms;
    uint8_t  held;    /* keys pressed during the splash and not yet released */
    bool     skipped; /* a key or an EC11 turn ended the hold early */
} ak_splash_hold_t;

/** Hold time for the image being shown. */
uint32_t ak_splash_hold_ms(bool custom);

void ak_splash_hold_begin(ak_splash_hold_t *h, bool custom, uint32_t now_ms);

/** Feed one key event from ak_input_poll (ak_input_event_t value). */
void ak_splash_hold_key(ak_splash_hold_t *h, int event);

/** Feed one EC11 rotation (signed detents) from ak_input_poll. */
void ak_splash_hold_encoder(ak_splash_hold_t *h, int32_t detents);

/** True when the splash may give way to the boot sequence. */
bool ak_splash_hold_done(const ak_splash_hold_t *h, uint32_t now_ms);

/**
 * Show the boot splash (custom image from USER0, else the built-in default),
 * wait until the frame is on the panel, then hold it (see ak_splash_hold_*),
 * polling the keys every AK_SPLASH_POLL_MS. Events seen during the hold are
 * dropped. Call after the status LCD and ak_input_init() are up.
 */
void ak_splash_show(void);

/**
 * Log the USER0 area state (erased flag, CRC of the first 64 bytes, CRC of
 * the whole area) and the flash partition addresses as the running firmware
 * sees them. Read-only. Used to confirm that the area is free on a real board.
 */
void ak_splash_report_region(void);

#endif /* AK_SPLASH_H */
