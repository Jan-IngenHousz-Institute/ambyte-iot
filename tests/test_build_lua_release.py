# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

from __future__ import annotations

import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from tools.build_lua_release import build


class LuaReleaseManifestTest(unittest.TestCase):
    def test_manifest_and_command_match_asset(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "source.lua"
            source.write_text("return 42\n", encoding="utf-8")

            manifest = build(
                source,
                root / "dist",
                "1.2.3",
                "4.5.6",
                "example/ambyte-iot",
            )

            asset = (root / "dist/main.lua").read_bytes()
            stored = json.loads((root / "dist/main.lua.manifest.json").read_text())
            digest = hashlib.sha256(asset).hexdigest()
            self.assertEqual(stored, manifest)
            self.assertEqual(manifest["script_name"], "main")
            self.assertEqual(manifest["sha256"], digest)
            self.assertEqual(manifest["script_update"]["checksum"], digest)
            self.assertEqual(manifest["script_update"]["script_version"], "1.2.3")
            self.assertEqual(manifest["built_against_fw"], "4.5.6")

    def test_builds_named_script_with_distinct_asset_and_campaign(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "experiment.lua"
            source.write_text("return 31\n", encoding="utf-8")

            manifest = build(
                source,
                root / "dist",
                "2.0.0",
                "4.5.6",
                "example/ambyte-iot",
                "legacy_1Hz_spec.lua",
            )

            self.assertEqual(manifest["script_name"], "legacy_1Hz_spec")
            self.assertEqual(
                manifest["script_update"]["id"],
                "lua-v2.0.0:legacy_1Hz_spec",
            )
            self.assertTrue((root / "dist/legacy_1Hz_spec.lua").is_file())
            self.assertTrue(
                (root / "dist/legacy_1Hz_spec.lua.manifest.json").is_file()
            )

    def test_rejects_non_semver(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "source.lua"
            source.write_text("return 42\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                build(source, Path(tmp) / "dist", "latest", "1.0.0", "example/repo")

    def test_rejects_unsafe_asset_name(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "source.lua"
            source.write_text("return 42\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                build(
                    source,
                    Path(tmp) / "dist",
                    "1.0.0",
                    "1.0.0",
                    "example/repo",
                    "../main.lua",
                )


if __name__ == "__main__":
    unittest.main()
