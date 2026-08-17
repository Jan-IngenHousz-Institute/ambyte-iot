# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = (ROOT / ".github/workflows/fleet-deploy-ambit.yml").read_text(encoding="utf-8")
ACTION = (ROOT / ".github/actions/fleet-deploy/action.yml").read_text(encoding="utf-8")


def boolean_default(workflow: str, name: str) -> bool:
    block = re.search(rf"(?m)^      {re.escape(name)}:\n((?:        .*\n)+)", workflow)
    if block is None:
        raise AssertionError(f"workflow input {name!r} is missing")
    default = re.search(r"(?m)^        default: (true|false)$", block.group(1))
    if default is None:
        raise AssertionError(f"workflow input {name!r} has no boolean default")
    return default.group(1) == "true"


class AmbitWorkflowTest(unittest.TestCase):
    def test_fixed_public_repo_and_safe_defaults(self) -> None:
        self.assertIn("repository: Jan-IngenHousz-Institute/ambit", WORKFLOW)
        self.assertIn('default: "latest"', WORKFLOW)
        self.assertTrue(boolean_default(WORKFLOW, "dry_run"))
        self.assertFalse(boolean_default(WORKFLOW, "allow_downgrade"))
        self.assertFalse(boolean_default(WORKFLOW, "allow_prerelease"))
        self.assertFalse(boolean_default(WORKFLOW, "force_reflash"))
        self.assertIn("kind: ambit", WORKFLOW)
        self.assertIn("allow-downgrade: ${{ inputs.allow_downgrade }}", WORKFLOW)
        self.assertIn("allow-prerelease: ${{ inputs.allow_prerelease }}", WORKFLOW)
        self.assertIn("force-reflash: ${{ inputs.force_reflash }}", WORKFLOW)

    def test_reuses_protected_environment_oidc_and_global_concurrency(self) -> None:
        self.assertIn("environment: fleet-deploy-${{ inputs.environment }}", WORKFLOW)
        self.assertIn("id-token: write", WORKFLOW)
        self.assertIn("group: fleet-deploy", WORKFLOW)
        self.assertIn("cancel-in-progress: false", WORKFLOW)
        self.assertIn("aws-role-arn: ${{ secrets.AWS_FLEET_DEPLOY_ROLE_ARN }}", WORKFLOW)

    def test_terminal_budget_workflow_and_oidc_duration_are_coherent(self) -> None:
        self.assertIn("timeout-minutes: 90", WORKFLOW)
        self.assertIn('ambit-final-seconds: "3600"', WORKFLOW)
        self.assertIn('default: "3600"', ACTION)
        self.assertIn('ARGS+=(--final-seconds "${AMBIT_FINAL_SECONDS}")', ACTION)
        self.assertIn("role-duration-seconds: 7200", ACTION)

    def test_exposes_and_forwards_shared_targeting_inputs(self) -> None:
        expected = {
            "version_op": "version-op: ${{ inputs.version_op }}",
            "version": "version: ${{ inputs.version }}",
            "percentage": "percentage: ${{ inputs.percentage }}",
            "devices": "devices: ${{ inputs.devices }}",
            "discovery_window_minutes": "discovery-window-minutes: ${{ inputs.discovery_window_minutes }}",
            "dry_run": "dry-run: ${{ inputs.dry_run }}",
        }
        for name, forwarding in expected.items():
            with self.subTest(name=name):
                self.assertIn(f"      {name}:\n", WORKFLOW)
                self.assertIn(forwarding, WORKFLOW)

    def test_shared_action_requires_rest_release_proof_and_ambit_assets(self) -> None:
        self.assertIn("ambit)", ACTION)
        self.assertIn("REQUIRED_ASSETS=(manifest.json)", ACTION)
        self.assertIn('"/repos/${REPO}/releases/tags/${TAG}"', ACTION)
        self.assertIn(".immutable == true", ACTION)
        self.assertIn(".draft == false and .published_at != null", ACTION)
        self.assertIn(".prerelease == false", ACTION)
        self.assertIn('"/repos/${REPO}" --jq', ACTION)
        self.assertIn('X-GitHub-Api-Version: 2022-11-28', ACTION)
        self.assertIn("python tools/fleet_deploy/ambit_deploy.py", ACTION)


if __name__ == "__main__":
    unittest.main()
