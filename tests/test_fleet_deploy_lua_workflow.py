from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
LUA_WORKFLOW = (ROOT / ".github/workflows/fleet-deploy-lua.yml").read_text(
    encoding="utf-8"
)
OTA_WORKFLOW = (ROOT / ".github/workflows/fleet-deploy.yml").read_text(
    encoding="utf-8"
)
DEPLOY_ACTION = (ROOT / ".github/actions/fleet-deploy/action.yml").read_text(
    encoding="utf-8"
)
PR_WORKFLOW = (ROOT / ".github/workflows/pr.yml").read_text(encoding="utf-8")


class LuaWorkflowTest(unittest.TestCase):
    def test_form_exposes_and_forwards_every_targeting_input(self) -> None:
        inputs = {
            "environment": "fleet-deploy-${{ inputs.environment }}",
            "release_tag": "release-tag: ${{ inputs.release_tag }}",
            "version_op": "version-op: ${{ inputs.version_op }}",
            "version": "version: ${{ inputs.version }}",
            "percentage": "percentage: ${{ inputs.percentage }}",
            "devices": "devices: ${{ inputs.devices }}",
            "discovery_window_minutes": (
                "discovery-window-minutes: ${{ inputs.discovery_window_minutes }}"
            ),
            "reboot": "reboot: ${{ inputs.reboot }}",
            "dry_run": "dry-run: ${{ inputs.dry_run }}",
        }
        for input_name, forwarding in inputs.items():
            with self.subTest(input_name=input_name):
                self.assertIn(f"      {input_name}:\n", LUA_WORKFLOW)
                self.assertIn(forwarding, LUA_WORKFLOW)

        self.assertIn("      dry_run:\n", LUA_WORKFLOW)
        self.assertIn("        default: true\n", LUA_WORKFLOW)
        self.assertIn("environment: fleet-deploy-${{ inputs.environment }}", LUA_WORKFLOW)
        self.assertIn("uses: ./.github/actions/fleet-deploy", LUA_WORKFLOW)
        self.assertIn("kind: lua", LUA_WORKFLOW)

    def test_shared_action_scopes_latest_and_requires_kind_assets(self) -> None:
        self.assertIn("gh release list", DEPLOY_ACTION)
        self.assertIn("TAG_PATTERN='^v[0-9]+", DEPLOY_ACTION)
        self.assertIn("TAG_PATTERN='^lua-v[0-9]+", DEPLOY_ACTION)
        self.assertIn("REQUIRED_ASSETS=(firmware.bin)", DEPLOY_ACTION)
        self.assertIn(
            "REQUIRED_ASSETS=(main.lua main.lua.manifest.json)", DEPLOY_ACTION
        )
        self.assertIn("isDraft == false", DEPLOY_ACTION)
        self.assertIn("isPrerelease == false", DEPLOY_ACTION)
        self.assertNotIn(
            'gh release view --repo "${REPO}" --json tagName', DEPLOY_ACTION
        )

    def test_both_visible_workflows_use_the_shared_targeting_action(self) -> None:
        self.assertIn("uses: ./.github/actions/fleet-deploy", OTA_WORKFLOW)
        self.assertIn("kind: ota", OTA_WORKFLOW)
        self.assertIn("allow-downgrade: ${{ inputs.allow_downgrade }}", OTA_WORKFLOW)
        self.assertIn("uses: ./.github/actions/fleet-deploy", LUA_WORKFLOW)
        self.assertIn("kind: lua", LUA_WORKFLOW)
        self.assertIn("reboot: ${{ inputs.reboot }}", LUA_WORKFLOW)
        self.assertIn("python tools/fleet_deploy/fleet_deploy.py", DEPLOY_ACTION)
        self.assertIn("python tools/fleet_deploy/lua_deploy.py", DEPLOY_ACTION)

    def test_pr_firmware_steps_are_path_gated(self) -> None:
        self.assertIn("node tools/release/pr-build-scope.js", PR_WORKFLOW)
        condition = "if: steps.scope.outputs.firmware-build-required == 'true'"
        self.assertEqual(PR_WORKFLOW.count(condition), 3)
        self.assertIn("python -m unittest discover -s tests -v", PR_WORKFLOW)
        self.assertIn("cp release-preview.md dist/release-preview.md", PR_WORKFLOW)


if __name__ == "__main__":
    unittest.main()
