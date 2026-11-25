# WS2812 power switch bring-up (v0.3.2)

Validates the v0.3.2 high-side switch that powers the WS2812 chain and its level
shifter U2 (SN74LV1T34):

```text
P18 (LED_PWR_EN, J1.11) -> R10 -> Q3 S8050 -> Q2 AO3401A gate
VDD_5V -> Q2 -> VDD_LED -> U2.VCC, C1, LED1..LED7
P4 (SPI1_MOSI) -> U2 -> series resistor -> LED1 DIN
```

P18 high = chain powered; R9 holds it off while P18 floats during reset.

> **Board revision.** Only v0.3.2 has the switch. On lite / v0.3.1 boards P18 is
> not connected: the chain is always powered and the P18 steps do nothing. The
> app still runs there (useful as a WS2812 check) but its power results do not
> apply. `ws2812` and `full_selftest` also power the chain through P18 at boot
> (build option `BRINGUP_LED_PWR_AUTO`, default on; same shared sequence in
> `common/src/bringup_led_power.c`).

## Sequences

- **Power on:** DIN (P4) idle low -> P18 high -> wait 10 ms for VDD_LED to settle
  (90 % at ~3.3 ms with C15 = 1 µF) -> 1 ms reset (DIN low) -> first frame.
- **Power off:** all-zero frame -> DIN idle low -> P18 low. The next power-on
  waits at least 250 ms (Q2 turns off in ~160 ms through R9/C15).

The test pattern is LED1..LED7 = **R G B W R G B** at low brightness, so a wrong
or shifted first frame is easy to see. Colour order is GRB, as in `firmware/app`
and `full_selftest` (`ws2812/` uses `RGB_ORDER`; if red and green look swapped
here, record it).

## Steps

1. **Back-feed check (at boot, 10 s).** P18 low, P4 driven high as a GPIO.
   Measure VDD_LED (Q2.D or C1+): expect < 0.3 V and no LED glowing. Higher
   means a back-feed path through U2 (study §3.3, no Ioff in its datasheet).
   Afterwards P4 goes back to SPI1. Reboot to repeat.
2. Commands, from a UART0 byte (115200 8N1) or a key:

| Byte | Key | Step |
|------|-----|------|
| `1` | K1 | Power on with the test pattern |
| `0` | K2 | Power off; check all 7 LEDs dark |
| `d` | K5 | Power on with all LEDs dark (standby current with the chain on) |
| `s` / `S` | K3 | Stress: 50 / 500 on-off cycles (200 ms on, 250 ms off); any byte or K2 stops |
| `w` | K4 | Settle sweep 0 / 1 / 3 / 5 / 10 / 20 ms from fully off; check the first frame |
| `p` | | Print P18 state |
| `h` | | Help |

Each step prints one JSON result line (`"type":"bringup","name":"led_power"`) and
`# ` lines saying what to look at or measure. `driver_errors` in the stress
result only counts SPI/driver failures; what you see on the LEDs has to be
recorded by hand.

## Measurement points

| What | Where | Expect |
|------|-------|--------|
| Standby current, chain off vs on-dark | 5 V input current, `0` vs `d` | Difference ≈ 4 mA (7 WS2812 idle) |
| Back-feed | VDD_LED with P18 low, P4 high (boot step) | < 0.3 V |
| Inrush | VDD_5V current at P18 rising (trigger on J1.11) | ≈ 0.5 A peak with C15 = 1 µF |
| VDD_LED rise | Q2.D / C1 at P18 rising | 90 % in ≈ 3.3 ms, before the first frame |
| DIN order | P4 or LED1 DIN vs P18 | No data before P18 high + settle; DIN low before P18 falls |
| VDD_LED fall | Q2.D / C1 after `0` | ≈ 0 V within ~200 ms |

## Build

```bash
source <TuyaOpen>/export.sh
cd firmware/bringup/led_power
tos.py config choice -c TUYA_T5AI_CORE.config
tos.py build
```

`CONFIG_ENABLE_LEDS_PIXEL=y` pulls in the SPI WS2812 driver, as in `ws2812/`.

## Flash

Flash `dist/<version>/led_power_QIO_<version>.bin` from the Windows host (dev-board Type-C) with tyutool (or `tos.py flash`):

```powershell
tyutool_cli write -d t5ai -p COMx -b 460800 -f <app>_QIO_<version>.bin   # see tyutool_cli --help
```

UART0 is 115200 8N1. Every JSON line carries `t_ms` (ms since boot), so the
time between P18 high and the first frame, or the length of each stress cycle,
can be read off the log.

## Expected behaviour

| When | v0.3.2 | lite / v0.3.1 (no switch) |
|------|--------|---------------------------|
| Boot | All LEDs dark; 10 s back-feed window, then `"status":"ready"` | LEDs may keep whatever they showed before reset; back-feed step does not apply |
| `1` / K1 | LED1..LED7 light R G B W R G B ~11 ms after the command | Same pattern (chain always powered) |
| `0` / K2 | All dark; VDD_LED falls to ~0 V | All dark (zero frame), VDD_LED stays at 5 V |
| `d` / K5 | Dark, chain powered: 5 V current ≈ 4 mA higher than after `0` | No current difference |
| `s` / `S` | Every cycle lights correctly and goes fully dark; `driver_errors` 0 | Same, but P18 does nothing |
| `w` | 10 ms (firmware value) and longer give a correct first frame | All waits correct (chain never unpowered) |
