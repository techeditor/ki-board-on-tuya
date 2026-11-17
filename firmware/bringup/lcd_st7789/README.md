# ST7789 LCD bring-up (v0.3)

Validates the v0.3 12P ST7789P3 panel wiring:

- SPI0 SCK=P14, MOSI=P16, CS=P15, DC=P17
- RST=P6, backlight=P8, **TE=P7 (input)**
- Native resolution 240x320, RGB565, 48 MHz

> v0.3 replaced the 18P CL28CK230 panel with a 12P ST7789P3. Panel VDD is now
> tied directly to 3V3, so the old **LCD_PWR (P7)** power switch is gone; **P7 is
> reused as TE (tearing-effect) input**. This bring-up configures P7 as an input
> and does not drive it. The SPI/RST/BL pins are unchanged.
>
> Doc note: the schematic review (2026-10-01) says TE reaches P7 via R8=10k as a
> GPIO input, while the change-scope §5 pin table lists TE as NC/floating. Either
> way the firmware never drives P7; the TE line is not used by this tester.

The screen cycles through RGB565 color bars and full-screen red, green, blue,
white, and black patterns. UART0 reports the current pattern at 115200 baud.

Verify:

- The entire panel is addressed with no clipped rows or columns.
- Red, green, and blue are not swapped.
- Color bars have sharp boundaries without noise.
- The backlight turns on reliably after reset.
