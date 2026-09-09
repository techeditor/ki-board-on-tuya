/**
 * @file ak_scene.c
 * @brief Single-mascot scene engine (FP-1 T3).
 *
 * Sprite assets, the RLE runtime, and the animation registry are ported
 * verbatim from CodeTank (ref_projects/CodeTank), whose sprite/scene logic is
 * adapted from clawd-tank (MIT). This file is a compact re-implementation of
 * the single-slot subset of app_scene.c: one image, per-anim frame timing,
 * looping vs one-shot with fallback. Multi-session / transitions / HUD are out
 * of scope for this test.
 */

#include "ak_scene.h"

#include "tal_api.h"

#include "lvgl.h"
#include "lv_vendor.h"

#include "app_rle_sprite.h"
/* Pulls in every sprite_mascot_*.h and defines mascot_anim_defs[MASCOT_ANIM_COUNT]. */
#include "assets/generated/sprite_mascot_registry.h"

/* RGB565 value treated as fully transparent (matches CodeTank app_scene.c). */
#define AK_SCENE_TRANSPARENT_KEY 0x18C5

/* Base tick for the frame timer; per-anim frame_ms gates actual advances. */
#define AK_SCENE_TICK_MS 20U

typedef struct {
    lv_obj_t        *img;
    lv_image_dsc_t   dsc;
    uint8_t         *frame_buf; /* sized for the largest sprite, RGB565A8 */
    size_t           frame_buf_size;
    lv_timer_t      *timer;
    mascot_anim_id_t cur_anim;        /* currently displayed */
    mascot_anim_id_t persistent_anim; /* looping state to fall back to */
    bool             oneshot;         /* cur_anim is a one-shot in flight */
    int              frame_idx;
    uint32_t         last_advance_ms;
} ak_scene_t;

static ak_scene_t s_scene = {0};

static const char *const s_anim_names[MASCOT_ANIM_COUNT] = {
    "idle",     "alert",    "happy",      "sleeping", "disconnected", "thinking", "typing",
    "juggling", "building", "confused",   "dizzy",    "sweeping",     "walking",  "going_away",
    "debugger", "wizard",   "conducting", "beacon",   "mini_mascot",
};

const char *ak_scene_anim_name(mascot_anim_id_t anim)
{
    if (anim < 0 || anim >= MASCOT_ANIM_COUNT) {
        return "?";
    }
    return s_anim_names[anim];
}

mascot_anim_id_t ak_scene_persistent(void)
{
    return s_scene.persistent_anim;
}

/* Decode frame `idx` of the current anim and point the LVGL image at it. */
static void decode_and_apply(int idx)
{
    const mascot_anim_def_t *def = &mascot_anim_defs[s_scene.cur_anim];
    int                      w   = def->width;
    int                      h   = def->height;

    if (idx >= def->frame_count) {
        idx = def->frame_count - 1;
    }

    const uint16_t *frame_rle = &def->rle_data[def->frame_offsets[idx]];
    rle_decode_rgb565a8(frame_rle, s_scene.frame_buf, w * h, AK_SCENE_TRANSPARENT_KEY);

    s_scene.dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_scene.dsc.header.w      = w;
    s_scene.dsc.header.h      = h;
    s_scene.dsc.header.cf     = LV_COLOR_FORMAT_RGB565A8;
    s_scene.dsc.header.stride = w * 2;
    s_scene.dsc.data          = s_scene.frame_buf;
    s_scene.dsc.data_size     = w * h * 3;

    lv_image_set_src(s_scene.img, &s_scene.dsc);
}

/* Resize + re-center the image to the current anim and show frame 0. */
static void apply_anim_geometry(void)
{
    const mascot_anim_def_t *def = &mascot_anim_defs[s_scene.cur_anim];

    s_scene.frame_idx       = 0;
    s_scene.last_advance_ms = tal_system_get_millisecond();
    decode_and_apply(0);
    lv_obj_set_size(s_scene.img, def->width, def->height);
    lv_obj_align(s_scene.img, LV_ALIGN_CENTER, 0, def->y_offset);
}

/* Frame timer — runs on the LVGL task, already inside the vendor lock. */
static void frame_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    const mascot_anim_def_t *def = &mascot_anim_defs[s_scene.cur_anim];
    uint32_t                 now = tal_system_get_millisecond();

    if ((now - s_scene.last_advance_ms) < (uint32_t)def->frame_ms) {
        return;
    }
    s_scene.last_advance_ms = now;

    if (s_scene.oneshot && !def->looping) {
        if (s_scene.frame_idx < def->frame_count - 1) {
            s_scene.frame_idx++;
            decode_and_apply(s_scene.frame_idx);
        } else {
            /* One-shot finished: fall back to the persistent looping anim. */
            s_scene.oneshot  = false;
            s_scene.cur_anim = s_scene.persistent_anim;
            apply_anim_geometry();
        }
    } else {
        s_scene.frame_idx = (s_scene.frame_idx + 1) % def->frame_count;
        decode_and_apply(s_scene.frame_idx);
    }
}

void ak_scene_set_anim(mascot_anim_id_t anim)
{
    if (anim < 0 || anim >= MASCOT_ANIM_COUNT) {
        return;
    }
    lv_vendor_disp_lock();
    s_scene.persistent_anim = anim;
    s_scene.cur_anim        = anim;
    s_scene.oneshot         = false;
    apply_anim_geometry();
    lv_vendor_disp_unlock();
    PR_NOTICE("scene set_anim -> %s", ak_scene_anim_name(anim));
}

void ak_scene_play_oneshot(mascot_anim_id_t anim)
{
    if (anim < 0 || anim >= MASCOT_ANIM_COUNT) {
        return;
    }
    lv_vendor_disp_lock();
    /* persistent_anim is left untouched so we return to it when done. */
    s_scene.cur_anim = anim;
    s_scene.oneshot  = true;
    apply_anim_geometry();
    lv_vendor_disp_unlock();
    PR_NOTICE("scene oneshot -> %s (fallback %s)", ak_scene_anim_name(anim),
              ak_scene_anim_name(s_scene.persistent_anim));
}

OPERATE_RET ak_scene_init(void)
{
    size_t max_bytes = 0;

    /* Buffer big enough for the largest sprite (RGB565A8 = 3 bytes/pixel). */
    for (int i = 0; i < MASCOT_ANIM_COUNT; i++) {
        size_t bytes = (size_t)mascot_anim_defs[i].width * mascot_anim_defs[i].height * 3U;
        if (bytes > max_bytes) {
            max_bytes = bytes;
        }
    }

    s_scene.frame_buf = tal_malloc(max_bytes);
    if (NULL == s_scene.frame_buf) {
        PR_ERR("scene frame buffer alloc failed (%u bytes)", (unsigned)max_bytes);
        return OPRT_MALLOC_FAILED;
    }
    s_scene.frame_buf_size = max_bytes;

    lv_vendor_disp_lock();
    s_scene.img = lv_image_create(lv_screen_active());
    lv_image_set_inner_align(s_scene.img, LV_IMAGE_ALIGN_CENTER);

    s_scene.persistent_anim = MASCOT_ANIM_IDLE;
    s_scene.cur_anim        = MASCOT_ANIM_IDLE;
    s_scene.oneshot         = false;
    apply_anim_geometry();

    s_scene.timer = lv_timer_create(frame_timer_cb, AK_SCENE_TICK_MS, NULL);
    lv_vendor_disp_unlock();

    PR_NOTICE("scene init: %d anims, frame buf %u bytes", MASCOT_ANIM_COUNT, (unsigned)max_bytes);
    return OPRT_OK;
}
