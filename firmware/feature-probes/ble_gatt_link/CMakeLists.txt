##
# @file CMakeLists.txt
# @brief FP-2 ble-gatt-link — wireless companion link over BLE GATT.
#
# Reuses the product firmware's transport/input/protocol modules from
# firmware/app/common (same JSONL protocol v1 as the UART backend).
#

set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})
set(AK_APP_COMMON ${APP_PATH}/../../app/common)

aux_source_directory(${APP_PATH}/src APP_SRCS)

add_library(${EXAMPLE_LIB})
target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRCS}
        ${AK_APP_COMMON}/src/ak_uart.c
        ${AK_APP_COMMON}/src/ak_input.c
        ${AK_APP_COMMON}/src/ak_input_fsm.c
        ${AK_APP_COMMON}/src/ak_protocol.c
)
target_include_directories(${EXAMPLE_LIB}
    PRIVATE
        ${APP_PATH}/include
        ${AK_APP_COMMON}/include
)
