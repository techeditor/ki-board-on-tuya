# Ki-board product firmware

Product firmware for the Ki-board v0.3 hardware (Tuya T5AI-Core + passive
expansion board with 6 keys, an EC11 knob, 7 WS2812 LEDs and a 320×240 ST7789
LCD). For the hardware tests used on a new board, see
[`../bringup/`](../bringup/).

Wire protocol: [`shared/protocol/protocol.md`](../../shared/protocol/protocol.md).

## What it does

| Area | Summary |
|---|---|
| Input | Scans K1–K6 and the EC11 knob, debounces them and classifies single / double / long presses; a keymap (default table, or one sent by the Companion and kept across reboots) turns gestures into actions |
| Display | Status bar, three chat-slot icons coloured by agent state, voice input confirm page, paged menus for approvals / questions / options / model and mode, settings page; Chinese and English UI |
| LEDs | Per-key colours for menus and states, breathing / blinking effects, chain power off / on / auto-off |
| Links | USB serial (UART0) and BLE GATT, used together; BLE frames go through an encrypted channel; sensitive commands are accepted from USB only |
| Voice | Hold-to-talk key; board-side Tuya cloud ASR or PC speech recognition; recognised text is confirmed on the keyboard before it is sent; optional chat mode with a cloud agent |
| Provisioning | Pairing window opened on the keyboard, Wi-Fi and device licence written over USB, Tuya app activation |
| Update | Local OTA over USB is built in but not open yet: it takes only an officially signed package. Public builds carry a placeholder release key and reject every OTA package; update by flashing a new image |
| Settings | Volume, brightness, language, voice mode, LED power; language, voice mode and LED power survive a reboot (KV storage) |

The shared code lives in `common/` (one `ak_*` module per area) and
`src/tuya_app_main.c` wires it together.

## Build profiles

One app, two TuyaOpen profiles in `config/`. `app_default.config` is a copy
of `TUYA_T5AI_CORE_dual_cloudasr.config`.

| Profile | Links | Board cloud ASR | Use |
|---|---|---|---|
| `TUYA_T5AI_CORE_dual_cloudasr.config` | USB + BLE | on | **release profile** |
| `TUYA_T5AI_CORE_dual_cloudasr_debug.config` | USB + BLE | on | development only: adds UART licence readout, the OTA test key and debug probes; never shipped |

The firmware advertises as `Ki-board-XXXXXX` (for example `Ki-board-D3E4F5`), where `XXXXXX` is
the last three bytes of the Wi-Fi station MAC (the BLE address when that is
unavailable), so several keyboards nearby can be told apart. Earlier firmware
advertised `Agentic Keyboard-XXXXXX` or `AK-KB`; the Companion still finds those.

The release profile contains no device credentials. The device licence
(UUID / AuthKey) is written over USB inside the pairing window and stored on
the device. Only the debug profile can fall back to a gitignored
`include/ak_tuya_authkey.h` (copy `ak_tuya_authkey.h.example`).

## Your own PID and License on a public firmware

A public firmware has no Tuya product ID (PID) and no device License
(UUID / AuthKey) in it. You write both after flashing, over USB, with the
Companion. Without a PID the Tuya cloud stays off (status bar: "No product ID:
write it over USB"), but the keyboard, the LCD and the USB / Bluetooth
Companion link work normally.

1. Flash `app_QIO_<version>.bin` and connect the keyboard over USB to the
   Companion.
2. **PID** (16 characters of `a-z` and `0-9`, from your Tuya IoT platform
   product): Companion, Device page, "Tuya product ID", Write product ID.
   The keyboard accepts a PID **once, only while it has none**, only over USB.
   There is no command to change or clear it, and unbinding or resetting the
   network does not touch it. Restart the keyboard to apply it (the status bar
   says so). To use another PID, erase the whole flash and flash again.
3. **License**: on the keyboard open Settings > Pairing mode and confirm with
   K6 (a keyboard without a License opens the 180 s window at once). Inside the
   window write UUID / AuthKey with "Write license". The License is stored in
   the device's authorization area, not in the firmware.
4. Still in the window, write your 2.4 GHz Wi-Fi with the Wi-Fi form, then
   finish with SmartLife (QR code in the Companion) or, after Settings >
   Pairing mode restarts the keyboard, with the Tuya app over Bluetooth.

Settings > Credentials shows only SET / NOT SET for the PID, UUID and AuthKey,
and the keyboard never sends any of them back. UUID and AuthKey are never
logged; the PID is not a secret and appears once in the boot log. The PID is in effect from the
restart after it was written; a PID compiled in with `CONFIG_TUYA_PRODUCT_ID`
(`tos.py config menu`, then `tos.py clean -f`) is used when none was written
at runtime, and a PID written at runtime takes precedence over it. Do not
commit a PID, UUID or AuthKey.

## Build

```bash
source <TuyaOpen>/export.sh
cd firmware/app
tos.py config choice -c TUYA_T5AI_CORE_dual_cloudasr.config
tos.py clean -f && tos.py build      # clean is required after any .config change
```

Output in `dist/app_<version>/`:

- `app_QIO_<version>.bin`: full image, flashed over USB.
- `app_UG_<version>.bin`: upgrade image, used only for officially signed OTA
  packages (not open yet).

A public build carries a placeholder OTA release key and rejects every OTA
package. An official release build (`AK_RELEASE_BUILD=1`) refuses debug
profiles and the placeholder key. The boot splash is the built-in one; custom
splash images are not supported yet.

## Flash

Connect the T5AI-Core USB port and flash the QIO image with `tos.py flash`
(TuyaOpen) or with tyutool on the host PC. Close the Companion first; it holds
the serial port. To update a keyboard, flash the new QIO image the same way;
OTA updates from the Companion are not open yet.
