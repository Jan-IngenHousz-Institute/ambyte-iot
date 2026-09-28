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
    "retry_connect_error_reschedules",
    "inline_connect_error_no_recursion",
    "connect_error_exhausts_budget",
    "join_connect_error_clears_flag",
    "join_silent_timeout_retries",
    "stale_callback_vs_new_join",
    "dispatched_callback_after_new_join",
    "timer_create_failure_terminal",
    "timer_lazy_create_recovers",
    "timer_start_failure_terminal",
    "boot_initial_connect_error_recovers",
    "set_config_password_error_distinct",
    "late_event_of_old_attempt_after_join",
    "old_dispatch_vs_new_arm",
    "join_wait_races_got_ip",
    "join_wait_races_association_dhcp_pending",
    "join_associated_dhcp_slow",
    "join_auth_reject_timer_failure",
    "join_silent_timeout_timer_failure",
    "old_outcome_after_drain_timeout",
    "silent_original_retry_then_join",
    "stale_got_ip_during_join",
    "stale_connected_after_deferred_join",
    "stored_reentry_does_not_overwrite",
    "kick_outcome_before_relock",
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
            listed = subprocess.run([str(binary), "--list"], check=True,
                                    capture_output=True, text=True).stdout.split()
            self.assertEqual(sorted(listed), sorted(SCENARIOS),
                             "SCENARIOS and the C registry have drifted")
            for scenario in SCENARIOS:
                with self.subTest(scenario=scenario):
                    subprocess.run([str(binary), scenario], check=True)


if __name__ == "__main__":
    unittest.main()
