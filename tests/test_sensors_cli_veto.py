"""Exact CLI functions keep refresh excluded across entire diagnostic sweeps."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_schedule_light_cleanup import function

ROOT = Path(__file__).resolve().parents[1]


class SensorCliVetoTest(unittest.TestCase):
    def test_whole_cli_contexts_and_failure_exits(self):
        cli = (ROOT / "components/CLI/CLI.c").read_text()
        exact = [function(cli, name) for name in ("cli_parse_channel", "cli_cmd_ambit_versions",
                                                "cli_cmd_ambit_probe", "cli_cmd_ambit_dl")]
        with tempfile.TemporaryDirectory(prefix="sensor-cli-veto-") as name:
            tmp = Path(name)
            (tmp / "cli-veto-production.inc").write_text("\n".join(exact))
            includes = [tmp, ROOT / "components/device_commands/include", ROOT / "components/domain/include", ROOT / "components/sensor_inventory/include"]
            build = subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-O1", "-g", "-fsanitize=address,undefined", *(f"-I{p}" for p in includes),
                str(ROOT / "tests/sensors_host/cli_veto_driver.c"),
                str(ROOT / "components/sensor_inventory/sensor_inventory.c"), "-o", str(tmp / "cli")],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([tmp / "cli"], capture_output=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stderr.decode())
            self.assertIn(b"versions=1 probe_all=1 download=1 error_release=1", run.stdout)
