##
# @file CMakeLists.txt
# @brief FP-WS2812 status-LED feature-probe (A1: ak_leds driver bring-up).
#
# Reuses the authoritative v1.2 IO map from firmware/app/common/include
# (ak_board_io.h) and the shared bring-up UART0 helper. The official SPI WS2812
# driver headers (tdd_pixel_ws2812.h / tdl_pixel_*.h) are exported by the
# CONFIG_ENABLE_LEDS_PIXEL component, so no manual include dir is needed.
#

set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})
set(AK_APP_COMMON ${APP_PATH}/../../app/common)
set(BRINGUP_COMMON ${APP_PATH}/../../bringup/common)

aux_source_directory(${APP_PATH}/src APP_SRCS)

add_library(${EXAMPLE_LIB})
target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRCS}
        ${BRINGUP_COMMON}/src/bringup_uart.c
)
target_include_directories(${EXAMPLE_LIB}
    PRIVATE
        ${APP_PATH}/include
        ${AK_APP_COMMON}/include
        ${BRINGUP_COMMON}/include
)
