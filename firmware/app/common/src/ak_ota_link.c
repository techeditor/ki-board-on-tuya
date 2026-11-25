/**
 * @file ak_ota_link.c
 * @brief Local OTA on the device (see ak_ota_link.h).
 *
 * Flash layer: the T5AI platform's own OTA bookkeeping (platform/T5AI/
 * tuyaos/tuyaos_adapter/src/system/ota_head.c) decides where the UG package
 * lives in the ota partition and writes the manage block tuyaboot reads at
 * the next boot. tkl_ota.c is not used: it starts writing before the header
 * is checked and cannot retry within one boot.
 * No Tuya cloud service is involved.
 */

#include "ak_ota_link.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ak_board_io.h"
#include "ak_kv.h"
#include "ak_rx.h"
#include "ak_uart.h"
#include "mbedtls/base64.h"
#include "tal_api.h"
#include "tkl_flash.h"

#if AK_OTA_USE_TEST_KEY
#include "ak_ota_key_test.h"
#define AK_OTA_KEY_IS_RELEASE false
#define AK_OTA_KEY_NAME       "test"
#else
#include "ak_ota_key_release.h"
#define AK_OTA_KEY_IS_RELEASE true
#define AK_OTA_KEY_NAME       (AK_OTA_KEY_PLACEHOLDER ? "none" : "release")
#endif

#ifndef PROJECT_VERSION
#define PROJECT_VERSION "0.0.0"
#endif

/* Platform OTA bookkeeping (ota_head.c); not in a public header. */
extern OPERATE_RET bk_ota_get_file_init(uint32_t image_size);
extern OPERATE_RET bk_ota_get_file_header(TUYA_OTA_DATA_T *pack);
extern OPERATE_RET bk_ota_write_manage_info(void);

#define AK_OTA_KV_HIGHEST "ak_ota_hi"   /* highest version ever running */
#define AK_OTA_KV_PENDING "ak_ota_pend" /* version handed to tuyaboot */
#define SECTOR            4096U
#define ERASE_STEP        (32U * 1024U)
#define OTA_LINE_MAX          192U
#define B64_HEADER_MAX    352U /* base64(256) = 344 */
#define B64_CHUNK_MAX     352U /* base64(256) = 344 */
#define COMMIT_FLUSH_MS   300U

static const uint8_t      s_key_point[AK_OTA_KEY_POINT_LEN] = AK_OTA_KEY_POINT;
static const ak_ota_key_t s_key = {s_key_point, AK_OTA_KEY_PLACEHOLDER != 0, AK_OTA_KEY_IS_RELEASE};

static ak_ota_link_cb_t s_cb;
static ak_ota_session_t s_session;
static ak_ota_version_t s_running;
static ak_ota_version_t s_highest;
static bool             s_have_highest;
static bool             s_updated_pending_report;
static uint8_t          s_last_percent = 0xFFU;

/* ── flash layer ─────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t base;   /* where the UG package goes */
    uint32_t span;   /* sector-aligned bytes to erase */
    uint32_t erased; /* bytes erased so far */
} ak_ota_area_t;

static ak_ota_area_t s_area;
static uint8_t       s_flash_buf[SECTOR];

static uint32_t align_up(uint32_t v, uint32_t a)
{
    return (v + a - 1U) / a * a;
}

static int flash_prepare(void *ctx, const uint8_t ug_header[AK_OTA_UG_HEADER_LEN], uint32_t image_size)
{
    ak_ota_area_t         *area = ctx;
    TUYA_FLASH_BASE_INFO_T app;
    TUYA_FLASH_BASE_INFO_T mgr;
    uint8_t                header[AK_OTA_UG_HEADER_LEN];
    TUYA_OTA_DATA_T        pack;

    if (tkl_flash_get_one_type_info(TUYA_FLASH_TYPE_APP, &app) != OPRT_OK ||
        tkl_flash_get_one_type_info(TUYA_FLASH_TYPE_OTA, &mgr) != OPRT_OK ||
        bk_ota_get_file_init(image_size) != OPRT_OK) {
        PR_ERR("ota: flash layout unavailable");
        return -1;
    }
    memcpy(header, ug_header, sizeof(header));
    memset(&pack, 0, sizeof(pack));
    pack.total_len = image_size;
    pack.data      = header;
    pack.len       = sizeof(header);
    if (bk_ota_get_file_header(&pack) != OPRT_OK) {
        PR_ERR("ota: platform rejected the image header (too large for this layout?)");
        return -1;
    }
    /* The package must sit wholly inside the restore area, after the space the
     * installed image will occupy (34/32 on-flash expansion), and end before
     * the manage block. Anything else would let tuyaboot overwrite its own
     * input or let us erase the running image. */
    uint32_t dst_len   = (uint32_t)ug_header[20] | ((uint32_t)ug_header[21] << 8) |
                         ((uint32_t)ug_header[22] << 16) | ((uint32_t)ug_header[23] << 24);
    uint32_t app_start = app.partition[0].start_addr;
    uint32_t img_end   = app_start + align_up(dst_len / 32U * 34U + 34U, SECTOR);
    uint32_t mgr_start = mgr.partition[0].start_addr;
    uint32_t span      = align_up(image_size, SECTOR);
    if (pack.start_addr % SECTOR != 0U || pack.start_addr < img_end || pack.start_addr + span > mgr_start ||
        mgr_start > app_start + app.partition[0].size) {
        PR_ERR("ota: unsafe package address %08x (image end %08x, manage %08x)", (unsigned)pack.start_addr,
               (unsigned)img_end, (unsigned)mgr_start);
        return -1;
    }
    area->base   = pack.start_addr;
    area->span   = span;
    area->erased = 0U;
    PR_NOTICE("ota: package area %08x + %u", (unsigned)area->base, (unsigned)area->span);
    return 0;
}

static int flash_erase_step(void *ctx, bool *done)
{
    ak_ota_area_t *area = ctx;
    uint32_t       n    = area->span - area->erased;
    n                   = n > ERASE_STEP ? ERASE_STEP : n;
    if (n > 0U && tkl_flash_erase(area->base + area->erased, n) != OPRT_OK) {
        PR_ERR("ota: erase failed at %08x", (unsigned)(area->base + area->erased));
        return -1;
    }
    area->erased += n;
    *done = area->erased >= area->span;
    return 0;
}

static int flash_write(void *ctx, uint32_t offset, const uint8_t *data, uint32_t len)
{
    ak_ota_area_t *area = ctx;
    if (offset + len > area->span) {
        return -1;
    }
    return tkl_flash_write(area->base + offset, data, len) == OPRT_OK ? 0 : -1;
}

static int flash_readback_sha256(void *ctx, uint32_t len, uint8_t out[32])
{
    ak_ota_area_t           *area = ctx;
    mbedtls_sha256_context sha;
    int                      rc = 0;
    mbedtls_sha256_init(&sha);
    (void)mbedtls_sha256_starts(&sha, 0);
    for (uint32_t off = 0U; off < len && rc == 0; off += SECTOR) {
        uint32_t n = len - off > SECTOR ? SECTOR : len - off;
        if (tkl_flash_read(area->base + off, s_flash_buf, n) != OPRT_OK) {
            rc = -1;
        } else {
            (void)mbedtls_sha256_update(&sha, s_flash_buf, n);
        }
    }
    if (rc == 0) {
        (void)mbedtls_sha256_finish(&sha, out);
    }
    mbedtls_sha256_free(&sha);
    return rc;
}

static const ak_ota_flash_ops_t s_flash_ops = {
    flash_prepare, flash_erase_step, flash_write, flash_readback_sha256,
};

/* ── frames ──────────────────────────────────────────────────────────────── */

static void send_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void send_line(const char *fmt, ...)
{
    char    line[OTA_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0 && (size_t)n < sizeof(line) && s_cb.send_uart != NULL) {
        s_cb.send_uart(line, n);
    }
}

static void send_status(const char *state, ak_ota_err_t reason)
{
    char ver[24] = "";
    if (s_session.state != AK_OTA_IDLE || strcmp(state, "installing") == 0) {
        ak_ota_version_str(&s_session.manifest.version, ver, sizeof(ver));
    }
    if (reason != AK_OTA_OK) {
        send_line("{\"v\":1,\"type\":\"ota_status\",\"payload\":{\"state\":\"%s\",\"reason\":\"%s\",\"fw\":\"%s\"}}\n",
                  state, ak_ota_err_str(reason), PROJECT_VERSION);
    } else {
        send_line("{\"v\":1,\"type\":\"ota_status\",\"payload\":{\"state\":\"%s\",\"version\":\"%s\",\"next\":%u,"
                  "\"fw\":\"%s\"}}\n",
                  state, ver, (unsigned)s_session.next, PROJECT_VERSION);
    }
}

static void send_ack(uint32_t next)
{
    send_line("{\"v\":1,\"type\":\"ota_ack\",\"payload\":{\"next\":%u}}\n", (unsigned)next);
}

static void ui(ak_ota_ui_t what, const char *text)
{
    if (s_cb.ui != NULL) {
        s_cb.ui(what, text);
    }
}

static void report_failure(ak_ota_err_t err)
{
    PR_WARN("ota: update failed: %s", ak_ota_err_str(err));
    send_status("failed", err);
    ui(AK_OTA_UI_FAILED, ak_ota_err_str(err));
    s_last_percent = 0xFFU;
}

static bool power_ok(void)
{
    uint8_t percent = 0U;
    bool    usb     = s_cb.usb_power != NULL && s_cb.usb_power();
    bool    trusted = s_cb.battery_trusted != NULL && s_cb.battery_trusted(&percent);
    return ak_ota_power_allows(usb, trusted, percent);
}

static void on_begin(const char *payload, uint32_t now_ms)
{
    char    b64[B64_HEADER_MAX];
    uint8_t header[AK_OTA_HEADER_LEN];
    size_t  n = 0U;
    if (s_session.state != AK_OTA_IDLE) {
        send_status("failed", AK_OTA_E_BUSY); /* the open session continues */
        return;
    }
    if (ak_json_get_str(payload, "h", b64, sizeof(b64)) != AK_JSON_OK ||
        mbedtls_base64_decode(header, sizeof(header), &n, (const unsigned char *)b64, strlen(b64)) != 0 ||
        n != sizeof(header)) {
        report_failure(AK_OTA_E_FORMAT);
        return;
    }
    ak_ota_err_t err = ak_ota_begin(&s_session, header, &s_key, &s_running, s_have_highest ? &s_highest : NULL,
                                    power_ok(), now_ms);
    if (err != AK_OTA_OK) {
        report_failure(err);
        return;
    }
    char ver[24];
    ak_ota_version_str(&s_session.manifest.version, ver, sizeof(ver));
    PR_NOTICE("ota: signed package %s accepted; asking on the device", ver);
    send_status("confirm", AK_OTA_OK);
    ui(AK_OTA_UI_CONFIRM, ver);
}

static void on_data(const char *payload, uint32_t now_ms)
{
    char     b64[B64_CHUNK_MAX];
    uint8_t  chunk[AK_OTA_CHUNK_MAX];
    size_t   n      = 0U;
    uint32_t offset = 0U;
    uint32_t next   = s_session.next;
    if (s_session.state != AK_OTA_RECEIVING) {
        return; /* stale window after a failure: the status already went out */
    }
    if (ak_json_get_uint(payload, "o", AK_OTA_IMAGE_MAX, &offset) != AK_JSON_OK ||
        ak_json_get_str(payload, "d", b64, sizeof(b64)) != AK_JSON_OK ||
        mbedtls_base64_decode(chunk, sizeof(chunk), &n, (const unsigned char *)b64, strlen(b64)) != 0) {
        ak_ota_fail(&s_session, AK_OTA_E_SEQUENCE);
        report_failure(AK_OTA_E_SEQUENCE);
        return;
    }
    ak_ota_err_t err = ak_ota_data(&s_session, offset, chunk, (uint32_t)n, now_ms, &next);
    if (err != AK_OTA_OK) {
        report_failure(err);
        return;
    }
    send_ack(next);
    uint8_t percent = (uint8_t)((uint64_t)next * 100U / s_session.manifest.image_size);
    if (percent / 5U != s_last_percent / 5U) {
        s_last_percent = percent;
        ui(AK_OTA_UI_PROGRESS, "");
    }
}

static void commit_and_reboot(void)
{
    char ver[24];
    ak_ota_version_str(&s_session.manifest.version, ver, sizeof(ver));
    if (!power_ok()) {
        ak_ota_fail(&s_session, AK_OTA_E_POWER);
        report_failure(AK_OTA_E_POWER);
        return;
    }
    if (ak_kv_set(AK_OTA_KV_PENDING, (const uint8_t *)ver, strlen(ver) + 1U) != OPRT_OK) {
        PR_WARN("ota: could not store the pending marker (no 'Updated' notice)");
    }
    if (bk_ota_write_manage_info() != OPRT_OK) {
        (void)ak_kv_del(AK_OTA_KV_PENDING);
        ak_ota_fail(&s_session, AK_OTA_E_FLASH);
        report_failure(AK_OTA_E_FLASH);
        return;
    }
    PR_NOTICE("ota: %s verified; manage block written, rebooting into tuyaboot (t=%u ms)", ver,
              (unsigned)tal_system_get_millisecond());
    send_status("installing", AK_OTA_OK);
    ui(AK_OTA_UI_INSTALLING, "");
    tal_system_sleep(COMMIT_FLUSH_MS); /* let the frame and the LCD go out */
    tal_system_reset();
}

static void on_end(void)
{
    ak_ota_err_t err = ak_ota_end(&s_session);
    if (err != AK_OTA_OK) {
        if (err == AK_OTA_E_STATE) {
            send_status("failed", err);
        } else {
            report_failure(err);
        }
        return;
    }
    commit_and_reboot();
}

/* ── debug probes ────────────────────────────────────────────────────────── */

#if AK_ENABLE_DEBUG_PROBES
static uint32_t s_probe_baud_until;
static uint32_t s_probe_good;
static uint32_t s_probe_bad;

static void probe_baud_done(void)
{
    (void)ak_uart_set_baud(AK_UART_BAUDRATE);
    PR_NOTICE("probe: UART0 back at %u (good %u bad %u)", (unsigned)AK_UART_BAUDRATE, (unsigned)s_probe_good,
              (unsigned)s_probe_bad);
    send_line("{\"v\":1,\"type\":\"probe_result\",\"payload\":{\"op\":\"baud\",\"state\":\"done\",\"good\":%u,"
              "\"bad\":%u}}\n",
              (unsigned)s_probe_good, (unsigned)s_probe_bad);
    s_probe_baud_until = 0U;
}

static void probe_flash(void)
{
    TUYA_FLASH_BASE_INFO_T mgr;
    /* 256 KB below the manage block is inside the ota partition and holds no
     * package while no update is open; the manage block is never touched. */
    if (tkl_flash_get_one_type_info(TUYA_FLASH_TYPE_OTA, &mgr) != OPRT_OK) {
        return;
    }
    uint32_t addr = mgr.partition[0].start_addr - 256U * 1024U;
    addr -= addr % (64U * 1024U);
    uint32_t t0 = tal_system_get_millisecond();
    bool     ok = tkl_flash_erase(addr, 64U * 1024U) == OPRT_OK;
    uint32_t t1 = tal_system_get_millisecond();
    memset(s_flash_buf, 0x5A, sizeof(s_flash_buf));
    for (uint32_t off = 0U; ok && off < 64U * 1024U; off += AK_OTA_CHUNK_MAX) {
        ok = tkl_flash_write(addr + off, s_flash_buf, AK_OTA_CHUNK_MAX) == OPRT_OK;
    }
    uint32_t t2 = tal_system_get_millisecond();
    for (uint32_t off = 0U; ok && off < 64U * 1024U; off += SECTOR) {
        ok = tkl_flash_read(addr + off, s_flash_buf, SECTOR) == OPRT_OK && s_flash_buf[SECTOR - 1U] == 0x5A;
    }
    uint32_t t3 = tal_system_get_millisecond();
    ok          = ok && tkl_flash_erase(addr, 64U * 1024U) == OPRT_OK;
    send_line("{\"v\":1,\"type\":\"probe_result\",\"payload\":{\"op\":\"flash\",\"ok\":%s,\"addr\":%u,"
              "\"erase64k_ms\":%u,\"write64k_ms\":%u,\"read64k_ms\":%u}}\n",
              ok ? "true" : "false", (unsigned)addr, (unsigned)(t1 - t0), (unsigned)(t2 - t1), (unsigned)(t3 - t2));
}

static void on_probe(const char *payload, uint32_t now_ms)
{
    char op[8] = "";
    if (s_session.state != AK_OTA_IDLE || ak_json_get_str(payload, "op", op, sizeof(op)) != AK_JSON_OK) {
        return;
    }
    if (strcmp(op, "flash") == 0) {
        probe_flash();
    } else if (strcmp(op, "forget") == 0) {
        /* Test loops reflash an older version with tyutool; QIO does not
         * reach the KV area, so drop the anti-downgrade record here. */
        s_highest = s_running;
        (void)ak_kv_set(AK_OTA_KV_HIGHEST, (const uint8_t *)PROJECT_VERSION, strlen(PROJECT_VERSION) + 1U);
        send_line("{\"v\":1,\"type\":\"probe_result\",\"payload\":{\"op\":\"forget\",\"highest\":\"%s\"}}\n",
                  PROJECT_VERSION);
    } else if (strcmp(op, "baud") == 0 && s_probe_baud_until == 0U) {
        uint32_t baud = 0U;
        uint32_t secs = 0U;
        if (ak_json_get_uint(payload, "baud", 3000000U, &baud) != AK_JSON_OK || baud < 9600U ||
            ak_json_get_uint(payload, "secs", 120U, &secs) != AK_JSON_OK || secs < 5U) {
            return;
        }
        send_line("{\"v\":1,\"type\":\"probe_result\",\"payload\":{\"op\":\"baud\",\"state\":\"switching\","
                  "\"baud\":%u,\"secs\":%u}}\n",
                  (unsigned)baud, (unsigned)secs);
        tal_system_sleep(100U); /* drain TX at the old rate */
        s_probe_good = 0U;
        s_probe_bad  = 0U;
        if (ak_uart_set_baud(baud) != OPRT_OK) {
            (void)ak_uart_set_baud(AK_UART_BAUDRATE);
            return;
        }
        s_probe_baud_until = now_ms + secs * 1000U;
        if (s_probe_baud_until == 0U) {
            s_probe_baud_until = 1U;
        }
        PR_NOTICE("probe: UART0 at %u for %u s", (unsigned)baud, (unsigned)secs);
    }
}

/* probe_line {"i":N,"d":"<hex>","c":crc32 of the hex text}: echo the index
 * so the host sees what got through at the probed rate. */
static void on_probe_line(const char *payload)
{
    char     data[256];
    uint32_t index = 0U;
    uint32_t crc   = 0U;
    if (s_probe_baud_until == 0U) {
        return;
    }
    if (ak_json_get_uint(payload, "i", 0xFFFFFFFFUL, &index) == AK_JSON_OK &&
        ak_json_get_str(payload, "d", data, sizeof(data)) == AK_JSON_OK &&
        ak_json_get_uint(payload, "c", 0xFFFFFFFFUL, &crc) == AK_JSON_OK &&
        ak_ota_crc32((const uint8_t *)data, strlen(data)) == crc) {
        s_probe_good++;
        send_line("{\"v\":1,\"type\":\"probe_echo\",\"payload\":{\"i\":%u}}\n", (unsigned)index);
    } else {
        s_probe_bad++;
    }
}
#endif

/* ── public ──────────────────────────────────────────────────────────────── */

static bool kv_read_version(const char *key, ak_ota_version_t *out)
{
    uint8_t *blob = NULL;
    size_t   len  = 0U;
    bool     ok   = false;
    if (ak_kv_get(key, &blob, &len) == OPRT_OK && blob != NULL && len > 0U && len < 24U && blob[len - 1U] == '\0') {
        ok = ak_ota_version_parse((const char *)blob, out);
    }
    ak_kv_free(blob);
    return ok;
}

void ak_ota_link_init(const ak_ota_link_cb_t *cb)
{
    ak_ota_version_t pending;
    s_cb = *cb;
    ak_ota_init(&s_session, &s_flash_ops, &s_area);
    if (!ak_ota_version_parse(PROJECT_VERSION, &s_running)) {
        memset(&s_running, 0, sizeof(s_running));
    }
    s_have_highest = kv_read_version(AK_OTA_KV_HIGHEST, &s_highest);
    if (!s_have_highest || ak_ota_version_cmp(&s_running, &s_highest) > 0) {
        s_highest      = s_running;
        s_have_highest = true;
        (void)ak_kv_set(AK_OTA_KV_HIGHEST, (const uint8_t *)PROJECT_VERSION, strlen(PROJECT_VERSION) + 1U);
    }
    if (kv_read_version(AK_OTA_KV_PENDING, &pending)) {
        (void)ak_kv_del(AK_OTA_KV_PENDING);
        if (ak_ota_version_cmp(&pending, &s_running) == 0) {
            PR_NOTICE("ota: first boot after installing %s", PROJECT_VERSION);
            s_updated_pending_report = true;
            ui(AK_OTA_UI_UPDATED, PROJECT_VERSION);
        } else {
            PR_WARN("ota: tuyaboot did not install the pending package; still %s", PROJECT_VERSION);
        }
    }
    if (s_key.placeholder) {
        PR_WARN("ota: release key is the placeholder; this build accepts no OTA package");
    }
}

bool ak_ota_link_on_frame(const char *type, const char *payload, uint32_t now_ms)
{
    if (strcmp(type, "ota_begin") == 0) {
        on_begin(payload, now_ms);
    } else if (strcmp(type, "ota_data") == 0) {
        on_data(payload, now_ms);
    } else if (strcmp(type, "ota_end") == 0) {
        on_end();
    } else if (strcmp(type, "ota_abort") == 0) {
        if (s_session.state != AK_OTA_IDLE) {
            ak_ota_fail(&s_session, AK_OTA_E_ABORTED);
            report_failure(AK_OTA_E_ABORTED);
        }
#if AK_ENABLE_DEBUG_PROBES
    } else if (strcmp(type, "ota_probe") == 0) {
        on_probe(payload, now_ms);
    } else if (strcmp(type, "probe_line") == 0) {
        on_probe_line(payload);
#endif
    } else {
        return false;
    }
    return true;
}

void ak_ota_link_tick(uint32_t now_ms)
{
#if AK_ENABLE_DEBUG_PROBES
    if (s_probe_baud_until != 0U && (int32_t)(now_ms - s_probe_baud_until) >= 0) {
        probe_baud_done();
    }
#endif
    ak_ota_state_t before = s_session.state;
    if (!ak_ota_tick(&s_session, now_ms)) {
        return;
    }
    if (s_session.state == AK_OTA_IDLE) {
        report_failure(s_session.last_error);
    } else if (before == AK_OTA_ERASING && s_session.state == AK_OTA_RECEIVING) {
        send_status("ready", AK_OTA_OK); /* Companion starts sending at 0 */
        ui(AK_OTA_UI_PROGRESS, "");
    }
}

void ak_ota_link_on_hello(void)
{
    if (s_session.state != AK_OTA_IDLE) {
        return;
    }
    if (s_updated_pending_report) {
        s_updated_pending_report = false;
        send_line("{\"v\":1,\"type\":\"ota_status\",\"payload\":{\"state\":\"installed\",\"version\":\"%s\","
                  "\"fw\":\"%s\",\"key\":\"%s\"}}\n",
                  PROJECT_VERSION, PROJECT_VERSION, AK_OTA_KEY_NAME);
        return;
    }
    send_line("{\"v\":1,\"type\":\"ota_status\",\"payload\":{\"state\":\"idle\",\"fw\":\"%s\",\"key\":\"%s\"}}\n",
              PROJECT_VERSION, AK_OTA_KEY_NAME);
}

bool ak_ota_link_busy(void)
{
#if AK_ENABLE_DEBUG_PROBES
    if (s_probe_baud_until != 0U) {
        return true;
    }
#endif
    return s_session.state != AK_OTA_IDLE;
}

bool ak_ota_link_confirming(void)
{
    return s_session.state == AK_OTA_CONFIRM;
}

bool ak_ota_link_view(ak_ota_view_t *out)
{
    if (s_session.state == AK_OTA_IDLE) {
        return false;
    }
    out->state = s_session.state;
    ak_ota_version_str(&s_session.manifest.version, out->version, sizeof(out->version));
    out->percent = s_session.manifest.image_size == 0U
                       ? 0U
                       : (uint8_t)((uint64_t)s_session.next * 100U / s_session.manifest.image_size);
    return true;
}

void ak_ota_link_confirm(bool accept, uint32_t now_ms)
{
    ak_ota_err_t err = ak_ota_confirm(&s_session, accept, now_ms);
    if (err == AK_OTA_E_STATE) {
        return;
    }
    if (err != AK_OTA_OK) {
        report_failure(err);
        return;
    }
    PR_NOTICE("ota: confirmed on the device; erasing %u bytes", (unsigned)s_area.span);
    send_status("erasing", AK_OTA_OK);
    ui(AK_OTA_UI_PROGRESS, "");
}
