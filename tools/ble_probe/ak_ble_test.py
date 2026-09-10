#!/usr/bin/env python3
"""
ak_ble_test.py — PC-side BLE GATT client for the Agentic Keyboard (FP-2 ble-gatt-link).

Rehearses what the companion's BLE backend will do, using your computer's own
Bluetooth adapter (no extra hardware). It:
  1. scans for the board advertising as "AK-KB",
  2. connects and finds Tuya's fixed command service (0x1910),
  3. subscribes to the NOTIFY characteristic (0x2B10) and prints every JSONL
     frame the board pushes (key/nav/ping/link/hello_ack),
  4. lets you type lines that get written to the WRITE characteristic (0x2B11)
     — on the board these arrive as TAL_BLE_EVT_WRITE_REQ and show up on its
     UART/LCD as `link_rx` and an incremented RX counter.

UUIDs are Tuya's, defined in the SDK (src/tal_bluetooth/include/tal_bluetooth_def.h):
    service 0x1910, notify 0x2B10, write 0x2B11.

Usage (on your Windows/macOS/Linux machine, NOT the EC2 build host):
    pip install bleak
    python ak_ble_test.py                 # scan for AK-KB, connect, interactive
    python ak_ble_test.py --name AK-KB     # explicit advertised name
    python ak_ble_test.py --address <MAC>  # skip scan, connect directly
    python ak_ble_test.py --send "hello"   # connect, send one line, keep listening

Type text at the "write> " prompt to send it to 0x2B11. Ctrl+C to quit.
"""

import argparse
import asyncio
import sys
from datetime import datetime

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    sys.exit("bleak is not installed. Run:  pip install bleak")

# Tuya fixed command service (16-bit UUIDs expanded to the SIG base 128-bit form).
SERVICE_UUID = "00001910-0000-1000-8000-00805f9b34fb"
NOTIFY_UUID = "00002b10-0000-1000-8000-00805f9b34fb"
WRITE_UUID = "00002b11-0000-1000-8000-00805f9b34fb"

DEFAULT_NAME = "AK-KB"


def _ts() -> str:
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]


def _short(uuid: str) -> str:
    """Return the 16-bit part of a 128-bit UUID, else the uuid itself."""
    u = str(uuid).lower()
    if u.endswith("-0000-1000-8000-00805f9b34fb") and u.startswith("0000"):
        return u[4:8]
    return u


def _find_char(client, want_short):
    """Locate a characteristic by its 16-bit id (robust to 128-bit reporting)."""
    for service in client.services:
        for ch in service.characteristics:
            if _short(ch.uuid) == want_short:
                return ch
    return None


def _on_notify(_sender, data: bytearray):
    # Board sends newline-terminated JSONL; decode leniently and print each line.
    text = data.decode("utf-8", errors="replace").rstrip("\r\n")
    for line in text.split("\n"):
        if line:
            print(f"[{_ts()}] NOTIFY  {line}")
    print("write> ", end="", flush=True)


async def _scan_for(name: str, timeout: float):
    print(f"[{_ts()}] scanning for '{name}' ({timeout:.0f}s)...")
    device = await BleakScanner.find_device_by_filter(
        lambda d, adv: (adv.local_name or d.name or "") == name,
        timeout=timeout,
    )
    return device


async def _writer_loop(client, write_char):
    """Read stdin lines (off the event loop) and write them to 0x2B11."""
    loop = asyncio.get_event_loop()
    print("write> ", end="", flush=True)
    while client.is_connected:
        line = await loop.run_in_executor(None, sys.stdin.readline)
        if line == "":  # EOF (Ctrl+D)
            break
        payload = line.rstrip("\r\n")
        if not payload:
            print("write> ", end="", flush=True)
            continue
        try:
            await client.write_gatt_char(write_char, payload.encode("utf-8"), response=True)
            print(f"[{_ts()}] WROTE   {len(payload)} bytes -> 0x2B11: {payload!r}")
        except Exception as exc:  # noqa: BLE001
            print(f"[{_ts()}] write failed: {exc}")
        print("write> ", end="", flush=True)


async def run(args):
    address = args.address
    if not address:
        device = await _scan_for(args.name, args.timeout)
        if device is None:
            sys.exit(f"device '{args.name}' not found. Is the board powered and advertising?")
        print(f"[{_ts()}] found {device.name} @ {device.address}")
        address = device.address

    print(f"[{_ts()}] connecting to {address}...")
    async with BleakClient(address) as client:
        print(f"[{_ts()}] connected. MTU={getattr(client, 'mtu_size', '?')}")

        notify_char = _find_char(client, "2b10")
        write_char = _find_char(client, "2b11")
        if notify_char is None or write_char is None:
            print("could not find Tuya command chars. Services present:")
            for service in client.services:
                print(f"  service {_short(service.uuid)}")
                for ch in service.characteristics:
                    print(f"    char {_short(ch.uuid)}  props={','.join(ch.properties)}")
            sys.exit("expected notify 0x2B10 + write 0x2B11 — not found.")

        await client.start_notify(notify_char, _on_notify)
        print(f"[{_ts()}] subscribed to 0x2B10 — press board keys to see frames.")
        print(f"[{_ts()}] type text + Enter to write to 0x2B11. Ctrl+C to quit.\n")

        if args.send is not None:
            await client.write_gatt_char(write_char, args.send.encode("utf-8"), response=True)
            print(f"[{_ts()}] WROTE   {len(args.send)} bytes -> 0x2B11: {args.send!r}")

        try:
            await _writer_loop(client, write_char)
        finally:
            try:
                await client.stop_notify(notify_char)
            except Exception:  # noqa: BLE001
                pass
    print(f"\n[{_ts()}] disconnected.")


def main():
    parser = argparse.ArgumentParser(description="BLE GATT test client for AK-KB (FP-2).")
    parser.add_argument("--name", default=DEFAULT_NAME, help="advertised name to scan for (default: AK-KB)")
    parser.add_argument("--address", help="connect directly to this address/MAC, skip scanning")
    parser.add_argument("--timeout", type=float, default=10.0, help="scan timeout seconds (default: 10)")
    parser.add_argument("--send", help="write this one line to 0x2B11 right after connecting, then keep listening")
    args = parser.parse_args()

    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        print(f"\n[{_ts()}] interrupted, bye.")


if __name__ == "__main__":
    main()
