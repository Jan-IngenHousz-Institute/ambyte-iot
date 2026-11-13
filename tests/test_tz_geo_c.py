# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""The firmware's tz_geo reader must give the GUI resolver's exact answers.

Compiles components/timezone/tz_geo.c (production source, ASan/UBSan) against
the checked-in tz_geo.bin and compares it with flash_gui/tz_geo.py on the
reference points and on a dense random sample, then feeds it truncated and
bit-flipped blobs, which must fail closed without an out-of-bounds read.
"""

import os
import random
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from flash_gui import tz_geo
from test_tz_geo import BIN, OCEAN_POINTS, REFERENCE_POINTS

ROOT = Path(__file__).resolve().parents[1]


class FirmwareReaderParityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="tz-geo-")
        cls.binary = Path(cls.temp.name) / "tz-geo-host"
        subprocess.run([
            os.environ.get("CC", shutil.which("clang") or "cc"), "-std=c11", "-O1",
            "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all", "-g",
            f"-I{ROOT / 'components/timezone/include'}",
            str(ROOT / "tests/tz_geo_host.c"), str(ROOT / "components/timezone/tz_geo.c"),
            "-lm", "-o", str(cls.binary),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def resolve(self, points, blob=BIN, truncate=None):
        args = [str(self.binary), str(blob)] + ([str(truncate)] if truncate is not None else [])
        stdin = "".join(f"{lat!r} {lon!r}\n" for lat, lon in points)
        run = subprocess.run(args, input=stdin, capture_output=True, text=True,
                             errors="replace", timeout=120)
        self.assertEqual(run.returncode, 0, run.stderr)
        return [None if line == "-" else line for line in run.stdout.splitlines()]

    def test_reference_points_match_the_python_resolver(self):
        points = [(lat, lon) for _, lat, lon, _ in REFERENCE_POINTS]
        points += [(lat, lon) for _, lat, lon in OCEAN_POINTS]
        want = [z for *_, z in REFERENCE_POINTS] + [None] * len(OCEAN_POINTS)
        self.assertEqual(self.resolve(points), want)

    def test_random_points_match_the_python_resolver_exactly(self):
        rng = random.Random(20261113)
        points = [(rng.uniform(-90, 90), rng.uniform(-180, 180)) for _ in range(20000)]
        # Exact cell edges are where float evaluation could diverge.
        points += [(-90 + 180 * i / 8192, -180 + 360 * j / 8192)
                   for i in range(0, 8193, 257) for j in range(0, 8193, 263)]
        points += [(90.0, 180.0), (-90.0, -180.0), (0.0, 0.0), (91.0, 0.0), (0.0, 181.0)]
        self.assertEqual(self.resolve(points), [tz_geo.zone_for(a, b) for a, b in points])

    def test_truncated_and_corrupted_blobs_fail_closed(self):
        uluru = [(-25.3444, 131.0369)]
        for cut in (0, 10, 63, 64, 1000, BIN.stat().st_size - 1):
            self.assertEqual(self.resolve(uluru, truncate=cut), [None], cut)
        data = bytearray(BIN.read_bytes())
        rng = random.Random(7)
        corrupt = Path(self.temp.name) / "corrupt.bin"
        points = [(rng.uniform(-60, 70), rng.uniform(-180, 180)) for _ in range(200)]
        for _ in range(25):
            mutated = bytearray(data)
            for _ in range(64):
                mutated[rng.randrange(len(mutated))] ^= 1 << rng.randrange(8)
            corrupt.write_bytes(mutated)
            answers = self.resolve(points, blob=corrupt)  # no crash, no OOB read
            self.assertEqual(answers, [tz_geo._lookup(bytes(mutated), a, b) for a, b in points])

    def test_every_claimed_depth(self):
        # Every tile/max depth a header can claim, deterministically.
        # gpt-6-astra's review of #100 forged tile_depth 16: the tile count
        # wrapped to 0 in 32 bits, the directory bound passed and the reader
        # went out of bounds. Both readers now reject depths beyond 12 (tile)
        # / 24 (cell) before shifting or indexing; for every combination they
        # must agree and the C reader must stay inside the blob (ASan).
        data = BIN.read_bytes()
        points = [(-25.3444, 131.0369), (52.3676, 4.9041), (89.9, -179.9), (-89.9, 179.9)]
        blob = Path(self.temp.name) / "depth.bin"
        checked = 0
        for tile_depth in range(0, 26):
            for max_depth in sorted({tile_depth, min(tile_depth + 1, 25), 13, 24, 25, 255}):
                mutated = bytearray(data)
                mutated[5] = tile_depth & 0xFF
                mutated[6] = max_depth & 0xFF
                blob.write_bytes(mutated)
                want = [tz_geo._lookup(bytes(mutated), a, b) for a, b in points]
                got = self.resolve(points, blob=blob)
                self.assertEqual(got, want, (tile_depth, max_depth))
                if not (1 <= tile_depth <= 12 and tile_depth <= max_depth <= 24):
                    self.assertEqual(got, [None] * len(points), (tile_depth, max_depth))
                checked += 1
        self.assertGreater(checked, 100)


if __name__ == "__main__":
    unittest.main()
