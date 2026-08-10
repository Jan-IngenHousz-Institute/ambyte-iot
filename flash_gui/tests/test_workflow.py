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
    assert 'ARTIFACT_NAME="ambyte-flash-gui-linux-${HEAD_SHA}"' in WORKFLOW
    assert ".workflow_run.head_sha == $head" in WORKFLOW
    assert (
        "pattern: ambyte-flash-gui-*-${{ steps.artifact.outputs.head-sha }}"
        in WORKFLOW
    )
    assert "name: ambyte-flash-gui-${{ github.sha }}" in WORKFLOW


def test_tag_release_downloads_promoted_main_artifact():
    assert "name: Locate promoted main artifact" in WORKFLOW
    assert 'ARTIFACT_NAME="ambyte-flash-gui-${COMMIT_SHA}"' in WORKFLOW
    assert "name: ${{ steps.artifact.outputs.name }}" in WORKFLOW
    assert "needs: build\n    runs-on" not in WORKFLOW


def test_every_promotion_stage_verifies_all_platform_files():
    for asset in (
        "ambyte-flash-gui-macos",
        "ambyte-flash-gui-linux",
        "ambyte-flash-gui-windows.exe",
    ):
        assert WORKFLOW.count(f"test -s out/{asset}") >= 2


def test_gui_workflow_is_not_a_firmware_release_or_build_path():
    assert 'candidate === ".github/workflows/flash-gui-build.yml"' in PATH_SCOPE
