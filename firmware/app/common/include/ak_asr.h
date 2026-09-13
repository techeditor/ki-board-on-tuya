/**
 * @file ak_asr.h
 * @brief Board-side (Tuya cloud) ASR abstraction for the product firmware.
 *
 * Route B scaffolding: the product app carries a stable ASR interface so the
 * voice-routing state machine and the companion UI can select "cloud" (board
 * mic -> Tuya cloud ASR) vs "system" (PC's own ASR) regardless of whether the
 * cloud ASR *implementation* is compiled in.
 *
 * Gated by AK_ASR_BOARD_CLOUD (ak_features.h):
 *   - AK_ASR_BOARD_CLOUD == 0 (default): a compiling STUB. ak_asr_available()
 *     is false; ak_asr_start/stop are no-ops. The board cannot run cloud ASR,
 *     so selecting the "cloud" route degrades to "system" at the call site.
 *   - AK_ASR_BOARD_CLOUD == 1: the real backend (ported from the FP-3
 *     tuya_cloud_asr probe: ai_chat HOLD mode + Tuya AI agent). Needs the AI
 *     stack (ENABLE_AI_COMPONENTS), Wi-Fi provisioning and a PID bound to an
 *     AI agent. Wiring lands in the follow-up real-hardware integration.
 *
 * The recognized-text callback is how a cloud result reaches the app, which
 * then emits it as an `asr` protocol frame (ak_proto_asr) to the PC.
 */

#ifndef AK_ASR_H
#define AK_ASR_H

#include <stdbool.h>

#include "tuya_cloud_types.h"

/** Called with a NUL-terminated recognized-text string when cloud ASR yields
 *  a result. The callback runs on the ASR/event thread; keep it short. */
typedef void (*ak_asr_text_cb)(const char *text);

/** Bring up the board-side cloud ASR backend (no-op in the stub). Register the
 *  recognized-text callback. Returns OPRT_OK in the stub. */
OPERATE_RET ak_asr_init(ak_asr_text_cb on_text);

/** True only when a real cloud ASR backend is compiled in AND ready (authorized
 *  + networked). The stub always returns false. */
bool ak_asr_available(void);

/** Push-to-talk edges. In the stub these are no-ops. With the real backend,
 *  `start` begins a HOLD-mode capture and `stop` ends it; the recognized text
 *  arrives later via the ak_asr_text_cb. */
void ak_asr_start(void);
void ak_asr_stop(void);

#endif /* AK_ASR_H */
