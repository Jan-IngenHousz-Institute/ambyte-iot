"""Sprint 1 evidence tooling (SD write-integrity contract §5.1, C-00b, C-36, C-37).

Covers tools/check_evidence_manifest.py (record / consume / verify / compare-runs),
tools/elf_budget.py, tools/check_release_no_hil.py and the byte-identity of
tools/sprint1_evidence.sh with the frozen contract block. Pure stdlib + subprocess; no
board, no pio build, no C compiler.
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
MANIFEST_TOOL = TOOLS / "check_evidence_manifest.py"
BUDGET_TOOL = TOOLS / "elf_budget.py"
NOHIL_TOOL = TOOLS / "check_release_no_hil.py"
SCRIPT = TOOLS / "sprint1_evidence.sh"
CONTRACT = Path("/home/dv/.traycer/epics/630f41e0-ce32-48dc-bd3c-a2e550479725/artifacts/"
                "autobuild/sd-write-integrity/sprint-01/contract/index.md")

sys.path.insert(0, str(TOOLS))
import check_evidence_manifest as cem  # noqa: E402
from check_release_no_hil import SHN_UNDEF, build_elf  # noqa: E402

PY = sys.executable
EMPTY = hashlib.sha256(b"").hexdigest()


def run(*args: str | Path, **kw) -> subprocess.CompletedProcess:
    return subprocess.run([str(a) for a in args], capture_output=True, text=True, **kw)


def mtool(*args: str | Path) -> subprocess.CompletedProcess:
    return run(PY, MANIFEST_TOOL, *args)


def records(manifest: Path) -> list[dict]:
    return [json.loads(l) for l in manifest.read_text().splitlines() if l.strip()]


def contract_block() -> str | None:
    if not CONTRACT.is_file():
        return None
    lines = CONTRACT.read_text(encoding="utf-8").split("\n")
    h = next(i for i, l in enumerate(lines) if l.startswith("### 5.1 Frozen commands"))
    s = next(i for i in range(h, len(lines)) if lines[i] == "```bash")
    e = next(i for i in range(s + 1, len(lines)) if lines[i] == "```")
    return "\n".join(lines[s + 1:e]) + "\n"


# --------------------------------------------------------------------------- CLI surface

class Cli(unittest.TestCase):
    def test_help_exits_zero(self):
        for tool in (MANIFEST_TOOL, BUDGET_TOOL, NOHIL_TOOL):
            r = run(PY, tool, "--help")
            self.assertEqual(r.returncode, 0, (tool, r.stderr))
            self.assertIn("usage", r.stdout.lower())

    def test_bogus_flag_nonzero(self):
        for tool in (MANIFEST_TOOL, BUDGET_TOOL, NOHIL_TOOL):
            r = run(PY, tool, "--definitely-not-a-flag")
            self.assertNotEqual(r.returncode, 0, tool)

    def test_typos_and_missing_mode_args_nonzero(self):
        cases = [
            (MANIFEST_TOOL, ["--recrod", "--manifest", "m"]),
            (MANIFEST_TOOL, ["--verfiy"]),
            (MANIFEST_TOOL, ["--record", "--manifest", "m"]),          # missing --name etc.
            (MANIFEST_TOOL, ["--verify", "--script", "s"]),             # missing --manifest/--out
            (MANIFEST_TOOL, ["--consume", "m", "p"]),                   # no `-- cmd`
            (MANIFEST_TOOL, ["--record", "--verify"]),                  # mutually exclusive
            (MANIFEST_TOOL, []),                                        # no mode
            (BUDGET_TOOL, ["--base", "a", "--head", "b", "--max-flsh", "1", "--max-dram", "1",
                           "--max-rtc-noinit", "1"]),
            (BUDGET_TOOL, ["--base", "a", "--head", "b"]),              # limits required
            (NOHIL_TOOL, ["--expect-wraped", "--elf", "e", "--objdir", "d"]),
            (NOHIL_TOOL, ["--expect", "--elf", "e", "--objdir", "d"]),  # no abbreviations
            (NOHIL_TOOL, ["--elf", "e", "--bin", "b"]),                 # sdkconfig-json required
            (NOHIL_TOOL, ["--expect-wrapped", "--elf", "e"]),           # objdir required
            (NOHIL_TOOL, ["--self-test", "--expect-wrapped"]),
        ]
        for tool, args in cases:
            r = run(PY, tool, *args)
            self.assertNotEqual(r.returncode, 0, (tool.name, args, r.stdout))


# --------------------------------------------------------------------------- the frozen script

class FrozenScript(unittest.TestCase):
    def test_script_byte_identical_to_contract_block(self):
        block = contract_block()
        if block is None:
            self.skipTest(f"contract not present at {CONTRACT}")
        self.assertEqual(SCRIPT.read_bytes(), block.encode("utf-8"),
                         "tools/sprint1_evidence.sh drifted from contract §5.1 (C-00b)")

    def test_script_executable(self):
        self.assertTrue(os.access(SCRIPT, os.X_OK))

    def test_script_passes_static_check_and_lists_steps(self):
        text = SCRIPT.read_text()
        self.assertEqual(cem.static_check(text), [])
        steps = cem.script_steps(text)
        self.assertEqual(steps[0], "init")
        for s in ("pio_rel", "pio_hil", "pio_base", "budget", "nohil", "nohil_st", "hil_wrap"):
            self.assertIn(s, steps)

    def test_precreated_out_aborts_at_mkdir(self):
        # Statically: strict mode on, and $OUT is created WITHOUT -p (so an existing
        # directory is an error, and `set -e` turns it into an abort before any step).
        lines = SCRIPT.read_text().splitlines()
        self.assertIn("set -euo pipefail", lines[:3])
        mk = [l for l in lines if l.startswith('mkdir') and '"$OUT"' in l]
        self.assertEqual(len(mk), 1, mk)
        self.assertTrue(mk[0].startswith('mkdir -m 700 "$OUT"'), mk[0])
        self.assertNotIn("-p", mk[0].split("#")[0])
        first_step = next(i for i, l in enumerate(lines) if l.startswith("step "))
        self.assertLess(lines.index(mk[0]), first_step)
        # Dynamically: a non--p mkdir of an already existing dir under set -e aborts.
        r = run("bash", "-c", 'set -e; d=$(mktemp -d); trap "rmdir $d" EXIT; mkdir -m 700 "$d"; echo REACHED')
        self.assertNotEqual(r.returncode, 0)
        self.assertNotIn("REACHED", r.stdout)


# --------------------------------------------------------------------------- manifest tool

VERIFY_SCRIPT = """#!/usr/bin/env bash
set -euo pipefail
OUT=$1
step init "$OUT" true
step prod "$OUT/a.txt,$OUT/d" true
step cmp "" true
"""


class Manifest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="s1tools-"))
        self.addCleanup(shutil.rmtree, self.root, True)
        self.out = self.root / "run-1"
        self.out.mkdir(mode=0o700)
        self.m = self.out / "manifest.jsonl"
        self.marker = self.root / "cmd-ran"
        self.script = self.root / "evidence.sh"
        self.script.write_text(VERIFY_SCRIPT)

    # helpers -------------------------------------------------------------
    def record(self, name: str, outputs: str, rc: int = 0) -> subprocess.CompletedProcess:
        return mtool("--record", "--manifest", self.m, "--name", name, "--start", "1.0",
                     "--end", "2.0", "--exit", str(rc), "--outputs", outputs)

    def init(self) -> None:
        r = self.record("init", str(self.out))
        self.assertEqual(r.returncode, 0, r.stderr)

    def produce(self, rc: int = 0) -> None:
        (self.out / "a.txt").write_text("alpha\n")
        (self.out / "d" / "sub").mkdir(parents=True)
        (self.out / "d" / "x.c.o").write_bytes(b"obj-x")
        (self.out / "d" / "sub" / "y.c.o").write_bytes(b"obj-y")
        r = self.record("prod", f"{self.out}/a.txt,{self.out}/d", rc)
        self.assertEqual(r.returncode, 0, r.stderr)

    def consume(self, *paths: str | Path) -> subprocess.CompletedProcess:
        return mtool("--consume", self.m, *paths, "--",
                     PY, "-c", f"open({str(self.marker)!r}, 'w').write('x')")

    def assert_refused(self, r: subprocess.CompletedProcess, needle: str) -> None:
        self.assertEqual(r.returncode, 4, r.stdout + r.stderr)
        self.assertFalse(self.marker.exists(), "wrapped comparison ran despite refusal")
        self.assertIn(needle, r.stderr)

    def verify(self) -> subprocess.CompletedProcess:
        return mtool("--verify", "--script", self.script, "--manifest", self.m, "--out", self.out)

    def full_run(self) -> None:
        self.init()
        self.produce()
        r = mtool("--consume", self.m, self.out / "a.txt", self.out / "d", "--", "true")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.record("cmp", "").returncode, 0)

    # record ----------------------------------------------------------------
    def test_record_hashes_files_and_dirs(self):
        self.init()
        self.produce()
        recs = records(self.m)
        self.assertEqual(recs[0]["outputs"], [{"path": str(self.out), "kind": "dir", "sha256": EMPTY}])
        outs = {o["path"]: o for o in recs[1]["outputs"]}
        a = outs[str(self.out / "a.txt")]
        self.assertEqual((a["kind"], a["sha256"]), ("file", hashlib.sha256(b"alpha\n").hexdigest()))
        listing = (f"sub/y.c.o\t{hashlib.sha256(b'obj-y').hexdigest()}\n"
                   f"x.c.o\t{hashlib.sha256(b'obj-x').hexdigest()}\n")
        d = outs[str(self.out / "d")]
        self.assertEqual((d["kind"], d["sha256"]), ("dir", hashlib.sha256(listing.encode()).hexdigest()))
        self.assertEqual(recs[1]["type"], "step")
        self.assertEqual(recs[1]["exit"], 0)

    def test_record_empty_outputs(self):
        self.init()
        self.assertEqual(self.record("cmp", "").returncode, 0)
        self.assertEqual(records(self.m)[-1]["outputs"], [])

    def test_record_missing_output_fails_zero_exit_step(self):
        self.init()
        r = self.record("prod", str(self.out / "never-written.txt"))
        self.assertEqual(r.returncode, 3)
        self.assertEqual(records(self.m)[-1]["missing"], [str(self.out / "never-written.txt")])

    def test_record_missing_output_of_failed_step_keeps_step_rc(self):
        self.init()
        r = self.record("prod", str(self.out / "never-written.txt"), rc=2)
        self.assertEqual(r.returncode, 0)
        self.assertEqual(records(self.m)[-1]["exit"], 2)

    def test_record_firmware_provenance(self):
        self.init()
        (self.out / "s1.id").write_text("1111111111111111111111111111111111111111\ndiff...\n")
        (self.out / "base.id").write_text("e1ca6ee9bb1cecb497014d810966bd4dc4410279\n")
        for name, sub in (("pio_rel", "rel"), ("pio_base", "base")):
            b = self.out / sub
            b.mkdir()
            (b / "firmware.elf").write_bytes(b"elf-" + sub.encode())
            (b / "firmware.bin").write_bytes(b"bin-" + sub.encode())
            self.assertEqual(self.record(name, f"{b}/firmware.elf,{b}/firmware.bin").returncode, 0)
        rel, base = records(self.m)[1]["firmware"], records(self.m)[2]["firmware"]
        self.assertEqual(rel["commit"], "1111111111111111111111111111111111111111")
        self.assertEqual(base["commit"], "e1ca6ee9bb1cecb497014d810966bd4dc4410279")
        self.assertEqual(rel["elf_sha256"], hashlib.sha256(b"elf-rel").hexdigest())
        self.assertEqual(rel["bin_sha256"], hashlib.sha256(b"bin-rel").hexdigest())
        for k in ("pio_version", "platform_espressif32"):
            self.assertTrue(rel[k])

    # consume ---------------------------------------------------------------
    def test_happy_path_record_consume_verify(self):
        self.full_run()
        recs = records(self.m)
        cons = [r for r in recs if r["type"] == "consume"]
        self.assertEqual(len(cons), 1)
        self.assertEqual({i["producer"] for i in cons[0]["inputs"]}, {"prod"})
        self.assertEqual(cons[0]["cmd"], ["true"])
        r = self.verify()
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("VERIFY PASS", r.stdout)

    def test_consume_propagates_command_exit_code(self):
        self.init()
        self.produce()
        r = mtool("--consume", self.m, self.out / "a.txt", "--", PY, "-c", "raise SystemExit(7)")
        self.assertEqual(r.returncode, 7)

    def test_consume_runs_command_when_fresh(self):
        self.init()
        self.produce()
        r = self.consume(self.out / "a.txt")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(self.marker.exists())

    def test_consume_refuses_path_outside_out(self):
        self.init()
        self.produce()
        outside = self.root / "outside.txt"
        outside.write_text("alpha\n")
        self.assert_refused(self.consume(outside), "outside")

    def test_consume_refuses_symlink_escaping_out(self):
        self.init()
        outside = self.root / "outside.txt"
        outside.write_text("x")
        (self.out / "link.txt").symlink_to(outside)
        self.record("prod", str(self.out / "link.txt"))
        self.assert_refused(self.consume(self.out / "link.txt"), "outside")

    def test_consume_refuses_unrecorded_path(self):
        self.init()
        self.produce()
        (self.out / "unrecorded.txt").write_text("x")
        self.assert_refused(self.consume(self.out / "unrecorded.txt"), "not an output")

    def test_consume_refuses_rehashed_file(self):
        self.init()
        self.produce()
        (self.out / "a.txt").write_text("tampered\n")
        self.assert_refused(self.consume(self.out / "a.txt"), "differs")

    def test_consume_refuses_output_of_failed_step(self):
        self.init()
        self.produce(rc=1)
        self.assert_refused(self.consume(self.out / "a.txt"), "not an output")

    def test_consume_refuses_mutated_object_in_recorded_dir(self):
        # C-36 amendment: $HIL is consumed as one directory hash; a changed object file
        # inside it must refuse before the wrapper check runs.
        self.init()
        self.produce()
        (self.out / "d" / "sub" / "y.c.o").write_bytes(b"obj-y-patched")
        self.assert_refused(self.consume(self.out / "d"), "differs")

    def test_consume_refuses_file_added_to_recorded_dir(self):
        self.init()
        self.produce()
        (self.out / "d" / "extra.c.o").write_bytes(b"new")
        self.assert_refused(self.consume(self.out / "d"), "differs")

    def test_consume_refuses_deleted_input(self):
        self.init()
        self.produce()
        (self.out / "a.txt").unlink()
        self.assert_refused(self.consume(self.out / "a.txt"), "no longer exists")

    def test_consume_refuses_missing_manifest(self):
        (self.out / "a.txt").write_text("x")
        self.assert_refused(self.consume(self.out / "a.txt"), "manifest")

    # verify ----------------------------------------------------------------
    def test_verify_rejects_placeholder(self):
        self.full_run()
        self.script.write_text(VERIFY_SCRIPT + 'python tests/evq_host/run.py --out "<BASE EVQ_OUT>"\n')
        r = self.verify()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("placeholder", r.stdout)
        self.script.write_text(VERIFY_SCRIPT + "cat <…>\n")
        self.assertNotEqual(self.verify().returncode, 0)

    def test_verify_allows_heredoc_and_redirections(self):
        text = VERIFY_SCRIPT + "cat <<EOF >/dev/null\nhi\nEOF\nsort <\"$OUT/a\" >\"$OUT/b\"\n"
        self.assertEqual(cem.static_check(text), [])

    def test_verify_rejects_unassigned_variable(self):
        self.full_run()
        self.script.write_text(VERIFY_SCRIPT + 'echo "$FOO"\n')
        r = self.verify()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("$FOO", r.stdout)
        self.assertTrue(cem.static_check('X=1\necho "${BAR:-x}"\n'))
        self.assertTrue(cem.static_check('sh -c "echo \\$INNER"\n'))

    def test_static_check_variable_rules(self):
        ok = ('f() { local n=$1 outs=$2; shift 2; local t0 rc; t0=$(date); rc=$?; echo "$n $outs $t0 $rc $@ $# $$"; }\n'
              'export E=1; echo "$E ${1:-d} $0 $* $! $-"\n'
              'sh -c "test \\"\\$(git rev-parse HEAD)\\" = x"\n'
              'for f in a b; do echo "$f"; done\n')
        self.assertEqual(cem.static_check(ok), [])

    def test_verify_rejects_missing_step(self):
        self.init()
        self.produce()
        # no `cmp` record
        r = self.verify()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("FAIL step cmp", r.stdout)

    def test_verify_rejects_failed_step(self):
        self.init()
        self.produce()
        self.record("cmp", "", rc=1)
        r = self.verify()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("FAIL step cmp", r.stdout)

    def test_verify_rejects_duplicate_step(self):
        self.full_run()
        self.record("cmp", "")
        self.assertNotEqual(self.verify().returncode, 0)

    def test_verify_rejects_missing_init(self):
        self.produce()
        self.record("cmp", "")
        r = self.verify()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("init", r.stdout)

    def test_verify_rejects_reused_out(self):
        # $OUT already held something before init: the listing is not empty.
        (self.out / "stale-from-last-run.elf").write_bytes(b"old")
        self.full_run()
        r = self.verify()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("EMPTY", r.stdout)

    def test_verify_rejects_mutated_consumed_input(self):
        self.full_run()
        self.assertEqual(self.verify().returncode, 0)
        (self.out / "a.txt").write_text("changed after the comparison\n")
        r = self.verify()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("differs", r.stdout)

    def test_verify_rejects_forged_consume_outside_out(self):
        self.full_run()
        outside = self.root / "x.txt"
        outside.write_text("x")
        cem.append_record(str(self.m), {"type": "consume", "cmd": ["true"], "inputs": [
            {"path": str(outside), "sha256": hashlib.sha256(b"x").hexdigest(), "producer": "prod"}]})
        self.assertNotEqual(self.verify().returncode, 0)

    def test_verify_rejects_manifest_outside_out(self):
        self.full_run()
        other = self.root / "run-2"
        other.mkdir()
        r = mtool("--verify", "--script", self.script, "--manifest", self.m, "--out", other)
        self.assertNotEqual(r.returncode, 0)

    # compare-runs ----------------------------------------------------------
    def test_compare_runs_missing_manifests_exit_zero(self):
        r = mtool("--compare-runs", self.root / "nope-a", self.root / "nope-b")
        self.assertEqual(r.returncode, 0)
        self.assertIn("cannot read", r.stdout)


# The Evaluator's rerun case from critique 3: two invocations of the same evidence
# script, each into its own fresh run-* dir. Run 2's "ELF" is back-dated (older mtime
# than its own step start) and hashes differently from run 1. Freshness is proved by
# provenance, not mtimes or cross-run hash equality, so BOTH must pass --verify, and
# --compare-runs only reports the difference.
RERUN_SCRIPT = """#!/usr/bin/env bash
set -euo pipefail
ROLE_DIR=$1
SCRIPT=$2
MODE=${3:-new}
PY=@PY@
TOOL=@TOOL@
OUT="$ROLE_DIR/run-$(date -u +%Y%m%dT%H%M%SZ)-$$"
mkdir -m 700 "$OUT"
MANIFEST="$OUT/manifest.jsonl"
step() { local n=$1 outs=$2; shift 2; local t0 rc; t0=$(date +%s.%N)
  if "$@"; then rc=0; else rc=$?; fi
  "$PY" "$TOOL" --record --manifest "$MANIFEST" --name "$n" --start "$t0" \\
         --end "$(date +%s.%N)" --exit "$rc" --outputs "$outs"
  return "$rc"; }
step init "$OUT" true
step s1_id "$OUT/s1.id" sh -c "echo commit-$MODE > '$OUT/s1.id'"
REL="$OUT/build_rel"
step pio_rel "$REL/firmware.elf,$REL/firmware.bin" sh -c "mkdir -p '$REL' && printf 'ELF-%s' '$MODE' > '$REL/firmware.elf' \\
                && printf 'BIN' > '$REL/firmware.bin' \\
                && if [ '$MODE' = old ]; then touch -d '2001-01-01 00:00:00' '$REL/firmware.elf'; fi"
step budget "" "$PY" "$TOOL" --consume "$MANIFEST" "$REL/firmware.elf" -- true
"$PY" "$TOOL" --verify --script "$SCRIPT" --manifest "$MANIFEST" --out "$OUT"
echo "RUN_DIR=$OUT"
"""


class Rerun(unittest.TestCase):
    def test_evaluator_rerun_critique3(self):
        root = Path(tempfile.mkdtemp(prefix="s1rerun-"))
        self.addCleanup(shutil.rmtree, root, True)
        script = root / "evidence.sh"
        script.write_text(RERUN_SCRIPT.replace("@PY@", PY).replace("@TOOL@", str(MANIFEST_TOOL)))
        script.chmod(0o755)
        self.assertEqual(cem.static_check(script.read_text()), [])
        role = root / "evaluator"
        role.mkdir()

        runs = []
        for mode in ("new", "old"):
            r = run("bash", script, role, script, mode)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("VERIFY PASS", r.stdout)
            runs.append(Path(r.stdout.strip().splitlines()[-1].split("=", 1)[1]))
        self.assertNotEqual(runs[0], runs[1])

        elf1, elf2 = (d / "build_rel" / "firmware.elf" for d in runs)
        rec2 = next(x for x in records(runs[1] / "manifest.jsonl") if x.get("name") == "pio_rel")
        self.assertLess(elf2.stat().st_mtime, float(rec2["start"]),
                        "run 2 ELF must be older than its own step start")
        self.assertNotEqual(hashlib.sha256(elf1.read_bytes()).hexdigest(),
                            hashlib.sha256(elf2.read_bytes()).hexdigest())

        # Both pass --verify independently (re-run from outside, after the fact too).
        for d in runs:
            v = mtool("--verify", "--script", script, "--manifest", d / "manifest.jsonl", "--out", d)
            self.assertEqual(v.returncode, 0, v.stdout)

        c = mtool("--compare-runs", runs[0], runs[1])
        self.assertEqual(c.returncode, 0)
        self.assertIn("pio_rel: elf_sha256 different", c.stdout)

        # And the same run compared with itself reports equal (still exit 0).
        c = mtool("--compare-runs", runs[0], runs[0])
        self.assertEqual(c.returncode, 0)
        self.assertIn("pio_rel: elf_sha256 equal", c.stdout)


# --------------------------------------------------------------------------- elf_budget

def budget_elf(flash: int, data: int, bss: int, rtc: int, **kw) -> bytes:
    secs = [(".flash.text", 1, 0x6, b"\0" * flash),
            (".dram0.data", 1, 0x3, b"\0" * data),
            (".dram0.bss", 8, 0x3, bss),
            (".comment", 1, 0x0, b"not alloc" * 10)]
    if rtc:
        secs.append((".rtc_noinit", 8, 0x3, rtc))
    return build_elf(secs, [("app_main", 1)], **kw)


class Budget(unittest.TestCase):
    def setUp(self):
        self.d = Path(tempfile.mkdtemp(prefix="s1budget-"))
        self.addCleanup(shutil.rmtree, self.d, True)

    def w(self, name: str, data: bytes) -> Path:
        p = self.d / name
        p.write_bytes(data)
        return p

    def budget(self, base: Path, head: Path) -> subprocess.CompletedProcess:
        return run(PY, BUDGET_TOOL, "--base", base, "--head", head, "--max-flash", "8192",
                   "--max-dram", "1024", "--max-rtc-noinit", "256")

    def test_within_limits_passes(self):
        base = self.w("base.elf", budget_elf(10000, 500, 2000, 0))
        # flash counts .dram0.data too (its initialiser is in the image): +7992+200 = +8192.
        head = self.w("head.elf", budget_elf(17992, 700, 2824, 256))
        r = self.budget(base, head)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("PASS flash: base=10500 head=18692 delta=+8192 limit=+8192", r.stdout)
        self.assertIn("PASS dram: base=2500 head=3524 delta=+1024 limit=+1024", r.stdout)
        self.assertIn("PASS rtc_noinit: base=0 head=256 delta=+256", r.stdout)

    def test_flash_over_limit_fails(self):
        base = self.w("base.elf", budget_elf(10000, 0, 0, 0))
        head = self.w("head.elf", budget_elf(10000 + 8193, 0, 0, 0))
        r = self.budget(base, head)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("FAIL flash", r.stdout)

    def test_dram_over_limit_fails(self):
        base = self.w("base.elf", budget_elf(100, 100, 100, 0))
        head = self.w("head.elf", budget_elf(100, 100, 1125, 0))
        r = self.budget(base, head)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("FAIL dram", r.stdout)

    def test_rtc_noinit_over_limit_fails(self):
        base = self.w("base.elf", budget_elf(100, 0, 0, 0))
        head = self.w("head.elf", budget_elf(100, 0, 0, 257))
        r = self.budget(base, head)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("FAIL rtc_noinit", r.stdout)

    def test_shrink_passes(self):
        base = self.w("base.elf", budget_elf(20000, 900, 900, 64))
        head = self.w("head.elf", budget_elf(100, 0, 0, 0))
        self.assertEqual(self.budget(base, head).returncode, 0)

    def test_elf64_and_big_endian_parse_identically(self):
        import elf_budget
        ref = self.w("ref.elf", budget_elf(1234, 56, 78, 90))
        want = elf_budget.metrics(str(ref))
        self.assertEqual(want, {"flash": 1290, "dram": 134, "rtc_noinit": 90})
        for kw in ({"endian": ">"}, {"ei_class": 2}, {"ei_class": 2, "endian": ">"}):
            p = self.w(f"v{len(kw)}{kw.get('endian', '<') == '>'}.elf", budget_elf(1234, 56, 78, 90, **kw))
            self.assertEqual(elf_budget.metrics(str(p)), want, kw)

    def test_unparsable_fails(self):
        good = self.w("good.elf", budget_elf(100, 0, 0, 0))
        junk = self.w("junk.elf", b"this is not an elf file at all")
        trunc = self.w("trunc.elf", budget_elf(100, 0, 0, 0)[:60])
        for bad in (junk, trunc, self.d / "missing.elf"):
            r = self.budget(good, bad)
            self.assertNotEqual(r.returncode, 0, bad)
            r = self.budget(bad, good)
            self.assertNotEqual(r.returncode, 0, bad)


# --------------------------------------------------------------------------- check_release_no_hil

TEXT = (".flash.text", 1, 0x6, b"\x90" * 16)


class NoHil(unittest.TestCase):
    def setUp(self):
        self.d = Path(tempfile.mkdtemp(prefix="s1nohil-"))
        self.addCleanup(shutil.rmtree, self.d, True)
        self.elf = self.w("clean.elf", build_elf([TEXT], [("app_main", 1), ("sd_logger_init", 1)]))
        self.bin = self.w("clean.bin", b"\xe9 release image\0")
        self.cfg = self.w("cfg.json", json.dumps({"AMBYTE_EVQ_HIL": False}).encode())

    def w(self, name: str, data: bytes) -> Path:
        p = self.d / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        return p

    def check(self, elf=None, binf=None, cfg=None) -> subprocess.CompletedProcess:
        return run(PY, NOHIL_TOOL, "--elf", elf or self.elf, "--bin", binf or self.bin,
                   "--sdkconfig-json", cfg or self.cfg)

    def test_self_test_passes(self):
        r = run(PY, NOHIL_TOOL, "--self-test")
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("SELF-TEST PASS", r.stdout)

    def test_clean_passes(self):
        r = self.check()
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_polluted_symbols_fail(self):
        for sym in ("evq_hil_fopen", "evq_arm_fault", "s_evq_tr_state", "my_evq_hil_io_rename"):
            elf = self.w(f"{sym}.elf", build_elf([TEXT], [("app_main", 1), (sym, 1)]))
            r = self.check(elf=elf)
            self.assertNotEqual(r.returncode, 0, sym)
            self.assertIn(sym, r.stdout)

    def test_undefined_hil_symbol_also_fails(self):
        elf = self.w("u.elf", build_elf([TEXT], [("app_main", 1), ("evq_hil_io_fopen_w", SHN_UNDEF)]))
        self.assertNotEqual(self.check(elf=elf).returncode, 0)

    def test_stripped_elf_fails(self):
        elf = self.w("stripped.elf", build_elf([TEXT], None))
        r = self.check(elf=elf)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("symtab", r.stdout)

    def test_polluted_bin_fails(self):
        for s in (b"HIL_FAULT", b"evq_hil", b"fault io", b"power_cut"):
            b = self.w("p.bin", b"\xe9 xx " + s + b" yy\0")
            r = self.check(binf=b)
            self.assertNotEqual(r.returncode, 0, s)

    def test_sdkconfig_json_cases(self):
        cases = {
            "false": (json.dumps({"AMBYTE_EVQ_HIL": False}), True),
            "zero": (json.dumps({"AMBYTE_EVQ_HIL": 0}), True),
            "absent": (json.dumps({"IDF_TARGET": "esp32s3"}), True),
            "true": (json.dumps({"AMBYTE_EVQ_HIL": True}), False),
            "one": (json.dumps({"AMBYTE_EVQ_HIL": 1}), False),
            "y": (json.dumps({"AMBYTE_EVQ_HIL": "y"}), False),
            "null": (json.dumps({"AMBYTE_EVQ_HIL": None}), False),
            "prefixed": (json.dumps({"CONFIG_AMBYTE_EVQ_HIL": True}), False),
            "malformed": ("{\"AMBYTE_EVQ_HIL\": fal", False),
            "not-object": ("[false]", False),
            "empty-file": ("", False),
        }
        for name, (text, want_pass) in cases.items():
            cfg = self.w(f"{name}.json", text.encode())
            r = self.check(cfg=cfg)
            self.assertEqual(r.returncode == 0, want_pass, (name, r.stdout))
        r = self.check(cfg=self.d / "does-not-exist.json")
        self.assertNotEqual(r.returncode, 0)
        unreadable = self.w("binary.json", b"\xff\xfe\x00garbage")
        self.assertNotEqual(self.check(cfg=unreadable).returncode, 0)

    # --expect-wrapped (C-36) --------------------------------------------------
    def wrapped_tree(self, root: str, suffix: str = ".c.obj", unwrapped: str | None = None,
                     skip: str | None = None) -> Path:
        wrapped = build_elf([TEXT], [("w", 1), ("evq_hil_io_fopen_w", SHN_UNDEF), ("fwrite", SHN_UNDEF)],
                            e_type=1)
        # Defined (not undefined) wrapper symbol does not count as a reference.
        defined = build_elf([TEXT], [("w", 1), ("evq_hil_io_fopen_w", 1)], e_type=1)
        for base, comp in (("sd_logger", "sd_logger"), ("ambit_stage", "ambit_ota"),
                           ("ambit_flash_preflight", "ambit_flash")):
            if base == skip:
                continue
            self.w(f"{root}/esp-idf/{comp}/CMakeFiles/__idf_{comp}.dir/{base}{suffix}",
                   defined if base == unwrapped else wrapped)
        return self.d / root

    def expect_wrapped(self, objdir: Path, elf: Path | None = None) -> subprocess.CompletedProcess:
        hil = elf or self.w("hil.elf", build_elf([TEXT], [("evq_hil_io_fopen_w", 1), ("app_main", 1)]))
        return run(PY, NOHIL_TOOL, "--expect-wrapped", "--elf", hil, "--objdir", objdir)

    def test_expect_wrapped_passes(self):
        for suffix in (".c.obj", ".c.o"):
            r = self.expect_wrapped(self.wrapped_tree(f"ok{suffix}", suffix))
            self.assertEqual(r.returncode, 0, r.stdout)

    def test_expect_wrapped_fails_on_unwrapped_object(self):
        r = self.expect_wrapped(self.wrapped_tree("bad", unwrapped="ambit_stage"))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("ambit_stage", r.stdout)

    def test_expect_wrapped_fails_on_missing_object(self):
        r = self.expect_wrapped(self.wrapped_tree("miss", skip="ambit_flash_preflight"))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("ambit_flash_preflight.c.obj", r.stdout)
        self.assertIn("searched", r.stdout)

    def test_expect_wrapped_fails_without_hil_in_elf(self):
        r = self.expect_wrapped(self.wrapped_tree("noelf"), elf=self.elf)
        self.assertNotEqual(r.returncode, 0)

    def test_expect_wrapped_fails_on_missing_objdir(self):
        self.assertNotEqual(self.expect_wrapped(self.d / "nope").returncode, 0)


if __name__ == "__main__":
    unittest.main()
