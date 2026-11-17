"""Bench BLE diagnostics (docs/bench/ble-diagnostics.md).

The pure core (components/ble_diag/ble_diag_core.c: snapshot double buffer and
seq, page framing, page_offset parsing, meta/live JSON within the contract's
size ceilings) runs under ASan/UBSan via tests/ble_diag_host.c, with the
pinned SDK's cJSON parsing every rendered object. The configuration checks pin
that only the bench-ble env can switch Bluetooth on."""
import configparser
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FLAGS = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
         "-fno-sanitize-recover=all", "-g"]


def compiler():
    return os.environ.get("CC") or shutil.which("clang") or "cc"


def cjson_dir():
    packages = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio")) / "packages"
    sdk = packages / "framework-espidf@3.50500.0"
    if not sdk.exists():
        sdk = packages / "framework-espidf"
    cjson = sdk / "components/json/cJSON"
    if not (cjson / "cJSON.c").exists():
        raise RuntimeError("Pinned ESP-IDF cJSON is required for the BLE diagnostics host test")
    return cjson


class CoreTest(unittest.TestCase):
    def test_core_under_sanitizers(self):
        with tempfile.TemporaryDirectory(prefix="ble-diag-") as tmp:
            binary = Path(tmp) / "ble_diag_host"
            cjson = cjson_dir()
            subprocess.run([compiler(), *FLAGS,
                            f"-I{ROOT / 'components/ble_diag/include'}", f"-I{cjson}",
                            str(ROOT / "tests/ble_diag_host.c"),
                            str(ROOT / "components/ble_diag/ble_diag_core.c"),
                            str(cjson / "cJSON.c"),
                            "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("all checks passed", result.stdout)


class ConfigTest(unittest.TestCase):
    def test_bluetooth_only_in_bench_ble(self):
        ini = configparser.ConfigParser(interpolation=None)
        ini.read(ROOT / "platformio.ini")
        overlays = {}
        for section in ini.sections():
            args = ini[section].get("board_build.cmake_extra_args", "")
            overlays[section] = args
        ble_envs = [s for s, a in overlays.items() if "bench.ble" in a]
        self.assertEqual(ble_envs, ["env:bench-ble"])
        self.assertIn("# CONFIG_BT_ENABLED is not set", (ROOT / "sdkconfig.defaults").read_text())
        overlay = (ROOT / "sdkconfig.bench.ble.defaults").read_text()
        for line in ("CONFIG_BT_ENABLED=y", "CONFIG_BT_NIMBLE_ENABLED=y",
                     "CONFIG_AMBYTE_BLE_DIAG=y", "CONFIG_BT_NIMBLE_NVS_PERSIST=n",
                     "CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=247",
                     "CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1"):
            self.assertIn(line, overlay.splitlines())
        # The checked-in release sdkconfig never carries Bluetooth.
        release = (ROOT / "sdkconfig.esp32-s3-devkitm-1").read_text()
        self.assertIn("# CONFIG_BT_ENABLED is not set", release)
        self.assertNotIn("CONFIG_AMBYTE_BLE_DIAG=y", release)


if __name__ == "__main__":
    unittest.main()
