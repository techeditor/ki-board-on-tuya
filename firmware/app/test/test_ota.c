/**
 * @file test_ota.c
 * @brief Host tests for ak_ota (package checks, receive state machine) and
 *        the USB-only gate for OTA commands in ak_rx.
 */

#include <stdio.h>
#include <string.h>

#include "ak_ota.h"
#include "ak_rx.h"
#include "ota_vectors.h"

static int g_failures;

#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);      \
            g_failures++;                                                 \
        }                                                                 \
    } while (0)

/* RAM stand-in for the ota partition. */
typedef struct {
    uint8_t  mem[1024];
    uint32_t size;
    int      erase_steps_left;
    bool     fail_write;
    bool     corrupt_readback;
    int      prepared;
} fake_flash_t;

static int fake_prepare(void *ctx, const uint8_t *ug, uint32_t size)
{
    fake_flash_t *f = ctx;
    (void)ug;
    if (size > sizeof(f->mem)) {
        return -1;
    }
    f->size = size;
    f->prepared++;
    f->erase_steps_left = 3;
    return 0;
}

static int fake_erase_step(void *ctx, bool *done)
{
    fake_flash_t *f = ctx;
    if (--f->erase_steps_left <= 0) {
        memset(f->mem, 0xFF, sizeof(f->mem));
        *done = true;
    }
    return 0;
}

static int fake_write(void *ctx, uint32_t off, const uint8_t *data, uint32_t len)
{
    fake_flash_t *f = ctx;
    if (f->fail_write || off + len > f->size) {
        return -1;
    }
    memcpy(f->mem + off, data, len);
    return 0;
}

static int fake_readback(void *ctx, uint32_t len, uint8_t out[32])
{
    fake_flash_t *f = ctx;
    if (f->corrupt_readback) {
        f->mem[len / 2] ^= 1U;
    }
    return mbedtls_sha256(f->mem, len, out, 0);
}

static const ak_ota_flash_ops_t k_ops = {fake_prepare, fake_erase_step, fake_write, fake_readback};

static const ak_ota_key_t k_release = {ota_vec_point, false, true};
static const ak_ota_key_t k_test    = {ota_vec_point, false, false};

static ak_ota_version_t ver(unsigned a, unsigned b, unsigned c)
{
    ak_ota_version_t v = {{(uint16_t)a, (uint16_t)b, (uint16_t)c}};
    return v;
}

static void test_version(void)
{
    ak_ota_version_t v;
    char             buf[24];
    printf("test_version\n");
    CHECK(ak_ota_version_parse("0.3.54", &v) && v.part[2] == 54U, "parse");
    CHECK(!ak_ota_version_parse("0.3", &v), "two parts");
    CHECK(!ak_ota_version_parse("0.3.54.1", &v), "four parts");
    CHECK(!ak_ota_version_parse("0.3.x", &v), "letters");
    CHECK(!ak_ota_version_parse("0..3", &v), "empty part");
    CHECK(!ak_ota_version_parse("0.3.70000", &v), "overflow");
    ak_ota_version_t a = ver(0, 3, 99), b = ver(0, 3, 100), c = ver(0, 4, 0);
    CHECK(ak_ota_version_cmp(&a, &b) < 0 && ak_ota_version_cmp(&b, &c) < 0 &&
              ak_ota_version_cmp(&c, &c) == 0 && ak_ota_version_cmp(&c, &a) > 0,
          "compare numerically, not as text");
    ak_ota_version_str(&b, buf, sizeof(buf));
    CHECK(strcmp(buf, "0.3.100") == 0, "to string");
}

static void test_header(void)
{
    ak_ota_manifest_t m;
    ak_ota_version_t  run = ver(0, 3, 54);
    ak_ota_version_t  hi  = ver(0, 3, 59);
    uint8_t           h[AK_OTA_HEADER_LEN];
    printf("test_header\n");

    CHECK(ak_ota_check_header(ota_vec_header_release, &k_release, &run, &hi, &m) == AK_OTA_OK, "valid release");
    CHECK(m.version.part[2] == 60U && m.image_size == sizeof(ota_vec_image), "manifest fields");
    CHECK(memcmp(m.ug_header, ota_vec_image, 64) == 0, "ug header copy");

    CHECK(ak_ota_check_header(ota_vec_header_debug, &k_release, &run, NULL, &m) == AK_OTA_E_PROFILE,
          "release device refuses a debug package");
    CHECK(ak_ota_check_header(ota_vec_header_debug, &k_test, &run, NULL, &m) == AK_OTA_OK,
          "test-key device takes a debug package");
    CHECK(ak_ota_check_header(ota_vec_header_foreign, &k_release, &run, NULL, &m) == AK_OTA_E_KEY,
          "other key");

    ak_ota_key_t placeholder = {ota_vec_point, true, true};
    CHECK(ak_ota_check_header(ota_vec_header_release, &placeholder, &run, NULL, &m) == AK_OTA_E_NO_KEY,
          "placeholder key accepts nothing");

    ak_ota_version_t same = ver(0, 3, 60);
    CHECK(ak_ota_check_header(ota_vec_header_release, &k_release, &same, NULL, &m) == AK_OTA_E_DOWNGRADE,
          "same version");
    ak_ota_version_t newer = ver(0, 4, 0);
    CHECK(ak_ota_check_header(ota_vec_header_release, &k_release, &newer, NULL, &m) == AK_OTA_E_DOWNGRADE,
          "older than running");
    CHECK(ak_ota_check_header(ota_vec_header_release, &k_release, &run, &same, &m) == AK_OTA_E_DOWNGRADE,
          "not newer than the highest installed");

    /* Any change to the signed manifest breaks the signature. */
    memcpy(h, ota_vec_header_release, sizeof(h));
    h[40] ^= 1U;
    CHECK(ak_ota_check_header(h, &k_release, &run, NULL, &m) == AK_OTA_E_SIG, "version byte tampered");
    memcpy(h, ota_vec_header_release, sizeof(h));
    h[AK_OTA_MANIFEST_LEN + 10] ^= 1U;
    CHECK(ak_ota_check_header(h, &k_release, &run, NULL, &m) == AK_OTA_E_SIG, "signature tampered");
    memcpy(h, ota_vec_header_release, sizeof(h));
    h[0] = 'X';
    CHECK(ak_ota_check_header(h, &k_release, &run, NULL, &m) == AK_OTA_E_FORMAT, "magic");
    memcpy(h, ota_vec_header_release, sizeof(h));
    h[8] = 'K';
    CHECK(ak_ota_check_header(h, &k_release, &run, NULL, &m) == AK_OTA_E_PRODUCT, "product");

    CHECK(ak_ota_check_ug_header(ota_vec_image, sizeof(ota_vec_image)) == AK_OTA_OK, "ug ok");
    CHECK(ak_ota_check_ug_header(ota_vec_image, sizeof(ota_vec_image) + 1U) == AK_OTA_E_UG, "ug length");
    uint8_t ug[64];
    memcpy(ug, ota_vec_image, sizeof(ug));
    ug[4] = 0x51; /* overwritable segmented type */
    CHECK(ak_ota_check_ug_header(ug, sizeof(ota_vec_image)) == AK_OTA_E_UG, "ug type");
}

static void test_power(void)
{
    printf("test_power\n");
    CHECK(ak_ota_power_allows(true, false, 0U), "USB power ignores the gauge");
    CHECK(!ak_ota_power_allows(false, false, 100U), "untrusted reading never allows");
    CHECK(!ak_ota_power_allows(false, true, 29U), "29 %");
    CHECK(ak_ota_power_allows(false, true, 30U), "30 %");
}

static void test_crc32(void)
{
    printf("test_crc32\n");
    CHECK(ak_ota_crc32((const uint8_t *)"123456789", 9U) == 0xCBF43926UL, "IEEE check value");
}

/* Send the whole vector image in 256-byte chunks. */
static ak_ota_err_t send_all(ak_ota_session_t *s, uint32_t now)
{
    uint32_t     next = 0U;
    ak_ota_err_t err  = AK_OTA_OK;
    for (uint32_t off = 0U; off < sizeof(ota_vec_image) && err == AK_OTA_OK; off = next) {
        uint32_t n = (uint32_t)sizeof(ota_vec_image) - off;
        n          = n > AK_OTA_CHUNK_MAX ? AK_OTA_CHUNK_MAX : n;
        err        = ak_ota_data(s, off, ota_vec_image + off, n, now, &next);
    }
    return err;
}

static void start_session(ak_ota_session_t *s, fake_flash_t *f, uint32_t *now)
{
    ak_ota_version_t run = ver(0, 3, 54);
    memset(f, 0, sizeof(*f));
    ak_ota_init(s, &k_ops, f);
    CHECK(ak_ota_begin(s, ota_vec_header_release, &k_release, &run, NULL, true, *now) == AK_OTA_OK, "begin");
    CHECK(ak_ota_confirm(s, true, *now) == AK_OTA_OK, "confirm");
    while (s->state == AK_OTA_ERASING) {
        (void)ak_ota_tick(s, (*now += 5U));
    }
    CHECK(s->state == AK_OTA_RECEIVING, "erased, receiving");
}

static void test_session(void)
{
    ak_ota_session_t s;
    fake_flash_t     f;
    ak_ota_version_t run = ver(0, 3, 54);
    uint32_t         now = 1000U;
    uint32_t         next;
    printf("test_session\n");

    /* Happy path. */
    start_session(&s, &f, &now);
    CHECK(send_all(&s, now) == AK_OTA_OK, "data");
    CHECK(ak_ota_end(&s) == AK_OTA_OK && s.state == AK_OTA_VERIFIED, "verified");
    CHECK(memcmp(f.mem, ota_vec_image, sizeof(ota_vec_image)) == 0, "image written");

    /* Busy while a session is open; no power; cancel; confirm timeout. */
    start_session(&s, &f, &now);
    CHECK(ak_ota_begin(&s, ota_vec_header_release, &k_release, &run, NULL, true, now) == AK_OTA_E_BUSY, "busy");
    ak_ota_fail(&s, AK_OTA_E_ABORTED);
    CHECK(s.state == AK_OTA_IDLE && s.last_error == AK_OTA_E_ABORTED, "abort");
    f.prepared = 0;

    CHECK(ak_ota_begin(&s, ota_vec_header_release, &k_release, &run, NULL, false, now) == AK_OTA_E_POWER &&
              s.state == AK_OTA_IDLE,
          "power rule");
    CHECK(ak_ota_begin(&s, ota_vec_header_release, &k_release, &run, NULL, true, now) == AK_OTA_OK, "begin");
    CHECK(ak_ota_confirm(&s, false, now) == AK_OTA_E_CANCELLED && s.state == AK_OTA_IDLE && f.prepared == 0,
          "K1 cancels before any flash access");
    CHECK(ak_ota_begin(&s, ota_vec_header_release, &k_release, &run, NULL, true, now) == AK_OTA_OK, "begin");
    CHECK(!ak_ota_tick(&s, now + AK_OTA_CONFIRM_MS - 1U), "still asking");
    CHECK(ak_ota_tick(&s, now + AK_OTA_CONFIRM_MS) && s.last_error == AK_OTA_E_CANCELLED,
          "no answer in 10 s = cancel");

    /* Out-of-order chunk is ignored, then the resend from the ack works. */
    start_session(&s, &f, &now);
    CHECK(ak_ota_data(&s, 256U, ota_vec_image + 256, 256U, now, &next) == AK_OTA_OK && next == 0U,
          "ahead of next: ignored, ack 0");
    CHECK(ak_ota_data(&s, 0U, ota_vec_image, 256U, now, &next) == AK_OTA_OK && next == 256U, "in order");
    CHECK(ak_ota_data(&s, 0U, ota_vec_image, 256U, now, &next) == AK_OTA_OK && next == 256U,
          "duplicate: ignored");
    CHECK(ak_ota_end(&s) == AK_OTA_E_INCOMPLETE && s.state == AK_OTA_IDLE, "end too early");

    /* First bytes must equal the signed UG header. */
    start_session(&s, &f, &now);
    uint8_t bad[256];
    memcpy(bad, ota_vec_image, sizeof(bad));
    bad[10] ^= 1U;
    CHECK(ak_ota_data(&s, 0U, bad, sizeof(bad), now, &next) == AK_OTA_E_HASH && s.state == AK_OTA_IDLE,
          "ug header mismatch");

    /* Body byte changed: running hash catches it at end. */
    start_session(&s, &f, &now);
    CHECK(ak_ota_data(&s, 0U, ota_vec_image, 256U, now, &next) == AK_OTA_OK, "chunk 0");
    memcpy(bad, ota_vec_image + 256, sizeof(bad));
    bad[5] ^= 1U;
    CHECK(ak_ota_data(&s, 256U, bad, 256U, now, &next) == AK_OTA_OK, "chunk 1 (bad)");
    CHECK(ak_ota_data(&s, 512U, ota_vec_image + 512, (uint32_t)sizeof(ota_vec_image) - 512U, now, &next) ==
              AK_OTA_OK,
          "chunk 2");
    CHECK(ak_ota_end(&s) == AK_OTA_E_HASH, "hash mismatch");

    /* Flash that does not keep what was written: read-back catches it. */
    start_session(&s, &f, &now);
    f.corrupt_readback = true;
    CHECK(send_all(&s, now) == AK_OTA_OK, "data");
    CHECK(ak_ota_end(&s) == AK_OTA_E_HASH && s.state == AK_OTA_IDLE, "read-back mismatch");

    /* Oversized / overrunning chunk, write failure, idle timeout. */
    start_session(&s, &f, &now);
    CHECK(ak_ota_data(&s, 0U, ota_vec_image, AK_OTA_CHUNK_MAX + 1U, now, &next) == AK_OTA_E_SEQUENCE,
          "chunk too long");
    start_session(&s, &f, &now);
    f.fail_write = true;
    CHECK(ak_ota_data(&s, 0U, ota_vec_image, 256U, now, &next) == AK_OTA_E_FLASH, "write failure");
    start_session(&s, &f, &now);
    CHECK(ak_ota_tick(&s, now + AK_OTA_IDLE_MS) && s.last_error == AK_OTA_E_TIMEOUT, "Companion went quiet");
    CHECK(ak_ota_data(&s, 0U, ota_vec_image, 256U, now, &next) == AK_OTA_E_STATE, "data after timeout");
}

static void test_rx_gate(void)
{
    static const char *const usb_only[] = {"ota_begin", "ota_data", "ota_end", "ota_abort", "ota_probe",
                                           "probe_line"};
    printf("test_rx_gate\n");
    for (size_t i = 0U; i < sizeof(usb_only) / sizeof(usb_only[0]); i++) {
        CHECK(!ak_rx_source_allows(usb_only[i], AK_RX_SRC_BLE), "OTA command refused from BLE");
        CHECK(ak_rx_source_allows(usb_only[i], AK_RX_SRC_UART), "OTA command allowed from USB");
        CHECK(!ak_rx_type_is_sensitive(usb_only[i]), "OTA is not a pairing-window command");
    }
}

int main(void)
{
    test_version();
    test_header();
    test_power();
    test_crc32();
    test_session();
    test_rx_gate();
    if (g_failures != 0) {
        printf("%d OTA TEST(S) FAILED\n", g_failures);
        return 1;
    }
    printf("ALL OTA TESTS PASSED\n");
    return 0;
}
