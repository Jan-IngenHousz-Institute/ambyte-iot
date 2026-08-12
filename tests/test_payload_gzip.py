"""Executable contract checks for the transport-gzip helper.

Python's gzip module is the same codec the OpenJII Silver layer uses in
``decompress_sample_value`` (``gzip.decompress`` of ``base64.b64decode``), so a
round trip here proves the firmware's framing is exactly what the existing
pipeline reverses.
"""

from __future__ import annotations

import base64
import gzip
import json
import os
from pathlib import Path
import subprocess
import shutil
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]

# Mirror of DC_V3_GZ_EVENT_ENVELOPE_FMT (device_commands.c). Keep in sync: the
# assertions below pin the transport contract the OpenJII ingest relies on.
GZ_ENVELOPE_FMT = (
    '{{"sample":"{b64}","_sample_encoding":"gzip+base64",'
    '"timestamp":"2026-08-05T19:26:00Z","device_battery":3.912,'
    '"timezone":"Europe/Amsterdam",'
    '"device_id":"28:37:2F:FF:E7:04","device_name":"AmbyteOnAir",'
    '"device_version":"V003","device_firmware":"1.6.6"}}'
)


def silver_decompress(encoded_sample: str, encoding: str) -> str:
    """Byte-for-byte re-statement of OpenJII's decompress_sample_value."""
    if encoding == "gzip+base64":
        return gzip.decompress(base64.b64decode(encoded_sample)).decode("utf-8")
    return encoded_sample


class PayloadGzipTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory(prefix="payload-gzip-host-")
        binary = Path(cls.tmp.name) / "payload_gzip_host"
        compile_cmd = [
            os.environ.get("CC", shutil.which("clang") or "cc"),
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            f"-I{ROOT / 'components/domain/include'}",
            str(ROOT / "tests/payload_gzip_host.c"),
            str(ROOT / "components/domain/payload_gzip.c"),
            "-o",
            str(binary),
        ]
        subprocess.run(compile_cmd, check=True, cwd=ROOT)
        result = subprocess.run(
            [str(binary)], check=True, cwd=ROOT, capture_output=True, text=True
        )
        cls.records = dict(
            line.split("=", 1) for line in result.stdout.splitlines()
        )

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def test_fixture_round_trips_through_silver_codec(self) -> None:
        decoded = silver_decompress(self.records["GZ_FIXTURE"], "gzip+base64")
        self.assertEqual(decoded, self.records["SRC_FIXTURE"])
        sample = json.loads(decoded)
        self.assertIsInstance(sample, list)
        self.assertEqual(sample[0]["schema"], "ambit.trace/3")

    def test_empty_input_is_a_valid_gzip_member(self) -> None:
        self.assertEqual(silver_decompress(self.records["GZ_EMPTY"], "gzip+base64"), "")

    def test_multi_block_stream_crc_and_isize(self) -> None:
        # >64 KiB forces several DEFLATE blocks; gzip.decompress verifies the
        # whole-stream CRC-32 and ISIZE trailers or raises.
        raw = gzip.decompress(base64.b64decode(self.records["GZ_BIG"]))
        self.assertEqual(len(raw), int(self.records["SRC_BIG_LEN"]))
        self.assertEqual(raw, bytes((0x30 + i % 75) for i in range(len(raw))))

    def test_gzip_header_matches_rfc1952_profile(self) -> None:
        stream = base64.b64decode(self.records["GZ_FIXTURE"])
        # magic, CM=8 deflate, FLG=0 (no name/extra/comment), MTIME=0, XFL=0,
        # OS=255 unknown; nothing device-identifying leaks into the header.
        self.assertEqual(stream[:10], b"\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff")

    def test_base64_is_single_line_standard_alphabet(self) -> None:
        for key in ("GZ_FIXTURE", "GZ_EMPTY", "GZ_BIG"):
            value = self.records[key]
            self.assertNotIn("\n", value)
            self.assertNotIn("-", value)  # no URL-safe alphabet
            self.assertNotIn("_", value)
            base64.b64decode(value, validate=True)

    def test_gz_envelope_shape_and_decode(self) -> None:
        envelope = json.loads(GZ_ENVELOPE_FMT.format(b64=self.records["GZ_FIXTURE"]))
        self.assertEqual(
            set(envelope),
            {
                "sample", "_sample_encoding", "timestamp", "device_battery",
                "timezone", "device_id", "device_name", "device_version",
                "device_firmware",
            },
        )
        self.assertEqual(envelope["_sample_encoding"], "gzip+base64")
        self.assertIsInstance(envelope["sample"], str)
        decoded = silver_decompress(envelope["sample"], envelope["_sample_encoding"])
        # After Silver decompression the sample is exactly the plain-envelope
        # array text, so downstream sees one shape regardless of the flag.
        self.assertEqual(json.loads(decoded), json.loads(self.records["SRC_FIXTURE"]))


if __name__ == "__main__":
    unittest.main()
