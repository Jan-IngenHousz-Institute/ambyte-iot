# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Timezone provisioning and firmware-contract regression tests."""

import importlib.util
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location("build_nvs_image", TOOLS / "build_nvs_image.py")
assert SPEC is not None and SPEC.loader is not None
BUILD_NVS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD_NVS)


class TimezoneProvisioningTest(unittest.TestCase):
    def test_legacy_alias_and_whitespace_are_canonicalized(self):
        self.assertEqual(BUILD_NVS.canonical_timezone(" AMT "), "Europe/Amsterdam")
        self.assertEqual(BUILD_NVS.canonical_timezone("Z"), "UTC")

    def test_supported_iana_zone_is_preserved(self):
        self.assertEqual(BUILD_NVS.canonical_timezone("Europe/London"), "Europe/London")

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
            os.environ.clear()
            os.environ.update(original)

    def test_firmware_and_provisioner_supported_zone_sets_match(self):
        source = (ROOT / "components/timezone/timezone.c").read_text()
        table = source.split("k_zones[] = {", 1)[1].split("};", 1)[0]
        firmware_zones = set(re.findall(r'\{\s*"([^"]+)"\s*,\s*"[^"]+"\s*\}', table))
        self.assertEqual(firmware_zones, BUILD_NVS.SUPPORTED_TIMEZONES)

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
                '#include <assert.h>\n#include <stdint.h>\n#include <string.h>\n#include "timezone.h"\n'
                "int32_t time_sync_get_utc_offset_seconds(void) { return 7200; }\n"
                "void time_sync_set_utc_offset_seconds(int32_t value) { (void)value; }\n"
                "int main(void) {\n"
                "  char out[48];\n"
                '  assert(timezone_canonicalize(" AMT ", out, sizeof(out)) == ESP_OK);\n'
                '  assert(strcmp(out, "Europe/Amsterdam") == 0);\n'
                '  assert(timezone_canonicalize("Z", out, sizeof(out)) == ESP_OK);\n'
                '  assert(strcmp(out, "UTC") == 0);\n'
                '  assert(timezone_canonicalize("Europe/London", out, sizeof(out)) == ESP_OK);\n'
                '  assert(strcmp(out, "Europe/London") == 0);\n'
                '  assert(timezone_canonicalize("Mars/Olympus", out, sizeof(out)) == ESP_ERR_INVALID_ARG);\n'
                "  assert(out[0] == '\\0');\n"
                '  assert(timezone_canonicalize("Europe/Amsterdam", out, 4) == ESP_ERR_INVALID_SIZE);\n'
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
