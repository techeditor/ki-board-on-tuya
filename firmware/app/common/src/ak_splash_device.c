/**
 * @file ak_splash_device.c
 * @brief Boot splash on the status LCD: custom image from the USER0 flash
 *        area when it checks out, otherwise the built-in default. Read-only.
 */

#include "ak_splash.h"

#include <stdio.h>
#include <string.h>

#include "ak_gfx.h"
#include "ak_input.h"
#include "ak_palette.h"
#include "ak_splash_lockup.h"
#include "ak_statuslcd.h"
#include "tal_api.h"
#include "tkl_flash.h"
#include "tkl_memory.h"
#include "tkl_system.h"

static ak_splash_hold_t s_hold;

/* Events during the splash only end the hold; none reaches the app. */
static void splash_on_key(ak_input_kind_t kind, const char *id, ak_input_event_t event, uint32_t held_ms)
{
    (void)kind;
    (void)id;
    (void)held_ms;
    ak_splash_hold_key(&s_hold, (int)event);
}

static void splash_on_encoder(int32_t detents)
{
    ak_splash_hold_encoder(&s_hold, detents);
}

/* Short sleeps, not one long one: the keys are polled the whole time.
 * Runs before ak_cloud_init(), so tuya_health's watchdog is not armed yet. */
static void splash_hold(bool custom)
{
    uint32_t now = tal_system_get_millisecond();

    ak_splash_hold_begin(&s_hold, custom, now);
    while (!ak_splash_hold_done(&s_hold, now)) {
        ak_input_poll(now, splash_on_key, splash_on_encoder);
        tkl_system_sleep(AK_SPLASH_POLL_MS);
        now = tal_system_get_millisecond();
    }
    PR_NOTICE("splash: hold %u ms%s after %u ms", (unsigned)s_hold.hold_ms, s_hold.skipped ? ", skipped by a key" : "",
              (unsigned)(now - s_hold.start_ms));
}

static bool splash_region(TUYA_FLASH_BASE_INFO_T *info)
{
    if (OPRT_OK != tkl_flash_get_one_type_info(TUYA_FLASH_TYPE_USER0, info)) {
        return false;
    }
    return info->partition[0].block_size != 0U && info->partition[0].size >= AK_SPLASH_MAX_BYTES;
}

/* Draw the built-in default: the lockup bitmap of ak_splash_lockup.h (INK
 * White, the rest of SHAPE Black, on Purple) centred, "Ki-board" in the 8x8
 * font at scale 2 below it. */
static void splash_draw_default(void)
{
    const char    *name = "Ki-board";
    const uint16_t cell = 8U * 2U;
    const uint16_t gap  = 24U;
    uint16_t       y    = (uint16_t)((AK_STATUSLCD_H - AK_SPLASH_LOCKUP_H - gap - cell) / 2U);
    uint16_t       x    = (uint16_t)((AK_STATUSLCD_W - strlen(name) * cell) / 2U);

    ak_gfx_clear(AK_UI_SPLASH_BG);
    ak_gfx_icon_shape((uint16_t)((AK_STATUSLCD_W - AK_SPLASH_LOCKUP_W) / 2U), y, AK_SPLASH_LOCKUP_INK,
                      AK_SPLASH_LOCKUP_SHAPE, AK_SPLASH_LOCKUP_W, AK_SPLASH_LOCKUP_H, AK_UI_SPLASH_INK,
                      AK_UI_SPLASH_EYES, AK_UI_SPLASH_BG);
    y = (uint16_t)(y + AK_SPLASH_LOCKUP_H + gap);
    for (const char *p = name; *p != '\0'; p++) {
        ak_gfx_char(x, y, *p, AK_UI_SPLASH_INK, AK_UI_SPLASH_BG, 2);
        x = (uint16_t)(x + cell);
    }
}

void ak_splash_show(void)
{
    TUYA_FLASH_BASE_INFO_T info;
    uint8_t               hdr[AK_SPLASH_HDR_LEN];
    uint8_t              *buf   = NULL;
    ak_splash_t           img;
    bool                  custom = false;

    /* Header first, so a blank or foreign area never costs a full read. */
    if (splash_region(&info) && tkl_flash_read(info.partition[0].start_addr, hdr, sizeof(hdr)) == OPRT_OK &&
        !ak_splash_is_erased(hdr, sizeof(hdr))) {
        size_t total = 0;
        if (ak_splash_parse_header(hdr, sizeof(hdr), &img, &total)) {
            buf = (uint8_t *)tkl_system_malloc(total);
            if (buf != NULL && tkl_flash_read(info.partition[0].start_addr, buf, (uint32_t)total) == OPRT_OK &&
                ak_splash_parse(buf, total, &img)) {
                custom = true;
            }
        }
        if (!custom && buf != NULL) {
            tkl_system_free(buf);
            buf = NULL;
        }
    }

    if (custom) {
        uint16_t x = (uint16_t)((AK_STATUSLCD_W - img.width) / 2U);
        uint16_t y = (uint16_t)((AK_STATUSLCD_H - img.height) / 2U);
        ak_gfx_clear(AK_UI_BG);
        ak_gfx_pal4(x, y, img.width, img.height, img.palette, img.data);
        PR_NOTICE("splash: custom image %ux%u from USER0", (unsigned)img.width, (unsigned)img.height);
    } else {
        splash_draw_default();
        PR_NOTICE("splash: built-in default (no valid custom image)");
    }
    ak_statuslcd_flush();
    (void)ak_statuslcd_flush_wait(); /* the whole frame is on the panel before we wait */
    splash_hold(custom);

    if (buf != NULL) {
        tkl_system_free(buf);
    }
}

void ak_splash_report_region(void)
{
    TUYA_FLASH_BASE_INFO_T info;
    uint8_t               *buf = NULL;

    if (!splash_region(&info)) {
        PR_WARN("splash region: USER0 not available");
        return;
    }
    buf = (uint8_t *)tkl_system_malloc(AK_SPLASH_MAX_BYTES);
    if (buf == NULL || tkl_flash_read(info.partition[0].start_addr, buf, AK_SPLASH_MAX_BYTES) != OPRT_OK) {
        PR_WARN("splash region: read failed");
    } else {
        PR_NOTICE("splash region: USER0 addr=0x%08x len=%u erased=%d head64_crc=%08x region_crc=%08x",
                  (unsigned)info.partition[0].start_addr, (unsigned)AK_SPLASH_MAX_BYTES,
                  (int)ak_splash_is_erased(buf, AK_SPLASH_MAX_BYTES),
                  (unsigned)ak_splash_crc32(buf, 64U), (unsigned)ak_splash_crc32(buf, AK_SPLASH_MAX_BYTES));
    }
    if (buf != NULL) {
        tkl_system_free(buf);
    }

    /* Partition addresses as the running firmware sees them: the TuyaOpen
     * adapter values (what TAL KV / OTA use). The BK partition table generated
     * at build time is not read here (its headers need sdkconfig.h, not on the
     * app include path); compare it with build/.../partitions.txt. */
    static const struct {
        const char        *name;
        TUYA_FLASH_TYPE_E  type;
    } s_kinds[] = {
        {"KV", TUYA_FLASH_TYPE_KV_DATA}, {"UF", TUYA_FLASH_TYPE_UF},   {"OTA", TUYA_FLASH_TYPE_OTA},
        {"APP", TUYA_FLASH_TYPE_APP},    {"USER0", TUYA_FLASH_TYPE_USER0},
    };
    for (size_t i = 0; i < sizeof(s_kinds) / sizeof(s_kinds[0]); i++) {
        if (tkl_flash_get_one_type_info(s_kinds[i].type, &info) == OPRT_OK) {
            PR_NOTICE("splash region: adapter %s start=0x%08x size=0x%x", s_kinds[i].name,
                      (unsigned)info.partition[0].start_addr, (unsigned)info.partition[0].size);
        }
    }

}
