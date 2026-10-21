"""No local package renames: qualify SDK selection using isolated metadata."""
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from pinned_idf import SOURCES, VERSION, pinned_idf


class PinnedSdkResolverTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.env = patch.dict(os.environ, {"PLATFORMIO_PACKAGES_DIR": str(self.root),
                                          "AMBYTE_REQUIRE_PINNED_IDF": "1"})
        self.env.start(); self.addCleanup(self.env.stop)

    def package(self, name, version=VERSION):
        p = self.root / name; p.mkdir()
        (p / "package.json").write_text(json.dumps(dict(name="framework-espidf", version=version)))
        for source in SOURCES:
            f = p / source; f.parent.mkdir(parents=True, exist_ok=True); f.write_text("qualified fixture")
        return p

    def test_versioned_and_unversioned_layouts_use_metadata(self):
        for name in ("framework-espidf", "framework-espidf@3.50500.0"):
            with self.subTest(name=name):
                p = self.package(name)
                self.assertEqual(pinned_idf(), p)
                (p / "package.json").unlink()

    def test_wrong_version_or_incomplete_sources_fail_when_required(self):
        p = self.package("framework-espidf", "3.50503.0")
        with self.assertRaisesRegex(RuntimeError, "3.50500.0.*coverage is required"):
            pinned_idf()
        (p / "package.json").write_text(json.dumps(dict(name="framework-espidf", version=VERSION)))
        (p / SOURCES[0]).unlink()
        with self.assertRaises(RuntimeError): pinned_idf()

    def test_missing_sdk_is_an_explicit_skip_only_outside_required_gate(self):
        with patch.dict(os.environ, {"AMBYTE_REQUIRE_PINNED_IDF": "0"}):
            with self.assertRaisesRegex(unittest.SkipTest, "framework-espidf@3.50500.0.*missing"):
                pinned_idf()
        with self.assertRaisesRegex(RuntimeError, "coverage is required"):
            pinned_idf()
