# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = (ROOT / ".github/workflows/flash-gui-build.yml").read_text(
    encoding="utf-8"
)
PATH_SCOPE = (ROOT / "tools/release/path-scoped.js").read_text(encoding="utf-8")


def test_builds_only_for_pull_requests_and_manual_runs():
    condition = (
        "if: github.event_name == 'pull_request' || "
        "github.event_name == 'workflow_dispatch'"
    )
    assert WORKFLOW.count(condition) == 2
    assert (
        "name: ambyte-flash-gui-${{ matrix.label }}-${{ env.SOURCE_SHA }}"
        in WORKFLOW
    )


def test_main_promotes_the_exact_pr_head_artifacts():
    assert "name: Promote PR executables" in WORKFLOW
    assert 'gh pr view "${PR_NUMBER}"' in WORKFLOW
    assert 'gh run watch "${RUN_ID}"' in WORKFLOW
    assert "--exit-status" in WORKFLOW
    assert 'actions/runs/${RUN_ID}/artifacts?per_page=100' in WORKFLOW
    for platform in ("linux", "macos", "windows"):
        assert f'("ambyte-flash-gui-{platform}-" + $head)' in WORKFLOW
    assert (
        "pattern: ambyte-flash-gui-*-${{ steps.artifact.outputs.head-sha }}"
        in WORKFLOW
    )
    assert "name: ambyte-flash-gui-${{ github.sha }}" in WORKFLOW


def test_tag_release_downloads_promoted_main_artifact():
    assert "name: Locate promoted main artifact" in WORKFLOW
    assert 'ARTIFACT_NAME="ambyte-flash-gui-${COMMIT_SHA}"' in WORKFLOW
    assert "name: ${{ steps.artifact.outputs.name }}" in WORKFLOW
    assert "draft: true" in WORKFLOW
    assert "name: Publish GitHub prerelease" in WORKFLOW
    assert 'gh release edit "${GITHUB_REF_NAME}"' in WORKFLOW
    assert "--draft=false" in WORKFLOW
    assert "needs: build\n    runs-on" not in WORKFLOW


def test_every_promotion_stage_verifies_all_platform_files():
    for asset in (
        "ambyte-flash-gui-macos.zip",
        "ambyte-flash-gui-linux.zip",
        "ambyte-flash-gui-windows.zip",
    ):
        assert WORKFLOW.count(f"test -s out/{asset}") >= 2


def test_build_is_onedir_not_onefile():
    # The onefile bootloader unpacks a PE payload to %TEMP% at runtime, which is
    # what Defender's ML models score as a dropper.
    assert "pyinstaller --noconfirm --clean --onedir --windowed" in WORKFLOW
    executable = [
        line for line in WORKFLOW.splitlines() if not line.lstrip().startswith("#")
    ]
    assert not [line for line in executable if "--onefile" in line]


def test_windows_build_embeds_publisher_metadata():
    assert "python -m flash_gui.build_version_info version_info.txt" in WORKFLOW
    assert '"${VERSION_FILE[@]}"' in WORKFLOW


def test_windows_bundle_is_authenticode_signed():
    assert "uses: azure/artifact-signing-action@v2" in WORKFLOW
    assert "files-folder-filter: exe,dll,pyd" in WORKFLOW
    # An expired certificate invalidates every past release without this.
    assert "timestamp-rfc3161: http://timestamp.acs.microsoft.com" in WORKFLOW
    # Publishing an unsigned or broken-signature bundle must fail the job, and
    # a missing signing config must be loud rather than silent.
    assert "refusing to publish" in WORKFLOW
    assert "::warning title=Unsigned release::" in WORKFLOW


def test_checksums_are_generated_after_signing():
    sign = WORKFLOW.index("name: Repack signed Windows bundle")
    checksums = WORKFLOW.index("name: Generate checksums")
    release = WORKFLOW.index("name: Create GitHub release with executables")
    assert sign < checksums < release
    assert "sha256sum ambyte-flash-gui-*.zip > SHA256SUMS" in WORKFLOW


def test_gui_workflow_is_not_a_firmware_release_or_build_path():
    assert 'candidate === ".github/workflows/flash-gui-build.yml"' in PATH_SCOPE
