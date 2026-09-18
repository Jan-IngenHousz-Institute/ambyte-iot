"""Run the production heartbeat task with a virtual clock and MQTT transport."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class StatusHeartbeatTest(unittest.TestCase):
    def test_deadlines_and_reconnect_liveness(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "heartbeat"
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                f"-I{ROOT / 'tests/heartbeat_stubs'}", f"-I{ROOT / 'tests/host_stubs'}",
                f"-I{ROOT / 'components/status_heartbeat/include'}",
                f"-I{ROOT / 'components/domain/include'}",
                str(ROOT / "tests/status_heartbeat_host.c"),
                str(ROOT / "components/status_heartbeat/status_heartbeat.c"),
                "-o", str(binary),
            ], check=True)
            for scenario in ("healthy", "mqtt_down",
                             "wifi_down", "reconnect", "publish_failure", "allocation_failure",
                             "sensor_hold", "jitter", "reconnect_jitter", "brief_reconnect",
                             "churn450", "churn899"):
                with self.subTest(scenario=scenario):
                    subprocess.run([str(binary), scenario], check=True)


if __name__ == "__main__":
    unittest.main()
