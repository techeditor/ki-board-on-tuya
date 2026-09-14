#!/usr/bin/env python3
"""Manage the Agentic Keyboard firmware semantic version.

VERSION is the source of truth. Every set/bump operation synchronizes all
TuyaOpen build profiles because tos.py uses CONFIG_PROJECT_VERSION when naming
and collecting firmware artifacts.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1]
VERSION_FILE = APP_DIR / "VERSION"
PROFILE_FILES = [APP_DIR / "app_default.config", *sorted((APP_DIR / "config").glob("*.config"))]
SEMVER_RE = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$")
CONFIG_RE = re.compile(r'^CONFIG_PROJECT_VERSION="[^"]*"$', re.MULTILINE)


def validate(value: str) -> tuple[int, int, int]:
    match = SEMVER_RE.fullmatch(value)
    if match is None:
        raise ValueError(f"invalid version {value!r}; expected MAJOR.MINOR.PATCH")
    return tuple(int(part) for part in match.groups())


def read_version() -> str:
    value = VERSION_FILE.read_text(encoding="utf-8").strip()
    validate(value)
    return value


def rendered_profiles(version: str) -> dict[Path, str]:
    rendered: dict[Path, str] = {}
    replacement = f'CONFIG_PROJECT_VERSION="{version}"'
    for path in PROFILE_FILES:
        content = path.read_text(encoding="utf-8")
        updated, count = CONFIG_RE.subn(replacement, content)
        if count != 1:
            raise ValueError(f"{path}: expected exactly one CONFIG_PROJECT_VERSION, found {count}")
        rendered[path] = updated
    return rendered


def set_version(version: str) -> None:
    validate(version)
    profiles = rendered_profiles(version)
    VERSION_FILE.write_text(version + "\n", encoding="utf-8")
    for path, content in profiles.items():
        path.write_text(content, encoding="utf-8")


def check_version() -> str:
    version = read_version()
    expected = f'CONFIG_PROJECT_VERSION="{version}"'
    errors = []
    for path in PROFILE_FILES:
        matches = CONFIG_RE.findall(path.read_text(encoding="utf-8"))
        if matches != [expected]:
            errors.append(f"{path}: expected {expected}, found {matches}")
    if errors:
        raise ValueError("version mismatch:\n  " + "\n  ".join(errors))
    return version


def bumped(version: str, part: str) -> str:
    major, minor, patch = validate(version)
    if part == "major":
        major, minor, patch = major + 1, 0, 0
    elif part == "minor":
        minor, patch = minor + 1, 0
    else:
        patch += 1
    return f"{major}.{minor}.{patch}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("show", help="print the current version")
    subparsers.add_parser("check", help="verify VERSION and all build profiles agree")

    set_parser = subparsers.add_parser("set", help="manually set MAJOR.MINOR.PATCH")
    set_parser.add_argument("version")

    bump_parser = subparsers.add_parser("bump", help="automatically increment a semantic version part")
    bump_parser.add_argument("part", choices=("major", "minor", "patch"), nargs="?", default="patch")

    args = parser.parse_args()
    try:
        current = read_version()
        if args.command == "show":
            print(current)
        elif args.command == "check":
            print(check_version())
        elif args.command == "set":
            set_version(args.version)
            print(f"{current} -> {args.version}")
        else:
            next_version = bumped(current, args.part)
            set_version(next_version)
            print(f"{current} -> {next_version}")
    except (OSError, ValueError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
