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
            self.assertEqual(manifest["sha256"], digest)
            self.assertEqual(manifest["script_update"]["checksum"], digest)
            self.assertEqual(manifest["script_update"]["script_version"], "1.2.3")
            self.assertEqual(manifest["built_against_fw"], "4.5.6")

    def test_rejects_non_semver(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "source.lua"
            source.write_text("return 42\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                build(source, Path(tmp) / "dist", "latest", "1.0.0", "example/repo")


if __name__ == "__main__":
    unittest.main()
