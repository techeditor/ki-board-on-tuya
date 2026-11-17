##
# @file CMakeLists.txt
# @brief ST7789 LCD bring-up application.
#

set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})
set(BRINGUP_COMMON_PATH ${APP_PATH}/../common)

aux_source_directory(${APP_PATH}/src APP_SRCS)

add_library(${EXAMPLE_LIB})
target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRCS}
        ${BRINGUP_COMMON_PATH}/src/bringup_uart.c
)
target_include_directories(${EXAMPLE_LIB}
    PRIVATE
        ${APP_PATH}/include
        ${BRINGUP_COMMON_PATH}/include
)
