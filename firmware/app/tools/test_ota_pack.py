#!/usr/bin/env python3
"""Unit tests for ota_pack.py (run by firmware/app/test/run_host_tests.sh)."""

import hashlib
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ota_pack  # noqa: E402

from cryptography.hazmat.primitives.asymmetric import ec  # noqa: E402


def fake_ug(body: bytes = b"\x5a" * 1000) -> bytes:
    header = struct.pack("<16I", ota_pack.UG_MAGIC, ota_pack.UG_TYPE_LZMA_FULL, 1, 1,
                         4096, 4096, 64, 0, len(body), 0, 0, 0x1000, 0x20000, 0, 0, 0)
    return header + body


class OtaPackTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.release = ec.generate_private_key(ec.SECP256R1())
        self.test = ec.generate_private_key(ec.SECP256R1())
        self.release_pt = ota_pack.point_bytes(self.release.public_key())
        self.test_pt = ota_pack.point_bytes(self.test.public_key())
        rel_h, test_h = self.dir / "rel.h", self.dir / "test.h"
        rel_h.write_text(ota_pack.render_header(self.release_pt, "release"))
        test_h.write_text(ota_pack.render_header(self.test_pt, "test"))
        self.saved = ota_pack.RELEASE_KEY_HEADER, ota_pack.TEST_KEY_HEADER
        ota_pack.RELEASE_KEY_HEADER, ota_pack.TEST_KEY_HEADER = rel_h, test_h

    def tearDown(self):
        ota_pack.RELEASE_KEY_HEADER, ota_pack.TEST_KEY_HEADER = self.saved
        self.tmp.cleanup()

    def test_header_round_trip(self):
        self.assertEqual(ota_pack.header_point(ota_pack.RELEASE_KEY_HEADER), self.release_pt)
        placeholder = Path(__file__).resolve().parents[1] / "common/include/ak_ota_key_release.h"
        self.assertIsNone(ota_pack.header_point(placeholder))

    def test_sign_verify(self):
        ug = fake_ug()
        pkg = ota_pack.sign_package(ug, (0, 3, 60), ota_pack.PROFILE_RELEASE, self.release)
        self.assertEqual(len(pkg), ota_pack.HEADER_LEN + len(ug))
        m = ota_pack.verify_package(pkg, self.release_pt)
        self.assertEqual(m.version, (0, 3, 60))
        self.assertEqual(m.image_sha256, hashlib.sha256(ug).digest())
        self.assertEqual(m.ug_header, ug[:64])

    def test_tamper_and_wrong_key(self):
        pkg = bytearray(ota_pack.sign_package(fake_ug(), (0, 3, 60),
                                              ota_pack.PROFILE_RELEASE, self.release))
        with self.assertRaisesRegex(ota_pack.PackError, "bad signature"):
            ota_pack.verify_package(bytes(pkg), self.test_pt)
        pkg[40] ^= 1  # version byte inside the signed manifest
        with self.assertRaisesRegex(ota_pack.PackError, "bad signature"):
            ota_pack.verify_package(bytes(pkg), self.release_pt)
        pkg[40] ^= 1
        pkg[-1] ^= 1  # image byte: signature holds, digest does not
        with self.assertRaisesRegex(ota_pack.PackError, "does not match"):
            ota_pack.verify_package(bytes(pkg), self.release_pt)

    def test_rejects_non_ug(self):
        with self.assertRaisesRegex(ota_pack.PackError, "magic"):
            ota_pack.check_ug(b"\0" * 2000)
        ug = bytearray(fake_ug())
        ug[4:8] = struct.pack("<I", 0x4C5A4351)
        with self.assertRaisesRegex(ota_pack.PackError, "type"):
            ota_pack.check_ug(bytes(ug))
        with self.assertRaisesRegex(ota_pack.PackError, "length"):
            ota_pack.check_ug(fake_ug() + b"x")
        # 4096 - 64 + 10 bytes of patch: the file needs 2 sectors, the
        # platform reserves 1 (patch + 28 <= 4096).
        with self.assertRaisesRegex(ota_pack.PackError, "sector gap"):
            ota_pack.check_ug(fake_ug(b"\1" * (4096 - 64 + 10)))

    def test_key_class_rules(self):
        ua = b"code" + self.release_pt + b"more"
        self.assertEqual(ota_pack.check_key_class(self.release_pt, ota_pack.PROFILE_RELEASE, ua),
                         "release")
        with self.assertRaisesRegex(ota_pack.PackError, "never signs debug"):
            ota_pack.check_key_class(self.release_pt, ota_pack.PROFILE_DEBUG, ua)
        with self.assertRaisesRegex(ota_pack.PackError, "does not embed"):
            ota_pack.check_key_class(self.release_pt, ota_pack.PROFILE_RELEASE, b"no key here")
        with self.assertRaisesRegex(ota_pack.PackError, "test public key"):
            ota_pack.check_key_class(self.release_pt, ota_pack.PROFILE_RELEASE,
                                     ua + self.test_pt)
        self.assertEqual(ota_pack.check_key_class(self.test_pt, ota_pack.PROFILE_DEBUG, None),
                         "test")
        other = ota_pack.point_bytes(ec.generate_private_key(ec.SECP256R1()).public_key())
        with self.assertRaisesRegex(ota_pack.PackError, "neither"):
            ota_pack.check_key_class(other, ota_pack.PROFILE_RELEASE, ua)

    def test_profile_of(self):
        self.assertEqual(ota_pack.profile_of({"CONFIG_AK_LICENSE_DEBUG_READ": "y"}),
                         ota_pack.PROFILE_DEBUG)
        self.assertEqual(ota_pack.profile_of({"CONFIG_AK_OTA_TEST_KEY": "y"}),
                         ota_pack.PROFILE_DEBUG)
        self.assertEqual(ota_pack.profile_of({}), ota_pack.PROFILE_RELEASE)

    def test_sign_command_checks_version(self):
        bin_dir = self.dir / "bin"
        bin_dir.mkdir()
        ug_path = bin_dir / "app_UG_0.3.60.bin"
        ug_path.write_bytes(fake_ug())
        (self.dir / "VERSION").write_text("0.3.60\n")
        (self.dir / "using.config").write_text(
            'CONFIG_PROJECT_VERSION="0.3.59"\nCONFIG_AK_LICENSE_DEBUG_READ=y\n')
        key_path = self.dir / "test.pem"
        from cryptography.hazmat.primitives import serialization
        key_path.write_bytes(self.test.private_bytes(
            serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption()))
        argv = ["sign", "--ug", str(ug_path), "--config", str(self.dir / "using.config"),
                "--version-file", str(self.dir / "VERSION"), "--key", str(key_path)]
        self.assertEqual(ota_pack.main(argv), 1)  # 0.3.59 != 0.3.60
        (self.dir / "using.config").write_text(
            'CONFIG_PROJECT_VERSION="0.3.60"\nCONFIG_AK_LICENSE_DEBUG_READ=y\n')
        self.assertEqual(ota_pack.main(argv), 0)
        pkg = (bin_dir / "ki-board_0.3.60_debug_test.akota").read_bytes()
        self.assertEqual(ota_pack.verify_package(pkg, self.test_pt).profile,
                         ota_pack.PROFILE_DEBUG)


if __name__ == "__main__":
    unittest.main()
