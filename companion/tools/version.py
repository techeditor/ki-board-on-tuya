#!/usr/bin/env python3
"""Manage the Agentic Keyboard Companion semantic version.

companion/VERSION is the source of truth. Every set/bump operation synchronizes
the three build-manifest version fields that would otherwise drift:
  - companion/src-tauri/tauri.conf.json   ("version": "X.Y.Z")
  - companion/src-tauri/Cargo.toml        (package version = "X.Y.Z")
  - companion/package.json                ("version": "X.Y.Z")

Mirrors firmware/app/tools/version.py in structure and CLI verbs so the two
version schemes behave identically.
"""

from __future__ import annotations

import argparse
import re
import subprocess
from pathlib import Path

COMPANION_DIR = Path(__file__).resolve().parents[1]
VERSION_FILE = COMPANION_DIR / "VERSION"
TAURI_CONF = COMPANION_DIR / "src-tauri" / "tauri.conf.json"
CARGO_TOML = COMPANION_DIR / "src-tauri" / "Cargo.toml"
PACKAGE_JSON = COMPANION_DIR / "package.json"
MANIFEST_FILES = [TAURI_CONF, CARGO_TOML, PACKAGE_JSON]
MANAGED_FILES = [VERSION_FILE, *MANIFEST_FILES]
REPO_ROOT = COMPANION_DIR.parent

SEMVER_RE = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$")
# JSON manifests: first top-level `"version": "X.Y.Z"` (2-space indent).
JSON_VERSION_RE = re.compile(r'^(?P<pre>  "version": ")[^"]*(?P<post>",?)$', re.MULTILINE)
# Cargo package version: a line-anchored `version = "X.Y.Z"` (dependency
# versions are inline tables like `foo = { version = "..." }` or indented, so a
# bare line-start `version = ` only ever matches the [package] field).
CARGO_VERSION_RE = re.compile(r'^(?P<pre>version = ")[^"]*(?P<post>")$', re.MULTILINE)

# Which regex applies to each manifest.
MANIFEST_RE = {
    TAURI_CONF: JSON_VERSION_RE,
    CARGO_TOML: CARGO_VERSION_RE,
    PACKAGE_JSON: JSON_VERSION_RE,
}


def validate(value: str) -> tuple[int, int, int]:
    match = SEMVER_RE.fullmatch(value)
    if match is None:
        raise ValueError(f"invalid version {value!r}; expected MAJOR.MINOR.PATCH")
    return tuple(int(part) for part in match.groups())


def read_version() -> str:
    value = VERSION_FILE.read_text(encoding="utf-8").strip()
    validate(value)
    return value


def _rendered_manifest(path: Path, version: str) -> str:
    pattern = MANIFEST_RE[path]
    content = path.read_text(encoding="utf-8")
    updated, count = pattern.subn(rf'\g<pre>{version}\g<post>', content)
    if count != 1:
        raise ValueError(f"{path}: expected exactly one version field, found {count}")
    return updated


def rendered_manifests(version: str) -> dict[Path, str]:
    return {path: _rendered_manifest(path, version) for path in MANIFEST_FILES}


def set_version(version: str) -> None:
    validate(version)
    manifests = rendered_manifests(version)  # raises before any write if malformed
    VERSION_FILE.write_text(version + "\n", encoding="utf-8")
    for path, content in manifests.items():
        path.write_text(content, encoding="utf-8")


def _manifest_version(path: Path) -> list[str]:
    pattern = MANIFEST_RE[path]
    return [m.group(0) for m in pattern.finditer(path.read_text(encoding="utf-8"))]


def check_version() -> str:
    version = read_version()
    errors = []
    for path in MANIFEST_FILES:
        found = _manifest_version(path)
        expected = _rendered_manifest(path, version)
        # Compare the single matched line, robust to surrounding content.
        exp_line = MANIFEST_RE[path].search(expected)
        if len(found) != 1 or (exp_line and found[0] != exp_line.group(0)):
            errors.append(f"{path}: expected version {version}, found {found}")
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


def staged_companion_changes() -> list[str]:
    """Staged companion IMPLEMENTATION changes (src / src-tauri/src), excluding
    the managed version files themselves."""
    result = run_git(
        "diff", "--cached", "--name-only", "--diff-filter=ACMR",
        "--", "companion/src", "companion/src-tauri/src",
    )
    managed = {repo_relative(path) for path in MANAGED_FILES}
    return [line for line in result.stdout.splitlines() if line and line not in managed]


def ensure_managed_files_are_version_only(version: str) -> None:
    """Refuse to auto-stage a manifest that carries unrelated concurrent edits."""
    for path in MANAGED_FILES:
        committed = head_file(path)
        if committed is None:
            continue
        if path == VERSION_FILE:
            expected = version + "\n"
        else:
            pattern = MANIFEST_RE[path]
            expected, count = pattern.subn(rf'\g<pre>{version}\g<post>', committed)
            if count != 1:
                raise ValueError(f"{path}: committed manifest has {count} version fields")
        if path.read_text(encoding="utf-8") != expected:
            raise ValueError(
                f"{path}: contains changes beyond the companion version; "
                "refusing to auto-stage concurrent edits"
            )


def auto_bump_staged(dry_run: bool = False) -> str:
    changes = staged_companion_changes()
    current = check_version()
    if not changes:
        print("no staged companion implementation changes; version unchanged")
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
        print(f"{action}: {current} -> {target}; staged companion changes: {len(changes)}")
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
    subparsers.add_parser("check", help="verify VERSION and all manifests agree")

    set_parser = subparsers.add_parser("set", help="manually set MAJOR.MINOR.PATCH")
    set_parser.add_argument("version")

    bump_parser = subparsers.add_parser("bump", help="increment a semantic version part")
    bump_parser.add_argument("part", choices=("major", "minor", "patch"), nargs="?", default="patch")

    auto_parser = subparsers.add_parser(
        "auto-bump-staged",
        help="bump patch when staged companion implementation changes are committed",
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
