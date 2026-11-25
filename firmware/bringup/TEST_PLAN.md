# Unified hardware bring-up test plan

## Safety prerequisites

- Connect only one Type-C UART bridge at a time.
- Keep WS2812 brightness at the bring-up defaults (7 LEDs at full white draw a
  lot of current).
- Power and flash through the dev-board Type-C.

## Build all applications

```bash
cd firmware/bringup
./build_all.sh
```

Current automated result: all eight T5AI QIO firmware images build successfully.
The steps below require the expansion baseboard, a Windows flash host, and
visual/RF observation; they cannot be closed by the cloud build alone.

## Windows flashing pattern

Flash with tyutool (or `tos.py flash`) from a Windows host, for example:

```powershell
tyutool_cli write -d t5ai -p COMx -b 460800 -f <app>_QIO_<version>.bin   # see tyutool_cli --help
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
| 7 (v0.3.2 only) | `led_power` | See "Step 7 detail" below. Skip on lite / v0.3.1 (no power switch; P18 not connected) |

Record the COM port, board revision, observed result, and any
scope/logic-analyzer measurements for each step in your own test notes. Firmware
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

## Step 7 detail - v0.3.2 WS2812 power switch (`led_power`)

v0.3.2 only (Q2/Q3/R9-R12/C15 fitted, P18 = LED_PWR_EN). With the default
build option (`BRINGUP_LED_PWR_AUTO` = 1) `ws2812` and `full_selftest` also
power the chain through P18 at boot, so step 5 and the combined self-test run
on v0.3.2 as on older boards; check their boot line shows
`"led_pwr_option":"on"`. Tools: scope (2 channels), multimeter, a
current meter on the 5 V input. Commands are UART0 bytes or keys; see
`led_power/README.md`.

| Sub-step | Action | Pass criteria |
|----------|--------|---------------|
| 7a back-feed | Boot; during the 10 s window measure VDD_LED (Q2.D / C1+) | VDD_LED < 0.3 V with P18 low and P4 high; no LED glows |
| 7b on | `1` / K1 | LED1..LED7 = R G B W R G B, first frame correct |
| 7c power-on order | Scope CH1 = P18 (J1.11), CH2 = VDD_LED, then CH2 = LED1 DIN; `1` | VDD_LED reaches 90 % in ≈ 3.3 ms; no DIN activity before P18 high + settle |
| 7d inrush | Current probe on the 5 V input, trigger on P18 rising; `1` | Peak ≈ 0.5 A (C15 = 1 µF); dev-board 5 V does not brown out |
| 7e off | `0` / K2; scope DIN and P18 | All 7 LEDs dark; DIN low before P18 falls; VDD_LED ≈ 0 V within ~200 ms |
| 7f standby current | Read 5 V current after `d` (on, dark) and after `0` (off) | Off is lower by ≈ 4 mA |
| 7g stress | `s` (50) and `S` (500) | Every cycle shows the correct pattern and goes fully dark; `driver_errors` = 0 |
| 7h settle sweep | `w` | Record which waits (0/1/3/5/10/20 ms) give a correct first frame; 10 ms (firmware value) must be correct |

Record each sub-step in your own test notes.

## Known diagnostic boundary

WS2812 data is driven from P4 (SPI1_MOSI) through the official SPI driver, so
step 5 timing is produced by the SPI peripheral, not CPU bit-banging. If step 5
fails, measure the P4 / level-shifter output waveform before changing software
or hardware.
