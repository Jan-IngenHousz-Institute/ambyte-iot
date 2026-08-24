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


def test_release_publishes_the_exact_pr_head_artifacts():
    assert "name: Release from main" in WORKFLOW
    assert 'gh pr view "${PR_NUMBER}"' in WORKFLOW
    assert 'gh run watch "${RUN_ID}"' in WORKFLOW
    assert "--exit-status" in WORKFLOW
    assert "actions/runs/${RUN_ID}/artifacts?per_page=100" in WORKFLOW
    for platform in ("linux", "macos", "windows"):
        assert f'("ambyte-flash-gui-{platform}-" + $head)' in WORKFLOW
    assert (
        "pattern: ambyte-flash-gui-*-${{ steps.artifact.outputs.head-sha }}"
        in WORKFLOW
    )
    # Never rebuilt at release time: the bytes published are the bytes the PR
    # built and CI tested.
    assert WORKFLOW.count("pyinstaller ") == 1


def test_pr_build_embeds_and_release_verifies_gui_identity():
    assert "name: Determine expected GUI release tag" in WORKFLOW
    assert "python -m flash_gui.build_release_info" in WORKFLOW
    assert '--source-sha "${SOURCE_SHA}"' in WORKFLOW
    assert '--add-data "release_info.json${SEP}flash_gui"' in WORKFLOW
    assert "name: Verify packaged GUI release identity" in WORKFLOW
    assert "EXPECTED_TAG: ${{ steps.version.outputs.tag }}" in WORKFLOW
    assert "EXPECTED_SOURCE_SHA: ${{ steps.artifact.outputs.head-sha }}" in WORKFLOW
    assert 'endswith(' in WORKFLOW
    assert '"/flash_gui/release_info.json"' in WORKFLOW
    assert "group: flash-gui-release" in WORKFLOW
    assert "cancel-in-progress: false" in WORKFLOW


def test_every_merge_to_main_releases_without_a_hand_cut_tag():
    assert "if: github.event_name == 'push' && github.ref == 'refs/heads/main'" in WORKFLOW
    # A tag trigger would mean a human has to remember to push one.
    assert "tags:" not in WORKFLOW
    assert "startsWith(github.ref, 'refs/tags/" not in WORKFLOW
    assert "name: Determine release tag" in WORKFLOW
    assert "tag_name: ${{ steps.version.outputs.tag }}" in WORKFLOW
    assert "target_commitish: ${{ github.sha }}" in WORKFLOW


def test_release_tag_is_patch_incremented_from_the_highest_existing_tag():
    assert "git ls-remote --tags origin 'refs/tags/flash-gui-v*'" in WORKFLOW
    assert 'NEXT="flash-gui-v${MAJOR}.${MINOR}.$((PATCH + 1))"' in WORKFLOW
    # First release on a repo with no GUI tags yet must not crash.
    assert 'NEXT="flash-gui-v0.1.0"' in WORKFLOW


def test_release_is_never_the_repository_latest():
    # /releases/latest must keep resolving to the newest FIRMWARE release.
    assert "prerelease: true" in WORKFLOW
    assert "make_latest: false" in WORKFLOW
    # Immutable releases: upload while draft, then publish.
    assert "draft: true" in WORKFLOW
    assert "name: Publish GitHub prerelease" in WORKFLOW
    assert "--draft=false" in WORKFLOW


def test_release_verifies_all_platform_files():
    for asset in (
        "ambyte-flash-gui-macos.zip",
        "ambyte-flash-gui-linux.zip",
        "ambyte-flash-gui-windows.zip",
    ):
        assert f"test -s out/{asset}" in WORKFLOW


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
