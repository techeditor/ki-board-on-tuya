# Unified hardware bring-up test plan

Run the applications sequentially. Keeping each firmware focused makes a
failure attributable to one hardware block; this is the unified acceptance
sequence, not one monolithic diagnostic image.

## Safety prerequisites

- Connect only one Type-C UART bridge at a time.
- Connect only one battery to the shared BAT net.
- The expansion-board Type-C powers the CH340C only; ensure the T5 system
  itself is powered.
- Keep WS2812 brightness at the bring-up defaults.

## Build all applications

```bash
cd firmware/bringup
./build_all.sh
```

Current automated result: all six T5AI QIO firmware images build successfully.
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
| 0 | Power-off visual inspection | No shorts, reversed FPC/battery, solder bridges, or two batteries |
| 1 | `uart_loopback` | Ready/heartbeat received; arbitrary sent bytes echo exactly |
| 2 | `key1_uart` | KEY1 produces one debounced down and one up frame with plausible `held_ms` |
| 3 | `input_scan` | Five keys and five navigation directions each produce correct down/up frames; no idle chatter |
| 4 | `lcd_st7789` | Full 240x320 area displays stable color bars and solid RGB/white/black in the correct colors |
| 5 | `ws2812` | LED1 red, LED2 green, LED3 blue, all dim white, then all off; no flicker |
| 6 | LED4/LED5 manual check | LED4 follows Type-C VBUS; LED5 follows battery BAT_P |
| 7 | Battery/switch manual check | Battery switch controls the battery path; Type-C behavior matches the schematic; only one cell connected |
| 8 | `wireless_selftest` | Wi-Fi scan finds nearby APs; BLE scanner sees `AK-Bringup`; connect/disconnect events appear |

Record the COM port, board revision, observed result, and any
scope/logic-analyzer measurements for each step in `TEST_RESULTS.md`. Firmware
SHA-256 is optional during bring-up; capture it for release/OTA builds.

## Step 0 detail - power-off visual inspection

No power, no flashing. Purely eyes plus a multimeter before the first power-up,
to catch the low-level mistakes that can destroy the LCD, battery, or T5 the
instant power is applied. Do this with all Type-C unplugged, the battery switch
off, and ideally the battery removed.

1. Solder quality (magnifier): check dense-pin parts (T5 header, CH340C/U3,
   level shifter/U2, LDO/U5) for solder bridges between adjacent pins; look for
   cold joints, tombstoned parts, and unsoldered pads.
2. Polarity/orientation: LDO, transistors Q1/Q2, diodes, the three WS2812
   (notch/marking), and the Type-C receptacle are all oriented correctly.
3. FPC / LCD (most likely to fry the panel): match the 18P order in PRD 3.3
   (Pin 1 = GND, Pin 2 = RST); verify the flex golden-finger side matches the
   connector (bottom- vs top-contact), fully inserted, latch closed, not shifted
   by one pin.
4. Battery / power reversal (most likely to burst): JST BAT+/BAT- polarity is
   correct; only one cell connected (two cells in parallel back-feed - unsafe);
   battery switch (SW1/SK12D07VG5) off.
5. Multimeter continuity (buzzer, unpowered): no short from 3V3-GND, 5V-GND, or
   VBUS-GND (use 3V3 at J2.1 and a GND point as probes); confirm GND points are
   continuous.

Pass: no rail-to-ground short, correct FPC/battery orientation, no solder
bridges, at most one cell connected.

## Known diagnostic boundary

The WS2812 DIN is P13, which is not a supported SPI MOSI route on T5. Its
bring-up therefore uses T5-specific cycle-counted GPIO output. If step 5 fails,
measure the P13 and level-shifter output waveform before changing software or
hardware.
