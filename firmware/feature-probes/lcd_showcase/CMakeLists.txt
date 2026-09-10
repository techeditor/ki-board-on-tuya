##
# @file CMakeLists.txt
# @brief lcd_showcase feature-probe (FP-lcd-showcase: 富信息 LCD 界面示例).
#
# Reuses the authoritative v1.2 IO map from firmware/app/common/include
# (ak_board_io.h) and the 5-key + 5-way input scanner (ak_input.c /
# ak_input_fsm.c). The display layer (ak_panel/ak_scene/ak_notify_ui/
# app_notification + sprite headers) is self-contained under this probe's
# src/ + include/ (soft-copied from FP-1 agent_display so probes stay
# independent). LVGL headers come from the CONFIG_ENABLE_LIBLVGL component.
#

set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})
set(AK_APP_COMMON ${APP_PATH}/../../app/common)

aux_source_directory(${APP_PATH}/src APP_SRCS)

add_library(${EXAMPLE_LIB})
target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRCS}
        ${AK_APP_COMMON}/src/ak_input.c
        ${AK_APP_COMMON}/src/ak_input_fsm.c
)
target_include_directories(${EXAMPLE_LIB}
    PRIVATE
        ${APP_PATH}/include
        ${AK_APP_COMMON}/include
)
