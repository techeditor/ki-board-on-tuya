##
# @file CMakeLists.txt
# @brief v0.3.2 WS2812 chain power switch (LED_PWR_EN = P18) bring-up application.
#

set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})
set(BRINGUP_COMMON_PATH ${APP_PATH}/../common)

aux_source_directory(${APP_PATH}/src APP_SRCS)

add_library(${EXAMPLE_LIB})
target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRCS}
        ${BRINGUP_COMMON_PATH}/src/bringup_uart.c
        ${BRINGUP_COMMON_PATH}/src/bringup_led_power.c
)
target_include_directories(${EXAMPLE_LIB}
    PRIVATE
        ${APP_PATH}/include
        ${BRINGUP_COMMON_PATH}/include
)
