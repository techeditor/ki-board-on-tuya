#!/usr/bin/env python3
"""Validate the versioned v0.3 hardware definition.

This checker reads pinmap-v0.3.json and the v0.3-suffixed documents only.
It intentionally does not read or modify the unsuffixed v0.2 baseline files.

Exit code 0 means the checked invariants are internally consistent.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
JSON_PATH = ROOT / "pinmap-v0.3.json"
REQUIRED_DOCS = [
    ROOT / "README-v0.3.md",
    ROOT / "pinmap-v0.3.md",
    ROOT / "netlist-v0.3.md",
    ROOT / "bom-v0.3.md",
    ROOT / "component-notes-v0.3.md",
]
PIN_RE = re.compile(r"^P(?:[0-9]|[1-4][0-9]|5[0-5])$")

EXPECTED_ASSIGNMENTS = {
    "P4": "WS_DIN_3V3",
    "P6": "LCD_RST",
    "P7": "LCD_TE",
    "P8": "LCD_BL",
    "P14": "LCD_SCK",
    "P15": "LCD_CS",
    "P16": "LCD_MOSI",
    "P17": "LCD_DC",
    "P20": "EC11_A",
    "P21": "EC11_B",
    "P22": "EC11_SW",
    "P23": "KEY5",
    "P24": "KEY1",
    "P25": "KEY2",
    "P26": "KEY3",
    "P27": "KEY4",
    "P32": "KEY6",
}

EXPECTED_LCD_PINS = {
    "1": "GND",
    "2": "LCD_RST",
    "3": "LCD_MOSI via R3 0R",
    "4": "LCD_SCK via R4 0R",
    "5": "LCD_CS",
    "6": "LCD_DC",
    "7": "LCD_TE via R8 10k",
    "8": "GND",
    "9": "VDD_3V3",
    "10": "VDD_3V3 / LEDA+",
    "11": "LEDK- via R5 10R and Q1",
    "12": "GND",
    "13": "GND / shell",
    "14": "GND / shell",
}


def load_json(path: Path) -> dict:
    with path.open(encoding="utf-8") as stream:
        return json.load(stream)


def expect(condition: bool, message: str, errors: list[str]) -> None:
    if not condition:
        errors.append(message)


def validate_json(data: dict) -> list[str]:
    errors: list[str] = []

    expect(data.get("project_version") == "v0.3", "project_version must be v0.3", errors)
    expect(data.get("hardware_version") == "v0.3.0", "hardware_version must be v0.3.0", errors)
    expect(
        data.get("expansion_board") == "Expansion Board_v0.3_lite",
        "unexpected expansion_board",
        errors,
    )

    assigned_entries = data.get("assigned", [])
    assigned: dict[str, str] = {}
    for entry in assigned_entries:
        pin = entry.get("pin", "")
        net = entry.get("net", "")
        expect(bool(PIN_RE.fullmatch(pin)), f"invalid assigned pin: {pin}", errors)
        if pin in assigned:
            errors.append(f"duplicate assigned pin: {pin} ({assigned[pin]}, {net})")
        assigned[pin] = net

    expect(assigned == EXPECTED_ASSIGNMENTS, "assigned pin map differs from v0.3 decision", errors)

    internal_entries = data.get("board_internal", [])
    internal = {entry.get("pin") for entry in internal_entries}
    for pin in internal:
        expect(bool(pin and PIN_RE.fullmatch(pin)), f"invalid board-internal pin: {pin}", errors)
    overlap = set(assigned) & internal
    expect(not overlap, f"assigned pins collide with board-internal pins: {sorted(overlap)}", errors)

    reserved_entries = data.get("reserved", [])
    reserved = {entry.get("pin") for entry in reserved_entries}
    for pin in reserved:
        expect(bool(pin and PIN_RE.fullmatch(pin)), f"invalid reserved pin: {pin}", errors)
    reserved_overlap = set(assigned) & reserved
    expect(not reserved_overlap, f"assigned pins collide with reserved pins: {sorted(reserved_overlap)}", errors)

    expect("P30" in reserved, "P30 must be free/reserved after K7 removal", errors)

    # Variant-only pins (v0.3.2) must be free on the base board, so one
    # firmware image can drive them on every board revision.
    for variant, entries in data.get("variant_assignments", {}).items():
        for entry in entries:
            pin = entry.get("pin", "")
            expect(bool(PIN_RE.fullmatch(pin)), f"invalid {variant} pin: {pin}", errors)
            expect(pin in reserved, f"{variant} pin {pin} must be free (reserved) on the base board", errors)
            expect(pin not in assigned and pin not in internal, f"{variant} pin {pin} collides", errors)
    v032 = {e.get("pin"): e.get("net") for e in data.get("variant_assignments", {}).get("v0.3.2", [])}
    expect(v032 == {"P18": "LED_PWR_EN"}, "v0.3.2 must assign exactly P18 = LED_PWR_EN", errors)
    board_io = (ROOT.parent.parent / "firmware/app/common/include/ak_board_io.h").read_text(encoding="utf-8")
    expect(
        re.search(r"#define\s+AK_PIN_LED_PWR_EN\s+TUYA_GPIO_NUM_18\b", board_io) is not None,
        "firmware AK_PIN_LED_PWR_EN must be TUYA_GPIO_NUM_18",
        errors,
    )
    bringup_io = (ROOT.parent.parent / "firmware/bringup/common/include/agentic_board_io.h").read_text(encoding="utf-8")
    expect(
        re.search(r"#define\s+AK_LED_PWR_EN_PIN\s+TUYA_GPIO_NUM_18\b", bringup_io) is not None,
        "bring-up AK_LED_PWR_EN_PIN must be TUYA_GPIO_NUM_18",
        errors,
    )
    expect("P28" in internal, "P28 ADC_BAT must remain board-internal", errors)
    expect("P38" in internal, "P38 CHG_DET_N must remain board-internal", errors)

    keys = data.get("keys", {})
    expect(keys.get("count") == 6, "key count must be 6", errors)
    expect(keys.get("nets") == [f"KEY{i}" for i in range(1, 7)], "key nets must be KEY1..KEY6", errors)
    expect(keys.get("socket", {}).get("lcsc") == "C2803348", "key socket must be C2803348", errors)
    expect(keys.get("socket", {}).get("quantity") == 6, "six key sockets required", errors)

    encoder = data.get("encoder", {})
    expect(encoder.get("lcsc") == "C9900001788", "encoder must be C9900001788", errors)
    expect(encoder.get("pins", {}).get("2_C") == "GND", "EC11 C must connect to GND", errors)
    expect(encoder.get("pins", {}).get("4_E") == "GND", "EC11 E must connect to GND", errors)

    lcd = data.get("lcd", {})
    expect(lcd.get("model") == "T280C7-C12-26", "unexpected LCD model", errors)
    expect(lcd.get("driver") == "ST7789P3", "LCD driver must be ST7789P3", errors)
    expect(lcd.get("fpc", {}).get("lcsc") == "C2856799", "FPC must be C2856799", errors)
    expect(lcd.get("fpc", {}).get("contact") == "bottom", "FPC contact must be bottom", errors)
    expect(lcd.get("pins") == EXPECTED_LCD_PINS, "LCD/FPC pin map differs from reviewed mapping", errors)

    leds = data.get("rgb_leds", {})
    expect(leds.get("count") == 7, "RGB LED count must be 7", errors)
    expect(leds.get("lcsc") == "C2890364", "RGB LED must be C2890364", errors)
    expect(
        leds.get("chain") == [f"LED{i}" for i in range(1, 8)],
        "RGB chain must be LED1..LED7",
        errors,
    )
    expect(leds.get("roles", {}).get("LED7") == "EC11", "LED7 must serve EC11", errors)
    expect(leds.get("direct_drive_bypass", {}).get("assembly") == "DNP", "R2 bypass must be DNP", errors)
    expect(leds.get("level_shifter", {}).get("lcsc") == "C100024", "U2 must be C100024", errors)

    power = data.get("power", {})
    expect(power.get("on_expansion_usb") is False, "expansion USB must be absent", errors)
    expect(power.get("on_expansion_battery") is False, "expansion battery must be absent", errors)
    expect(power.get("on_expansion_charger") is False, "expansion charger must be absent", errors)

    review = data.get("pcb_review", {})
    expected_review = {
        "component_count": 54,
        "net_count": 36,
        "trace_count": 292,
        "via_count": 147,
        "ground_pours": 2,
        "drc_errors": 0,
    }
    for key, value in expected_review.items():
        expect(review.get(key) == value, f"pcb_review.{key} must be {value}", errors)

    return errors


def validate_documents(data: dict) -> list[str]:
    errors: list[str] = []
    for path in REQUIRED_DOCS:
        expect(path.exists(), f"missing document: {path.name}", errors)

    if errors:
        return errors

    texts = {path.name: path.read_text(encoding="utf-8") for path in REQUIRED_DOCS}
    combined = "\n".join(texts.values())

    required_tokens = [
        "Expansion Board_v0.3_lite",
        "C2803348",
        "C9900001788",
        "C2856799",
        "T280C7-C12-26",
        "C2890364",
        "C6119971",
        "SN74LV1T34",
        "LED7",
        "EC11",
    ]
    for token in required_tokens:
        expect(token in combined, f"v0.3 documents do not mention required token: {token}", errors)

    for name, text in texts.items():
        expect("KEY7" not in text, f"obsolete KEY7 found in {name}", errors)

    expect("R2" in texts["bom-v0.3.md"] and "DNP" in texts["bom-v0.3.md"], "BOM must mark R2 DNP", errors)
    expect("54" in texts["README-v0.3.md"], "README must record 54 PCB components", errors)
    expect("36" in texts["README-v0.3.md"], "README must record 36 nets", errors)
    expect(
        "EasyEDA严格DRC返回0条错误" in texts["README-v0.3.md"],
        "README must state the exact EasyEDA DRC result",
        errors,
    )

    return errors


def main() -> int:
    errors: list[str] = []
    if not JSON_PATH.exists():
        print(f"ERROR: missing {JSON_PATH.name}")
        return 1

    try:
        data = load_json(JSON_PATH)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"ERROR: cannot load {JSON_PATH.name}: {exc}")
        return 1

    errors.extend(validate_json(data))
    errors.extend(validate_documents(data))

    print(f"version: {data.get('project_version')} / {data.get('hardware_version')}")
    print(f"board: {data.get('expansion_board')}")
    print(f"assigned GPIOs: {len(data.get('assigned', []))}")
    print(f"keys: {data.get('keys', {}).get('count')}")
    print(f"RGB LEDs: {data.get('rgb_leds', {}).get('count')}")
    print(
        "PCB review: "
        f"{data.get('pcb_review', {}).get('component_count')} components, "
        f"{data.get('pcb_review', {}).get('net_count')} nets, "
        f"DRC {data.get('pcb_review', {}).get('drc_errors')}"
    )

    if errors:
        print(f"FAIL: {len(errors)} issue(s)")
        for error in errors:
            print(f"  - {error}")
        return 1

    print("OK: v0.3 pin map and versioned documents are internally consistent.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
