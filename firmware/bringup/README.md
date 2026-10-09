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
| `input_scan` | Validate all six keys (K1-K6) and the EC11 rotary encoder | K1-K6; EC11 A=P20, B=P21, SW=P22 |
| `lcd_st7789` | Validate panel backlight, SPI, reset, and RGB565 output | RST=P6, BL=P8, TE=P7 (v0.3), P14-P17 |
| `ws2812` | Validate seven addressable RGB LEDs and level shifter | P4=SPI1_MOSI (v0.3) |
| `wireless_selftest` | Validate Wi-Fi scanning and BLE advertising | T5 radio |
| `full_selftest` | Combined input/LCD/WS2812/UART/radio self-test in one image | all of the above |

> **v0.3 (expansion baseboard, 2026-10-01)** changed the input and LED blocks:
> the 5-way joystick became an **EC11 rotary encoder** (A=P20/B=P21/SW=P22), a
> **sixth key K6=P32** was added, the WS2812 chain grew **3 -> 7** LEDs, the LCD
> changed to a 12P ST7789P3 panel whose **P7 is now TE (not LCD_PWR)** — panel
> VDD is tied directly to 3V3 — and the expansion board's **USB/battery/VBUS &
> BAT indicator circuits were removed** (flash via the dev-board Type-C). The
> per-app READMEs below reflect v0.3.

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

> On v0.3 the expansion board no longer has its own CH340C/Type-C — flash and
> monitor through the **dev-board Type-C** instead.

All seven applications have passed the TuyaOpen T5AI build. The complete
sequential physical hardware acceptance procedure is in `TEST_PLAN.md`.
