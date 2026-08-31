##
# @file CMakeLists.txt
# @brief P13 WS2812 bring-up application.
#

set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})
set(BRINGUP_COMMON_PATH ${APP_PATH}/../common)
set(T5AI_AP_ROOT $ENV{OPEN_SDK_ROOT}/platform/T5AI/t5_os/ap)

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
        ${T5AI_AP_ROOT}/middleware/arch/cm33
        ${T5AI_AP_ROOT}/components/cmsis/CMSIS_5/Device/Beken/armstar
        ${T5AI_AP_ROOT}/components/cmsis/CMSIS_5/CMSIS/Core/Include
)
