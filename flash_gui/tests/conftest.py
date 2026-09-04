# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Keep every test out of the developer's real cache.

release_fetch caches the release listing and the immutable Schedule manifests under
the user config dir. Without this, tests both pollute a real machine and leak
into each other: a manifest cached by one test was served to the next, which hid
a validation failure the second test existed to prove.
"""

import pytest

from flash_gui import release_fetch


@pytest.fixture(autouse=True)
def isolated_cache(tmp_path, monkeypatch):
    monkeypatch.setattr(release_fetch, "CACHE_DIR", tmp_path)
    monkeypatch.setattr(release_fetch, "RELEASES_CACHE", tmp_path / "releases.json")
    monkeypatch.setattr(release_fetch, "MANIFEST_CACHE", tmp_path / "manifests.json")
