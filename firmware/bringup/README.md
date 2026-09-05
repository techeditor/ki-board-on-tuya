# Firmware bring-up applications

These are small, standalone TuyaOpen applications used to validate the
expansion baseboard one hardware block at a time. They are not product firmware
and are not host-side unit tests.

## IO source of truth

All bring-up applications include `common/include/agentic_board_io.h`. Its
values mirror `hardware/pinmap/pinmap.json` and `hardware/pinmap/pinmap.md`.
Do not copy pin definitions from reference projects.

## Applications

| Application | Purpose | Main IO |
|-------------|---------|---------|
| `uart_loopback` | Validate bidirectional CH340C/UART0 communication | UART0 RX=P10, TX=P11 |
| `key1_uart` | Validate flashing, UART0 communication, and KEY1 input/debounce | KEY1=P24; UART0 RX=P10, TX=P11 |
| `input_scan` | Validate all five keys and five navigation inputs | P20-P27, P30, P32 |
| `lcd_st7789` | Validate panel power, backlight, SPI, reset, and RGB565 output | RST=P6, PWR=P7, BL=P8 (v1.2), P14-P17 |
| `ws2812` | Validate three addressable RGB LEDs and level shifter | P4=SPI1_MOSI (v1.2) |
| `wireless_selftest` | Validate Wi-Fi scanning and BLE advertising | T5 radio |
| `full_selftest` | Combined input/LCD/WS2812/UART/radio self-test in one image | all of the above |

## Build

```bash
source /data/projects/TuyaOpen/export.sh
cd firmware/bringup/key1_uart
tos.py config choice -c TUYA_T5AI_CORE.config
tos.py build
```

Artifacts are written to `dist/<version>/` and ignored by Git.

Build every application and run the shared format/IO checks:

```bash
cd firmware/bringup
./build_all.sh
```

To rebuild selected applications after a local change:

```bash
./build_all.sh ws2812 wireless_selftest
```

## Flash from Windows

Run from the repository's `scripts` directory:

```powershell
.\flash.ps1 -Port COM5 -App "firmware/bringup/key1_uart" -Build -Monitor
```

Only connect one of the two Type-C UART bridges at a time. UART0 uses
115200 8N1; the separate Tuya debug-log port uses 460800 baud.

All six applications have passed the TuyaOpen T5AI build. The complete
sequential physical hardware acceptance procedure is in `TEST_PLAN.md`.
