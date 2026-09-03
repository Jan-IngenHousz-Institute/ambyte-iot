# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

from flash_gui import build_version_info


def test_rendered_resource_carries_publisher_metadata():
    rendered = build_version_info.render()
    assert "'Jan Ingenhousz Institute'" in rendered
    assert "'Ambyte Flash GUI'" in rendered
    assert "'Flash and provision ambyte sensor boards'" in rendered


def test_rendered_resource_claims_no_version():
    # The release job publishes a promoted artifact instead of rebuilding at tag
    # time, so any version compiled in here could not be corrected to match the
    # tag. Absent beats wrong.
    rendered = build_version_info.render()
    assert "FileVersion" not in rendered
    assert "ProductVersion" not in rendered
    assert "filevers=(0, 0, 0, 0)" in rendered


def test_no_hand_maintained_version_constant():
    # It sat at 0.1.0 across two releases because nothing read it.
    import flash_gui

    assert not hasattr(flash_gui, "__version__")


def test_rendered_resource_is_valid_pyinstaller_syntax():
    # PyInstaller eval()s this file against its version-info classes, so a
    # syntax error only surfaces mid-build on Windows. Parse it here instead.
    compile(build_version_info.render(), "version_info.txt", "eval")
