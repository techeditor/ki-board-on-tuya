# WS2812 bring-up

Validates the three daisy-chained WS2812 LEDs:

```text
P13 -> SN74LV1T34 -> R3 -> LED1 -> LED2 -> LED3
```

P13 is not a T5 SPI MOSI pin, so TuyaOpen's SPI-based WS2812 driver cannot be
used with this PCB. This bring-up uses a T5-specific GPIO register writer and
DWT cycle counter. It holds a CPU1 power-management vote at 120 MHz because the
platform otherwise lowers the shared CPU clock to 60 MHz after boot. The fixed
clock is used to reproduce TuyaOpen's official 6.5 MHz SPI encoding timings:
about 1.23 us per bit, 0.31 us T0H, and 0.62 us T1H. P13 high/low edges use the
same direct register values (`2`/`0`) as the T5AI SDK's `GPIO_UP` and
`GPIO_DOWN` macros.

The sequence is:

1. LED1 red
2. LED2 green
3. LED3 blue
4. all three dim white
5. all off

Both the Tuya debug log and UART0 report each step. Startup output includes the
project version, build timestamp, driver revision, and SDK/platform commits.
UART0 runs at 115200 baud. Brightness is deliberately limited.
If colors flicker or do not latch, verify P13 timing with a logic analyzer
before changing the PCB or driver.
