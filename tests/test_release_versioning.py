# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
PR_WORKFLOW = (ROOT / ".github/workflows/pr.yml").read_text(encoding="utf-8")


class ReleaseVersioningContractTest(unittest.TestCase):
    def test_no_manual_firmware_version_file(self) -> None:
        self.assertFalse(
            (ROOT / "version.txt").exists(),
            "semantic-release is the firmware version authority; do not add version.txt",
        )

    def test_cmake_uses_ci_override_then_idf_git_fallback(self) -> None:
        self.assertIn('set(PROJECT_VER "$ENV{AMBYTE_PROJECT_VER}")', CMAKE)
        self.assertIn("ESP-IDF's git-derived version", CMAKE)
        self.assertNotIn("version.txt", CMAKE)

    def test_pr_installs_imported_tooling_dependencies_before_tests(self) -> None:
        dependency_install = (
            "python -m pip install --disable-pip-version-check "
            "-r flash_gui/requirements.txt"
        )
        self.assertIn(dependency_install, PR_WORKFLOW)
        self.assertLess(
            PR_WORKFLOW.index(dependency_install),
            PR_WORKFLOW.index("python -m unittest discover -s tests -v"),
        )

    def test_pr_build_injects_semantic_release_preview(self) -> None:
        self.assertIn(
            "AMBYTE_PROJECT_VER: ${{ steps.preview.outputs.firmware-build-version }}",
            PR_WORKFLOW,
        )
        self.assertIn("App version: ${FIRMWARE_VERSION}", PR_WORKFLOW)


if __name__ == "__main__":
    unittest.main()
