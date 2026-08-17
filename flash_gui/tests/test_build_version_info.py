# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

import pytest

from flash_gui import build_version_info


@pytest.mark.parametrize(
    "version,expected",
    [
        ("0.2.1", (0, 2, 1, 0)),
        ("1.0", (1, 0, 0, 0)),
        ("2.3.4.5", (2, 3, 4, 5)),
        # Tags are cut by hand, so pre-release and local suffixes reach this.
        ("0.2.1-rc1", (0, 2, 1, 0)),
        ("0.2.1+dirty", (0, 2, 1, 0)),
    ],
)
def test_version_tuple_is_always_four_integers(version, expected):
    assert build_version_info.version_tuple(version) == expected


def test_rendered_resource_carries_publisher_metadata():
    rendered = build_version_info.render("0.2.1")
    assert "'Jan Ingenhousz Institute'" in rendered
    assert "'Ambyte Flash GUI'" in rendered
    assert "'Flash and provision ambyte sensor boards'" in rendered
    assert "filevers=(0, 2, 1, 0)" in rendered
    assert "'0.2.1.0'" in rendered


def test_rendered_resource_is_valid_pyinstaller_syntax():
    # PyInstaller eval()s this file against its version-info classes, so a
    # syntax error only surfaces mid-build on Windows. Parse it here instead.
    compile(build_version_info.render("0.2.1"), "version_info.txt", "eval")
