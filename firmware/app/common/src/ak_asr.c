/**
 * @file ak_asr.c
 * @brief Board-side (Tuya cloud) ASR backend — stub + gated real impl.
 *
 * See ak_asr.h. AK_ASR_BOARD_CLOUD selects the implementation at build time.
 */

#include "ak_asr.h"

#include "ak_features.h"
#include "tal_api.h"

static uint8_t s_volume = AK_ASR_DEFAULT_VOLUME;

#if AK_ASR_BOARD_CLOUD

/* ── Real backend (Tuya cloud ASR) ──────────────────────────────────────────
 * Ported from the tuya_cloud_asr probe, see git tag archive/feature-probes (ai_chat HOLD mode + Tuya
 * AI agent). Requires ENABLE_AI_COMPONENTS, Wi-Fi provisioning and a PID bound
 * to an AI agent.
 *
 * K4 stays owned by ak_input (no ai_chat hardware button is registered). The
 * app injects HOLD long-press/release events only when the selected route is
 * cloud, preserving system-mode Windows dictation. HOLD itself owns codec/VAD
 * capture and the cloud uploader; recognized text arrives on
 * AI_USER_EVT_ASR_OK and is handed to s_on_text as a NUL-terminated string. */

#include "ai_chat_main.h"
#include "ai_user_event.h"
#include "ai_audio_player.h"
#include "ai_manage_mode.h"
#include "netmgr.h"
#include "tdl_button_manage.h"
#include "tuya_ai_agent.h"
#include "ak_i18n.h"
#include "tal_mutex.h"

#include <string.h>

static ak_asr_text_cb s_on_text   = NULL;
static ak_asr_error_cb s_on_error = NULL;
static bool           s_ai_ready   = false; /* ai_chat_init succeeded */
/* When true the board does NOT speak the AI's TTS reply: the recognized text is
 * routed to the PC / Kiro instead, so the keyboard stays silent. Local alert
 * tones (AI_USER_EVT_PLAY_ALERT) use the same player at a different time and are
 * left untouched. Ported from FP-3 (app_chat_bot.c). */
static volatile bool  s_mute_ai_reply = true;

/* Chat turn shared between the AI event thread (writer) and the app thread
 * (ak_asr_chat_take). Guarded by s_chat_mutex; s_chat_dirty tells the app a
 * new copy is worth taking. */
static MUTEX_HANDLE  s_chat_mutex = NULL;
static ak_asr_chat_t s_chat;
static volatile bool s_chat_dirty;

static bool chat_lock(void)
{
    return s_chat_mutex != NULL && tal_mutex_lock(s_chat_mutex) == OPRT_OK;
}

static void chat_unlock(void)
{
    (void)tal_mutex_unlock(s_chat_mutex);
}

/* Record one Chat event. `t` may be NULL (no text). */
static void chat_record(AI_USER_EVT_TYPE_E type, const AI_NOTIFY_TEXT_T *t)
{
    if (!chat_lock()) {
        return;
    }
    const char *data = (t != NULL) ? t->data : NULL;
    size_t      len  = (t != NULL && t->data != NULL) ? t->datalen : 0U;
    switch (type) {
    case AI_USER_EVT_ASR_OK:
        ak_chat_text_reset(&s_chat.user);
        ak_chat_text_append(&s_chat.user, data, len, AK_CHAT_USER_MAX);
        ak_chat_text_reset(&s_chat.reply);
        s_chat.replying   = true;
        s_chat.reply_done = false;
        s_chat.turn++;
        break;
    case AI_USER_EVT_TEXT_STREAM_START:
        ak_chat_text_reset(&s_chat.reply);
        ak_chat_text_append(&s_chat.reply, data, len, AK_CHAT_REPLY_MAX);
        s_chat.replying   = true;
        s_chat.reply_done = false;
        break;
    case AI_USER_EVT_TEXT_STREAM_DATA:
        ak_chat_text_append(&s_chat.reply, data, len, AK_CHAT_REPLY_MAX);
        break;
    case AI_USER_EVT_TEXT_STREAM_STOP:
        ak_chat_text_append(&s_chat.reply, data, len, AK_CHAT_REPLY_MAX);
        s_chat.replying   = false;
        s_chat.reply_done = true;
        break;
    default: /* abort / chat break: keep what arrived */
        if (s_chat.replying) {
            s_chat.replying   = false;
            s_chat.reply_done = true;
        }
        break;
    }
    s_chat_dirty = true;
    chat_unlock();
}

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
        if (!s_mute_ai_reply) {
            /* Chat mode: the utterance stays on the board. It is never handed
             * to s_on_text, so no `asr` frame can reach Kiro Crew (CP-01). */
            chat_record(event->type, (const AI_NOTIFY_TEXT_T *)event->data);
            break;
        }
        /* Recognized text ready -> hand to the app, which emits an `asr` frame. */
        __emit_text((const AI_NOTIFY_TEXT_T *)event->data);
        break;
    case AI_USER_EVT_TEXT_STREAM_START:
    case AI_USER_EVT_TEXT_STREAM_DATA:
    case AI_USER_EVT_TEXT_STREAM_STOP:
    case AI_USER_EVT_TEXT_STREAM_ABORT:
    case AI_USER_EVT_CHAT_BREAK:
        /* The agent's reply text. ASR mode ignores it (it is the reply to a
         * dictation the user did not ask the agent about). */
        if (!s_mute_ai_reply) {
            chat_record(event->type, (const AI_NOTIFY_TEXT_T *)event->data);
        }
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

    if (s_chat_mutex == NULL && tal_mutex_create_init(&s_chat_mutex) != OPRT_OK) {
        PR_ERR("ak_asr: chat mutex create failed");
        s_chat_mutex = NULL;
    }
    AI_CHAT_MODE_CFG_T cfg = {
        .default_mode = AI_CHAT_MODE_HOLD, /* MANUAL VAD; capture driven by us */
        .default_vol  = AK_ASR_DEFAULT_VOLUME,
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
    /* A Wi-Fi link alone is not enough: between association and MQTT/AI-agent
     * initialization tuya_ai_input_start() silently does nothing. Only expose
     * cloud routing after both the link and AI agent are ready. */
    netmgr_status_e st = NETMGR_LINK_DOWN;
    if (!s_ai_ready || !tuya_ai_agent_is_ready()) {
        return false;
    }
    netmgr_conn_get(NETCONN_AUTO, NETCONN_CMD_STATUS, &st);
    return st == NETMGR_LINK_UP;
}

void ak_asr_start(void)
{
    if (!s_ai_ready || !tuya_ai_agent_is_ready()) {
        return;
    }
#if defined(ENABLE_BUTTON) && (ENABLE_BUTTON == 1)
    /* FR-01 L1: tell the cloud agent the interface language for this turn.
     * Sent as sessionAttributes "custom.param" on the next event start (the
     * SDK clears it after use); the agent prompt on the Tuya console must
     * read it, see shared/protocol/protocol.md "界面语言". */
    tuya_ai_agent_set_event_param((CHAR_T *)ak_lang_agent_param(ak_lang_get()));
    /* Inject the same event FP-3's registered ai_chat_button emits. HOLD then
     * transitions IDLE -> LISTEN, wakes the codec/VAD, and its VAD callback
     * starts tuya_ai_input only after real microphone data can flow. */
    OPERATE_RET rt = ai_mode_handle_key(TDL_BUTTON_LONG_PRESS_START, NULL);
    if (rt != OPRT_OK) {
        PR_ERR("ak_asr: HOLD long-press event failed: %d", rt);
    }
#else
    PR_ERR("ak_asr: ENABLE_BUTTON is required for HOLD event injection");
#endif
}

void ak_asr_stop(void)
{
    if (!s_ai_ready) {
        return;
    }
#if defined(ENABLE_BUTTON) && (ENABLE_BUTTON == 1)
    /* HOLD transitions LISTEN -> UPLOAD; that path disables VAD and ends the
     * cloud input session after queued microphone frames are drained. */
    OPERATE_RET rt = ai_mode_handle_key(TDL_BUTTON_PRESS_UP, NULL);
    if (rt != OPRT_OK) {
        PR_ERR("ak_asr: HOLD release event failed: %d", rt);
    }
#endif
}

void ak_asr_play_prompt(void)
{
    /* Same clip FP-3 sounds on a K4 double-click (its ai_chat button callback
     * plays AI_AUDIO_ALERT_LONG_KEY_TALK + mode on DOUBLE_CLICK). We only want
     * the prompt tone, not FP-3's mode switch, so play the HOLD-talk clip
     * directly and leave the chat mode locked on HOLD. */
#if defined(ENABLE_COMP_AI_AUDIO) && (ENABLE_COMP_AI_AUDIO == 1)
    ai_audio_player_alert(AI_AUDIO_ALERT_LONG_KEY_TALK);
#endif
}

void ak_asr_play_availability_prompt(bool cloud_available)
{
    /* Reuse SDK-local clips; do not synthesize or download audio. The caller
     * invokes this only on route transitions, so MQTT reconnect chatter cannot
     * replay the same prompt repeatedly. */
#if defined(ENABLE_COMP_AI_AUDIO) && (ENABLE_COMP_AI_AUDIO == 1)
    if (s_ai_ready) {
        ai_audio_player_alert(cloud_available ? AI_AUDIO_ALERT_NETWORK_CONNECTED : AI_AUDIO_ALERT_NETWORK_FAIL);
    }
#else
    (void)cloud_available;
#endif
}

void ak_asr_set_chat_mode(bool chat)
{
    s_mute_ai_reply = !chat;
    if (chat_lock()) {
        ak_chat_text_reset(&s_chat.user);
        ak_chat_text_reset(&s_chat.reply);
        s_chat.replying   = false;
        s_chat.reply_done = false;
        s_chat_dirty      = true;
        chat_unlock();
    }
}

void ak_asr_interrupt(void)
{
    if (!s_ai_ready) {
        return;
    }
#if defined(ENABLE_BUTTON) && (ENABLE_BUTTON == 1)
    /* HOLD's single click = stop playback + AI_EVENT_CHAT_BREAK. */
    OPERATE_RET rt = ai_mode_handle_key(TDL_BUTTON_PRESS_SINGLE_CLICK, NULL);
    if (rt != OPRT_OK) {
        PR_WARN("ak_asr: interrupt failed: %d", rt);
    }
#endif
}

bool ak_asr_chat_take(ak_asr_chat_t *out)
{
    if (out == NULL || !s_chat_dirty || !chat_lock()) {
        return false;
    }
    *out         = s_chat;
    s_chat_dirty = false;
    chat_unlock();
    return true;
}

#else /* !AK_ASR_BOARD_CLOUD — stub */

void ak_asr_set_chat_mode(bool chat)
{
    (void)chat;
}

void ak_asr_interrupt(void)
{
}

bool ak_asr_chat_take(ak_asr_chat_t *out)
{
    (void)out;
    return false;
}

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

void ak_asr_play_prompt(void)
{
}

void ak_asr_play_availability_prompt(bool cloud_available)
{
    (void)cloud_available;
}

#endif /* AK_ASR_BOARD_CLOUD */

bool ak_asr_audio_busy(void)
{
#if AK_ASR_BOARD_CLOUD && defined(ENABLE_COMP_AI_AUDIO) && \
    (ENABLE_COMP_AI_AUDIO == 1)
    return s_ai_ready && ai_audio_player_is_playing();
#else
    return false;
#endif
}

OPERATE_RET ak_asr_set_volume(uint8_t volume)
{
    s_volume = volume > 100U ? 100U : volume;
#if AK_ASR_BOARD_CLOUD
    if (s_ai_ready) {
        return ai_chat_set_volume(s_volume);
    }
#endif
    return OPRT_OK;
}

uint8_t ak_asr_get_volume(void)
{
#if AK_ASR_BOARD_CLOUD
    if (s_ai_ready) {
        int volume = ai_chat_get_volume();
        if (volume >= 0 && volume <= 100) {
            s_volume = (uint8_t)volume;
        }
    }
#endif
    return s_volume;
}
