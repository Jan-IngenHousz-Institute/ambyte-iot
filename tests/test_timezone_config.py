# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Timezone provisioning and firmware-contract regression tests.

The zone table is generated (tools/gen_tz_table.py) into two checked-in
artefacts — the firmware's C table and flash_gui's name set — and consumed by a
third place (tools/build_nvs_image.py, which parses the C table). These tests
exist to make sure those never disagree, and that a future regeneration cannot
quietly emit rules the device's libc would reject.
"""

import importlib.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

from tools.site_state_blob import decode_site_state, encode_site_state


ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
TZ_TABLE_INC = ROOT / "components/timezone/tz_zone_table.inc"
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(ROOT))
SPEC = importlib.util.spec_from_file_location("build_nvs_image", TOOLS / "build_nvs_image.py")
assert SPEC is not None and SPEC.loader is not None
BUILD_NVS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD_NVS)

GEN_SPEC = importlib.util.spec_from_file_location("gen_tz_table", TOOLS / "gen_tz_table.py")
assert GEN_SPEC is not None and GEN_SPEC.loader is not None
GEN_TZ = importlib.util.module_from_spec(GEN_SPEC)
GEN_SPEC.loader.exec_module(GEN_TZ)

# `{ "Zone/Name", <rule index>, <std offset> },` rows of k_tz_zones.
ZONE_ROW_RE = re.compile(r'\{\s*"([^"]+)"\s*,\s*(\d+)\s*,\s*(-?\d+)\s*\}')

# Two instants half a year apart — one per DST season. Same values the firmware
# uses to check that libc accepted a rule (components/timezone/timezone.c).
JAN_PROBE = 1768478400   # 2026-01-15 12:00 UTC
JUL_PROBE = 1784116800   # 2026-07-15 12:00 UTC


def firmware_zone_rows() -> list[tuple[str, int, int]]:
    text = TZ_TABLE_INC.read_text(encoding="utf-8")
    table = text.split("k_tz_zones[TZ_TABLE_ZONE_COUNT] = {", 1)[1].split("};", 1)[0]
    return [(m.group(1), int(m.group(2)), int(m.group(3)))
            for m in ZONE_ROW_RE.finditer(table)]


def firmware_rules() -> list[str]:
    text = TZ_TABLE_INC.read_text(encoding="utf-8")
    pool = text.split("k_tz_rules[TZ_TABLE_RULE_COUNT] = {", 1)[1].split("};", 1)[0]
    return re.findall(r'"([^"]+)"', pool)


class TimezoneProvisioningTest(unittest.TestCase):
    def test_legacy_alias_and_whitespace_are_canonicalized(self):
        self.assertEqual(BUILD_NVS.canonical_timezone(" AMT "), "Europe/Amsterdam")
        self.assertEqual(BUILD_NVS.canonical_timezone("Z"), "UTC")

    def test_supported_iana_zone_is_preserved(self):
        self.assertEqual(BUILD_NVS.canonical_timezone("Europe/London"), "Europe/London")

    def test_worldwide_zones_are_accepted(self):
        # The regression that motivated the full table: a Bolivian on-boarding
        # (and its Brazilian neighbours) hit ESP_ERR_INVALID_ARG on the
        # Europe-only list.
        for zone in ("America/La_Paz", "America/Manaus", "America/Sao_Paulo",
                     "Asia/Kolkata", "Africa/Nairobi", "Pacific/Auckland",
                     "US/Pacific"):
            self.assertEqual(BUILD_NVS.canonical_timezone(zone), zone)

    def test_unknown_timezone_fails_closed(self):
        with self.assertRaisesRegex(ValueError, "unsupported"):
            BUILD_NVS.canonical_timezone("Mars/Olympus")

    def test_collect_values_rejects_invalid_timezone_before_image_generation(self):
        required = {
            env: "/tmp/cert.pem" if kind == "file" else "value"
            for env, _namespace, _key, kind in BUILD_NVS.FIELDS
        }
        required["AMBYTE_TIMEZONE"] = "AMT"
        original = os.environ.copy()
        original_read_pem = BUILD_NVS._read_pem
        try:
            os.environ.clear()
            os.environ.update(required)
            BUILD_NVS._read_pem = lambda _path: "PEM"
            values = BUILD_NVS._collect_values()
            self.assertEqual(
                values[("device_cfg", "timezone")],
                ("string", "Europe/Amsterdam"),
            )
        finally:
            BUILD_NVS._read_pem = original_read_pem
            os.environ.clear()
            os.environ.update(original)

    def test_collect_values_encodes_optional_site_metadata(self):
        required = {
            env: "/tmp/cert.pem" if kind == "file" else "value"
            for env, _namespace, _key, kind in BUILD_NVS.FIELDS
        }
        original = os.environ.copy()
        original_read_pem = BUILD_NVS._read_pem
        try:
            os.environ.clear()
            os.environ.update(required)
            BUILD_NVS._read_pem = lambda _path: "PEM"
            values = BUILD_NVS._collect_values(
                lat=52.173, lon=5.819, deployment="greenhouse-a"
            )
            encoded = values[("device_cfg", "site_state")]
            self.assertEqual(encoded, (
                "hex2bin",
                encode_site_state(52.173, 5.819, "greenhouse-a").hex(),
            ))
            self.assertEqual(
                decode_site_state(bytes.fromhex(encoded[1])).deployment,
                "greenhouse-a",
            )
        finally:
            BUILD_NVS._read_pem = original_read_pem
            os.environ.clear()
            os.environ.update(original)

    def test_collect_values_rejects_oversized_deployment(self):
        with self.assertRaisesRegex(ValueError, "63-byte"):
            BUILD_NVS._collect_values(deployment="x" * 64)


class GeneratedTableContractTest(unittest.TestCase):
    """The three consumers of the generated table must agree."""

    def test_firmware_and_provisioner_supported_zone_sets_match(self):
        firmware_zones = {name for name, _rule, _off in firmware_zone_rows()}
        self.assertGreater(len(firmware_zones), 400, "table looks truncated")
        self.assertEqual(firmware_zones, set(BUILD_NVS.SUPPORTED_TIMEZONES))

    def test_flash_gui_zone_set_mirrors_the_firmware_table(self):
        from flash_gui.tz_zone_table import ZONES
        firmware_zones = {name for name, _rule, _off in firmware_zone_rows()}
        self.assertEqual(set(ZONES), firmware_zones)

    def test_zone_table_is_sorted_for_binary_search(self):
        names = [name for name, _rule, _off in firmware_zone_rows()]
        encoded = [n.encode("ascii") for n in names]
        self.assertEqual(encoded, sorted(encoded),
                         "timezone.c binary-searches k_tz_zones; keep it strcmp-sorted")
        self.assertEqual(len(set(names)), len(names))

    def test_declared_counts_match_the_tables(self):
        text = TZ_TABLE_INC.read_text(encoding="utf-8")
        zone_count = int(re.search(r"#define TZ_TABLE_ZONE_COUNT (\d+)", text).group(1))
        rule_count = int(re.search(r"#define TZ_TABLE_RULE_COUNT (\d+)", text).group(1))
        self.assertEqual(zone_count, len(firmware_zone_rows()))
        self.assertEqual(rule_count, len(firmware_rules()))

    def test_rule_indices_are_in_range(self):
        rules = firmware_rules()
        for name, rule, _off in firmware_zone_rows():
            self.assertLess(rule, len(rules), f"{name} points past the rule pool")

    def test_rules_stay_inside_the_newlib_posix_subset(self):
        """newlib's tzset() silently drops DST on a field it cannot scan.

        Every rule must match the conservative POSIX.1 grammar, and every
        transition time must sit in 00:00..24:00 — the TZif v3 extensions
        (Greenland's /-1, Gaza's /50) are clamped by the generator precisely so
        this holds.
        """
        for rule in firmware_rules():
            self.assertRegex(rule, GEN_TZ._RULE_RE, f"rule outside the subset: {rule}")
            for m in GEN_TZ._TIME_FIELD_RE.finditer(rule):
                self.assertNotEqual(m.group("sign"), "-", f"negative time in {rule}")
                self.assertLessEqual(int(m.group("h")), 24, f"time > 24 h in {rule}")

    def test_names_fit_the_nvs_and_canonicalize_buffers(self):
        for name, _rule, _off in firmware_zone_rows():
            self.assertLessEqual(len(name), 47, f"{name} will not fit NVS/char[48]")
            self.assertTrue(name.isascii(), name)


class GeneratorFreshnessTest(unittest.TestCase):
    """Guards against hand-edits and a stale checked-in table."""

    @staticmethod
    def _has_tzdata() -> bool:
        try:
            import zoneinfo
            return bool(zoneinfo.available_timezones())
        except Exception:
            return False

    def test_checked_in_tables_match_a_fresh_generator_run(self):
        if not self._has_tzdata():
            self.skipTest("no IANA tzdata available to this interpreter")
        # A newer tzdata than the one the tables were generated from is a
        # legitimate difference, not a failure — only flag hand-edits.
        text = TZ_TABLE_INC.read_text(encoding="utf-8")
        stamped = re.search(r'#define TZ_TABLE_TZDATA_VERSION "([^"]+)"', text).group(1)
        if stamped != GEN_TZ._tzdata_version():
            self.skipTest(f"table generated from tzdata {stamped}, host has "
                          f"{GEN_TZ._tzdata_version()}")
        self.assertEqual(GEN_TZ.main(["--check"]), 0,
                         "generated tables are out of date or hand-edited — run "
                         "python tools/gen_tz_table.py")

    def test_fallback_offsets_are_real_offsets_of_their_zone(self):
        """fallback_min is what the firmware schedules on if libc refuses a
        rule, so a sign slip there would go unnoticed until a device drifted by
        twice its offset. Assert it is an offset the zone genuinely uses during
        the generator's reference year -- not which one (Antarctica/Troll swings
        two hours, Africa/Casablanca's "standard" is the POSIX rule's DST).

        CI runs against the system tzdata, which may not be the release the
        table was generated from; a zone that changed its rules in between is a
        legitimate difference, so the check relaxes to a one-hour tolerance
        unless the interpreter's tzdata matches the stamped version exactly."""
        if not self._has_tzdata():
            self.skipTest("no IANA tzdata available to this interpreter")
        stamped = re.search(r'#define TZ_TABLE_TZDATA_VERSION "([^"]+)"',
                            TZ_TABLE_INC.read_text(encoding="utf-8")).group(1)
        exact = stamped == GEN_TZ._tzdata_version()
        import datetime
        import zoneinfo
        available = zoneinfo.available_timezones()
        checked = 0
        for name, _rule, fallback_min in firmware_zone_rows():
            if name not in available:
                continue        # host tzdata older/newer than the table's
            tz = zoneinfo.ZoneInfo(name)
            observed = set()
            for month in range(1, 13):
                for day in (1, 15):
                    moment = datetime.datetime(
                        GEN_TZ.REFERENCE_YEAR, month, day, 12,
                        tzinfo=datetime.timezone.utc).astimezone(tz)
                    offset = moment.utcoffset() or datetime.timedelta(0)
                    observed.add(int(offset.total_seconds()) // 60)
            if exact:
                self.assertIn(fallback_min, observed,
                              f"{name}: fallback {fallback_min} min is not an "
                              f"offset this zone uses ({sorted(observed)})")
            else:
                self.assertLessEqual(
                    min(abs(fallback_min - o) for o in observed), 60,
                    f"{name}: fallback {fallback_min} min is more than an hour "
                    f"from every offset this zone uses ({sorted(observed)})")
            checked += 1
        self.assertGreater(checked, 400, "almost nothing was checked")


@unittest.skipUnless(shutil.which("cc"), "no host C compiler (cc) available")
class FirmwareCanonicalizerTest(unittest.TestCase):
    """Compile the real component on the host and exercise it end to end.

    Host libc is glibc, not newlib, so this proves the plumbing and the rule
    strings (offsets, DST dates), not newlib's parser — hence the on-device
    rule_took_effect() guard in timezone.c.
    """

    def test_actual_c_firmware_canonicalizer(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            (temp / "esp_err.h").write_text(
                "#pragma once\n"
                "typedef int esp_err_t;\n"
                "#define ESP_OK 0\n"
                "#define ESP_ERR_INVALID_ARG 0x102\n"
                "#define ESP_ERR_INVALID_SIZE 0x104\n"
            )
            (temp / "esp_log.h").write_text(
                "#pragma once\n"
                "#define ESP_LOGE(...) do {} while (0)\n"
                "#define ESP_LOGI(...) do {} while (0)\n"
            )
            (temp / "time_sync.h").write_text(
                "#pragma once\n"
                "#include <stdint.h>\n"
                "int32_t time_sync_get_utc_offset_seconds(void);\n"
                "void time_sync_set_utc_offset_seconds(int32_t value);\n"
            )
            (temp / "test.c").write_text(
                '#include <assert.h>\n#include <stdint.h>\n#include <string.h>\n'
                '#include "timezone.h"\n'
                "static int32_t s_fallback = 7200;\n"
                "int32_t time_sync_get_utc_offset_seconds(void) { return s_fallback; }\n"
                "void time_sync_set_utc_offset_seconds(int32_t value) { s_fallback = value; }\n"
                f"#define JAN {JAN_PROBE}LL\n"
                f"#define JUL {JUL_PROBE}LL\n"
                "int main(void) {\n"
                "  char out[48];\n"
                # canonicalization
                '  assert(timezone_canonicalize(" AMT ", out, sizeof(out)) == ESP_OK);\n'
                '  assert(strcmp(out, "Europe/Amsterdam") == 0);\n'
                '  assert(timezone_canonicalize("Z", out, sizeof(out)) == ESP_OK);\n'
                '  assert(strcmp(out, "UTC") == 0);\n'
                '  assert(timezone_canonicalize("Europe/London", out, sizeof(out)) == ESP_OK);\n'
                '  assert(strcmp(out, "Europe/London") == 0);\n'
                '  assert(timezone_canonicalize("America/La_Paz", out, sizeof(out)) == ESP_OK);\n'
                '  assert(strcmp(out, "America/La_Paz") == 0);\n'
                # deprecated aliases are accepted and NOT rewritten
                '  assert(timezone_canonicalize("Asia/Calcutta", out, sizeof(out)) == ESP_OK);\n'
                '  assert(strcmp(out, "Asia/Calcutta") == 0);\n'
                '  assert(timezone_canonicalize("Mars/Olympus", out, sizeof(out)) == ESP_ERR_INVALID_ARG);\n'
                "  assert(out[0] == '\\0');\n"
                '  assert(timezone_canonicalize("Europe/Amsterdam", out, 4) == ESP_ERR_INVALID_SIZE);\n'
                # offsets: fixed-offset zone, DST zone, half-hour zone
                '  timezone_apply("America/La_Paz");\n'
                "  assert(timezone_utc_offset_seconds(JAN) == -4 * 3600);\n"
                "  assert(timezone_utc_offset_seconds(JUL) == -4 * 3600);\n"
                '  timezone_apply("Europe/Amsterdam");\n'
                "  assert(timezone_utc_offset_seconds(JAN) == 3600);\n"
                "  assert(timezone_utc_offset_seconds(JUL) == 2 * 3600);\n"
                '  timezone_apply("Asia/Kolkata");\n'
                "  assert(timezone_utc_offset_seconds(JAN) == 5 * 3600 + 1800);\n"
                '  timezone_apply("Pacific/Auckland");\n'
                "  assert(timezone_utc_offset_seconds(JAN) == 13 * 3600);\n"
                "  assert(timezone_utc_offset_seconds(JUL) == 12 * 3600);\n"
                # an unknown zone leaves the time_sync fallback in charge
                '  timezone_apply("Mars/Olympus");\n'
                "  assert(timezone_utc_offset_seconds(JAN) == s_fallback);\n"
                "  return 0;\n}\n"
            )
            binary = temp / "timezone-test"
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-D_POSIX_C_SOURCE=200809L",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(temp),
                    "-I",
                    str(ROOT / "components/timezone/include"),
                    str(ROOT / "components/timezone/timezone.c"),
                    str(temp / "test.c"),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
