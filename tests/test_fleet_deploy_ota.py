import unittest

from tools.fleet_deploy import fleet_deploy


class TerminalProofTest(unittest.TestCase):
    def setUp(self) -> None:
        self.target = fleet_deploy.parse_version("v1.6.3")

    def reason(self, *, ack=False, state=None, fw=None):
        return fleet_deploy.deployment_failure_reason(
            {"ack": ack, "state": state, "detail": None, "fw": fw},
            self.target,
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


if __name__ == "__main__":
    unittest.main()
