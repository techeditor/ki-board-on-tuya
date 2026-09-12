##
# @file CMakeLists.txt
# @brief FP-6 ble-hid-fallback (T1 spike) — standard BLE HID keyboard bring-up.
#
# Compile+link spike: proves the TuyaOpen T5AI toolchain links the native
# NimBLE HID GATT + Security Manager symbols needed for a compliant HOGP
# keyboard (custom 0x1812 service, Report Map, Input Report + Report Reference
# 0x2908 descriptor, SM pairing/bonding). Reuses the shared status-LCD + input
# modules from firmware/app/common the same way FP-2 (ble_gatt_link) does.
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
        ${AK_APP_COMMON}/src/ak_gfx.c
        ${AK_APP_COMMON}/src/ak_statuslcd.c
)
target_include_directories(${EXAMPLE_LIB}
    PRIVATE
        ${APP_PATH}/include
        ${AK_APP_COMMON}/include
)
