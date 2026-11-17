# ST7789 LCD bring-up (v0.3)

Tests the v0.3 12-pin LCD (2.8" IPS, ST7789P3) and its wiring.

- SPI0: SCK=P14, MOSI=P16, CS=P15, DC=P17, 48 MHz
- RST=P6, backlight=P8 (high = on, through the low-side transistor)
- TE: FPC pin 7 → R8 10 kΩ → P7. This app does not configure or drive P7 and
  does not use TE.
- Panel VDD is tied to 3V3; there is no LCD power switch. (On v0.2 hardware P7
  was the LCD power enable for the 18-pin panel.)
- Native 240×320, driven in landscape: 320×240, RGB565, display inversion on
  (INVON, which this panel needs).

The app cycles every two seconds through colour bars and full-screen red,
green, blue, white and black. Each frame carries orientation labels: `TOP`,
`BOTTOM`, `L`, `R`, and `ST7789P3 / 320x240 M=A0` in the centre. UART0
(115200) reports each pattern:

```json
{"v":1,"type":"bringup","payload":{"name":"lcd_st7789","status":"ready","width":320,"height":240,"spi_hz":48000000}}
{"v":1,"type":"bringup","payload":{"name":"lcd_st7789","pattern":"color_bars","result":0}}
{"v":1,"type":"bringup","payload":{"name":"lcd_st7789","pattern":"red","result":0}}
```

Pass:

- The whole panel is covered, with no clipped rows or columns.
- All labels read upright and unmirrored, each at the edge it names. If not,
  the MADCTL value in the init table needs to change.
- Red, green and blue appear as named (not swapped, not inverted).
- Colour bar edges are sharp, without noise.
- The backlight comes on reliably after reset.

Build and flash instructions are in the parent [`README.md`](../README.md).
