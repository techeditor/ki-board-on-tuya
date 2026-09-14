/**
 * @file ak_voice_route.h
 * @brief Pure preferred/effective ASR routing state (host-testable).
 */

#ifndef AK_VOICE_ROUTE_H
#define AK_VOICE_ROUTE_H

#include <stdbool.h>

typedef enum {
    AK_VOICE_SYSTEM = 0,
    AK_VOICE_CLOUD,
} ak_voice_source_t;

typedef struct {
    ak_voice_source_t preferred; /* explicit/default user preference */
    ak_voice_source_t effective; /* route currently used by K4 */
    bool              fallback;  /* preferred=cloud but cloud unavailable */
} ak_voice_route_t;

/** Default policy: prefer and initially advertise Tuya cloud ASR. */
void ak_voice_route_init(ak_voice_route_t *route);

/** Set the user preference. Call ak_voice_route_refresh() afterwards. */
void ak_voice_route_set_preference(ak_voice_route_t *route, ak_voice_source_t preferred);

/**
 * Recompute the effective route from the preference and cloud availability.
 * Returns true when effective source or fallback state changed.
 */
bool ak_voice_route_refresh(ak_voice_route_t *route, bool cloud_available);

const char *ak_voice_source_str(ak_voice_source_t source);

#endif /* AK_VOICE_ROUTE_H */
