# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Offline coordinate -> timezone dataset (tools/gen_tz_geo.py) contract tests.

The generated quadtree is checked in twice -- components/timezone/tz_geo.bin
(embedded by the firmware) and flash_gui/tz_geo_data.py (the frozen GUI) -- and
read by two implementations (tz_geo.c, flash_gui/tz_geo.py). These tests pin the
bytes to the generator's source pin and parameters, keep both copies identical,
and exercise the real resolver on the deployment points that motivated it.
A full regeneration needs the pinned 51 MB source zip and shapely; it runs when
both are available (see test_checked_in_data_matches_a_fresh_generator_run).
"""

import base64
import hashlib
import importlib.util
import math
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from flash_gui import tz_geo, tz_geo_data  # noqa: E402
from flash_gui.tz_zone_table import ZONES, TZDATA_VERSION  # noqa: E402

BIN = ROOT / "components/timezone/tz_geo.bin"
GEN_SPEC = importlib.util.spec_from_file_location("gen_tz_geo", ROOT / "tools/gen_tz_geo.py")
assert GEN_SPEC is not None and GEN_SPEC.loader is not None
GEN = importlib.util.module_from_spec(GEN_SPEC)
# Registered so the generator's multiprocessing workers can pickle _work.
sys.modules["gen_tz_geo"] = GEN
GEN_SPEC.loader.exec_module(GEN)

# (label, lat, lon, expected zone). Shared with tests/tz_geo_host.c via
# REFERENCE_POINTS so the C reader is held to exactly the same answers.
REFERENCE_POINTS = [
    ("Uluru (field site)", -25.3444, 131.0369, "Australia/Darwin"),
    ("Darwin", -12.4634, 130.8456, "Australia/Darwin"),
    ("Alice Springs", -23.6980, 133.8807, "Australia/Darwin"),
    ("Adelaide", -34.9285, 138.6007, "Australia/Adelaide"),
    ("Perth", -31.9523, 115.8613, "Australia/Perth"),
    ("Amsterdam", 52.3676, 4.9041, "Europe/Amsterdam"),
    ("Wageningen", 51.9692, 5.6654, "Europe/Amsterdam"),
    ("Berlin", 52.5200, 13.4050, "Europe/Berlin"),
    # Border pairs ~10 km either side: NT/QLD (half-hour vs whole-hour, no DST
    # on either side), NT/SA (same offset, SA has DST), NL/DE near Venlo.
    ("NT side of the QLD border", -20.0, 137.90, "Australia/Darwin"),
    ("QLD side of the NT border", -20.0, 138.10, "Australia/Brisbane"),
    ("NT side of the SA border", -25.90, 133.0, "Australia/Darwin"),
    ("SA side of the NT border", -26.10, 133.0, "Australia/Adelaide"),
    ("Venlo (NL)", 51.3700, 6.1700, "Europe/Amsterdam"),
    ("Kaldenkirchen (DE)", 51.3200, 6.2000, "Europe/Berlin"),
    ("Broken Hill (NSW, on SA time)", -31.9500, 141.4500, "Australia/Broken_Hill"),
    ("La Paz", -16.4897, -68.1193, "America/La_Paz"),
]
OCEAN_POINTS = [
    ("mid Atlantic", 0.0, -30.0),
    ("south Pacific", -30.0, -140.0),
    ("Indian Ocean", -20.0, 80.0),
    ("Null Island placeholder", 0.0, 0.0),
]


class ResolverTest(unittest.TestCase):
    def test_reference_points(self):
        for label, lat, lon, want in REFERENCE_POINTS:
            with self.subTest(label):
                self.assertEqual(tz_geo.zone_for(lat, lon), want)

    def test_open_ocean_and_placeholder_resolve_to_none(self):
        for label, lat, lon in OCEAN_POINTS:
            with self.subTest(label):
                self.assertIsNone(tz_geo.zone_for(lat, lon))

    def test_invalid_input_is_none(self):
        for lat, lon in [(91, 0), (-90.5, 10), (10, 180.01), (10, -181),
                         (math.nan, 5), (5, math.inf), ("x", 1), (None, 1),
                         (True, 5.0), (52.0, False)]:
            with self.subTest(lat=lat, lon=lon):
                self.assertIsNone(tz_geo.zone_for(lat, lon))

    def test_extreme_edges_are_clamped_not_rejected(self):
        # The last cell column/row owns lon=180 / lat=90; neither may raise.
        for lat, lon in [(90.0, 180.0), (-90.0, -180.0), (-90.0, 180.0), (90.0, -180.0)]:
            result = tz_geo.zone_for(lat, lon)
            self.assertTrue(result is None or result in ZONES)

    def test_every_resolvable_zone_is_supported_by_the_firmware(self):
        names = _blob_names(_bin_bytes())
        self.assertTrue(names)
        self.assertEqual(sorted(names), names, "names must be sorted")
        self.assertEqual(set(names) - ZONES, set())

    def test_corrupt_or_truncated_blob_fails_closed(self):
        blob = _bin_bytes()
        self.assertIsNone(tz_geo._lookup(blob[:-1], -25.3444, 131.0369))
        self.assertIsNone(tz_geo._lookup(b"XXXX" + blob[4:], -25.3444, 131.0369))
        self.assertIsNone(tz_geo._lookup(b"", 0.0, 0.0))


def _bin_bytes() -> bytes:
    return BIN.read_bytes()


def _blob_names(blob: bytes) -> list[str]:
    n = struct.unpack_from("<H", blob, 8)[0]
    names_off, tiles_off = struct.unpack_from("<II", blob, 52)
    out = []
    for i in range(n):
        at = names_off + struct.unpack_from("<H", blob, names_off + 2 * i)[0]
        out.append(blob[at:blob.index(b"\0", at, tiles_off)].decode("ascii"))
    return out


class GeneratedDataContractTest(unittest.TestCase):
    def test_gui_copy_is_byte_identical_to_the_firmware_copy(self):
        gui = base64.b64decode("".join(tz_geo_data.BLOB_B64))
        self.assertEqual(gui, _bin_bytes())
        self.assertEqual(hashlib.sha256(gui).hexdigest(), tz_geo_data.BLOB_SHA256)

    def test_header_matches_the_generator_pin_and_parameters(self):
        blob = _bin_bytes()
        (magic, version, tile_depth, max_depth, coast, _n, reserved, release, sha,
         _names_off, _tiles_off, size) = struct.unpack_from("<4sBBBBHH8s32sIII", blob)
        self.assertEqual(magic, b"TZG1")
        self.assertEqual(version, GEN.FORMAT_VERSION)
        self.assertEqual((tile_depth, max_depth, coast),
                         (GEN.TILE_DEPTH, GEN.MAX_DEPTH, GEN.COAST_DEPTH))
        self.assertEqual(reserved, 0)
        self.assertEqual(release.rstrip(b"\0").decode(), GEN.TBB_RELEASE)
        self.assertEqual(sha.hex(), GEN.TBB_SHA256)
        self.assertEqual(size, len(blob))
        self.assertEqual(tz_geo_data.SOURCE_RELEASE, GEN.TBB_RELEASE)

    def test_boundary_release_matches_the_zone_table_tzdata(self):
        # Boundaries and the compiled rule table must describe the same tzdata
        # release, or a renamed/split zone could resolve to a name the device
        # cannot apply. Regenerate both together.
        self.assertEqual(GEN.TBB_RELEASE, TZDATA_VERSION)

    def test_python_module_matches_its_renderer(self):
        self.assertEqual(GEN.render_py(_bin_bytes()),
                         (ROOT / "flash_gui/tz_geo_data.py").read_text(encoding="utf-8"))

    def test_data_fits_the_flash_budget(self):
        # Measured app headroom when this was added was ~1.29 MB of the 3 MB
        # (0x2F0000) app slot; keep the map a small, deliberate share of it.
        self.assertLess(len(_bin_bytes()), 320 * 1024)

    def test_checked_in_data_matches_a_fresh_generator_run(self):
        try:
            import shapely  # noqa: F401
        except ImportError:
            self.skipTest("shapely not installed (generator-only dependency)")
        source = GEN.default_source()
        if not source.exists():
            self.skipTest(f"pinned source zip not cached at {source}")
        self.assertEqual(GEN.main(["--check", "--source", str(source)]), 0,
                         "tz_geo data is out of date or hand-edited -- run "
                         "python tools/gen_tz_geo.py")


if __name__ == "__main__":
    unittest.main()
