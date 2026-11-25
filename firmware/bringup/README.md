# Firmware bring-up applications

These are small, standalone TuyaOpen applications used to validate the
expansion baseboard one hardware block at a time. They are not product firmware
and are not host-side unit tests.

## IO source of truth

All bring-up applications include `common/include/agentic_board_io.h`. Its
values mirror `hardware/pinmap/pinmap-v0.3.json` and `hardware/pinmap/pinmap-v0.3.md`;
`build_all.sh` checks them with `hardware/pinmap/check_pins_v0.3.py`.
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
| `led_power` | v0.3.2 only: WS2812 chain power switch, power-on order, back-feed, stress, settle sweep | LED_PWR_EN=P18, P4=SPI1_MOSI |

> **v0.3.2** adds a high-side switch for the WS2812 chain on **P18 = LED_PWR_EN**
> (`AK_LED_PWR_EN_PIN`). The power-on/off sequence is shared in
> `common/src/bringup_led_power.c` (DIN low -> P18 high -> 10 ms -> reset ->
> data; off: zero frame -> DIN low -> P18 low).
>
> - `led_power` always drives P18 (it is the switch test).
> - `ws2812` and `full_selftest` power the chain at boot when the build option
>   `BRINGUP_LED_PWR_AUTO` is 1 (**default**), so their LEDs light on v0.3.2 as
>   soon as they are flashed. Set it to 0 in
>   `common/include/bringup_led_power.h` (one place, or `-DBRINGUP_LED_PWR_AUTO=0`)
>   to leave P18 alone; on v0.3.2 the chain then stays dark in those two apps.
> - Default 1 is safe on lite / v0.3.1: P18 has no connection on those
>   expansion boards and the T5AI-Core board support does not use it.
> - At boot each of the three apps prints one UART0 line
>   `"led_pwr_option":"on"|"off"|"always"` so you can tell which image is flashed.
> - No other bring-up app drives the LED chain or P18.

## Build

```bash
source <TuyaOpen>/export.sh
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

Flash the built image (`dist/<version>/<app>_QIO_<version>.bin`) with tyutool (or `tos.py flash`), for example:

```powershell
tyutool_cli write -d t5ai -p COMx -b 460800 -f <app>_QIO_<version>.bin   # see tyutool_cli --help
```

Only connect one of the two Type-C UART bridges at a time. UART0 uses
115200 8N1; the separate Tuya debug-log port uses 460800 baud.

> On v0.3 the expansion board no longer has its own CH340C/Type-C — flash and
> monitor through the **dev-board Type-C** instead.

All eight applications build with the TuyaOpen T5AI SDK (each built with the
same `config choice` + `build` steps as `build_all.sh`). The complete
sequential physical hardware acceptance procedure is in `TEST_PLAN.md`.
