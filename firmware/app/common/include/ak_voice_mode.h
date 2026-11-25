/**
 * @file ak_voice_mode.h
 * @brief Voice mode (ASR vs Chat) decisions and the Chat reply buffer. Pure,
 *        host-testable.
 *
 * ASR  : recognized text goes to the Companion and into Kiro Crew (existing).
 * Chat : the board talks to the Tuya cloud agent; the reply is spoken and
 *        shown on the LCD and in the Companion, and is NEVER injected into
 *        Kiro Crew (companion-review CP-01). The mode is changed on the
 *        device (Settings > Voice mode) or by the Companion (`device_settings`
 *        voice_mode over USB or the BLE secure channel, FR-02) and persists
 *        across reboots (Q6).
 */

#ifndef AK_VOICE_MODE_H
#define AK_VOICE_MODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    AK_VOICE_MODE_ASR = 0,
    AK_VOICE_MODE_CHAT,
} ak_voice_mode_t;

/** What a press of the voice key does in the current mode. */
typedef enum {
    AK_PTT_CLOUD_HOLD = 0,    /* board mic -> Tuya cloud (HOLD) */
    AK_PTT_SYSTEM_DICTATION,  /* voice start/stop frames -> PC speech input */
    AK_PTT_CHAT_UNAVAILABLE,  /* Chat without the cloud: show a hint, send nothing */
} ak_ptt_action_t;

/** What the voice key does once the Companion is taken into account. */
typedef enum {
    AK_PTT_PLAN_CLOUD = 0,  /* board mic -> Tuya cloud (HOLD) */
    AK_PTT_PLAN_PC,         /* voice start/stop frames -> PC speech input (ASR mode) */
    AK_PTT_PLAN_CHAT_AS_PC, /* Chat without the cloud: this press is speech input (ASR mode) */
    AK_PTT_PLAN_NEED_LINK,  /* PC speech input is the only way, but no Companion is connected */
} ak_ptt_plan_t;

/** Wire / UI name: "asr" / "chat". */
const char *ak_voice_mode_str(ak_voice_mode_t mode);

/** device_settings `voice_mode` -> mode ("asr" / "chat"); false otherwise. */
bool ak_voice_mode_from_wire(const char *s, ak_voice_mode_t *out);

/** Decode the 1-byte KV blob. Any other length or value is rejected. */
bool ak_voice_mode_from_blob(const uint8_t *blob, size_t len, ak_voice_mode_t *out);

uint8_t ak_voice_mode_to_byte(ak_voice_mode_t mode);

/**
 * ASR: cloud when the effective route is cloud, else PC speech input. Chat:
 * cloud when available, else UNAVAILABLE — Chat never falls back to PC
 * speech input, which would type the user's words into Kiro Crew.
 */
ak_ptt_action_t ak_voice_mode_ptt_action(ak_voice_mode_t mode, bool cloud_available, bool route_is_cloud);

/**
 * The voice key plan. The board's own cloud ASR is used while the Tuya cloud
 * answers; in every other case (no License, not provisioned or activated,
 * pairing window open, cloud or Wi-Fi down, or PC speech input chosen) the
 * words go to the Companion's PC speech input, which needs a Companion link.
 * Chat has no PC fallback of its own: without the cloud the press is treated
 * as speech input in ASR mode (the words land in the input strip).
 */
ak_ptt_plan_t ak_voice_ptt_plan(ak_voice_mode_t mode, bool cloud_available, bool route_is_cloud,
                                bool companion_linked);

/* Agent reply kept for the LCD / Companion. Longer replies are cut at a UTF-8
 * character boundary and end with "...". */
#define AK_CHAT_REPLY_MAX 1024U
#define AK_CHAT_USER_MAX  256U

typedef struct {
    char   text[AK_CHAT_REPLY_MAX];
    size_t len;
    bool   truncated;
} ak_chat_text_t;

void ak_chat_text_reset(ak_chat_text_t *t);

/** Append raw UTF-8 bytes (not NUL-terminated). Never overflows `t->text`. */
void ak_chat_text_append(ak_chat_text_t *t, const char *data, size_t len, size_t cap);

#endif /* AK_VOICE_MODE_H */
