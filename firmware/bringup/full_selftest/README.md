# full_selftest — combined hardware self-test

One flashable image that exercises every hardware block the standalone bring-up
apps validate individually, so the whole baseboard can be checked in a single
flash. The current test and instructions are shown on the LCD; long-pressing the
**EC11 encoder knob** switches between tests.

## Tests (cycle order)

| # | Mode | What it checks | How to verify |
|---|------|----------------|---------------|
| 1 | `INPUT` | 6 keys (K1-K6) + EC11 rotary encoder (turn + push) with 20 ms debounce | Press each key — its tile turns green and a `key` JSON event is emitted; turn the knob to see `enc` CW/CCW frames and the live step count; push emits `enc_sw` |
| 2 | `LCD` | ST7789 panel: color bars + solid R/G/B/W/black | Colors cycle every 1.5 s; each pattern name is emitted on UART0 |
| 3 | `WS2812` | 7 addressable LEDs on SPI1 (P4) | LEDs cycle off -> red -> green -> blue -> white every 1 s |
| 4 | `UART` | UART0 loopback (P10/P11) | Anything you type into the serial terminal echoes back; RX/echo counters shown on LCD |
| 5 | `RADIO` | Wi-Fi scan + BLE advertising | AP count / best RSSI refresh every 5 s; device advertises as `AK-Selftst` |

## Switching tests

Hold the **EC11 knob (enc_sw)** for >= 0.8 s. This works in every mode (in
`INPUT` a short push is still reported as a normal `enc_sw` event; only the
long-press switches). Leaving `WS2812` turns the LEDs off automatically.

## UART protocol

UART0 emits newline-delimited JSON at 115200 8N1. Examples:

```json
{"v":1,"type":"selftest","payload":{"status":"ready","lcd":true,"ws2812":true,"modes":5}}
{"v":1,"type":"selftest","payload":{"mode":"INPUT","index":1,"count":5}}
{"v":1,"seq":7,"type":"key","payload":{"id":"k3","action":"down","held_ms":0}}
{"v":1,"seq":8,"type":"enc","payload":{"dir":"cw","steps":1}}
{"v":1,"seq":9,"type":"key","payload":{"id":"enc_sw","action":"up","held_ms":142}}
{"v":1,"type":"selftest","payload":{"mode":"RADIO","wifi":"scan_ok","ap_count":9,"best_rssi":-41,"ble":"advertising"}}
```

The Tuya debug log stays on UART1 at 460800 baud.

## Build

```bash
source /data/projects/TuyaOpen/export.sh
cd firmware/bringup/full_selftest
tos.py config choice -c TUYA_T5AI_CORE.config
tos.py build
```

Or with the shared driver from `firmware/bringup`:

```bash
./build_all.sh full_selftest
```

## Flash from Windows

```powershell
.\flash.ps1 -Port COM5 -App "firmware/bringup/full_selftest" -Build -Monitor
```

Connect only one Type-C UART bridge at a time.

## Notes

- IO comes from `common/include/agentic_board_io.h` (mirrors `hardware/pinmap/`).
  This is v0.3 wiring: 6 keys (K6=P32), EC11 encoder A=P20/B=P21/SW=P22, 7 WS2812
  on SPI1_MOSI=P4, LCD backlight on P8, and P7 reused as LCD TE (not driven).
- The LCD text is rendered by a small embedded 8x8 font (`include/ak_font8x8.h`,
  public domain) via `src/ak_gfx.c`, since the display driver only exposes solid
  fills. This tester does **not** use LVGL.
- The Wi-Fi scan in `RADIO` is blocking (~1-3 s); an encoder long-press issued
  during a scan takes effect when the scan returns.
