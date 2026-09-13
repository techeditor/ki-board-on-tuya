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
# ISOLATION ARCHITECTURE (see design.md D11) — solves BOTH D10 spike blockers
# WITHOUT editing the vendored SDK:
#   * src/ak_ble_hid.c   includes ONLY nimble/* headers (never a TAL header),
#     so the two-parameter LIST_HEAD from tuya_ble_linkqueue.h never shares a
#     translation unit with the one-parameter LIST_HEAD from tuya_list.h
#     (blocker #1).
#   * src/tuya_app_main.c includes ONLY tal_api.h + the plain-C bridge header
#     ak_ble_hid.h (never a nimble header).
#   * The private nimble/host include dir (holding tuya_ble_mempool.h, pulled
#     transitively by ble_hs.h) is added to the PROBE's include path below, so
#     ak_ble_hid.c resolves it — blocker #2 — with no SDK edit. nimble/include
#     is already PUBLIC via src/tal_bluetooth/CMakeLists.txt but is listed here
#     too so the probe is self-describing.
#

set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})
set(AK_APP_COMMON ${APP_PATH}/../../app/common)

# TuyaOpen SDK root (exported by export.sh). Used only to reach the native
# NimBLE host-private include dir; we do NOT compile or modify any SDK source.
set(TUYA_SDK_ROOT $ENV{OPEN_SDK_ROOT})
set(NIMBLE_DIR ${TUYA_SDK_ROOT}/src/tal_bluetooth/nimble)

aux_source_directory(${APP_PATH}/src APP_SRCS)

add_library(${EXAMPLE_LIB})
target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRCS}
        ${AK_APP_COMMON}/src/ak_input.c
        ${AK_APP_COMMON}/src/ak_input_fsm.c
        ${AK_APP_COMMON}/src/ak_feedback.c
        ${AK_APP_COMMON}/src/ak_uart.c
        ${AK_APP_COMMON}/src/ak_protocol.c
        ${AK_APP_COMMON}/src/ak_gfx.c
        ${AK_APP_COMMON}/src/ak_statuslcd.c
)
target_include_directories(${EXAMPLE_LIB}
    PRIVATE
        ${APP_PATH}/include
        ${AK_APP_COMMON}/include
        # Native NimBLE headers so ak_ble_hid.c can drive the HOGP GATT server.
        # nimble/include is already PUBLIC from the SDK; nimble/host is PRIVATE
        # there and carries tuya_ble_mempool.h (D10 blocker #2) — add it here.
        ${NIMBLE_DIR}/include
        ${NIMBLE_DIR}/host
)
