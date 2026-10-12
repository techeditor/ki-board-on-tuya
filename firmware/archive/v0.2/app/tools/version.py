#!/usr/bin/env python3
"""Manage the Agentic Keyboard firmware semantic version.

VERSION is the source of truth. Every set/bump operation synchronizes all
TuyaOpen build profiles because tos.py uses CONFIG_PROJECT_VERSION when naming
and collecting firmware artifacts.
"""

from __future__ import annotations

import argparse
import re
import subprocess
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1]
VERSION_FILE = APP_DIR / "VERSION"
PROFILE_FILES = [APP_DIR / "app_default.config", *sorted((APP_DIR / "config").glob("*.config"))]
REPO_ROOT = APP_DIR.parents[1]
MANAGED_FILES = [VERSION_FILE, *PROFILE_FILES]
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


def repo_relative(path: Path) -> str:
    return path.relative_to(REPO_ROOT).as_posix()


def run_git(*args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        ["git", *args], cwd=REPO_ROOT, text=True, capture_output=True, check=False
    )
    if check and result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise ValueError(f"git {' '.join(args)} failed: {detail}")
    return result


def head_file(path: Path) -> str | None:
    result = run_git("show", f"HEAD:{repo_relative(path)}", check=False)
    return result.stdout if result.returncode == 0 else None


def staged_firmware_changes() -> list[str]:
    result = run_git(
        "diff", "--cached", "--name-only", "--diff-filter=ACMR", "--", "firmware/app"
    )
    managed = {repo_relative(path) for path in MANAGED_FILES}
    return [line for line in result.stdout.splitlines() if line and line not in managed]


def ensure_managed_files_are_version_only(version: str) -> None:
    """Refuse to stage a profile containing unrelated concurrent edits."""
    for path in MANAGED_FILES:
        committed = head_file(path)
        if committed is None:
            continue
        if path == VERSION_FILE:
            expected = version + "\n"
        else:
            expected, count = CONFIG_RE.subn(
                f'CONFIG_PROJECT_VERSION="{version}"', committed
            )
            if count != 1:
                raise ValueError(
                    f"{path}: committed profile has {count} project-version entries"
                )
        if path.read_text(encoding="utf-8") != expected:
            raise ValueError(
                f"{path}: contains changes beyond the firmware version; "
                "refusing to auto-stage concurrent edits"
            )


def auto_bump_staged(dry_run: bool = False) -> str:
    changes = staged_firmware_changes()
    current = check_version()
    if not changes:
        print("no staged firmware implementation changes; version unchanged")
        return current

    committed_text = head_file(VERSION_FILE)
    if committed_text is None:
        target = current
        action = "initial version"
    else:
        committed = committed_text.strip()
        committed_parts = validate(committed)
        current_parts = validate(current)
        if current_parts == committed_parts:
            target = bumped(current, "patch")
            action = "automatic patch bump"
        elif current_parts > committed_parts:
            target = current
            action = "existing manual bump"
        else:
            raise ValueError(
                f"working version {current} is not newer than committed version {committed}"
            )
        ensure_managed_files_are_version_only(current)

    if dry_run:
        print(f"{action}: {current} -> {target}; staged firmware changes: {len(changes)}")
        return target

    if target != current:
        set_version(target)
    check_version()
    run_git("add", "--", *(repo_relative(path) for path in MANAGED_FILES))
    print(f"{action}: {current} -> {target}; synchronized and staged version files")
    return target


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("show", help="print the current version")
    subparsers.add_parser("check", help="verify VERSION and all build profiles agree")

    set_parser = subparsers.add_parser("set", help="manually set MAJOR.MINOR.PATCH")
    set_parser.add_argument("version")

    bump_parser = subparsers.add_parser("bump", help="automatically increment a semantic version part")
    bump_parser.add_argument("part", choices=("major", "minor", "patch"), nargs="?", default="patch")

    auto_parser = subparsers.add_parser(
        "auto-bump-staged",
        help="bump patch when staged firmware implementation changes are committed",
    )
    auto_parser.add_argument(
        "--dry-run", action="store_true", help="report the decision without changing files or index"
    )

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
        elif args.command == "bump":
            next_version = bumped(current, args.part)
            set_version(next_version)
            print(f"{current} -> {next_version}")
        else:
            auto_bump_staged(args.dry_run)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
