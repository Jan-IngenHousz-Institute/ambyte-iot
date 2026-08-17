# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

import unittest

from tools.fleet_deploy import release_selection


class ReleaseSelectionTest(unittest.TestCase):
    def test_selects_highest_stable_semver_not_most_recent_publish(self) -> None:
        releases = [
            {
                "tagName": "v1.4.1",
                "isDraft": False,
                "isPrerelease": False,
                "publishedAt": "2026-01-01T00:00:00Z",
            },
            {
                "tagName": "v1.2.9",
                "isDraft": False,
                "isPrerelease": False,
                "publishedAt": "2026-02-01T00:00:00Z",
            },
            {
                "tagName": "lua-v9.0.0",
                "isDraft": False,
                "isPrerelease": False,
            },
        ]
        self.assertEqual(release_selection.select_latest(releases, "ota"), "v1.4.1")

    def test_release_families_and_prereleases_cannot_cross(self) -> None:
        releases = [
            {"tagName": "v9.0.0", "isDraft": False, "isPrerelease": False},
            {
                "tagName": "lua-v1.1.0-rc.1",
                "isDraft": False,
                "isPrerelease": True,
            },
            {
                "tagName": "lua-v1.0.0",
                "isDraft": False,
                "isPrerelease": False,
            },
        ]
        self.assertEqual(
            release_selection.select_latest(releases, "lua"), "lua-v1.0.0"
        )
        with self.assertRaises(ValueError):
            release_selection.parse_tag("lua-v01.0.0", "lua")
        with self.assertRaises(ValueError):
            release_selection.parse_tag("lua-v1.0.0.x", "lua")

    def test_ambit_uses_v_tags_and_latest_is_stable_only(self) -> None:
        releases = [
            {"tagName": "v1.1.1", "isDraft": False, "isPrerelease": False},
            {
                "tagName": "v1.1.2-recovery.1",
                "isDraft": False,
                "isPrerelease": True,
            },
            {"tagName": "lua-v9.0.0", "isDraft": False, "isPrerelease": False},
        ]
        self.assertEqual(
            release_selection.select_latest(releases, "ambit"), "v1.1.1"
        )
        self.assertEqual(
            release_selection.parse_tag("v1.1.2-recovery.1", "ambit"),
            release_selection.parse_tag("v1.1.2-recovery.1", "ota"),
        )
        with self.assertRaisesRegex(ValueError, "no published stable ambit release"):
            release_selection.select_latest([], "ambit")


if __name__ == "__main__":
    unittest.main()
