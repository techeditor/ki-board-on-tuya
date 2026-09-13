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
 * to an AI agent. The full wiring lands in the real-hardware integration step;
 * this translation unit is where app_chat_bot.c's ASR init / HOLD capture /
 * AI_USER_EVT_ASR_OK text extraction gets adapted to call `s_on_text`.
 */
static ak_asr_text_cb s_on_text = NULL;

OPERATE_RET ak_asr_init(ak_asr_text_cb on_text)
{
    s_on_text = on_text;
    /* TODO(real-hw): init ai_chat in HOLD mode, register the AI agent event
     * callback, and on AI_USER_EVT_ASR_OK sanitize the text to a single JSON
     * line and call s_on_text(text). Depends on authorized + networked device. */
    PR_NOTICE("ak_asr: cloud backend compiled in (wiring pending real-hw integration)");
    return OPRT_OK;
}

bool ak_asr_available(void)
{
    /* Real readiness (authorized + networked + agent bound) is asserted during
     * the real-hw integration; until then report not-ready so callers degrade
     * to the system (PC) route rather than silently dropping speech. */
    return false;
}

void ak_asr_start(void)
{
    /* TODO(real-hw): begin HOLD-mode capture. */
}

void ak_asr_stop(void)
{
    /* TODO(real-hw): end HOLD-mode capture; text arrives via the event cb. */
}

#else /* !AK_ASR_BOARD_CLOUD — stub */

OPERATE_RET ak_asr_init(ak_asr_text_cb on_text)
{
    (void)on_text;
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
