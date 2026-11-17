/**
 * @file ak_ota_link.h
 * @brief Local OTA on the device: ota_* frames from the USB Companion, the
 *        flash layer (ota partition + TuyaOS manage block, tuyaboot installs),
 *        install markers in KV, and the debug-only board probes.
 *
 * Wire protocol: shared/protocol/protocol.md "固件升级（仅 USB）".
 * Frames reach this module only from UART0 (ak_rx_source_allows); the caller
 * drops BLE copies before calling in.
 */

#ifndef AK_OTA_LINK_H
#define AK_OTA_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "ak_ota.h"

typedef enum {
    AK_OTA_UI_CONFIRM,   /* ask on the device: K6 install / K1 cancel */
    AK_OTA_UI_PROGRESS,  /* erasing / receiving: percent */
    AK_OTA_UI_INSTALLING,/* manage block written; reboot follows */
    AK_OTA_UI_FAILED,    /* session ended without installing */
    AK_OTA_UI_UPDATED,   /* first boot after an install */
} ak_ota_ui_t;

typedef struct {
    /* Write one complete JSONL line to UART0 only (never mirrored to BLE). */
    void (*send_uart)(const char *line, int length);
    /* USB power present (UART0 Companion link is live). */
    bool (*usb_power)(void);
    /* Battery reading the gauge marks trusted, and its percent. */
    bool (*battery_trusted)(uint8_t *percent);
    /* LCD hook. `text` is data, not a sentence: the reason word for FAILED,
     * the version for CONFIRM / UPDATED, "" otherwise. The app words it
     * through the string table (ak_i18n). */
    void (*ui)(ak_ota_ui_t what, const char *text);
} ak_ota_link_cb_t;

/** Call once after ak_kv_init and display init: reads the install markers
 *  (shows "Updated to x.y.z" on the first boot of a new version). */
void ak_ota_link_init(const ak_ota_link_cb_t *cb);

/** Handle an ota_* / ota_probe / probe_line frame. Returns false when `type`
 *  is not one of them. */
bool ak_ota_link_on_frame(const char *type, const char *payload, uint32_t now_ms);

/** App-loop step: erase, timeouts, commit. */
void ak_ota_link_tick(uint32_t now_ms);

/** After a USB hello: report OTA availability (and a finished install once). */
void ak_ota_link_on_hello(void);

/** True while an update (or a debug probe) owns the keys and the UART. */
bool ak_ota_link_busy(void);

/** True while the device waits for K6 / K1. */
bool ak_ota_link_confirming(void);

/** What the LCD shows while busy (system confirm ⑦ / progress). */
typedef struct {
    ak_ota_state_t state;
    char           version[24]; /* the package's version */
    uint8_t        percent;     /* RECEIVING: bytes written */
} ak_ota_view_t;

/** False when no update is open. */
bool ak_ota_link_view(ak_ota_view_t *out);

/** The device answer (K6 = true, K1 = false). */
void ak_ota_link_confirm(bool accept, uint32_t now_ms);

#endif /* AK_OTA_LINK_H */
