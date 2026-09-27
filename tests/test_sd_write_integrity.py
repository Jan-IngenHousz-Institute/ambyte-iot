"""Sprint 1 SD write-integrity host tests (contract C-00, C-27..C-35, C-41, C-49).

Builds and runs the pure-C host checks under tests/sdwi_host, the CLI parser
behaviour of the red/green harnesses, the evlog/heartbeat rendering of the new
fields, and static guards (no format call, no SD I/O in sd_diag, power_cut can
never succeed)."""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CC = os.environ.get("CC") or shutil.which("clang") or "clang"
if os.path.basename(CC) == "cc":
    CC = shutil.which("clang") or "clang"
SAN = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
BASE_REV = "e1ca6ee"


def _build(srcs, incs, defs, out: Path) -> Path:
    cmd = [CC, "-std=gnu11", "-g", "-O1", "-Wall", "-Wextra", "-Werror", *SAN, *defs,
           *[f"-I{ROOT / i}" for i in incs], *[str(ROOT / s) for s in srcs], "-lm", "-o", str(out)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise AssertionError(f"compile failed: {' '.join(cmd)}\n{r.stderr}")
    return out


def _checks(stdout: str) -> list[dict]:
    return [json.loads(x) for x in stdout.splitlines() if x.startswith('{"check"')]


class SdDiagCore(unittest.TestCase):
    """C-27, C-28, C-32 (render): the pure sd_diag core."""

    def test_reset_matrix_persist_policy_and_render(self):
        with tempfile.TemporaryDirectory() as d:
            exe = _build(["components/sd_card/sd_diag_core.c", "tests/sdwi_host/sd_diag_host.c"],
                         ["tests/sdwi_host/stubs", "components/sd_card"], [], Path(d) / "diag")
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            checks = _checks(r.stdout)
            self.assertGreaterEqual(len(checks), 17)
            bad = [c["check"] for c in checks if not c["ok"]]
            self.assertEqual(bad, [], r.stdout)
            self.assertEqual(r.returncode, 0)


class SdDiagGlue(unittest.TestCase):
    """C-27/C-28 on the PRODUCTION glue (sd_diag.c): every NVS failure mode stays
    inexact, boot never writes NVS, failed snapshot writes are retried, and a
    heartbeat snapshot racing the forced reboot snapshot can never leave an
    older block durable (eval round 3: whole transaction serialized)."""

    RACE_CHECKS = (
        "race: forced persist waited for the in-flight heartbeat snapshot",
        "race: durable snapshot is the newest gen with all counters",
        "race: no older snapshot landed after a newer one",
        "race: unchanged block after the race is skipped",
        "race/A fails: forced caller wrote the newest block",
        "race/B fails: failed forced write is retried",
    )

    def test_nvs_failure_modes_fail_closed(self):
        with tempfile.TemporaryDirectory() as d:
            exe = _build(["components/sd_card/sd_diag.c", "components/sd_card/sd_diag_core.c",
                          "tests/sdwi_host/sd_diag_glue_host.c"],
                         ["tests/sdwi_host/glue_stubs", "tests/sdwi_host/stubs", "components/sd_card"], ["-pthread"],
                         Path(d) / "glue")
            r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
            checks = _checks(r.stdout)
            self.assertGreaterEqual(len(checks), 28)
            names = {c["check"] for c in checks}
            for want in self.RACE_CHECKS:
                self.assertIn(want, names)
            self.assertEqual([c["check"] for c in checks if not c["ok"]], [], r.stdout)
            self.assertEqual(r.returncode, 0)


class FaultCommand(unittest.TestCase):
    """C-33, C-34, C-35: production evq_hil_trace.c + evq_hil_io.c on the host."""

    def test_parser_nth_count_short_applied_reset(self):
        with tempfile.TemporaryDirectory() as d:
            exe = _build(["components/event_log/evq_hil_trace.c", "components/event_log/evq_hil_io.c",
                          "components/sd_card/sd_diag_core.c", "tests/sdwi_host/fault_io_host.c"],
                         ["tests/sdwi_host/stubs", "components/event_log", "components/event_log/include",
                          "components/sd_card"], ["-DEVQ_HIL_TRACE_HOST", "-DEVQ_HIL_HOST"], Path(d) / "fio")
            st = Path(d) / "state"
            st.mkdir()
            r = subprocess.run([str(exe), str(st)], capture_output=True, text=True)
            checks = _checks(r.stdout)
            self.assertGreaterEqual(len(checks), 9)
            self.assertEqual([c["check"] for c in checks if not c["ok"]], [], r.stdout)
            fired = [ln for ln in r.stdout.splitlines() if ln.startswith("HIL_FAULT fired")]
            reset = [ln for ln in fired if "kind=cpu_reset" in ln]
            self.assertEqual(len(reset), 1, fired)
            self.assertIn("sd_power=not_interrupted mechanism=esp_rom_software_reset_system", reset[0])
            for ln in fired:
                if "kind=cpu_reset" not in ln:
                    self.assertNotIn("sd_power", ln)       # only CPU resets carry the disclaimer

    def test_power_cut_never_succeeds(self):
        src = (ROOT / "components/evq_hil/evq_hil.c").read_text()
        body = src.split('if (strcmp(sub, "power_cut") == 0) {', 1)[1].split("\n    }\n", 1)[0]
        self.assertIn("HIL_ERR power_cut unsupported", body)
        self.assertIn("return 1;", body)
        self.assertNotIn("return 0", body)
        self.assertNotRegex(body, r"\bif\b")                # unconditional: no path to success

    def test_every_reset_announcement_carries_the_disclaimer(self):
        src = (ROOT / "components/event_log/evq_hil_io.c").read_text()
        for m in re.finditer(r'printf\("\\r\\nHIL_FAULT fired[^;]*;', src, re.S):
            self.assertTrue("sd_power=not_interrupted" in m.group(0), m.group(0))


class RenderAndHeartbeat(unittest.TestCase):
    """C-32: pending floors never render as exact-empty; new fields extend (never
    replace) the STATUS schema."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        exe = _build(["components/event_log/evq_render.c", "components/payload_codec/payload_v3.c",
                      "tests/sdwi_host/render_host.c"],
                     ["tests/sdwi_host/stubs", "components/event_log/include", "components/domain/include",
                      "components/payload_codec/include"], [], Path(cls.tmp.name) / "render")
        out = subprocess.run([str(exe)], capture_output=True, text=True, check=True).stdout
        cls.rec = dict(ln.split("=", 1) for ln in out.splitlines())

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_evlog_floor_and_new_counters(self):
        t = self.rec["EVLOG"]
        self.assertIn("pending>=0 pending_exact=0", t)
        self.assertNotRegex(t, r"(^|\s)pending=")          # a floor is never shown as an exact count
        self.assertIn("sd_rename_ambiguous=3 sd_verify_fail=2", t)

    def test_heartbeat_extends_storage(self):
        j = json.loads(self.rec["TELEMETRY"])
        evq = j["health"]["storage"]["evq"]
        self.assertIs(evq["pending_exact"], False)
        for k in ("pending", "refused_full", "refused_media", "refused_too_large", "refused_unavailable",
                  "sd_retired_names", "sd_bad_copies", "blocked_reason", "sd_state"):
            self.assertIn(k, evq)
        self.assertEqual((evq["sd_rename_ambiguous"], evq["sd_verify_fail"]), (3, 2))
        diag = j["health"]["storage"]["sd_diag"]
        self.assertIs(diag["exact"], False)
        self.assertEqual(diag["epoch"], 2)
        self.assertEqual((diag["refused"]["first_id"], diag["refused"]["last_id"]), (100, 106))
        self.assertEqual(j["health"]["storage"]["sdlog"]["rolled_back"], 12)
        nod = json.loads(self.rec["TELEMETRY_NODIAG"])
        self.assertNotIn("sd_diag", nod["health"]["storage"])
        self.assertEqual(set(nod["health"]["storage"]["evq"]), set(evq))


class StaticGuards(unittest.TestCase):
    def _diff(self, *paths) -> str:
        return subprocess.run(["git", "diff", BASE_REV, "--", *paths], cwd=ROOT, capture_output=True, text=True,
                              check=True).stdout

    def test_no_format_introduced_and_mount_never_formats(self):
        added = "\n".join(ln for ln in self._diff("components", "main").splitlines() if ln.startswith("+"))
        for bad in ("f_mkfs", "esp_vfs_fat_sdcard_format", "esp_littlefs_format", "format_if_mount_failed = true"):
            self.assertNotIn(bad, added)
        self.assertIn(".format_if_mount_failed = false", (ROOT / "components/sd_card/sd_card.c").read_text())

    def test_sd_diag_never_touches_the_sd(self):
        for f in ("components/sd_card/sd_diag.c", "components/sd_card/sd_diag_core.c"):
            src = (ROOT / f).read_text()
            for bad in (r"\bfopen\s*\(", r"\bfwrite\s*\(", r"\bfsync\s*\(", r"\brename\s*\(", r"\bremove\s*\(",
                        r"/sdcard", r"SD_MOUNT_POINT", r"\bmkdir\s*\(", r"\bopendir\s*\("):
                self.assertNotRegex(src, bad, f"{f} calls {bad}")

    def test_readers_gain_no_mutating_call(self):
        diff = self._diff("components/evlog_replay", "components/event_log/evlog_inventory.c")
        added = [ln for ln in diff.splitlines() if ln.startswith("+") and not ln.startswith("+++")]
        for ln in added:
            self.assertNotRegex(ln, r"\b(fopen\([^)]*\"[wa]|fwrite|rename|remove|unlink|mkdir|ftruncate)\b", ln)


class CompareSuites(unittest.TestCase):
    """C-40 gate: a pre-existing id that disappears or newly fails must fail it."""

    def _files(self, d, base_ids, br_ids, base_bad=(), br_bad=()):
        def xml(ids, bad):
            cases = "".join(
                f'<testcase classname="{i.split("::")[0]}" name="{i.split("::")[1]}">'
                + ("<failure/>" if i in bad else "") + "</testcase>" for i in ids)
            return f"<testsuites><testsuite>{cases}</testsuite></testsuites>"
        def col(ids):
            return "\n".join(i.replace(".", "/", 1).replace("::", ".py::", 1) for i in ids) + "\n"
        paths = []
        for n, t in (("b.txt", col(base_ids)), ("r.txt", col(br_ids)), ("b.xml", xml(base_ids, base_bad)),
                     ("r.xml", xml(br_ids, br_bad))):
            (Path(d) / n).write_text(t)
            paths.append(str(Path(d) / n))
        return paths

    def _run(self, paths):
        return subprocess.run([sys.executable, "tests/evq_host/compare_suites.py", *paths], cwd=ROOT,
                              capture_output=True, text=True).returncode

    def test_gate(self):
        base = ["tests.test_evq_a::T::x", "tests.test_b::T::y"]
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(self._run(self._files(d, base, base + ["tests.test_new::T::z"])), 0)
            self.assertEqual(self._run(self._files(d, base, base[1:])), 1)                  # evq id vanished
            self.assertEqual(self._run(self._files(d, base, base, br_bad={base[0]})), 1)    # evq id newly fails
            self.assertEqual(self._run([]), 2)


class HarnessCli(unittest.TestCase):
    """C-00: parser behaviour of the new red/green harnesses."""

    SCRIPTS = ["tests/sdlog_host/red_green.py", "tests/ambit_host/red_green.py"]

    def _run(self, *args, timeout=900):
        return subprocess.run([sys.executable, *args], cwd=ROOT, capture_output=True, text=True, timeout=timeout,
                              env=dict(os.environ, CC=CC))

    def test_help_and_bad_arguments(self):
        for s in self.SCRIPTS:
            self.assertEqual(self._run(s, "--help").returncode, 0, s)
            self.assertNotEqual(self._run(s, "--bogus").returncode, 0, s)
            self.assertNotEqual(self._run(s).returncode, 0, s)                     # missing --rev/--out
            with tempfile.TemporaryDirectory() as d:
                self.assertEqual(self._run(s, "--rev", BASE_REV, "--out", d + "/o", "--only", "NOPE").returncode, 2, s)

    def test_red_green_detects_red_and_green(self):
        with tempfile.TemporaryDirectory() as d:
            for s, only in (("tests/sdlog_host/red_green.py", "SL1,SL3"), ("tests/ambit_host/red_green.py", "AO1,AF2")):
                ok = self._run(s, "--rev", BASE_REV, "--out", f"{d}/{Path(s).parent.name}", "--only", only)
                self.assertEqual(ok.returncode, 0, ok.stdout + ok.stderr)
                sc = self._run(s, "--rev", BASE_REV, "--out", f"{d}/{Path(s).parent.name}_sc", "--only", only,
                               "--self-check")
                self.assertNotEqual(sc.returncode, 0, "self-check must detect RED: " + sc.stdout)


if __name__ == "__main__":
    unittest.main()
