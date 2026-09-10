/**
 * @file ak_features.h
 * @brief Compile-time variant selection for the Agentic Keyboard product app.
 *
 * Mechanism A: ONE product app, a single shared core, and pluggable
 * transport / ASR backends chosen at build time. Each build profile under
 * config/ defines exactly one transport (and optionally the board-side cloud
 * ASR) and produces a distinct flashable binary, so variants can be flashed
 * and compared like the bring-up apps -- without forking the shared code.
 *
 * Defaults below describe the phase-1 wired variant (v1_uart): UART transport,
 * voice handled PC-side by the Companion (Windows ASR), so the board-side cloud
 * ASR module is off. A build profile overrides these via CONFIG_* / -D flags.
 */

#ifndef AK_FEATURES_H
#define AK_FEATURES_H

/* ---- Transport backend (exactly one enabled) ---- */
#ifndef AK_TRANSPORT_UART
#define AK_TRANSPORT_UART 1 /* phase 1: UART0 / CH340C, JSONL protocol v1 */
#endif
#ifndef AK_TRANSPORT_BLE_GATT
#define AK_TRANSPORT_BLE_GATT 0 /* phase 2: custom GATT service, same JSONL */
#endif
#ifndef AK_TRANSPORT_BLE_HID
#define AK_TRANSPORT_BLE_HID 0 /* phase 3: native NimBLE HID (no JSONL) */
#endif

/* ---- Voice / ASR method ----
 * v1 uses PC-side Windows ASR (the board only emits a voice:start trigger),
 * so the board-side cloud-ASR module is compiled out by default.
 */
#ifndef AK_ASR_BOARD_CLOUD
#define AK_ASR_BOARD_CLOUD 0
#endif

/* ---- Display (ST7789 bring-up; TDL only, LVGL UI deferred) ---- */
#ifndef AK_ENABLE_DISPLAY
#define AK_ENABLE_DISPLAY 1
#endif

#if (AK_TRANSPORT_UART + AK_TRANSPORT_BLE_GATT + AK_TRANSPORT_BLE_HID) != 1
#error "Select exactly one AK_TRANSPORT_* backend in the build profile"
#endif

#endif /* AK_FEATURES_H */
