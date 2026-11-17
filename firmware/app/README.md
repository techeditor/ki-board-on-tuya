# Ki-board product firmware

Product firmware for the Ki-board v0.3 hardware (Tuya T5AI-Core + passive
expansion board with 6 keys, an EC11 knob, 7 WS2812 LEDs and a 320×240 ST7789
LCD). For the hardware tests used on a new board, see
[`../bringup/`](../bringup/).

Wire protocol: [`shared/protocol/protocol.md`](../../shared/protocol/protocol.md).
Local firmware update (OTA): [`docs/ota.md`](../../docs/ota.md).

## What it does

| Area | Summary |
|---|---|
| Input | Scans K1–K6 and the EC11 knob, debounces them and classifies single / double / long presses; a keymap (default table, or one sent by the Companion and kept across reboots) turns gestures into actions |
| Display | Status bar, three chat-slot icons coloured by agent state, voice input confirm page, paged menus for approvals / questions / options / model and mode, settings page; Chinese and English UI |
| LEDs | Per-key colours for menus and states, breathing / blinking effects, chain power off / on / auto-off |
| Links | USB serial (UART0) and BLE GATT, used together in the dual profiles; BLE frames go through an encrypted channel; sensitive commands are accepted from USB only |
| Voice | Hold-to-talk key; board-side Tuya cloud ASR or PC dictation; recognised text is confirmed on the keyboard before it is sent; optional chat mode with a cloud agent |
| Provisioning | Pairing window opened on the keyboard, Wi-Fi and device licence written over USB, Tuya app activation |
| Update | Local OTA over USB: signed package, version only goes up, confirmed on the keyboard |
| Settings | Volume, brightness, language, voice mode, LED power; language, voice mode and LED power survive a reboot (KV storage) |

The shared code lives in `common/` (one `ak_*` module per area) and
`src/tuya_app_main.c` wires it together. Host unit tests are in `test/`,
build and signing tools in `tools/`.

## Build profiles

One app, five TuyaOpen profiles in `config/`. `app_default.config` is a copy
of `TUYA_T5AI_CORE_dual_cloudasr.config`.

| Profile | Links | Board cloud ASR | Use |
|---|---|---|---|
| `TUYA_T5AI_CORE.config` | USB serial | off | minimal wired build |
| `TUYA_T5AI_CORE_ble.config` | BLE GATT only | off | wireless-only test build; cannot pair (pairing needs USB) unless a key is already stored |
| `TUYA_T5AI_CORE_dual.config` | USB + BLE | off | dual link without cloud |
| `TUYA_T5AI_CORE_dual_cloudasr.config` | USB + BLE | on | **release profile** |
| `TUYA_T5AI_CORE_dual_cloudasr_debug.config` | USB + BLE | on | development only: adds UART licence readout, the OTA test key and debug probes; never shipped |

The BLE profiles advertise as `Agentic Keyboard-XXXXXX`, where `XXXXXX` comes
from the device address, so several keyboards nearby can be told apart.

Release profiles contain no device credentials. The device licence
(UUID / AuthKey) is written over USB inside the pairing window and stored on
the device. Only the debug profile can fall back to a gitignored
`include/ak_tuya_authkey.h` (copy `ak_tuya_authkey.h.example`).

## Tuya product ID (PID)

Board-side cloud ASR needs a Tuya product ID. Set it with `tos.py config menu`
or by editing `CONFIG_TUYA_PRODUCT_ID` in `app_default.config` /
`config/<profile>.config`, then run `tos.py clean -f` before building. Without
a PID the firmware still runs, but the cloud features stay off. Do not commit
a PID, UUID or AuthKey.

## Build

```bash
source <TuyaOpen>/export.sh
cd firmware/app
tos.py config choice -c TUYA_T5AI_CORE_dual_cloudasr.config
tos.py clean -f && tos.py build      # clean is required after any .config change
```

Output in `dist/app_<version>/`:

- `app_QIO_<version>.bin`: full image, flashed over USB.
- `app_UG_<version>.bin`: upgrade image, the input for an OTA package.

A release build sets `AK_RELEASE_BUILD=1`. It refuses debug profiles and the
placeholder OTA release key; see [`docs/ota.md`](../../docs/ota.md).

## Flash

Connect the T5AI-Core USB port and flash the QIO image with `tos.py flash`
(TuyaOpen) or with tyutool on the host PC. Close the Companion first; it holds
the serial port. Once a keyboard runs a firmware with OTA support, later
versions can be installed from the Companion with a signed `.akota` package.

## Version number

`VERSION` holds the firmware version (for example `0.3.66`).
`tools/version.py` keeps it in sync with `CONFIG_PROJECT_VERSION` in every
profile, so `hello_ack`, the cloud software version and the output file names
agree.

```bash
cd firmware/app
python3 tools/version.py show          # current version
python3 tools/version.py bump          # patch: 0.3.66 -> 0.3.67
python3 tools/version.py bump minor    # 0.3.67 -> 0.4.0
python3 tools/version.py set 0.3.70    # exact version
python3 tools/version.py check         # VERSION and profiles agree
```

A commit that stages changes under `firmware/app` bumps the patch version
automatically (pre-commit hook in `.githooks/`; enable it once per clone with
`git config core.hooksPath .githooks`). Building never changes the version.
The numbering scheme is in [`docs/versioning.md`](../../docs/versioning.md).

## Host unit tests

```bash
firmware/app/test/run_host_tests.sh
```

Compiles the pure modules (input state machine, gestures, protocol, receive
validation, menus, OTA checks and more) with the host gcc and runs them; no
board needed. Set `TUYAOPEN_ROOT` if the SDK is not at the default location.
