# ST7789 LCD bring-up

Validates the project-specific CL28CK230-18A wiring:

- SPI0 SCK=P14, MOSI=P16, CS=P15, DC=P17
- RST=P6, backlight=P8 (v1.2, was P5), power=P7
- Native resolution 240x320, RGB565, 48 MHz

The screen cycles through RGB565 color bars and full-screen red, green, blue,
white, and black patterns. UART0 reports the current pattern at 115200 baud.

Verify:

- The entire panel is addressed with no clipped rows or columns.
- Red, green, and blue are not swapped.
- Color bars have sharp boundaries without noise.
- Backlight and panel power turn on reliably after reset.
