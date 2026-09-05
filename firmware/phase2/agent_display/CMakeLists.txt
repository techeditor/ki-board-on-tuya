##
# @file CMakeLists.txt
# @brief Phase-2 agent_display test (T1: LVGL bring-up).
#
# Reuses the authoritative v1.2 IO map from firmware/app/common/include
# (ak_board_io.h). LVGL headers (lvgl.h / lv_vendor.h) are exported by the
# CONFIG_ENABLE_LIBLVGL component, so no manual include dir is needed for them.
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
