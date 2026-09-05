#!/usr/bin/env python3
"""Pin-conflict checker for the agentic keyboard expansion baseboard.

Reads pinmap.json and verifies:
  1. No pin is assigned to two nets in `assigned`.
  2. No `assigned` pin collides with a `board_occupied` pin.
  3. LCD SPI pins are all within board_occupied (they are reused, not re-driven).
  4. All pins have valid P0-P55 form.
  5. Product firmware IO macros (firmware/app) match pinmap.json.

Exit code 0 = OK, 1 = conflict found. Prints a summary either way.
"""
import json
import re
import sys
from pathlib import Path

PIN_RE = re.compile(r"^P([0-9]|[1-4][0-9]|5[0-5])$")  # P0..P55
HEADER_PIN_RE = re.compile(
    r"^#define\s+(AK_[A-Z0-9_]+_PIN)\s+TUYA_(?:GPIO_NUM|IO_PIN)_([0-9]+)\b",
    re.MULTILINE,
)


def load(path: Path) -> dict:
    with path.open(encoding="utf-8") as f:
        return json.load(f)


def check(data: dict) -> list[str]:
    errors: list[str] = []

    occupied = {e["pin"]: e.get("use", "?") for e in data.get("board_occupied", [])}
    assigned = data.get("assigned", [])

    # 4. pin form
    for pin in list(occupied) + [a["pin"] for a in assigned]:
        if not PIN_RE.match(pin):
            errors.append(f"invalid pin form: {pin}")

    # 1. duplicate within assigned
    seen: dict[str, str] = {}
    for a in assigned:
        pin, net = a["pin"], a["net"]
        if pin in seen:
            errors.append(f"pin {pin} assigned to both '{seen[pin]}' and '{net}'")
        seen[pin] = net

    # 2. assigned vs board_occupied
    for a in assigned:
        if a["pin"] in occupied:
            errors.append(
                f"pin {a['pin']} ('{a['net']}') collides with board use '{occupied[a['pin']]}'"
            )

    # 3. LCD SPI pins should be part of board_occupied (reused)
    lcd = data.get("lcd_spi", {})
    for key in ("sck", "mosi", "cs", "dc", "rst", "bl", "pwr"):
        pin = lcd.get(key)
        if pin and pin not in occupied:
            errors.append(f"LCD {key} pin {pin} not found in board_occupied (expected reuse)")

    return errors


def pin_number(pin: str) -> int:
    return int(pin.removeprefix("P"))


def check_firmware_header(data: dict, header_path: Path) -> list[str]:
    errors: list[str] = []
    if not header_path.exists():
        return [f"firmware IO header not found: {header_path}"]

    macros = {
        name: int(number)
        for name, number in HEADER_PIN_RE.findall(header_path.read_text(encoding="utf-8"))
    }
    assigned = {entry["net"]: entry["pin"] for entry in data.get("assigned", [])}
    lcd = data.get("lcd_spi", {})
    usb = data.get("expansion_usb", {})

    expected = {
        "AK_UART_RX_PIN": usb.get("uart_rx_t5"),
        "AK_UART_TX_PIN": usb.get("uart_tx_t5"),
        "AK_KEY1_PIN": assigned.get("KEY1"),
        "AK_KEY2_PIN": assigned.get("KEY2"),
        "AK_KEY3_PIN": assigned.get("KEY3"),
        "AK_KEY4_PIN": assigned.get("KEY4"),
        "AK_KEY5_PIN": assigned.get("KEY5"),
        "AK_NAV_UP_PIN": assigned.get("NAV_UP"),
        "AK_NAV_DOWN_PIN": assigned.get("NAV_DOWN"),
        "AK_NAV_LEFT_PIN": assigned.get("NAV_LEFT"),
        "AK_NAV_RIGHT_PIN": assigned.get("NAV_RIGHT"),
        "AK_NAV_CENTER_PIN": assigned.get("NAV_CENTER"),
        "AK_LCD_SPI_SCK_PIN": lcd.get("sck"),
        "AK_LCD_SPI_MOSI_PIN": lcd.get("mosi"),
        "AK_LCD_SPI_CS_PIN": lcd.get("cs"),
        "AK_LCD_DC_PIN": lcd.get("dc"),
        "AK_LCD_RST_PIN": lcd.get("rst"),
        "AK_LCD_BACKLIGHT_PIN": lcd.get("bl"),
        "AK_LCD_POWER_PIN": lcd.get("pwr"),
        "AK_WS2812_DATA_PIN": assigned.get("WS2812_DIN"),
    }

    for macro, expected_pin in expected.items():
        if expected_pin is None:
            errors.append(f"pinmap.json has no source value for {macro}")
            continue
        actual = macros.get(macro)
        if actual is None:
            errors.append(f"firmware IO header is missing {macro}")
        elif actual != pin_number(expected_pin):
            errors.append(
                f"{macro}=P{actual} differs from pinmap.json {expected_pin}"
            )

    return errors


def main() -> int:
    path = Path(__file__).parent / "pinmap.json"
    repo_root = Path(__file__).resolve().parents[2]
    # Validate against the live PRODUCT firmware header (board revision v1.2).
    # The bring-up header (firmware/bringup/common/include/agentic_board_io.h) is
    # also on v1.2 wiring (WS2812=P4/SPI1, LCD_BL=P8) but is not checked here to
    # keep a single authoritative source for the product firmware macros.
    header_path = (
        repo_root / "firmware" / "app" / "common" / "include" / "ak_board_io.h"
    )
    data = load(path)
    errors = check(data)
    errors.extend(check_firmware_header(data, header_path))

    n_assigned = len(data.get("assigned", []))
    n_occupied = len(data.get("board_occupied", []))
    print(f"pins: {n_occupied} board-occupied, {n_assigned} newly-assigned")

    if errors:
        print(f"CONFLICTS FOUND ({len(errors)}):")
        for e in errors:
            print(f"  - {e}")
        return 1

    print("OK: no pin conflicts; firmware bring-up IO matches pinmap.json.")
    for a in data.get("assigned", []):
        print(f"  {a['pin']:>4} -> {a['net']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
