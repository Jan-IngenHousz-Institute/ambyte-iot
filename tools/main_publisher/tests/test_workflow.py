from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
WORKFLOW = ROOT / ".github" / "workflows" / "firmware-main-publisher.yml"


class PublisherWorkflowContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = WORKFLOW.read_text(encoding="utf-8")

    def test_trusted_triggers_permissions_and_stable_queue(self) -> None:
        self.assertRegex(self.text, r"(?m)^  push:\n    branches:\n      - main$")
        self.assertIn("workflow_dispatch:", self.text)
        self.assertRegex(
            self.text,
            r"(?m)^permissions:\n  contents: write\n  actions: read\n  pull-requests: read$",
        )
        self.assertRegex(
            self.text,
            r"(?m)^concurrency:\n  group: ambyte-firmware-main-publisher\n  queue: max$",
        )
        self.assertNotIn("cancel-in-progress", self.text)

    def test_manual_retry_requires_all_immutable_identifiers_without_defaults(self) -> None:
        for name in ("target_sha", "pr_workflow_run_id", "artifact_id"):
            self.assertRegex(
                self.text,
                rf"(?m)^      {name}:\n(?:        .+\n)+?        required: true$",
            )
        self.assertNotRegex(self.text, r"(?m)^\s+default:")
        self.assertIn("github.sha", self.text)
        self.assertIn("inputs.target_sha", self.text)

    def test_exact_checkout_no_rebuild_and_local_publisher(self) -> None:
        self.assertIn("ref: ${{ github.event_name == 'workflow_dispatch'", self.text)
        self.assertIn("persist-credentials: false", self.text)
        self.assertIn("refs/remotes/origin/main", self.text)
        self.assertIn("'refs/tags/*:refs/tags/*'", self.text)
        self.assertIn("tools/main_publisher/cli.py", self.text)
        for forbidden in ("pio run", "platformio run", "build-firmware-candidate"):
            self.assertNotIn(forbidden, self.text)

    def test_all_external_actions_are_pinned_by_full_sha(self) -> None:
        uses = re.findall(r"(?m)^\s*uses:\s*([^\s#]+)", self.text)
        self.assertGreater(len(uses), 0)
        for value in uses:
            self.assertRegex(value, r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+@[0-9a-f]{40}$")


if __name__ == "__main__":
    unittest.main()
