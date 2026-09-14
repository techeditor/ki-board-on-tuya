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

#define AK_ASR_DEFAULT_VOLUME 70U

/** Called with a NUL-terminated recognized-text string when cloud ASR yields
 *  a result. The callback runs on the ASR/event thread; keep it short. */
typedef void (*ak_asr_text_cb)(const char *text);

/** Called when cloud ASR fails to yield text: code is "asr_error" (recognition
 *  error) or "asr_empty" (no speech). The app emits it as an `asr` error frame
 *  so the PC knows the press produced nothing, vs a silent drop. */
typedef void (*ak_asr_error_cb)(const char *code);

/** Bring up the board-side cloud ASR backend (no-op in the stub). Register the
 *  recognized-text callback and the error callback (either may be NULL).
 *  Returns OPRT_OK in the stub. */
OPERATE_RET ak_asr_init(ak_asr_text_cb on_text, ak_asr_error_cb on_error);

/** True only when a real cloud ASR backend is compiled in AND ready (authorized
 *  + networked). The stub always returns false. */
bool ak_asr_available(void);

/** Set/get board audio output volume as a percentage (0..100). The stub keeps
 *  the value so the settings UI behaves consistently without cloud ASR. */
OPERATE_RET ak_asr_set_volume(uint8_t volume);
uint8_t ak_asr_get_volume(void);

/** Push-to-talk edges. In the stub these are no-ops. With the real backend,
 *  `start` begins a HOLD-mode capture and `stop` ends it; the recognized text
 *  arrives later via the ak_asr_text_cb. */
void ak_asr_start(void);
void ak_asr_stop(void);

/** Play the local "long-press to talk" prompt tone (AI_AUDIO_ALERT_LONG_KEY_TALK),
 *  the same clip FP-3 sounds on a K4 double-click. No-op in the stub and a no-op
 *  when the AI audio component is not compiled in. Used for the double-click
 *  prompt; does NOT start capture or change chat mode. */
void ak_asr_play_prompt(void);

/** Announce a cloud-ASR availability edge using built-in local clips:
 *  NETWORK_FAIL when falling back to PC ASR and NETWORK_CONNECTED when cloud
 *  ASR recovers. No-op until the real AI audio backend is initialized. */
void ak_asr_play_availability_prompt(bool cloud_available);

/** True while the board audio player is actively outputting a prompt or TTS. */
bool ak_asr_audio_busy(void);

#endif /* AK_ASR_H */
