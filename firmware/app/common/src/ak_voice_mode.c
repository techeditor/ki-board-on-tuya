/**
 * @file ak_voice_mode.c
 * @brief Voice mode decisions and Chat text buffer (pure). See ak_voice_mode.h.
 */

#include "ak_voice_mode.h"

#include <string.h>

const char *ak_voice_mode_str(ak_voice_mode_t mode)
{
    return mode == AK_VOICE_MODE_CHAT ? "chat" : "asr";
}

bool ak_voice_mode_from_wire(const char *s, ak_voice_mode_t *out)
{
    if (s == NULL || out == NULL) {
        return false;
    }
    if (strcmp(s, "asr") == 0) {
        *out = AK_VOICE_MODE_ASR;
    } else if (strcmp(s, "chat") == 0) {
        *out = AK_VOICE_MODE_CHAT;
    } else {
        return false;
    }
    return true;
}

bool ak_voice_mode_from_blob(const uint8_t *blob, size_t len, ak_voice_mode_t *out)
{
    if (blob == NULL || out == NULL || len != 1U || blob[0] > (uint8_t)AK_VOICE_MODE_CHAT) {
        return false;
    }
    *out = (ak_voice_mode_t)blob[0];
    return true;
}

uint8_t ak_voice_mode_to_byte(ak_voice_mode_t mode)
{
    return mode == AK_VOICE_MODE_CHAT ? 1U : 0U;
}

ak_ptt_action_t ak_voice_mode_ptt_action(ak_voice_mode_t mode, bool cloud_available, bool route_is_cloud)
{
    if (mode == AK_VOICE_MODE_CHAT) {
        return cloud_available ? AK_PTT_CLOUD_HOLD : AK_PTT_CHAT_UNAVAILABLE;
    }
    return route_is_cloud ? AK_PTT_CLOUD_HOLD : AK_PTT_SYSTEM_DICTATION;
}

ak_ptt_plan_t ak_voice_ptt_plan(ak_voice_mode_t mode, bool cloud_available, bool route_is_cloud,
                                bool companion_linked)
{
    switch (ak_voice_mode_ptt_action(mode, cloud_available, route_is_cloud)) {
    case AK_PTT_CLOUD_HOLD:
        return AK_PTT_PLAN_CLOUD;
    case AK_PTT_SYSTEM_DICTATION:
        return companion_linked ? AK_PTT_PLAN_PC : AK_PTT_PLAN_NEED_LINK;
    case AK_PTT_CHAT_UNAVAILABLE:
    default:
        return companion_linked ? AK_PTT_PLAN_CHAT_AS_PC : AK_PTT_PLAN_NEED_LINK;
    }
}

void ak_chat_text_reset(ak_chat_text_t *t)
{
    t->text[0]   = '\0';
    t->len       = 0;
    t->truncated = false;
}

/* Length of the longest prefix of s[0..len) that ends on a UTF-8 boundary. */
static size_t utf8_boundary(const char *s, size_t len)
{
    size_t lead = len;
    while (lead > 0 && ((unsigned char)s[lead - 1] & 0xC0U) == 0x80U) {
        lead--;
    }
    if (lead == 0) {
        return 0;
    }
    unsigned char c    = (unsigned char)s[lead - 1];
    size_t        need = 1;
    if ((c & 0xE0U) == 0xC0U) {
        need = 2;
    } else if ((c & 0xF0U) == 0xE0U) {
        need = 3;
    } else if ((c & 0xF8U) == 0xF0U) {
        need = 4;
    }
    return (lead - 1 + need <= len) ? len : lead - 1;
}

void ak_chat_text_append(ak_chat_text_t *t, const char *data, size_t len, size_t cap)
{
    static const char ellipsis[] = "...";
    if (cap > sizeof(t->text)) {
        cap = sizeof(t->text);
    }
    if (t->truncated || data == NULL || len == 0 || cap < sizeof(ellipsis) + 1U) {
        return;
    }
    size_t room = cap - 1U - t->len; /* keep the terminator */
    if (len <= room) {
        memcpy(t->text + t->len, data, len);
        t->len += len;
        t->text[t->len] = '\0';
        return;
    }
    /* Does not fit: keep what fits before "...", cut on a character boundary. */
    size_t keep_room = cap - 1U - (sizeof(ellipsis) - 1U);
    if (t->len > keep_room) {
        t->len = utf8_boundary(t->text, keep_room);
    } else {
        size_t take = keep_room - t->len;
        memcpy(t->text + t->len, data, take);
        t->len = utf8_boundary(t->text, t->len + take);
    }
    memcpy(t->text + t->len, ellipsis, sizeof(ellipsis) - 1U);
    t->len += sizeof(ellipsis) - 1U;
    t->text[t->len] = '\0';
    t->truncated    = true;
}
