from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
WORKFLOW = ROOT / ".github" / "workflows" / "firmware-pr-gate.yml"


class WorkflowContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = WORKFLOW.read_text(encoding="utf-8")

    def test_stable_check_names_and_exact_trigger_set(self) -> None:
        self.assertIn("name: Conventional PR title", self.text)
        self.assertIn("name: Firmware release candidate", self.text)
        for event in ("opened", "edited", "synchronize", "reopened", "labeled", "unlabeled"):
            self.assertRegex(self.text, rf"(?m)^\s+- {event}$")
        # GitHub cannot filter a pull_request label name in `on`. Both stable
        # checks rerun on every label event so an unrelated skipped run cannot
        # supersede a failed required check; only the helper gives this name meaning.
        self.assertNotIn("github.event.label.name", self.text)

    def test_permissions_are_read_only_and_fork_safe(self) -> None:
        self.assertRegex(self.text, r"(?m)^permissions:\n  contents: read\n  pull-requests: read$")
        self.assertNotIn("pull_request_target", self.text)
        self.assertNotIn("contents: write", self.text)
        self.assertNotIn("pull-requests: write", self.text)
        self.assertNotIn("secrets.", self.text)

    def test_exact_head_checkout_and_separate_fact_fetches(self) -> None:
        self.assertGreaterEqual(
            self.text.count("ref: ${{ github.event.pull_request.head.sha }}"), 2
        )
        self.assertGreaterEqual(self.text.count("fetch-depth: 0"), 2)
        self.assertGreaterEqual(self.text.count("submodules: recursive"), 2)
        self.assertGreaterEqual(self.text.count("persist-credentials: false"), 2)
        self.assertIn('ACTUAL_HEAD_SHA="$(git rev-parse HEAD)"', self.text)
        self.assertIn('fetch --no-tags origin "${BASE_SHA}"', self.text)
        self.assertIn("fetch --force origin 'refs/tags/*:refs/tags/*'", self.text)

    def test_third_party_actions_are_full_sha_pinned(self) -> None:
        uses = re.findall(r"(?m)^\s*uses:\s*([^\s#]+)", self.text)
        external_uses = [value for value in uses if not value.startswith("./")]
        self.assertGreater(len(external_uses), 0)
        for value in external_uses:
            self.assertRegex(value, r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+@[0-9a-f]{40}$")

    def test_one_exact_upload_and_no_ticket_four_selection(self) -> None:
        self.assertEqual(self.text.count("actions/upload-artifact@"), 1)
        self.assertEqual(self.text.count("Upload the one exact candidate artifact"), 1)
        self.assertIn("name: ${{ steps.build.outputs.artifact-name }}", self.text)
        self.assertIn("path: ${{ steps.build.outputs.candidate-path }}", self.text)
        self.assertNotIn("download-artifact", self.text)
        self.assertNotIn("artifact_id", self.text)

    def test_ticket_two_action_and_runner_temp_metadata_are_used(self) -> None:
        self.assertEqual(
            self.text.count("uses: ./.github/actions/build-firmware-candidate"), 1
        )
        self.assertIn('${RUNNER_TEMP}/ambyte-pr-gate-', self.text)
        self.assertIn("--output-dir \"${AMBYTE_PR_GATE_TEMP}/prepared\"", self.text)
        self.assertIn("--repo-root \"${GITHUB_WORKSPACE}\"", self.text)
        self.assertIn("github.workflow_sha", self.text)
        self.assertIn("GITHUB_RUN_ID", self.text)
        self.assertIn("GITHUB_RUN_ATTEMPT", self.text)

    def test_summary_and_non_blocking_updatable_comment_are_present(self) -> None:
        self.assertIn("GITHUB_STEP_SUMMARY", self.text)
        self.assertIn("<!-- ambyte-firmware-release-candidate -->", self.text)
        self.assertIn("--method PATCH", self.text)
        self.assertIn("--method POST", self.text)
        self.assertIn("continue-on-error: true", self.text)


if __name__ == "__main__":
    unittest.main()
