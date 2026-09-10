##
# @file CMakeLists.txt
# @brief Agentic Keyboard for Kiro -- product firmware (mechanism A: one app,
#        shared core + build-time-selected transport/ASR backends).
#

set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})
set(AK_COMMON_PATH ${APP_PATH}/common)

# Shared core sources compiled into every variant.
set(AK_CORE_SRCS
    ${AK_COMMON_PATH}/src/ak_uart.c
    ${AK_COMMON_PATH}/src/ak_input_fsm.c
    ${AK_COMMON_PATH}/src/ak_input.c
    ${AK_COMMON_PATH}/src/ak_protocol.c
    ${AK_COMMON_PATH}/src/ak_rx.c
    ${AK_COMMON_PATH}/src/ak_display.c
)

aux_source_directory(${APP_PATH}/src APP_SRCS)

add_library(${EXAMPLE_LIB})

target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRCS}
        ${AK_CORE_SRCS}
)

target_include_directories(${EXAMPLE_LIB}
    PRIVATE
        ${APP_PATH}/include
        ${AK_COMMON_PATH}/include
)
