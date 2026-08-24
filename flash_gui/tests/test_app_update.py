# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

import json

import pytest

from flash_gui import app_update, build_release_info, release_fetch


SOURCE_SHA = "a" * 40


def _release(tag: str, *, complete: bool = True, draft: bool = False) -> dict:
    assets = sorted(app_update.GUI_ASSETS)
    if not complete:
        assets.pop()
    return {
        "tag_name": tag,
        "draft": draft,
        # GUI releases intentionally are prereleases so they never replace the
        # newest firmware on the repository landing page.
        "prerelease": True,
        "html_url": f"https://example.test/releases/tag/{tag}",
        "assets": [{"name": name} for name in assets],
    }


def _installed(tag: str) -> app_update.InstalledGuiRelease:
    return app_update.InstalledGuiRelease(
        tag=tag,
        version=tag.removeprefix("flash-gui-v"),
        source_sha=SOURCE_SHA,
    )


def test_picks_highest_complete_published_gui_release():
    releases = [
        _release("flash-gui-v0.2.9"),
        _release("v9.9.9"),
        _release("flash-gui-v0.4.0", complete=False),
        _release("flash-gui-v0.3.0", draft=True),
        _release("flash-gui-v0.2.10"),
    ]
    assert app_update.pick_gui_release(releases)["tag_name"] == (
        "flash-gui-v0.2.10"
    )


@pytest.mark.parametrize(
    ("installed", "expected"),
    [
        (_installed("flash-gui-v0.2.9"), "current"),
        (_installed("flash-gui-v0.2.8"), "outdated"),
        (_installed("flash-gui-v0.3.0"), "ahead"),
        (None, "unknown"),
    ],
)
def test_evaluates_update_state(installed, expected):
    status = app_update.evaluate_update(
        installed, [_release("flash-gui-v0.2.9")]
    )
    assert status.state == expected
    assert status.latest_version == "0.2.9"
    assert status.release_url.endswith("/flash-gui-v0.2.9")


def test_loads_only_valid_ci_release_identity(tmp_path):
    path = tmp_path / "release_info.json"
    build_release_info.write_release_info(
        path, "flash-gui-v1.2.3", SOURCE_SHA
    )
    installed = app_update.load_installed_release(path)
    assert installed == app_update.InstalledGuiRelease(
        tag="flash-gui-v1.2.3", version="1.2.3", source_sha=SOURCE_SHA
    )

    path.write_text(json.dumps(["not", "an", "object"]), encoding="utf-8")
    assert app_update.load_installed_release(path) is None

    path.write_text(
        json.dumps(
            {"schema_version": 1, "tag": "v1.2.3", "source_sha": SOURCE_SHA}
        ),
        encoding="utf-8",
    )
    assert app_update.load_installed_release(path) is None


def test_release_info_generator_rejects_identity_drift():
    assert build_release_info.payload("flash-gui-v1.2.3", SOURCE_SHA) == {
        "schema_version": 1,
        "tag": "flash-gui-v1.2.3",
        "source_sha": SOURCE_SHA,
    }
    with pytest.raises(ValueError, match="invalid GUI release tag"):
        build_release_info.payload("v1.2.3", SOURCE_SHA)
    with pytest.raises(ValueError, match="source SHA"):
        build_release_info.payload("flash-gui-v1.2.3", "abc")


def test_release_fetch_exposes_gui_update_as_release_error(monkeypatch):
    monkeypatch.setattr(release_fetch, "_release_list", lambda log=None: [])
    with pytest.raises(release_fetch.ReleaseError, match="complete flash-gui"):
        release_fetch.fetch_gui_update(None, log=lambda _message: None)


def test_stale_catalog_never_claims_the_gui_is_up_to_date(monkeypatch):
    def stale_list(log):
        log("Cannot reach GitHub. Using the previously cached release list.")
        return [_release("flash-gui-v0.2.9")]

    monkeypatch.setattr(release_fetch, "_release_list", stale_list)
    with pytest.raises(release_fetch.ReleaseError, match="Cannot reach GitHub"):
        release_fetch.fetch_gui_update(
            _installed("flash-gui-v0.2.9"), log=lambda _message: None
        )
