# SPDX-License-Identifier: GPL-3.0-or-later
"""Worker ownership, persist-intent boundaries and boot recovery in real C."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ServiceEngineTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.build.name) / "engine"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g", "-DSVC_HOST",
            "-I" + str(ROOT / "components/usb_service/include"),
            *(str(ROOT / "components/usb_service" / name) for name in
              ("svc_wire.c", "svc_store.c", "svc_protocol.c", "svc_engine.c")),
            str(ROOT / "tests/svc_host/engine_driver.c"), "-lcrypto", "-o", str(cls.binary),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "svc"

    def run_case(self, scenario, cut=False, root=None):
        p = subprocess.run([str(self.binary), str(root or self.root), scenario], capture_output=True, text=True)
        self.assertEqual(p.returncode, 86 if cut else 0, p.stderr)
        return json.loads(p.stdout) if p.returncode == 0 and p.stdout.startswith("{") else None

    def test_async_deadline_remains_owned_then_finishes_once(self):
        result = self.run_case("schedule")
        self.assertEqual(result["state"], "succeeded")
        self.assertTrue(result["applied"]); self.assertTrue(result["timed_out"])
        self.assertEqual(self.run_case("recover_schedule_match"), result)  # terminal never replayed

    def test_apply_errors_and_unverified_completion(self):
        for case, state, reason in (("apply_error", "failed", "workbook_mismatch"),
                                    ("false_completion", "uncertain", "activation_unverified"),
                                    ("already_active", "succeeded", "already_active")):
            root = Path(self.temp.name) / case
            result = self.run_case(case, root=root)
            self.assertEqual((result["state"], result["reason"]), (state, reason))
            if case == "already_active": self.assertFalse(result["applied"])

    def test_queue_and_maintenance_admission_failures_release_every_path(self):
        for case in ("queue_full", "maintenance_busy", "not_ready"):
            result = self.run_case(case, root=Path(self.temp.name) / case)
            self.assertEqual((result["state"], result["reason"]), ("failed", "busy"))

    def test_wifi_success_and_failure_describe_distinct_restoration(self):
        result = self.run_case("wifi_success")
        self.assertEqual(result["state"], "succeeded")
        self.assertTrue(result["saved"]); self.assertTrue(result["connected"])
        restored = self.run_case("auth_restore", root=Path(self.temp.name) / "restore")
        self.assertEqual((restored["state"], restored["reason"]), ("failed", "wifi_auth_rejected"))
        self.assertFalse(restored["saved"]); self.assertFalse(restored["connected"])
        self.assertTrue(restored["restored"]); self.assertTrue(restored["restoration_connected"])
        uncertain = self.run_case("save_error", root=Path(self.temp.name) / "save")
        self.assertEqual((uncertain["state"], uncertain["reason"]), ("uncertain", "unprovable"))
        self.assertIsNone(uncertain["saved"])

    def test_reboot_before_persist_intent_never_saves_or_replays(self):
        self.run_case("cut_trial", cut=True)
        result = self.run_case("recover_requested")
        self.assertEqual((result["state"], result["reason"]), ("uncertain", "interrupted"))
        self.assertFalse(result["saved"]); self.assertIsNone(result["connected"])

    def test_failed_final_write_retries_observed_result_without_rerunning(self):
        for case in ("final_save_error", "wifi_final_save_error"):
            result = self.run_case(case, root=Path(self.temp.name) / case)
            self.assertEqual(result["state"], "succeeded")
            if case.startswith("wifi"): self.assertTrue(result["saved"])

    def test_reboot_after_persist_intent_reconciles_true_false_unknown(self):
        for case, saved, reason in (("recover_requested", True, "interrupted"),
                                   ("recover_previous", False, "interrupted"),
                                   ("recover_other", None, "unprovable"),
                                   ("recover_unreadable", None, "unprovable")):
            root = Path(self.temp.name) / case
            self.run_case("cut_persist", root=root, cut=True)
            result = self.run_case(case, root=root)
            self.assertEqual((result["state"], result["reason"], result["saved"]), ("uncertain", reason, saved))
            self.assertIsNone(result["connected"])

    def test_interrupted_commit_reconstructed_only_after_boot_gate(self):
        for case, state in (("recover_schedule_match", "succeeded"), ("recover_schedule_other", "uncertain")):
            root = Path(self.temp.name) / case
            self.run_case("queued_schedule", root=root)
            result = self.run_case(case, root=root)
            self.assertEqual(result["state"], state)
            self.assertIsNone(result["applied"])
            self.assertEqual(result["reason"], "" if state == "succeeded" else "interrupted")


if __name__ == "__main__":
    unittest.main()
