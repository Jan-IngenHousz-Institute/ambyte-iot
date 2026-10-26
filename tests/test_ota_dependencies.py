"""OTA host coverage follows the shared SDK gate, not local install spelling."""
import importlib.util
import json
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from pinned_idf import SOURCES, VERSION

ROOT = Path(__file__).resolve().parents[1]


class AdmissionDependenciesTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.packages = Path(self.temporary.name)
        # Discovery/import must work even before dependency qualification.
        spec = importlib.util.spec_from_file_location(
            "ota_admission_dependency_fixture", ROOT / "tests/test_ota_admission.py")
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)
        self.compile = patch.object(self.module.subprocess, "run",
                                    return_value=SimpleNamespace(returncode=0, stderr=""))
        self.compiler = self.compile.start()
        self.addCleanup(self.compile.stop)
        self.addCleanup(self.cleanup_admission)

    def cleanup_admission(self):
        temporary = getattr(self.module.AdmissionTest, "temp", None)
        if temporary is not None:
            temporary.cleanup()

    def setup_admission(self, required):
        with patch.dict(os.environ, {"PLATFORMIO_PACKAGES_DIR": str(self.packages),
                                     "AMBYTE_REQUIRE_PINNED_IDF": required}):
            self.module.AdmissionTest.setUpClass()

    def test_missing_optional_sdk_skips_before_compilation(self):
        with self.assertRaisesRegex(unittest.SkipTest, "framework-espidf@3.50500.0.*missing"):
            self.setup_admission("0")
        self.compiler.assert_not_called()

    def test_missing_required_sdk_fails_before_compilation(self):
        with self.assertRaisesRegex(RuntimeError, "coverage is required"):
            self.setup_admission("1")
        self.compiler.assert_not_called()

    def test_qualified_metadata_selects_sdk_without_a_version_txt(self):
        package = self.packages / "framework-espidf-ci-layout"
        package.mkdir()
        (package / "package.json").write_text(json.dumps({"name": "framework-espidf", "version": VERSION}))
        for source in SOURCES:
            path = package / source
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("qualified SDK fixture")
        self.setup_admission("1")
        self.assertIn(str(package / "components/json/cJSON/cJSON.c"), self.compiler.call_args.args[0])
