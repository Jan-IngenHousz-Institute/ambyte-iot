from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
LUA_WORKFLOW = (ROOT / ".github/workflows/fleet-deploy-lua.yml").read_text(
    encoding="utf-8"
)
PR_WORKFLOW = (ROOT / ".github/workflows/pr.yml").read_text(encoding="utf-8")


class LuaWorkflowTest(unittest.TestCase):
    def test_form_exposes_and_forwards_every_targeting_input(self) -> None:
        inputs = {
            "environment": "fleet-deploy-${{ inputs.environment }}",
            "release_tag": "TAG: ${{ inputs.release_tag }}",
            "version_op": "VERSION_OP: ${{ inputs.version_op }}",
            "version": "VERSION: ${{ inputs.version }}",
            "percentage": "PERCENTAGE: ${{ inputs.percentage }}",
            "devices": "DEVICES: ${{ inputs.devices }}",
            "discovery_window_minutes": (
                "WINDOW_MINUTES: ${{ inputs.discovery_window_minutes }}"
            ),
            "reboot": "REBOOT: ${{ inputs.reboot }}",
            "dry_run": "DRY_RUN: ${{ inputs.dry_run }}",
        }
        for input_name, forwarding in inputs.items():
            with self.subTest(input_name=input_name):
                self.assertIn(f"      {input_name}:\n", LUA_WORKFLOW)
                self.assertIn(forwarding, LUA_WORKFLOW)

        self.assertIn("      dry_run:\n", LUA_WORKFLOW)
        self.assertIn("        default: true\n", LUA_WORKFLOW)
        self.assertIn("environment: fleet-deploy-${{ inputs.environment }}", LUA_WORKFLOW)

    def test_latest_is_lua_scoped_and_release_assets_are_required(self) -> None:
        self.assertIn("gh release list", LUA_WORKFLOW)
        self.assertIn('test(\"^lua-v[0-9]+\\\\.[0-9]+', LUA_WORKFLOW)
        self.assertIn("isDraft == false", LUA_WORKFLOW)
        self.assertIn("isPrerelease == false", LUA_WORKFLOW)
        self.assertIn("grep -qx 'main.lua'", LUA_WORKFLOW)
        self.assertIn("grep -qx 'main.lua.manifest.json'", LUA_WORKFLOW)
        self.assertNotIn("gh release view --repo \"${REPO}\" --json tagName", LUA_WORKFLOW)

    def test_pr_firmware_steps_are_path_gated(self) -> None:
        self.assertIn("node tools/release/pr-build-scope.js", PR_WORKFLOW)
        condition = "if: steps.scope.outputs.firmware-build-required == 'true'"
        self.assertEqual(PR_WORKFLOW.count(condition), 3)
        self.assertIn("python -m unittest discover -s tests -v", PR_WORKFLOW)
        self.assertIn("cp release-preview.md dist/release-preview.md", PR_WORKFLOW)


if __name__ == "__main__":
    unittest.main()
