/**
 * @file ak_voice_route.c
 * @brief Pure preferred/effective ASR routing state (host-testable).
 */

#include "ak_voice_route.h"

void ak_voice_route_init(ak_voice_route_t *route)
{
    route->preferred = AK_VOICE_CLOUD;
    route->effective = AK_VOICE_CLOUD;
    route->fallback  = false;
}

void ak_voice_route_set_preference(ak_voice_route_t *route, ak_voice_source_t preferred)
{
    route->preferred = preferred;
}

bool ak_voice_route_refresh(ak_voice_route_t *route, bool cloud_available)
{
    ak_voice_source_t effective =
        (route->preferred == AK_VOICE_CLOUD && cloud_available) ? AK_VOICE_CLOUD : AK_VOICE_SYSTEM;
    bool fallback = (route->preferred == AK_VOICE_CLOUD && !cloud_available);
    bool changed  = (route->effective != effective || route->fallback != fallback);

    route->effective = effective;
    route->fallback  = fallback;
    return changed;
}

const char *ak_voice_source_str(ak_voice_source_t source)
{
    return (source == AK_VOICE_CLOUD) ? "cloud" : "system";
}
