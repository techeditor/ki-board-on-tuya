#!/usr/bin/env python3
"""Build, sign and check Ki-board local OTA packages (.akota).

Format (docs/ota.md): a 192-byte manifest, a 64-byte ECDSA P-256 signature
(r||s, big endian) over SHA-256(manifest), then the TuyaOpen UG file as built.

Commands:
  keygen        create a key pair (private key encrypted with a passphrase)
  pubkey-header write the C header the firmware compiles the public key from
  sign          wrap a UG build output into a signed package
  verify        check a package against a public key and print its manifest

The private key never leaves the machine that runs `sign`; this tool does not
print it. Needs the `cryptography` package (pip install cryptography).
"""

from __future__ import annotations

import argparse
import getpass
import hashlib
import os
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1]
INCLUDE_DIR = APP_DIR / "common" / "include"
RELEASE_KEY_HEADER = INCLUDE_DIR / "ak_ota_key_release.h"
TEST_KEY_HEADER = INCLUDE_DIR / "ak_ota_key_test.h"

MAGIC = b"AKOT"
FORMAT = 1
MANIFEST_LEN = 192
SIG_LEN = 64
HEADER_LEN = MANIFEST_LEN + SIG_LEN
PRODUCT = b"ki-board"
HW = b"v0.3"
PROFILE_RELEASE = 1
PROFILE_DEBUG = 2
PROFILE_NAMES = {PROFILE_RELEASE: "release", PROFILE_DEBUG: "debug"}

# TuyaOpen UG header (platform/T5AI .../tuya_scripts/create_ug_file.py).
UG_MAGIC = 0x004D4D4D
UG_TYPE_LZMA_FULL = 0x4C5A4350
UG_HEADER_LEN = 64
# The ota partition is 2940K; the platform packer warns above 85 %.
OTA_PARTITION = 2940 * 1024
MAX_IMAGE = OTA_PARTITION * 85 // 100

SEMVER_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")


class PackError(Exception):
    pass


def _crypto():
    try:
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import ec
        from cryptography.hazmat.primitives.asymmetric.utils import (
            decode_dss_signature, encode_dss_signature)
    except ImportError as exc:  # pragma: no cover - environment issue
        raise PackError("missing dependency: pip install cryptography") from exc
    return hashes, serialization, ec, decode_dss_signature, encode_dss_signature


def parse_version(text: str) -> tuple[int, int, int]:
    m = SEMVER_RE.fullmatch(text.strip())
    if not m or any(int(p) > 0xFFFF for p in m.groups()):
        raise PackError(f"invalid version {text!r}")
    return tuple(int(p) for p in m.groups())  # type: ignore[return-value]


def point_bytes(public_key) -> bytes:
    _, serialization, _, _, _ = _crypto()
    return public_key.public_bytes(serialization.Encoding.X962,
                                   serialization.PublicFormat.UncompressedPoint)


def key_id(point: bytes) -> bytes:
    return hashlib.sha256(point).digest()[:8]


# ── manifest ────────────────────────────────────────────────────────────────

@dataclass
class Manifest:
    profile: int
    version: tuple[int, int, int]
    image_size: int
    image_sha256: bytes
    key_id: bytes
    ug_header: bytes

    def pack(self) -> bytes:
        out = struct.pack("<4sHH16s8sB3x3HxxI32s8s40x64s",
                          MAGIC, FORMAT, MANIFEST_LEN, PRODUCT, HW, self.profile,
                          *self.version, self.image_size, self.image_sha256,
                          self.key_id, self.ug_header)
        assert len(out) == MANIFEST_LEN
        return out

    @staticmethod
    def unpack(raw: bytes) -> "Manifest":
        if len(raw) != MANIFEST_LEN:
            raise PackError("manifest has the wrong length")
        (magic, fmt, mlen, product, hw, profile, v0, v1, v2, size, digest, kid,
         ug) = struct.unpack("<4sHH16s8sB3x3HxxI32s8s40x64s", raw)
        if magic != MAGIC or fmt != FORMAT or mlen != MANIFEST_LEN:
            raise PackError("not an AKOT v1 manifest")
        if product.rstrip(b"\0") != PRODUCT or hw.rstrip(b"\0") != HW:
            raise PackError("package is for another product or hardware")
        if profile not in PROFILE_NAMES:
            raise PackError("unknown profile")
        return Manifest(profile, (v0, v1, v2), size, digest, kid, ug)


def check_ug(ug: bytes) -> bytes:
    """Return the UG header after checking it is a full LZMA package."""
    if len(ug) < UG_HEADER_LEN:
        raise PackError("UG file is too short")
    fields = struct.unpack("<16I", ug[:UG_HEADER_LEN])
    magic, ota_type, patch_len = fields[0], fields[1], fields[8]
    if magic != UG_MAGIC:
        raise PackError("not a TuyaOpen UG file (wrong magic); did you pick QIO/UA?")
    if ota_type != UG_TYPE_LZMA_FULL:
        raise PackError(f"unsupported UG type {ota_type:#x}; expected full LZMA")
    if patch_len + UG_HEADER_LEN != len(ug):
        raise PackError("UG length does not match its header")
    if len(ug) > MAX_IMAGE:
        raise PackError(f"UG is {len(ug)} B; limit is {MAX_IMAGE} B (85 % of ota)")
    # The platform reserves align4k(patch + 28) bytes below the manage block
    # (ota_head.c _cal_patch_start_addr) but the file is patch + 64 bytes long:
    # sizes in that 36-byte gap would run into the manage block. The device
    # refuses them; refuse here first. Any rebuild that changes the size fixes it.
    if -(-len(ug) // 4096) != -(-(patch_len + 28) // 4096):
        raise PackError("UG size falls in the platform's 36-byte sector gap; change the build and rebuild")
    return ug[:UG_HEADER_LEN]


# ── build metadata ──────────────────────────────────────────────────────────

def read_config(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        m = re.match(r"^(CONFIG_[A-Z0-9_]+)=(.*)$", line)
        if m:
            values[m.group(1)] = m.group(2).strip('"')
    return values


def profile_of(config: dict[str, str]) -> int:
    debug = (config.get("CONFIG_AK_LICENSE_DEBUG_READ") == "y" or
             config.get("CONFIG_AK_OTA_TEST_KEY") == "y" or
             config.get("CONFIG_AK_DEBUG_PROBES") == "y")
    return PROFILE_DEBUG if debug else PROFILE_RELEASE


def header_point(path: Path) -> bytes | None:
    """The public key a firmware header compiles in; None for the placeholder."""
    text = path.read_text(encoding="utf-8")
    if re.search(r"#define\s+AK_OTA_KEY_PLACEHOLDER\s+1", text):
        return None
    body = re.search(r"AK_OTA_KEY_POINT\s*\{(.*?)\}", text, re.S)
    if not body:
        raise PackError(f"{path}: no AK_OTA_KEY_POINT")
    point = bytes(int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{2})", body.group(1)))
    if len(point) != 65 or point[0] != 4:
        raise PackError(f"{path}: AK_OTA_KEY_POINT is not an uncompressed P-256 point")
    return point


# ── commands ────────────────────────────────────────────────────────────────

def load_private(path: Path, passphrase_env: str | None):
    _, serialization, ec, _, _ = _crypto()
    data = path.read_bytes()
    password = None
    if b"ENCRYPTED" in data:
        if passphrase_env:
            secret = os.environ.get(passphrase_env)
            if secret is None:
                raise PackError(f"environment variable {passphrase_env} is not set")
            password = secret.encode()
        else:
            password = getpass.getpass("Key passphrase: ").encode()
    key = serialization.load_pem_private_key(data, password=password)
    if not isinstance(key, ec.EllipticCurvePrivateKey) or key.curve.name != "secp256r1":
        raise PackError("signing key must be ECDSA P-256")
    return key


def load_public(path: Path):
    _, serialization, ec, _, _ = _crypto()
    key = serialization.load_pem_public_key(path.read_bytes())
    if not isinstance(key, ec.EllipticCurvePublicKey) or key.curve.name != "secp256r1":
        raise PackError("public key must be ECDSA P-256")
    return key


def cmd_keygen(args) -> int:
    _, serialization, ec, _, _ = _crypto()
    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    priv_path, pub_path = out / f"{args.name}_private.pem", out / f"{args.name}_public.pem"
    if priv_path.exists() or pub_path.exists():
        raise PackError(f"{priv_path} or {pub_path} already exists; not overwriting")
    if args.no_passphrase:
        enc = serialization.NoEncryption()
    else:
        first = getpass.getpass("New key passphrase: ")
        if len(first) < 12 or first != getpass.getpass("Repeat passphrase: "):
            raise PackError("passphrases differ or are shorter than 12 characters")
        enc = serialization.BestAvailableEncryption(first.encode())
    key = ec.generate_private_key(ec.SECP256R1())
    fd = os.open(priv_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as fh:
        fh.write(key.private_bytes(serialization.Encoding.PEM,
                                   serialization.PrivateFormat.PKCS8, enc))
    pub_path.write_bytes(key.public_key().public_bytes(
        serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))
    print(f"wrote {priv_path} (keep offline) and {pub_path}")
    print(f"key_id {key_id(point_bytes(key.public_key())).hex()}")
    return 0


def render_header(point: bytes, kind: str) -> str:
    guard = f"AK_OTA_KEY_{kind.upper()}_H"
    rows = ", \\\n".join("    " + ", ".join(f"0x{b:02x}" for b in point[i:i + 13])
                         for i in range(0, len(point), 13))
    return (f"/* Generated by firmware/app/tools/ota_pack.py pubkey-header; do not edit.\n"
            f" * {kind} OTA public key (ECDSA P-256, uncompressed point),\n"
            f" * key_id {key_id(point).hex()}. */\n"
            f"#ifndef {guard}\n#define {guard}\n\n"
            f"#define AK_OTA_KEY_PLACEHOLDER 0\n"
            f"#define AK_OTA_KEY_POINT {{ \\\n{rows} \\\n}}\n\n#endif\n")


def cmd_pubkey_header(args) -> int:
    point = point_bytes(load_public(Path(args.pub)))
    target = Path(args.out) if args.out else (RELEASE_KEY_HEADER if args.kind == "release"
                                              else TEST_KEY_HEADER)
    target.write_text(render_header(point, args.kind), encoding="utf-8")
    print(f"wrote {target} (key_id {key_id(point).hex()})")
    return 0


def sign_package(ug: bytes, version: tuple[int, int, int], profile: int, private_key) -> bytes:
    hashes, _, ec, decode_dss_signature, _ = _crypto()
    ug_header = check_ug(ug)
    manifest = Manifest(profile, version, len(ug), hashlib.sha256(ug).digest(),
                        key_id(point_bytes(private_key.public_key())), ug_header).pack()
    der = private_key.sign(manifest, ec.ECDSA(hashes.SHA256()))
    r, s = decode_dss_signature(der)
    return manifest + r.to_bytes(32, "big") + s.to_bytes(32, "big") + ug


def check_key_class(point: bytes, profile: int, ua: bytes | None) -> str:
    """Refuse combinations that could ship debug firmware or an image that
    cannot take the next update."""
    release = header_point(RELEASE_KEY_HEADER)
    test = header_point(TEST_KEY_HEADER)
    if release is not None and point == release:
        if profile != PROFILE_RELEASE:
            raise PackError("the release key never signs debug/test builds")
        if ua is None:
            raise PackError("release signing needs --ua to check the image's embedded key")
        if release not in ua:
            raise PackError("image does not embed the release public key "
                            "(built with the placeholder?); it could never update again")
        if test is not None and test in ua:
            raise PackError("image embeds the test public key; rebuild with a release profile")
        return "release"
    if test is not None and point == test:
        return "test"
    raise PackError("signing key matches neither ak_ota_key_release.h nor ak_ota_key_test.h")


def cmd_sign(args) -> int:
    ug_path = Path(args.ug)
    config = read_config(Path(args.config))
    version_text = Path(args.version_file).read_text(encoding="utf-8").strip()
    if config.get("CONFIG_PROJECT_VERSION") != version_text:
        raise PackError(f"build config version {config.get('CONFIG_PROJECT_VERSION')!r} "
                        f"!= VERSION {version_text!r}")
    if f"_UG_{version_text}.bin" not in ug_path.name:
        raise PackError(f"{ug_path.name} is not the UG file of version {version_text}")
    profile = profile_of(config)
    key = load_private(Path(args.key), args.passphrase_env)
    ua = Path(args.ua).read_bytes() if args.ua else None
    kind = check_key_class(point_bytes(key.public_key()), profile, ua)
    package = sign_package(ug_path.read_bytes(), parse_version(version_text), profile, key)
    out = Path(args.out) if args.out else ug_path.with_name(
        f"ki-board_{version_text}_{PROFILE_NAMES[profile]}_{kind}.akota")
    out.write_bytes(package)
    print(f"wrote {out} ({len(package)} B, {PROFILE_NAMES[profile]}, signed by {kind} key)")
    print(f"sha256 {hashlib.sha256(package).hexdigest()}")
    return 0


def verify_package(package: bytes, point: bytes) -> Manifest:
    hashes, _, ec, _, encode_dss_signature = _crypto()
    from cryptography.exceptions import InvalidSignature
    if len(package) < HEADER_LEN + UG_HEADER_LEN:
        raise PackError("package too short")
    raw, sig, ug = package[:MANIFEST_LEN], package[MANIFEST_LEN:HEADER_LEN], package[HEADER_LEN:]
    public = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), point)
    der = encode_dss_signature(int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big"))
    try:
        public.verify(der, raw, ec.ECDSA(hashes.SHA256()))
    except InvalidSignature as exc:
        raise PackError("bad signature") from exc
    m = Manifest.unpack(raw)
    if m.key_id != key_id(point):
        raise PackError("manifest key_id does not match the key")
    if m.image_size != len(ug) or hashlib.sha256(ug).digest() != m.image_sha256:
        raise PackError("UG does not match the manifest")
    if check_ug(ug) != m.ug_header:
        raise PackError("UG header does not match the manifest")
    return m


def cmd_verify(args) -> int:
    if args.pub:
        point = point_bytes(load_public(Path(args.pub)))
    else:
        point = header_point(RELEASE_KEY_HEADER if args.kind == "release" else TEST_KEY_HEADER)
        if point is None:
            raise PackError("the release key header is still the placeholder")
    m = verify_package(Path(args.package).read_bytes(), point)
    print(f"OK version {'.'.join(map(str, m.version))} profile {PROFILE_NAMES[m.profile]} "
          f"image {m.image_size} B key_id {m.key_id.hex()}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("keygen", help="create an ECDSA P-256 key pair")
    p.add_argument("--out-dir", required=True)
    p.add_argument("--name", default="ki-board-ota-release")
    p.add_argument("--no-passphrase", action="store_true",
                   help="leave the private key unencrypted (test keys only)")
    p.set_defaults(func=cmd_keygen)

    p = sub.add_parser("pubkey-header", help="write the firmware public key header")
    p.add_argument("--pub", required=True)
    p.add_argument("--kind", choices=("release", "test"), required=True)
    p.add_argument("--out")
    p.set_defaults(func=cmd_pubkey_header)

    p = sub.add_parser("sign", help="sign a UG build output")
    p.add_argument("--ug", required=True, help=".build/bin/<app>_UG_<ver>.bin")
    p.add_argument("--ua", help=".build/bin/<app>_UA_<ver>.bin (required for the release key)")
    p.add_argument("--config", required=True, help=".build/cache/using.config of that build")
    p.add_argument("--version-file", default=str(APP_DIR / "VERSION"))
    p.add_argument("--key", required=True, help="private key PEM")
    p.add_argument("--passphrase-env", help="read the passphrase from this variable")
    p.add_argument("--out")
    p.set_defaults(func=cmd_sign)

    p = sub.add_parser("verify", help="verify a package")
    p.add_argument("package")
    p.add_argument("--pub", help="public key PEM (default: the header for --kind)")
    p.add_argument("--kind", choices=("release", "test"), default="release")
    p.set_defaults(func=cmd_verify)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except PackError as exc:
        print(f"ota_pack: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
