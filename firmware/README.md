# Firmware — T5AI-Core (TuyaOpen)

> The product firmware remains a skeleton. Standalone hardware validation apps
> live under `bringup/` and are developed before Phase 1.

This directory hosts the T5AI-Core firmware built with the native **TuyaOpen SDK + `tos.py`**,
compiled natively on Windows (Git Bash, no WSL required).

## Target

- MCU: T5-E1 module (Arm Cortex-M33F @ 480 MHz, Wi-Fi6 + BLE 5.4)
- Display: CL28CK230-18A LCD (ST7789 driver, 320×240, SPI)
- Input: WS-1002 5-way switch + 5× mechanical keys (CPG151101D06 brown switch)
- Audio: onboard mic / speaker

## Structure

```text
firmware/
├── bringup/        # standalone board-level hardware validation apps
│   ├── common/     # authoritative project IO definitions shared by bring-ups
│   ├── key1_uart/  # KEY1=P24 -> JSONL over UART0=P10/P11
│   ├── input_scan/ # five keys + five navigation directions
│   ├── lcd_st7789/ # project-specific 240x320 SPI display test
│   ├── ws2812/     # three LEDs on SPI1_MOSI=P4 (v1.2; official SPI driver)
│   ├── full_selftest/ # combined input/LCD/WS2812/UART/radio self-test
│   └── ...         # UART loopback and Wi-Fi/BLE self-test
├── app/            # planned TuyaOpen product application
├── src/            # planned input/state/transport/UI implementation
├── include/        # planned product headers
└── README.md       # this file
```

## Notes

- Code comments in English (see repo dev rules).
- Run `bringup/build_all.sh` for all firmware build/static checks; follow
  `bringup/TEST_PLAN.md` for sequential physical acceptance.
- Transport layer is pluggable: UART (Phase 1) → BLE GATT (Phase 2) → BLE HID (Phase 3).
- The wire protocol is shared with the companion app; see `../shared/protocol/protocol.md`.
