"""Run the production wifi_manager reconnect policy against host stubs.

Regression for the 2026-09-28 DEV bench strand: one reason=202 AUTH_FAIL on a
boot-time join from stored (correct) credentials stopped reconnection for good;
and a wifi_join on an unassociated station left the reconfigure flag armed so a
later real disconnect (BEACON_TIMEOUT) was swallowed with no reconnect.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

SCENARIOS = (
    "classification",
    "delay_schedule",
    "boot_authfail_recovers",
    "wrong_password_bounded",
    "mixed_reasons",
    "established_auth_leave",
    "non_auth_unchanged",
    "join_wrong_password",
    "join_timeout_and_success",
    "ssid_32_chars",
    "join_unassociated_then_beacon_timeout",
    "join_unassociated_connect_fails",
    "join_while_connected",
)


class WifiManagerReconnectPolicyTest(unittest.TestCase):
    def test_reconnect_policy(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "wifi_manager_host"
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                f"-I{ROOT / 'tests/wifi_manager_stubs'}",
                f"-I{ROOT / 'components/wifi_manager'}",
                f"-I{ROOT / 'components/wifi_manager/include'}",
                str(ROOT / "tests/wifi_manager_host.c"),
                "-o", str(binary),
            ], check=True)
            for scenario in SCENARIOS:
                with self.subTest(scenario=scenario):
                    subprocess.run([str(binary), scenario], check=True)


if __name__ == "__main__":
    unittest.main()
