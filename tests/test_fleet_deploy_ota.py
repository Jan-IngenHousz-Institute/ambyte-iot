import unittest
from unittest import mock

from tools.fleet_deploy import fleet_deploy


class TerminalProofTest(unittest.TestCase):
    def setUp(self) -> None:
        self.target = fleet_deploy.parse_version("v1.6.3")

    def reason(self, *, ack=False, state=None, fw=None, verified=None):
        return fleet_deploy.deployment_failure_reason(
            {"ack": ack, "state": state, "detail": None, "fw": fw},
            self.target,
            verified,
        )

    def test_only_success_with_exact_target_version_passes(self) -> None:
        self.assertIsNone(self.reason(ack=True, state="success", fw="1.6.3"))
        self.assertEqual(
            self.reason(ack=True, state="success", fw=None),
            "target_version_not_confirmed",
        )
        self.assertEqual(
            self.reason(ack=True, state="success", fw="1.6.2"),
            "target_version_not_confirmed",
        )

    def test_every_unresolved_or_negative_terminal_is_a_failure(self) -> None:
        self.assertEqual(self.reason(ack=False), "no_reply")
        self.assertEqual(self.reason(ack=True), "accepted_no_final")
        self.assertEqual(self.reason(ack=True, state="dropped"), "dropped")
        self.assertEqual(self.reason(ack=True, state="failed"), "failed")

    def test_fresh_target_ping_recovers_only_missing_terminal_proof(self) -> None:
        self.assertIsNone(self.reason(ack=False, verified="1.6.3"))
        self.assertIsNone(self.reason(ack=True, verified="1.6.3"))
        self.assertEqual(
            self.reason(ack=True, state="success", fw=None, verified="1.6.3"),
            "target_version_not_confirmed",
        )
        self.assertEqual(
            self.reason(ack=True, verified="1.6.2"),
            "post_verify_target_mismatch",
        )
        self.assertEqual(
            self.reason(
                ack=True, state="success", fw="1.6.3", verified="1.6.2"
            ),
            None,
        )

    def test_fresh_ping_never_masks_explicit_failure_or_rollback(self) -> None:
        self.assertEqual(
            self.reason(ack=True, state="failed", verified="1.6.3"),
            "failed",
        )
        self.assertEqual(
            self.reason(ack=True, state="dropped", verified="1.6.3"),
            "dropped",
        )

    def test_unparseable_effect_proof_fails_closed(self) -> None:
        self.assertEqual(
            self.reason(ack=True, verified="unknown"),
            "accepted_no_final",
        )


class EffectPollingTest(unittest.TestCase):
    def setUp(self) -> None:
        self.target = fleet_deploy.parse_version("v1.6.3")
        self.results = {
            "AMBYTE_00:11:22:33:44:55": {
                "ack": True,
                "state": None,
                "detail": None,
                "fw": None,
            }
        }

    def test_old_version_is_retried_until_target_is_proven(self) -> None:
        with mock.patch.object(
            fleet_deploy,
            "fleet_ping",
            side_effect=[
                {"AMBYTE_00:11:22:33:44:55": "1.6.1"},
                {"AMBYTE_00:11:22:33:44:55": "1.6.3"},
            ],
        ) as ping, mock.patch.object(fleet_deploy.time, "sleep"):
            verified, attempts = fleet_deploy.verify_missing_by_effect(
                object(), self.results, self.target,
                fleet_deploy.time.time() + 60, 1, 0,
            )
        self.assertEqual(ping.call_count, 2)
        self.assertEqual(verified, {"AMBYTE_00:11:22:33:44:55": "1.6.3"})
        self.assertEqual(len(attempts), 2)

    def test_verify_exception_preserves_fail_closed_missing_proof(self) -> None:
        with mock.patch.object(
            fleet_deploy, "fleet_ping", side_effect=RuntimeError("offline")
        ):
            verified, attempts = fleet_deploy.verify_missing_by_effect(
                object(), self.results, self.target,
                fleet_deploy.time.time() - 1, 1, 0,
            )
        self.assertEqual(verified, {})
        self.assertEqual(attempts[0]["error"], "RuntimeError: offline")
        self.assertEqual(
            fleet_deploy.deployment_failure_reason(
                self.results["AMBYTE_00:11:22:33:44:55"],
                self.target,
                verified.get("AMBYTE_00:11:22:33:44:55"),
            ),
            "accepted_no_final",
        )


if __name__ == "__main__":
    unittest.main()
