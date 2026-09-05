#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sprite_mascot_idle.h"
#include "sprite_mascot_alert.h"
#include "sprite_mascot_happy.h"
#include "sprite_mascot_sleeping.h"
#include "sprite_mascot_disconnected.h"
#include "sprite_mascot_thinking.h"
#include "sprite_mascot_typing.h"
#include "sprite_mascot_juggling.h"
#include "sprite_mascot_building.h"
#include "sprite_mascot_confused.h"
#include "sprite_mascot_dizzy.h"
#include "sprite_mascot_sweeping.h"
#include "sprite_mascot_walking.h"
#include "sprite_mascot_going_away.h"
#include "sprite_mascot_debugger.h"
#include "sprite_mascot_wizard.h"
#include "sprite_mascot_conducting.h"
#include "sprite_mascot_beacon.h"
#include "sprite_mini_mascot.h"

typedef struct {
    const uint16_t *rle_data;
    const uint32_t *frame_offsets;
    int frame_count;
    int frame_ms;
    bool looping;
    int width;
    int height;
    int y_offset;
} mascot_anim_def_t;

static const mascot_anim_def_t mascot_anim_defs[MASCOT_ANIM_COUNT] = {
    [MASCOT_ANIM_IDLE] = {
        .rle_data = mascot_idle_rle_data,
        .frame_offsets = mascot_idle_frame_offsets,
        .frame_count = 16,
        .frame_ms = 166,
        .looping = true,
        .width = 54,
        .height = 68,
        .y_offset = -10,
    },
    [MASCOT_ANIM_ALERT] = {
        .rle_data = mascot_alert_rle_data,
        .frame_offsets = mascot_alert_frame_offsets,
        .frame_count = 8,
        .frame_ms = 100,
        .looping = false,
        .width = 63,
        .height = 88,
        .y_offset = -10,
    },
    [MASCOT_ANIM_HAPPY] = {
        .rle_data = mascot_happy_rle_data,
        .frame_offsets = mascot_happy_frame_offsets,
        .frame_count = 8,
        .frame_ms = 100,
        .looping = false,
        .width = 80,
        .height = 92,
        .y_offset = -10,
    },
    [MASCOT_ANIM_SLEEPING] = {
        .rle_data = mascot_sleeping_rle_data,
        .frame_offsets = mascot_sleeping_frame_offsets,
        .frame_count = 12,
        .frame_ms = 166,
        .looping = true,
        .width = 90,
        .height = 62,
        .y_offset = -10,
    },
    [MASCOT_ANIM_DISCONNECTED] = {
        .rle_data = mascot_disconnected_rle_data,
        .frame_offsets = mascot_disconnected_frame_offsets,
        .frame_count = 16,
        .frame_ms = 166,
        .looping = true,
        .width = 79,
        .height = 86,
        .y_offset = -10,
    },
    [MASCOT_ANIM_THINKING] = {
        .rle_data = mascot_thinking_rle_data,
        .frame_offsets = mascot_thinking_frame_offsets,
        .frame_count = 8,
        .frame_ms = 125,
        .looping = true,
        .width = 65,
        .height = 87,
        .y_offset = -10,
    },
    [MASCOT_ANIM_TYPING] = {
        .rle_data = mascot_typing_rle_data,
        .frame_offsets = mascot_typing_frame_offsets,
        .frame_count = 8,
        .frame_ms = 125,
        .looping = true,
        .width = 66,
        .height = 80,
        .y_offset = -10,
    },
    [MASCOT_ANIM_JUGGLING] = {
        .rle_data = mascot_juggling_rle_data,
        .frame_offsets = mascot_juggling_frame_offsets,
        .frame_count = 8,
        .frame_ms = 125,
        .looping = true,
        .width = 78,
        .height = 102,
        .y_offset = -10,
    },
    [MASCOT_ANIM_BUILDING] = {
        .rle_data = mascot_building_rle_data,
        .frame_offsets = mascot_building_frame_offsets,
        .frame_count = 8,
        .frame_ms = 125,
        .looping = true,
        .width = 75,
        .height = 82,
        .y_offset = -10,
    },
    [MASCOT_ANIM_CONFUSED] = {
        .rle_data = mascot_confused_rle_data,
        .frame_offsets = mascot_confused_frame_offsets,
        .frame_count = 16,
        .frame_ms = 125,
        .looping = true,
        .width = 97,
        .height = 94,
        .y_offset = -10,
    },
    [MASCOT_ANIM_DIZZY] = {
        .rle_data = mascot_dizzy_rle_data,
        .frame_offsets = mascot_dizzy_frame_offsets,
        .frame_count = 8,
        .frame_ms = 125,
        .looping = true,
        .width = 83,
        .height = 102,
        .y_offset = -10,
    },
    [MASCOT_ANIM_SWEEPING] = {
        .rle_data = mascot_sweeping_rle_data,
        .frame_offsets = mascot_sweeping_frame_offsets,
        .frame_count = 8,
        .frame_ms = 125,
        .looping = false,
        .width = 84,
        .height = 82,
        .y_offset = -10,
    },
    [MASCOT_ANIM_WALKING] = {
        .rle_data = mascot_walking_rle_data,
        .frame_offsets = mascot_walking_frame_offsets,
        .frame_count = 8,
        .frame_ms = 125,
        .looping = true,
        .width = 51,
        .height = 62,
        .y_offset = -10,
    },
    [MASCOT_ANIM_GOING_AWAY] = {
        .rle_data = mascot_going_away_rle_data,
        .frame_offsets = mascot_going_away_frame_offsets,
        .frame_count = 8,
        .frame_ms = 125,
        .looping = false,
        .width = 65,
        .height = 80,
        .y_offset = -10,
    },
    [MASCOT_ANIM_DEBUGGER] = {
        .rle_data = mascot_debugger_rle_data,
        .frame_offsets = mascot_debugger_frame_offsets,
        .frame_count = 12,
        .frame_ms = 125,
        .looping = true,
        .width = 120,
        .height = 82,
        .y_offset = -10,
    },
    [MASCOT_ANIM_WIZARD] = {
        .rle_data = mascot_wizard_rle_data,
        .frame_offsets = mascot_wizard_frame_offsets,
        .frame_count = 16,
        .frame_ms = 125,
        .looping = true,
        .width = 136,
        .height = 97,
        .y_offset = -10,
    },
    [MASCOT_ANIM_CONDUCTING] = {
        .rle_data = mascot_conducting_rle_data,
        .frame_offsets = mascot_conducting_frame_offsets,
        .frame_count = 8,
        .frame_ms = 125,
        .looping = true,
        .width = 90,
        .height = 104,
        .y_offset = -10,
    },
    [MASCOT_ANIM_BEACON] = {
        .rle_data = mascot_beacon_rle_data,
        .frame_offsets = mascot_beacon_frame_offsets,
        .frame_count = 16,
        .frame_ms = 125,
        .looping = true,
        .width = 125,
        .height = 102,
        .y_offset = -10,
    },
    [MASCOT_ANIM_MINI_MASCOT] = {
        .rle_data = mini_mascot_rle_data,
        .frame_offsets = mini_mascot_frame_offsets,
        .frame_count = 8,
        .frame_ms = 250,
        .looping = true,
        .width = 10,
        .height = 7,
        .y_offset = -5,
    },
};
    