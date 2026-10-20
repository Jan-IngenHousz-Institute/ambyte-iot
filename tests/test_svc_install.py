# SPDX-License-Identifier: GPL-3.0-or-later
"""Whole production installer + compiler + release provider, with SDK ports."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
WORKBOOK = "11111111-2222-4333-8444-555555555555"
FIXTURE = (ROOT / "tests/identity_host/bench.yaml").read_bytes()
OLD = FIXTURE.replace(b"a2-identity-bench", b"previous-schedule")


class ServiceInstallTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.build.name) / "install"
        includes = [ROOT / "tests/identity_host/include", ROOT / "tests/host_stubs"]
        includes += list((ROOT / "components").glob("*/include"))
        cmd = [os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g",
               "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-fsanitize=address,undefined",
               "-fno-omit-frame-pointer", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
               *(f"-I{path}" for path in includes),
               str(ROOT / "tests/svc_host/install_driver.c"),
               str(ROOT / "components/script_update/script_update.c"),
               *(str(p) for p in sorted((ROOT / "components/sched_spec").glob("*.c"))),
               str(ROOT / "components/time_sync/time_sync.c"),
               *(f"-Wl,--wrap={name}" for name in ("open", "fopen", "stat", "remove", "rename")),
               "-lcrypto", "-lm", "-o", str(cls.binary)]
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode: raise RuntimeError(p.stdout + p.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "svc").mkdir()
        (self.root / "svc/stage.bin").write_bytes(FIXTURE)
        (self.root / "schedule.yaml").write_bytes(OLD)
        # The service must never truncate or consume the legacy staging path.
        (self.root / "schedule.yaml.new").write_bytes(b"legacy staged bytes")

    def run_case(self, case="normal", data=FIXTURE, workbook=WORKBOOK, sha=None, size=None, cut=None, root=None):
        root = root or self.root
        p = subprocess.run([str(self.binary), str(root), case, sha or hashlib.sha256(data).hexdigest(),
                            str(len(data) if size is None else size), workbook], capture_output=True, text=True,
                           env={**os.environ, **({"SVC_INSTALL_CUT": str(cut)} if cut else {})})
        if cut and p.returncode == 86: return None
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual((root / "schedule.yaml.new").read_bytes(), b"legacy staged bytes")
        return json.loads(p.stdout)

    def test_real_compile_swap_provenance_and_activation(self):
        result = self.run_case()
        self.assertEqual((result["reason"], result["applied"], result["verified"]), ("", True, True))
        self.assertEqual((result["suspended"], result["resumed"], result["writes"]), (1, 1, 4))
        self.assertEqual((self.root / "schedule.yaml").read_bytes(), FIXTURE)
        self.assertEqual((self.root / "schedule.yaml.bak").read_bytes(), OLD)
        self.assertTrue(self.run_case("check")["verified"])

    def test_rejections_precede_any_install_side_effect(self):
        for case, data, workbook, sha, size, reason in (
            ("normal", FIXTURE, WORKBOOK, "a" * 64, None, "checksum_mismatch"),
            ("normal", b"invalid: [yaml", WORKBOOK, None, None, "compile_failed"),
            ("normal", FIXTURE, "", None, None, "workbook_mismatch"),
            ("normal", FIXTURE, "22222222-2222-4333-8444-555555555555", None, None, "workbook_mismatch"),
            ("normal", FIXTURE, WORKBOOK, None, len(FIXTURE) + 1, "incomplete"),
        ):
            (self.root / "svc/stage.bin").write_bytes(data)
            result = self.run_case(case, data=data, workbook=workbook, sha=sha, size=size)
            self.assertEqual(result["reason"], reason)
            self.assertFalse(result["applied"])
            self.assertEqual((result["suspended"], result["resumed"], result["renames"], result["writes"]), (0, 0, 0, 0))
            self.assertEqual((self.root / "schedule.yaml").read_bytes(), OLD)

    def test_already_active_reports_no_apply_and_no_rotation(self):
        (self.root / "schedule.yaml").write_bytes(FIXTURE)
        result = self.run_case("already_active")
        self.assertEqual((result["reason"], result["applied"], result["verified"]), ("", False, True))
        self.assertEqual((result["suspended"], result["resumed"], result["renames"], result["writes"]), (0, 0, 0, 0))

    def test_unverified_tuple_never_reports_success(self):
        for case in ("fallback", "stale_loaded", "nvs_error", "stop_error"):
            root = self.root / case; shutil.copytree(self.root, root, ignore=shutil.ignore_patterns(*("fallback", "stale_loaded", "nvs_error", "stop_error")))
            result = self.run_case(case, root=root)
            self.assertEqual(result["reason"], "activation_unverified")
            self.assertFalse(result["verified"])
            self.assertEqual((result["suspended"], result["resumed"]), (1, 1))
            if case == "stop_error": self.assertEqual((root / "schedule.yaml").read_bytes(), OLD)

    def test_source_rename_failure_restores_previous_or_reports_uncertainty(self):
        result = self.run_case("rename_error")
        self.assertEqual(result["reason"], "storage_error")
        self.assertFalse(result["applied"])
        self.assertEqual((self.root / "schedule.yaml").read_bytes(), OLD)
        result = self.run_case("rollback_error")
        self.assertEqual(result["reason"], "activation_unverified")
        self.assertFalse(result["verified"])
        self.assertEqual((self.root / "schedule.yaml.bak").read_bytes(), OLD)

    def test_restart_at_each_install_boundary_only_verifies_exact_tuple(self):
        snapshot = self.root / "snapshot"; snapshot.mkdir()
        for name in ("svc", "schedule.yaml", "schedule.yaml.new"):
            source = self.root / name
            if source.is_dir(): shutil.copytree(source, snapshot / name)
            else: shutil.copy2(source, snapshot / name)
        observed = set()
        for cut in range(1, 12):
            case = self.root / f"cut-{cut}"; shutil.copytree(snapshot, case)
            self.run_case(cut=cut, root=case)
            result = self.run_case("check", root=case)
            exact = ((case / "schedule.yaml").exists() and
                     (case / "schedule.yaml").read_bytes() == FIXTURE and (case / "metadata").exists())
            self.assertEqual(result["verified"], exact, cut)
            observed.add(result["verified"])
            self.assertTrue((case / "schedule.yaml.bak").exists() or (case / "schedule.yaml").exists())
        self.assertEqual(observed, {False, True})


if __name__ == "__main__":
    unittest.main()
