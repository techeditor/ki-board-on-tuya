##
# @file CMakeLists.txt
# @brief Agentic Keyboard for Kiro -- product firmware (mechanism A: one app,
#        shared core + build-time-selected transport/ASR backends).
#

set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})
set(AK_COMMON_PATH ${APP_PATH}/common)

# VERSION is the firmware's single source of truth. tools/version.py keeps the
# TuyaOpen profiles synchronized because tos.py also uses CONFIG_PROJECT_VERSION
# to name and collect build artifacts.
file(STRINGS "${APP_PATH}/VERSION" AK_PROJECT_VERSION LIMIT_COUNT 1)
string(STRIP "${AK_PROJECT_VERSION}" AK_PROJECT_VERSION)
if(NOT AK_PROJECT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "Invalid firmware VERSION: '${AK_PROJECT_VERSION}'")
endif()
if(DEFINED CONFIG_PROJECT_VERSION AND
   NOT CONFIG_PROJECT_VERSION STREQUAL AK_PROJECT_VERSION)
    message(FATAL_ERROR
        "VERSION (${AK_PROJECT_VERSION}) and CONFIG_PROJECT_VERSION "
        "(${CONFIG_PROJECT_VERSION}) differ; run tools/version.py set ${AK_PROJECT_VERSION}")
endif()

# Shared core sources compiled into every variant.
set(AK_CORE_SRCS
    ${AK_COMMON_PATH}/src/ak_uart.c
    ${AK_COMMON_PATH}/src/ak_ble_gatt.c
    ${AK_COMMON_PATH}/src/ak_asr.c
    ${AK_COMMON_PATH}/src/ak_cloud.c
    ${AK_COMMON_PATH}/src/ak_reset_netcfg.c
    ${AK_COMMON_PATH}/src/ak_window_marker.c
    ${AK_COMMON_PATH}/src/ak_wifi.c
    ${AK_COMMON_PATH}/src/ak_input_fsm.c
    ${AK_COMMON_PATH}/src/ak_keymap.c
    ${AK_COMMON_PATH}/src/ak_voice_route.c
    ${AK_COMMON_PATH}/src/ak_settings.c
    ${AK_COMMON_PATH}/src/ak_input.c
    ${AK_COMMON_PATH}/src/ak_battery.c
    ${AK_COMMON_PATH}/src/ak_bat_gauge.c
    ${AK_COMMON_PATH}/src/ak_leds.c
    ${AK_COMMON_PATH}/src/ak_protocol.c
    ${AK_COMMON_PATH}/src/ak_rx.c
    ${AK_COMMON_PATH}/src/ak_menu.c
    ${AK_COMMON_PATH}/src/ak_ui.c
    ${AK_COMMON_PATH}/src/ak_validate.c
    ${AK_COMMON_PATH}/src/ak_provision.c
    ${AK_COMMON_PATH}/src/ak_cred.c
    ${AK_COMMON_PATH}/src/ak_voice_mode.c
    ${AK_COMMON_PATH}/src/ak_byte_ring.c
    ${AK_COMMON_PATH}/src/ak_led_power.c
    ${AK_COMMON_PATH}/src/ak_led_policy.c
    ${AK_COMMON_PATH}/src/ak_kv.c
    ${AK_COMMON_PATH}/src/ak_ble_sec.c
    ${AK_COMMON_PATH}/src/ak_ota.c
    ${AK_COMMON_PATH}/src/ak_ota_link.c
    ${AK_COMMON_PATH}/src/ak_gfx.c
    ${AK_COMMON_PATH}/src/ak_font.c
    ${AK_COMMON_PATH}/src/ak_font_data.c
    ${AK_COMMON_PATH}/src/ak_fonttest.c
    ${AK_COMMON_PATH}/src/ak_font_ui_data.c
    ${AK_COMMON_PATH}/src/ak_text.c
    ${AK_COMMON_PATH}/src/ak_i18n.c
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
        # T5AI SPI DMA needs an explicit cache clean for PSRAM framebuffers.
        ${CMAKE_SOURCE_DIR}/platform/T5AI/t5_os/ap/components/cmsis/CMSIS_5/CMSIS/Core/Include
        ${CMAKE_SOURCE_DIR}/platform/T5AI/t5_os/ap/components/cmsis/CMSIS_5/Device/Beken/armstar
)

target_compile_definitions(${EXAMPLE_LIB}
    PRIVATE PROJECT_VERSION=\"${AK_PROJECT_VERSION}\"
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

# Opt-in development diagnostic. The request is accepted only over UART0 and
# never returns more than the last four AuthKey characters.
if (CONFIG_AK_LICENSE_DEBUG_READ STREQUAL "y")
    target_compile_definitions(${EXAMPLE_LIB} PRIVATE AK_ENABLE_LICENSE_DEBUG_READ=1)
endif()

# Release gate (docs/ota.md §4): AK_RELEASE_BUILD=1 refuses debug profiles and
# the placeholder OTA release key, so a published image can take later updates.
file(READ "${AK_COMMON_PATH}/include/ak_ota_key_release.h" AK_OTA_RELEASE_KEY_H)
if (CONFIG_AK_OTA_TEST_KEY STREQUAL "y")
    target_compile_definitions(${EXAMPLE_LIB} PRIVATE AK_OTA_USE_TEST_KEY=1)
elseif (AK_OTA_RELEASE_KEY_H MATCHES "#define AK_OTA_KEY_PLACEHOLDER 1")
    message(WARNING "OTA release public key is the PLACEHOLDER: this firmware rejects every "
                    "OTA package. Generate the release key (docs/ota.md 3.1) before shipping.")
endif()
if (CONFIG_AK_DEBUG_PROBES STREQUAL "y")
    target_compile_definitions(${EXAMPLE_LIB} PRIVATE AK_ENABLE_DEBUG_PROBES=1)
endif()
if ("$ENV{AK_RELEASE_BUILD}" STREQUAL "1")
    if (CONFIG_AK_LICENSE_DEBUG_READ STREQUAL "y" OR CONFIG_AK_OTA_TEST_KEY STREQUAL "y" OR
        CONFIG_AK_DEBUG_PROBES STREQUAL "y")
        message(FATAL_ERROR "AK_RELEASE_BUILD=1 with a debug/test-key profile")
    endif()
    if (AK_OTA_RELEASE_KEY_H MATCHES "#define AK_OTA_KEY_PLACEHOLDER 1")
        message(FATAL_ERROR "AK_RELEASE_BUILD=1 but ak_ota_key_release.h is the placeholder; "
                            "see docs/ota.md 3.1")
    endif()
endif()
