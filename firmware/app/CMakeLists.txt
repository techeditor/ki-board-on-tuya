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
    ${AK_COMMON_PATH}/src/ak_ble_gatt.c
    ${AK_COMMON_PATH}/src/ak_asr.c
    ${AK_COMMON_PATH}/src/ak_cloud.c
    ${AK_COMMON_PATH}/src/ak_wifi.c
    ${AK_COMMON_PATH}/src/ak_input_fsm.c
    ${AK_COMMON_PATH}/src/ak_input.c
    ${AK_COMMON_PATH}/src/ak_protocol.c
    ${AK_COMMON_PATH}/src/ak_rx.c
    ${AK_COMMON_PATH}/src/ak_display.c
    ${AK_COMMON_PATH}/src/ak_gfx.c
    ${AK_COMMON_PATH}/src/ak_statuslcd.c
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

# Transport backend selection (mechanism A). A build profile under config/
# sets exactly one CONFIG_AK_TRANSPORT_* = y; translate it into the app-level
# C macro that ak_features.h / tuya_app_main.c switch on. Absent any of these
# (the default TUYA_T5AI_CORE.config), ak_features.h defaults to UART.
if (CONFIG_AK_TRANSPORT_DUAL STREQUAL "y")
    target_compile_definitions(${EXAMPLE_LIB} PRIVATE AK_TRANSPORT_DUAL=1 AK_TRANSPORT_UART=1 AK_TRANSPORT_BLE_GATT=1)
elseif (CONFIG_AK_TRANSPORT_BLE_GATT STREQUAL "y")
    target_compile_definitions(${EXAMPLE_LIB} PRIVATE AK_TRANSPORT_UART=0 AK_TRANSPORT_BLE_GATT=1)
elseif (CONFIG_AK_TRANSPORT_BLE_HID STREQUAL "y")
    target_compile_definitions(${EXAMPLE_LIB} PRIVATE AK_TRANSPORT_UART=0 AK_TRANSPORT_BLE_HID=1)
endif()

# Board-side cloud ASR (ai_chat). Independent of the transport selection above.
if (CONFIG_AK_ASR_BOARD_CLOUD STREQUAL "y")
    target_compile_definitions(${EXAMPLE_LIB} PRIVATE AK_ASR_BOARD_CLOUD=1)
endif()
