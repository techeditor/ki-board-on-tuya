/**
 * @file ak_asr.c
 * @brief Board-side (Tuya cloud) ASR backend — stub + gated real impl.
 *
 * See ak_asr.h. AK_ASR_BOARD_CLOUD selects the implementation at build time.
 */

#include "ak_asr.h"

#include "ak_features.h"
#include "tal_api.h"

#if AK_ASR_BOARD_CLOUD

/* ── Real backend (Tuya cloud ASR) ──────────────────────────────────────────
 * Ported from firmware/feature-probes/tuya_cloud_asr (ai_chat HOLD mode + Tuya
 * AI agent). Requires ENABLE_AI_COMPONENTS, Wi-Fi provisioning and a PID bound
 * to an AI agent.
 *
 * K4 stays owned by ak_input (no ai_chat button is registered): the app's
 * on_ptt_edge drives capture directly via tuya_ai_input_start/stop, so the same
 * physical key serves both the "system" (PC ASR) and "cloud" routes selected at
 * runtime. HOLD mode is still used for its MANUAL-VAD audio pipeline + event
 * stream; recognized text arrives on AI_USER_EVT_ASR_OK and is handed to
 * s_on_text as a NUL-terminated string. */

#include "ai_chat_main.h"
#include "ai_user_event.h"
#include "ai_audio_player.h"
#include "netmgr.h"
#include "tuya_ai_agent.h"

#include <string.h>

static ak_asr_text_cb s_on_text   = NULL;
static ak_asr_error_cb s_on_error = NULL;
static bool           s_ai_ready   = false; /* ai_chat_init succeeded */
/* When true the board does NOT speak the AI's TTS reply: the recognized text is
 * routed to the PC / Kiro instead, so the keyboard stays silent. Local alert
 * tones (AI_USER_EVT_PLAY_ALERT) use the same player at a different time and are
 * left untouched. Ported from FP-3 (app_chat_bot.c). */
static bool           s_mute_ai_reply = true;

/* Copy AI_NOTIFY_TEXT_T into a NUL-terminated buffer and hand to the callback.
 * The full UTF-8 text is preserved (the caller sanitizes for JSON on the wire). */
static void __emit_text(const AI_NOTIFY_TEXT_T *t)
{
    char buf[256];
    uint16_t n;

    if (s_on_text == NULL || t == NULL || t->data == NULL || t->datalen == 0) {
        return;
    }
    n = (t->datalen < sizeof(buf) - 1) ? t->datalen : (uint16_t)(sizeof(buf) - 1);
    memcpy(buf, t->data, n);
    buf[n] = '\0';
    s_on_text(buf);
}

static void __ai_event_cb(AI_NOTIFY_EVENT_T *event)
{
    if (event == NULL) {
        return;
    }
    switch (event->type) {
    case AI_USER_EVT_MIC_DATA:
        /* Mic capture is streaming => K4 push-to-talk is held. (FP-3 emits a
         * "listening" edge here; the product reflects status via the LCD from
         * tuya_app_main, so nothing to forward from this layer.) */
        break;
    case AI_USER_EVT_ASR_OK:
        /* Recognized text ready -> hand to the app, which emits an `asr` frame. */
        __emit_text((const AI_NOTIFY_TEXT_T *)event->data);
        break;
    case AI_USER_EVT_ASR_ERROR:
        PR_WARN("ak_asr: cloud ASR error");
        if (s_on_error != NULL) {
            s_on_error("asr_error");
        }
        break;
    case AI_USER_EVT_ASR_EMPTY:
        PR_NOTICE("ak_asr: cloud ASR returned empty (no speech recognized)");
        if (s_on_error != NULL) {
            s_on_error("asr_empty");
        }
        break;
    case AI_USER_EVT_TTS_START:
    case AI_USER_EVT_TTS_DATA:
        /* Suppress the AI's spoken reply: the recognized text is routed to the
         * PC / Kiro, so the board must not talk back. Stop the tone player as the
         * reply TTS streams. Local alert tones fire on AI_USER_EVT_PLAY_ALERT at a
         * different time and are left untouched. (Ported from FP-3.) */
        if (s_mute_ai_reply) {
            ai_audio_player_stop(AI_AUDIO_PLAYER_ALL);
        }
        break;
    default:
        break;
    }
}

OPERATE_RET ak_asr_init(ak_asr_text_cb on_text, ak_asr_error_cb on_error)
{
    s_on_text  = on_text;
    s_on_error = on_error;

    AI_CHAT_MODE_CFG_T cfg = {
        .default_mode = AI_CHAT_MODE_HOLD, /* MANUAL VAD; capture driven by us */
        .default_vol  = 70,
        .evt_cb       = __ai_event_cb,
    };
    OPERATE_RET rt = ai_chat_init(&cfg);
    if (rt != OPRT_OK) {
        PR_ERR("ak_asr: ai_chat_init failed: %d", rt);
        s_ai_ready = false;
        return rt;
    }
    s_ai_ready = true;
    PR_NOTICE("ak_asr: cloud backend up (ai_chat HOLD, capture driven by K4)");
    return OPRT_OK;
}

bool ak_asr_available(void)
{
    /* Ready = ai_chat initialized AND the device is networked (cloud ASR needs
     * the link up). Authorization is a prerequisite of a successful network
     * connect, so netmgr LINK_UP implies an authorized, provisioned device. */
    netmgr_status_e st = NETMGR_LINK_DOWN;
    if (!s_ai_ready) {
        return false;
    }
    netmgr_conn_get(NETCONN_AUTO, NETCONN_CMD_STATUS, &st);
    return st == NETMGR_LINK_UP;
}

void ak_asr_start(void)
{
    if (s_ai_ready) {
#if defined(ENABLE_COMP_AI_AUDIO) && (ENABLE_COMP_AI_AUDIO == 1)
        /* Long-press "start talking" prompt tone (长按提示音). In HOLD mode the SDK
         * normally plays this from its own button/mode path (ai_chat_main); we drive
         * capture manually via tuya_ai_input_start (to gate cloud vs system on K4),
         * which bypasses that path — so play the alert here to match FP-3's UX. */
        ai_audio_player_alert(AI_AUDIO_ALERT_LONG_KEY_TALK);
#endif
        /* Begin a capture (force=false: normal agent scode). Recognized text
         * comes back later via __ai_event_cb / AI_USER_EVT_ASR_OK. */
        tuya_ai_input_start(false);
    }
}

void ak_asr_stop(void)
{
    if (s_ai_ready) {
        tuya_ai_input_stop();
    }
}

#else /* !AK_ASR_BOARD_CLOUD — stub */

OPERATE_RET ak_asr_init(ak_asr_text_cb on_text, ak_asr_error_cb on_error)
{
    (void)on_text;
    (void)on_error;
    return OPRT_OK;
}

bool ak_asr_available(void)
{
    return false;
}

void ak_asr_start(void)
{
}

void ak_asr_stop(void)
{
}

#endif /* AK_ASR_BOARD_CLOUD */
