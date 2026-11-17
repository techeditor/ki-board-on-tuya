#!/usr/bin/env python3
"""Send a signed .akota package to a Ki-board over USB serial, and run the
debug board probes.

Same wire protocol as the Companion (shared/protocol/protocol.md,
"固件升级（仅 USB）"). Close the Companion first: only one program can own
the COM port. Needs pyserial (pip install pyserial).

  python ota_send.py COM6 ki-board_0.3.60_debug_test.akota
  python ota_send.py COM6 --probe-flash
  python ota_send.py COM6 --probe-baud 460800 --secs 20
"""

from __future__ import annotations

import argparse
import base64
import json
import sys
import time
import zlib

try:
    import serial  # type: ignore
except ImportError:  # pragma: no cover
    sys.exit("pip install pyserial")

HEADER_LEN = 256
CHUNK = 256
WINDOW = 4
BAUD = 115200


class Link:
    def __init__(self, port: str, baud: int = BAUD):
        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.buf = b""

    def send(self, typ: str, payload: dict | None = None) -> None:
        frame = {"v": 1, "type": typ}
        if payload is not None:
            frame["payload"] = payload
        line = json.dumps(frame, separators=(",", ":")).encode() + b"\n"
        if len(line) > 512:
            raise RuntimeError("frame over 511 bytes")
        self.ser.write(line)

    def frames(self, timeout: float):
        """Yield decoded frames until `timeout` seconds pass without one."""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.buf += self.ser.read(4096)
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                try:
                    frame = json.loads(line)
                except ValueError:
                    continue
                end = time.monotonic() + timeout
                yield frame

    def wait(self, types: set[str], timeout: float):
        for frame in self.frames(timeout):
            if frame.get("type") in types:
                return frame
            if frame.get("type") == "ping":
                self.send("pong")
        return None


def status(frame) -> str:
    p = frame.get("payload", {}) if frame else {}
    return f"{p.get('state')} {p.get('reason', '')}".strip()


def ota(link: Link, path: str) -> int:
    pkg = open(path, "rb").read()
    header, image = pkg[:HEADER_LEN], pkg[HEADER_LEN:]
    version = ".".join(str(int.from_bytes(header[36 + 2 * i:38 + 2 * i], "little")) for i in range(3))
    link.send("hello", {"client": "ota_send", "version": "0"})
    hello = link.wait({"ota_status"}, 3.0)
    if hello is None:
        print("no ota_status after hello: firmware without OTA, or the port is wrong")
        return 1
    print("device:", hello["payload"])
    link.send("ota_begin", {"h": base64.b64encode(header).decode()})
    st = link.wait({"ota_status"}, 5.0)
    print("->", status(st))
    if not st or st["payload"].get("state") != "confirm":
        return 1
    print(f"press K6 on the keyboard to install {version} (K1 cancels, 10 s)")
    t_confirm = time.monotonic()
    while True:
        st = link.wait({"ota_status"}, 60.0)
        print("->", status(st))
        if not st or st["payload"].get("state") in ("failed",):
            return 1
        if st["payload"].get("state") == "ready":
            break
    t_ready = time.monotonic()
    print(f"erase took {t_ready - t_confirm:.1f} s (includes your K6 press)")
    acked, sent = 0, 0
    while acked < len(image):
        while sent < len(image) and sent - acked < WINDOW * CHUNK:
            chunk = image[sent:sent + CHUNK]
            link.send("ota_data", {"o": sent, "d": base64.b64encode(chunk).decode()})
            sent += len(chunk)
        frame = link.wait({"ota_ack", "ota_status"}, 3.0)
        if frame is None:
            print(f"no ack; resending from {acked}")
            sent = acked
            continue
        if frame["type"] == "ota_status":
            print("->", status(frame))
            return 1
        nxt = frame["payload"]["next"]
        if nxt == acked and sent > nxt:
            sent = nxt  # a line was lost: the device ignores what follows it; go back
        acked = max(acked, nxt)
        print(f"\r{acked * 100 // len(image):3d}%  {acked}/{len(image)}", end="")
    t_sent = time.monotonic()
    print(f"\ntransfer {t_sent - t_ready:.1f} s ({len(image) / (t_sent - t_ready) / 1024:.1f} KB/s)")
    link.send("ota_end")
    st = link.wait({"ota_status"}, 30.0)
    print("->", status(st))
    if not st or st["payload"].get("state") != "installing":
        return 1
    t_install = time.monotonic()
    print("installing; waiting for the new firmware to answer hello ...")
    while time.monotonic() - t_install < 300:
        link.send("hello", {"client": "ota_send", "version": "0"})
        frame = link.wait({"hello_ack", "ota_status"}, 1.0)
        if frame and frame.get("payload", {}).get("fw") == version:
            print(f"new firmware {version} up {time.monotonic() - t_install:.1f} s after 'installing'")
            print("device:", frame["payload"])
            return 0
    print("no answer from the new version within 300 s (check the board, see the test doc)")
    return 1


def probe_flash(link: Link) -> int:
    link.send("ota_probe", {"op": "flash"})
    frame = link.wait({"probe_result"}, 10.0)
    print(frame["payload"] if frame else "no answer (debug firmware with AK_DEBUG_PROBES?)")
    return 0 if frame else 1


def probe_baud(port: str, link: Link, baud: int, secs: int) -> int:
    link.send("ota_probe", {"op": "baud", "baud": baud, "secs": secs})
    frame = link.wait({"probe_result"}, 3.0)
    if not frame:
        print("no answer (debug firmware with AK_DEBUG_PROBES?)")
        return 1
    link.ser.close()
    time.sleep(0.3)
    fast = Link(port, baud)
    sent = echoed = 0
    t0 = time.monotonic()
    while time.monotonic() - t0 < secs - 2:
        data = (b"%08x" % sent) * 24  # 192 hex chars (device buffer is 255)
        fast.send("probe_line", {"i": sent, "d": data.decode(), "c": zlib.crc32(data)})
        sent += 1
        for f in fast.frames(0.0):
            if f.get("type") == "probe_echo":
                echoed += 1
    for f in fast.frames(0.5):
        if f.get("type") == "probe_echo":
            echoed += 1
    elapsed = time.monotonic() - t0
    fast.ser.close()
    print(f"{baud}: sent {sent} lines, {echoed} echoed ({sent - echoed} lost), "
          f"{sent * 260 / elapsed / 1024:.1f} KB/s offered")
    back = Link(port, BAUD)
    frame = back.wait({"probe_result"}, secs + 5.0)
    print("device:", frame["payload"] if frame else "no summary at 115200")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("port")
    ap.add_argument("package", nargs="?")
    ap.add_argument("--probe-flash", action="store_true")
    ap.add_argument("--forget-highest", action="store_true",
                    help="debug firmware: reset the anti-downgrade record to the running version")
    ap.add_argument("--probe-baud", type=int)
    ap.add_argument("--secs", type=int, default=20)
    args = ap.parse_args()
    link = Link(args.port)
    if args.forget_highest:
        link.send("ota_probe", {"op": "forget"})
        frame = link.wait({"probe_result"}, 5.0)
        print(frame["payload"] if frame else "no answer (debug firmware with AK_DEBUG_PROBES?)")
        return 0 if frame else 1
    if args.probe_flash:
        return probe_flash(link)
    if args.probe_baud:
        return probe_baud(args.port, link, args.probe_baud, args.secs)
    if not args.package:
        ap.error("give a .akota package or a probe option")
    return ota(link, args.package)


if __name__ == "__main__":
    sys.exit(main())
