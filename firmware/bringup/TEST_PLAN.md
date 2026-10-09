# Unified hardware bring-up test plan

> **v0.3 update (expansion baseboard, 2026-10-01):** the input and LED blocks
> changed. The 5-way joystick is now an **EC11 rotary encoder** (A=P20/B=P21/
> SW=P22), a **sixth key K6=P32** was added, the WS2812 chain is now **7 LEDs**,
> and the LCD is a 12P ST7789P3 whose **P7 is TE** (panel VDD tied to 3V3, no
> LCD_PWR). The expansion board's **own USB bridge, battery path, VBUS/BAT
> indicator LEDs and battery switch were all removed** — flash via the dev-board
> Type-C. Acceptance steps 6 (LED4/LED5) and 7 (battery/switch) therefore no
> longer apply and have been dropped; step 3 now covers 6 keys + the encoder and
> step 5 covers 7 LEDs. See `hardware/eda/expansion-baseboard-v0.3-change-scope.md`.

> **v1.2 follow-up (2026-09):** This plan was written for the v1.0 board. On
> v1.2 the WS2812 data pin moved P13 -> P4 (SPI1_MOSI) and is now driven by the
> official SPI WS2812 driver, and the LCD backlight moved P5 -> P8. Where the
> text below still says "WS2812 DIN is P13 / GPIO bit-bang" it describes the
> v1.0 setup; the v1.2/v0.3 wiring is authoritative in `hardware/pinmap/` and
> the per-app READMEs. Historical text is kept as-is for traceability.

## Safety prerequisites

- Connect only one Type-C UART bridge at a time.
- Keep WS2812 brightness at the bring-up defaults (7 LEDs at full white draws
  more current than the old 3-LED chain).

> v0.3 removed the expansion board's battery path and its own CH340C, so the
> old "one battery only" / "CH340C powered by expansion Type-C" cautions no
> longer apply; power and flash through the dev-board Type-C.

## Build all applications

```bash
cd firmware/bringup
./build_all.sh
```

Current automated result: all seven T5AI QIO firmware images build successfully.
The steps below require the expansion baseboard, a Windows flash host, and
visual/RF observation; they cannot be closed by the EC2 build alone.

## Windows flashing pattern

From `scripts/`:

```powershell
.\flash.ps1 -Port COM5 -App "firmware/bringup/<app>" -Build -Monitor
```

The flash baud is 460800. UART0 application monitoring defaults to 115200.

## Acceptance sequence

| Step | Application / check | Pass criteria |
|------|---------------------|---------------|
| 0 | Power-off visual inspection | No shorts, reversed/shifted FPC, or solder bridges |
| 1 | `uart_loopback` | Ready/heartbeat received; arbitrary sent bytes echo exactly |
| 2 | `key1_uart` | KEY1 produces one debounced down and one up frame with plausible `held_ms` |
| 3 | `input_scan` | All six keys (K1-K6) produce correct down/up frames; rotating the EC11 CW/CCW emits `enc` frames with the matching `dir` and one `steps` per detent; the encoder push (`enc_sw`) emits down/up; no idle chatter |
| 4 | `lcd_st7789` | Full 240x320 area displays stable color bars and solid RGB/white/black in the correct colors |
| 5 | `ws2812` | LED1-LED7 light one at a time in chain order, then LED1 red / LED2 green / LED3 blue, all dim white, then all off; no flicker |
| 6 | `wireless_selftest` | Wi-Fi scan finds nearby APs; BLE scanner sees `AK-Bringup`; connect/disconnect events appear |

Record the COM port, board revision, observed result, and any
scope/logic-analyzer measurements for each step in `TEST_RESULTS.md`. Firmware
SHA-256 is optional during bring-up; capture it for release/OTA builds.

## Step 0 detail - power-off visual inspection

No power, no flashing. Purely eyes plus a multimeter before the first power-up,
to catch the low-level mistakes that can destroy the LCD or T5 the instant power
is applied. Do this with the dev-board Type-C unplugged.

1. Solder quality (magnifier): check dense-pin parts (T5 header, level
   shifter/U2, EC11/U3) for solder bridges between adjacent pins; look for cold
   joints, tombstoned parts, and unsoldered pads.
2. Polarity/orientation: the seven WS2812 (notch/marking), the EC11 footprint
   (rotated 270deg on the bottom layer), and the hot-swap key sockets are all
   oriented correctly.
3. FPC / LCD (most likely to fry the panel): the panel is now a **12P ST7789P3**
   FPC — match the connector pin order, verify the flex golden-finger side
   matches the connector (bottom- vs top-contact), fully inserted, latch closed,
   not shifted by one pin. (v0.3 ties panel VDD to 3V3; there is no LCD_PWR
   switch to get wrong, and FPC7 is TE/input only.)
4. Multimeter continuity (buzzer, unpowered): no short from 3V3-GND; confirm GND
   points are continuous.

Pass: no rail-to-ground short, correct FPC orientation, no solder bridges.

## Known diagnostic boundary

WS2812 data is driven from P4 (SPI1_MOSI) through the official SPI driver, so
step 5 timing is produced by the SPI peripheral, not CPU bit-banging. If step 5
fails, measure the P4 / level-shifter output waveform before changing software
or hardware. (The v1.0 P13 GPIO bit-bang path is gone — see `测试总结.md` §3-4.)
