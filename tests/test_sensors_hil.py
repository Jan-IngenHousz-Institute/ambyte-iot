"""Bench trigger uses the real recovery path and is absent from release."""
from pathlib import Path
import subprocess
import tempfile
import sys
import unittest
from test_sensors_uart import build_uart

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from check_release_no_hil import elf_symbols


class SensorRecoveryHilTest(unittest.TestCase):
    def test_flagged_production_path_and_release_binary_isolation(self):
        self.assertNotIn("AMBYTE_SENSOR_RECOVERY_HIL", (ROOT / "sdkconfig.defaults").read_text())
        kconfig = (ROOT / "components/sensor_inventory/Kconfig.projbuild").read_text()
        self.assertIn("default n", kconfig)
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "hil"
            build_uart(binary, ("-DCONFIG_AMBYTE_SENSOR_RECOVERY_HIL=1",))
            run = subprocess.run([binary], capture_output=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stderr.decode())
            self.assertIn(b"RECOVERY_HIL_OK idle_blocks=20 boot_idle=20 active_silent=1", run.stdout)
            names = {name for name, _ in elf_symbols(str(binary))}
            self.assertIn("sensor_inventory_recovery_hil", names)
            build_uart(binary)
            self.assertNotIn("sensor_inventory_recovery_hil", {name for name, _ in elf_symbols(str(binary))})
        elf = ROOT / ".pio/build/esp32-s3-devkitm-1/firmware.elf"
        if elf.exists():
            for symbol in ("sensor_inventory_recovery_hil", "cli_cmd_sensor_recovery_hil"):
                self.assertNotIn(symbol, {name for name, _ in elf_symbols(str(elf))})
            self.assertNotIn(b"sensors_recovery_hil", elf.read_bytes())
        # The existing post-build PR gate also scans the real release image's
        # symbols, bytes and effective config. Pollution is a hard failure.
        self.assertIn('"recovery_hil"', (ROOT / "tools/check_release_no_hil.py").read_text())


if __name__ == "__main__":
    unittest.main()
