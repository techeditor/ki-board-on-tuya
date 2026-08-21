#!/usr/bin/env python3
"""Pin-conflict checker for the agentic keyboard expansion baseboard.

Reads pinmap.json and verifies:
  1. No pin is assigned to two nets in `assigned`.
  2. No `assigned` pin collides with a `board_occupied` pin.
  3. LCD SPI pins are all within board_occupied (they are reused, not re-driven).
  4. All pins have valid P0-P55 form.

Exit code 0 = OK, 1 = conflict found. Prints a summary either way.
"""
import json
import re
import sys
from pathlib import Path

PIN_RE = re.compile(r"^P([0-9]|[1-4][0-9]|5[0-5])$")  # P0..P55


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


def main() -> int:
    path = Path(__file__).parent / "pinmap.json"
    data = load(path)
    errors = check(data)

    n_assigned = len(data.get("assigned", []))
    n_occupied = len(data.get("board_occupied", []))
    print(f"pins: {n_occupied} board-occupied, {n_assigned} newly-assigned")

    if errors:
        print(f"CONFLICTS FOUND ({len(errors)}):")
        for e in errors:
            print(f"  - {e}")
        return 1

    print("OK: no pin conflicts.")
    for a in data.get("assigned", []):
        print(f"  {a['pin']:>4} -> {a['net']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
