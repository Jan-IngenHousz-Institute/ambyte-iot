"""Run the production heartbeat task with a virtual clock and MQTT transport."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class StatusHeartbeatTest(unittest.TestCase):
    def test_reports_without_charging_or_storage(self):
        packages = Path(os.environ.get("PLATFORMIO_CORE_DIR", str(Path.home() / ".platformio"))) / "packages"
        candidates = sorted(packages.glob("framework-espidf*/components/json/cJSON/cJSON.c"))
        self.assertTrue(candidates, "Install PlatformIO project packages (as CI does) for cJSON")
        cjson = candidates[0]
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "heartbeat"
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                f"-I{ROOT / 'tests/heartbeat_stubs'}", f"-I{ROOT / 'tests/host_stubs'}",
                f"-I{ROOT / 'components/status_heartbeat/include'}",
                f"-I{ROOT / 'components/domain/include'}", f"-I{cjson.parent}",
                str(ROOT / "tests/status_heartbeat_host.c"),
                str(ROOT / "components/status_heartbeat/status_heartbeat.c"),
                str(cjson), "-lm", "-o", str(binary),
            ], check=True)
            for scenario in ("battery", "critical", "charging", "power_error", "mqtt_down",
                             "wifi_down", "reconnect", "publish_failure", "allocation_failure",
                             "sensor_hold", "jitter", "reconnect_jitter", "brief_reconnect", "no_sd_probe"):
                with self.subTest(scenario=scenario):
                    subprocess.run([str(binary), scenario], check=True)


if __name__ == "__main__":
    unittest.main()
