#!/usr/bin/env python3
"""Static boot-order guard for the product firmware (run by run_host_tests.sh).

Since TuyaOpen's tal_kv moved to littlefs, any KV access before tal_kv_init
MemFaults on hardware (0.3.48 boot loop). This check fails when:
  1. any file under firmware/app other than common/src/ak_kv.c calls tal_kv_*;
  2. user_main() does not call ak_kv_init() before its first ak_kv_* access
     and before ak_cloud_init().
"""

import re
import sys
from pathlib import Path

APP = Path(__file__).resolve().parents[1]
ALLOWED = {APP / "common" / "src" / "ak_kv.c"}
TAL_KV = re.compile(r"\btal_kv_(init|get|set|del|free|serialize_set|serialize_get)\s*\(")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def user_main_body(text: str) -> str:
    start = re.search(r"\bstatic\s+void\s+user_main\s*\(\s*void\s*\)\s*\{", text)
    if not start:
        raise SystemExit("check_boot_order: user_main() not found")
    depth, i = 1, start.end()
    while depth and i < len(text):
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        i += 1
    return text[start.end():i]


def main() -> int:
    errors = []
    for path in sorted((APP / "src").rglob("*.c")) + sorted((APP / "common").rglob("*.[ch]")):
        if path in ALLOWED:
            continue
        for lineno, line in enumerate(strip_comments(path.read_text(encoding="utf-8")).splitlines(), 1):
            if TAL_KV.search(line):
                errors.append(f"{path.relative_to(APP)}:{lineno}: call ak_kv_* instead of tal_kv_*")

    body = strip_comments(user_main_body((APP / "src" / "tuya_app_main.c").read_text(encoding="utf-8")))
    init = body.find("ak_kv_init(")
    if init < 0:
        errors.append("user_main: ak_kv_init() is never called")
    else:
        for name in ("ak_kv_get(", "ak_kv_set(", "ak_kv_del(", "ak_cloud_init("):
            pos = body.find(name)
            if 0 <= pos < init:
                errors.append(f"user_main: {name[:-1]} runs before ak_kv_init()")

    if errors:
        print("check_boot_order: FAIL")
        for e in errors:
            print("  " + e)
        return 1
    print("check_boot_order: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
