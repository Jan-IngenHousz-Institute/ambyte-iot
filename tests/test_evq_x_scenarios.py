"""Sprint-1 SD write-integrity tooling (tests/evq_host, contract §5.1/§5.3).

C-00 parser behaviour of red_green.py, compare_dups.py and run.py (help exits
0; unknown flags, missing arguments, unknown groups/scenarios exit non-zero),
compare_dups negative cases on synthetic evidence trees, the red/green
pipeline's ability to see RED (--self-check) and to pass a GREEN scenario, the
shim's FAT cross-link model, and a quick X1 head run asserting the C-13 GREEN
signature (retry-before-delivery lifecycle).
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "tests" / "evq_host"
sys.path.insert(0, str(HOST))

BASE_REV = "e1ca6ee"


def _py(*args: str, env: dict | None = None, timeout: int = 900) -> subprocess.CompletedProcess:
    e = dict(os.environ)
    e.setdefault("CC", "clang")
    e.update(env or {})
    return subprocess.run([sys.executable, *args], cwd=ROOT, capture_output=True, text=True, env=e, timeout=timeout)


def _tree(root: Path, rows: dict) -> None:
    for (scn, seed), rec in rows.items():
        d = root / scn / str(seed)
        d.mkdir(parents=True, exist_ok=True)
        (d / "reconcile.json").write_text(json.dumps(rec))


class ParserBehaviour(unittest.TestCase):
    def test_red_green_help_exits_zero(self):
        r = _py("tests/evq_host/red_green.py", "--help")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("--rev", r.stdout)

    def test_red_green_unknown_flag_fails(self):
        r = _py("tests/evq_host/red_green.py", "--rev", BASE_REV, "--out", "/tmp/x", "--bogus")
        self.assertNotEqual(r.returncode, 0)

    def test_red_green_missing_required_fails(self):
        self.assertNotEqual(_py("tests/evq_host/red_green.py").returncode, 0)
        self.assertNotEqual(_py("tests/evq_host/red_green.py", "--rev", BASE_REV).returncode, 0)
        self.assertNotEqual(_py("tests/evq_host/red_green.py", "--out", "/tmp/x").returncode, 0)

    def test_red_green_bad_values_fail(self):
        self.assertNotEqual(_py("tests/evq_host/red_green.py", "--rev", "no-such-rev-xyz", "--out", "/tmp/x").returncode, 0)
        self.assertNotEqual(_py("tests/evq_host/red_green.py", "--rev", BASE_REV, "--out", "/tmp/x",
                                "--only", "X99").returncode, 0)

    def test_compare_dups_help_and_bad_args(self):
        self.assertEqual(_py("tests/evq_host/compare_dups.py", "--help").returncode, 0)
        self.assertNotEqual(_py("tests/evq_host/compare_dups.py").returncode, 0)
        self.assertNotEqual(_py("tests/evq_host/compare_dups.py", "/nonexistent-a", "/nonexistent-b").returncode, 0)
        self.assertNotEqual(_py("tests/evq_host/compare_dups.py", "a", "b", "--bogus").returncode, 0)

    def test_run_unknown_group_or_scenario_fails(self):
        for args in (["--groups", "Y"], ["--groups", "X,Nope"], ["--only", "X99"], ["--only", "A2,Zed"],
                     ["--seeds", "one"]):
            r = _py("tests/evq_host/run.py", *args)
            self.assertNotEqual(r.returncode, 0, args)
            self.assertIn("error", r.stderr.lower(), args)

    def test_run_registers_group_x(self):
        import run as R
        self.assertEqual(R.GROUPS["X"], ["X1", "X2", "X3", "X4", "X5", "X6", "X7", "X8", "X9", "X10"])


class CompareDups(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="cmpdups-", dir=os.environ.get("EVQ_TMPDIR")))
        self.base, self.s1 = self.tmp / "base", self.tmp / "s1"

    def _run(self) -> subprocess.CompletedProcess:
        return _py("tests/evq_host/compare_dups.py", str(self.base), str(self.s1))

    def test_equal_and_s1_only_x_pass(self):
        _tree(self.base, {("A2", 1): {"duplicate_count": 3}, ("B4", 1): {"runs": [{"dups": 2}, {"dups": 1}]}})
        _tree(self.s1, {("A2", 1): {"duplicate_count": 3}, ("B4", 1): {"runs": [{"dups": 1}, {"dups": 1}]},
                        ("X1", 1): {"duplicate_count": 99}})
        r = self._run()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_more_dups_on_s1_fails(self):
        _tree(self.base, {("A2", 1): {"duplicate_count": 3}})
        _tree(self.s1, {("A2", 1): {"duplicate_count": 4}})
        r = self._run()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("WORSE", r.stdout)

    def test_nested_runs_more_dups_fails(self):
        _tree(self.base, {("C5", 7): {"runs": {"a": {"dups": 0}, "b": {"dups": 0}}}})
        _tree(self.s1, {("C5", 7): {"runs": {"a": {"dups": 0}, "b": {"dups": 1}}}})
        self.assertNotEqual(self._run().returncode, 0)

    def test_no_common_scenario_fails(self):
        _tree(self.base, {("A2", 1): {"duplicate_count": 0}})
        _tree(self.s1, {("X1", 1): {"duplicate_count": 0}})
        r = self._run()
        self.assertNotEqual(r.returncode, 0)

    def test_base_scenario_missing_in_s1_fails(self):
        _tree(self.base, {("A2", 1): {"duplicate_count": 0}, ("A2", 7): {"duplicate_count": 0}})
        _tree(self.s1, {("A2", 1): {"duplicate_count": 0}})
        r = self._run()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("MISSING", r.stdout)

    def test_empty_trees_fail(self):
        self.base.mkdir(parents=True)
        self.s1.mkdir(parents=True)
        self.assertNotEqual(self._run().returncode, 0)


class ShimAndScenarios(unittest.TestCase):
    """Driver-level checks on the working tree (clang ASan/UBSan builds)."""

    @classmethod
    def setUpClass(cls):
        os.environ.setdefault("CC", "clang")
        cls.out = Path(tempfile.mkdtemp(prefix="evqx-", dir=os.environ.get("EVQ_TMPDIR")))
        cls._saved_out = os.environ.get("EVQ_OUT")
        os.environ["EVQ_OUT"] = str(cls.out / "evidence")

    @classmethod
    def tearDownClass(cls):
        if cls._saved_out is None:
            os.environ.pop("EVQ_OUT", None)
        else:
            os.environ["EVQ_OUT"] = cls._saved_out

    def test_x1_seed1_head_green_signature(self):
        import scenarios as S
        rec = S.X1(1, kind="head", strict=False)
        sig = rec["signature"]
        # C-13 S1 signature (retry-before-delivery lifecycle)
        self.assertEqual(sig["xlink_unlink_total"], 0, sig["xlink_unlinked"])
        self.assertTrue(sig["tmp_and_dst_retired_as_pair"], sig["xlk_after_fault"])
        self.assertEqual(sig["sd_rename_ambiguous_after_fault"], 1)
        self.assertFalse(sig["dst_zero_filled_after_fault"])
        self.assertEqual(sig["diag"]["faults"], {"evlog.rename": 1})
        self.assertTrue(sig["reboot"]["pending_exact"])
        self.assertEqual(sig["reboot"]["pending"], sig["reboot"]["pending_truth_durable"])
        self.assertFalse([e for e in rec["oracle_errors"] if not e.startswith("[delivered_first]")], rec["oracle_errors"])
        self.assertFalse(rec["red"]["observed"], "head must not show the RED signature")

    def test_xlink_model_zero_fills_the_surviving_twin(self):
        """both_eio leaves a hard-linked twin; unlinking one name logs xlink_unlink
        and zero-fills the survivor (freed FAT clusters)."""
        import scenarios as S
        rec = S.X1(1, kind=f"rev:{BASE_REV}", strict=False)
        sig = rec["signature"]
        self.assertGreaterEqual(sig["xlink_unlink_after_fault"], 1)
        self.assertTrue(sig["dst_zero_filled_after_fault"])
        self.assertTrue(rec["red"]["observed"])


class RedGreenPipeline(unittest.TestCase):
    def test_self_check_detects_red(self):
        out = Path(tempfile.mkdtemp(prefix="rgself-", dir=os.environ.get("EVQ_TMPDIR")))
        r = _py("tests/evq_host/red_green.py", "--rev", BASE_REV, "--out", str(out), "--self-check",
                "--only", "X1", "--seeds", "1")
        self.assertEqual(r.returncode, 0, r.stdout[-3000:] + r.stderr[-3000:])
        summ = json.loads((out / "self_check" / "summary.json").read_text())
        self.assertGreater(summ["failed"], 0)

    def test_green_scenario_passes_red_green(self):
        """X4 (C-16) is RED on the baseline and GREEN on the working tree."""
        out = Path(tempfile.mkdtemp(prefix="rgx4-", dir=os.environ.get("EVQ_TMPDIR")))
        r = _py("tests/evq_host/red_green.py", "--rev", BASE_REV, "--out", str(out), "--only", "X4", "--seeds", "1")
        self.assertEqual(r.returncode, 0, r.stdout[-3000:] + r.stderr[-3000:])
        summ = json.loads((out / "summary.json").read_text())
        rows = {(x["variant"], x["scenario"]): x for x in summ["rows"]}
        self.assertTrue(rows[("base", "X4")]["passed"] and rows[("base", "X4")]["expected"] == "RED")
        self.assertTrue(rows[("head", "X4")]["passed"] and rows[("head", "X4")]["expected"] == "GREEN")
        self.assertTrue((out / "base" / "X4" / "1" / "reconcile.json").exists())
        self.assertTrue((out / "head" / "X4" / "1" / "reconcile.json").exists())


if __name__ == "__main__":
    unittest.main()
