# Agentic Keyboard for Kiro — product firmware

Product firmware for the Tuya T5AI-Core + expansion baseboard **v1.2**.
Distinct from `firmware/bringup/` (throwaway per-peripheral hardware validation).

## Architecture (mechanism A)

ONE app, one shared core, and **build-time-selected** transport / ASR backends.
Each build profile produces a distinct flashable binary, so variants can be
flashed and compared like the bring-up apps — without forking the shared code.

```
firmware/app/
├── common/
│   ├── include/
│   │   ├── ak_board_io.h   # authoritative v1.2 pinout (WS2812=P4/SPI1, LCD_BL=P8)
│   │   ├── ak_features.h   # compile-time variant selection (transport / ASR)
│   │   └── ak_uart.h       # UART0 transport backend
│   └── src/
│       └── ak_uart.c
├── src/
│   └── tuya_app_main.c     # entry: hello_ack + heartbeat over selected transport
├── config/
│   └── TUYA_T5AI_CORE.config   # active build profile (v1: UART)
├── app_default.config          # mirrors the active profile (see build note)
└── CMakeLists.txt
```

### Variant matrix (planned)

| Profile | Transport | Board cloud ASR | Phase |
|---------|-----------|-----------------|-------|
| v1 (current) | UART0 (`AK_TRANSPORT_UART`) | off (PC-side Windows ASR) | 1 |
| v2 | BLE GATT (`AK_TRANSPORT_BLE_GATT`) | off | 2 |
| +cloud-asr | any | on (`AK_ASR_BOARD_CLOUD`) | 1–2 |
| v3 | BLE HID (`AK_TRANSPORT_BLE_HID`, no JSONL) | — | 3 |

Exactly one `AK_TRANSPORT_*` is enabled per build (guarded in `ak_features.h`).
The BLE GATT profiles advertise as `Agentic Keyboard-XXXXXX`, where `XXXXXX`
is derived from the device address so multiple nearby keyboards can be told
apart. The shared core (input scan, event FSM, protocol, display) is written
once and reused across all profiles.

## Firmware version

`VERSION` is the single source of truth. The version command synchronizes it
with every TuyaOpen profile so the runtime `hello_ack`, cloud software version,
and generated BIN/dist names stay aligned.

```bash
cd firmware/app
python3 tools/version.py show          # current version
python3 tools/version.py bump          # automatic patch bump: 0.1.0 -> 0.1.1
python3 tools/version.py bump minor    # 0.1.1 -> 0.2.0
python3 tools/version.py bump major    # 0.2.0 -> 1.0.0
python3 tools/version.py set 0.3.0     # manually set an exact version
python3 tools/version.py check         # CI/build consistency check
```

Patch versions increase automatically when a commit contains staged
`firmware/app` implementation changes. The pre-commit hook synchronizes and
stages `VERSION` plus every TuyaOpen profile in the same commit. Re-running a
failed commit does not bump again, and an explicit `set`/`bump minor`/`bump
major` is preserved rather than receiving an extra patch increment. Builds do
not change the version, so repeated builds of one commit remain reproducible.

Enable the tracked hook once per clone (this checkout is configured already):

```bash
git config core.hooksPath .githooks
```

## Build (EC2)

```bash
source /data/projects/TuyaOpen/export.sh
cd firmware/app
tos.py config choice -c TUYA_T5AI_CORE.config
tos.py clean -f && tos.py build      # clean needed after any .config change
```

Artifacts: `dist/<version>/*_QIO_*.bin` (flashed on the board) / `*_UG_*.bin`
(OTA). Build on EC2; flash from Windows with `scripts/flash.ps1`.

> `app_default.config` and `config/TUYA_T5AI_CORE.config` are kept identical for
> the current single profile (same convention as `firmware/bringup/`). When more
> profiles are added, each lives under `config/<variant>.config` and is selected
> with `tos.py config choice -c <variant>.config`.

## Host unit tests (no board)

Pure modules (`ak_input_fsm`, `ak_protocol`) are unit-tested on the host:

```bash
firmware/app/test/run_host_tests.sh   # gcc-compiles + runs; needs no SDK/board
```

Covers debounce, single-DOWN, one-shot LONG at the 800 ms threshold, bounce
rejection, and exact JSONL frame strings.

## Status

- **Task 3 (M1)** — skeleton: v1.2 board config + UART0 `hello_ack` + 2 s `ping`
  heartbeat (JSONL protocol v1). EC2 build ✅ / clang-format ✅ / check_pins ✅.
- **Task 4** — input scan: 5 keys + 5-way nav with 20 ms debounce + 800 ms
  long-press; logic in the pure host-tested `ak_input_fsm`; emits `key`/`nav`
  events. EC2 build ✅ / host tests ✅.
- **Task 5** — protocol v1 complete: pure `ak_protocol` frame builders
  (key/nav/voice/hello_ack/ping/pong/ack) + pure `ak_rx` line assembler & field
  extractors. App RX loop replies `hello`→`hello_ack`, `ping`→`pong`,
  `config`→`ack`, logs `display` (LCD render pending Task 9), ignores unknown
  types. EC2 build ✅ / host tests ✅ / clang-format ✅.

- **Task 9 (partial)** — LCD bring-up: `ak_display` registers the ST7789 on the
  v1.2 wiring (SPI0 + DC/RST, backlight P8) with the CL28CK230 INVOFF init
  sequence validated in bring-up, opens via TDL, sets backlight and draws a
  colour-bar test pattern. `CONFIG_ENABLE_SPI`/`ENABLE_DISPLAY` on; LVGL not yet
  needed. EC2 build ✅. The LVGL 3-zone product UI is deferred until the
  interaction design is finalised and the v1.2 board is in hand.

Awaiting the v1.2 board to smoke-test on hardware (keys/nav/UART RX/TX on the
v1.0 board now; WS2812=P4, LCD_BL=P8, and the colour-bar/INVOFF check need v1.2).

Next: Companion port from `ref_projects/ki-board/` (transport + protocol v1 +
injection); then LVGL UI + Task 10 once the interaction design lands.
