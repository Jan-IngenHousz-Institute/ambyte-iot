"""Sprint 2 sd_logger verification-trace host tests (contract a722b3f2 §2.1):
ARM-1 (H8b auto-arm), QUI-1 (H7 quiesce on the logger side + single-exit
resume), SDE-1 (C/Python pad twins), SDE-2 (exact record length R(u)), SLT-1
(replay reproduces the harness's byte-exact files).

All run the PRODUCTION components/sd_logger/sd_logger.c in the tests/sdlog_host
lock-step harness, built with its CONFIG_AMBYTE_EVQ_HIL hooks and the host
variant of components/event_log/evq_hil_sdl.c."""
from __future__ import annotations

import base64
import hashlib
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
sys.path.insert(0, str(ROOT / "tests" / "sdlog_host"))
sys.path.insert(0, str(ROOT / "tools" / "evq_hil"))
import sdlog_lib as L  # noqa: E402

_EXE: dict[str, Path] = {}


def _exe(tmp: Path) -> Path:
    if "hil" not in _EXE:
        _EXE["hil"] = L.build(tmp / "build", None, hil=True)
    return _EXE["hil"]


def _entries(trace: list[str]) -> list[list[str]]:
    return [ln.split() for ln in trace if ln.startswith("SLT_E ")]


def _writer(ents):
    return [e for e in ents if e[1] not in ("P", "POP")]


class _Base(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._tmp = Path(tempfile.mkdtemp(prefix="sdlhil-"))
        cls.exe = _exe(cls._tmp)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls._tmp, ignore_errors=True)
        _EXE.clear()

    def dev(self, name: str) -> L.Dev:
        d = L.new_dev(self.exe, self._tmp / name, "hil")
        d.start()
        return d


class Arm1(_Base):
    """H8b: a valid auto-arm flag starts the trace inside sd_logger_init, before
    the writer's first OPEN; it is one-shot and ignored after a power-on."""

    def test_autoarm_traces_first_open_and_is_one_shot(self):
        d = self.dev("arm")
        d.cmd("power_on 0", expect=True)
        d.cmd("autoarm", expect=True)
        d.init()
        for i in range(3):
            d.log(i)
        d.step(12)
        d.cmd("trace_drain", expect=True)
        self.assertTrue(any(re.match(r"^SLT_ON \d+ \d+ autoarm$", ln) for ln in d.trace), d.trace[:5])
        w = _writer(_entries(d.trace))
        self.assertTrue(w, d.trace)
        self.assertEqual(w[0][1], "OPEN", w[:3])               # nothing the writer did escaped the trace
        n_on = sum(ln.startswith("SLT_ON ") for ln in d.trace)
        d.cmd("trace_off", expect=True)
        d.cmd("boot_hook", expect=True)                        # flag already consumed
        self.assertEqual(sum(ln.startswith("SLT_ON ") for ln in d.trace), n_on)
        d.finish()

    def test_autoarm_ignored_after_power_on(self):
        d = self.dev("arm_po")
        d.cmd("power_on 1", expect=True)
        d.cmd("autoarm", expect=True)
        d.init()
        d.step(4)
        d.cmd("trace_drain", expect=True)
        self.assertFalse(any(ln.startswith("SLT_ON ") for ln in d.trace), d.trace)
        self.assertEqual(_entries(d.trace), [])
        d.finish()


class Qui1(_Base):
    """H7 (logger side): after the pause handshake the writer reports paused and
    performs NO file op; producers keep pushing, overflow is dropped whole,
    counted in dropped_ring_bytes and traced as `P dropped`; resume reopens (OPEN
    traced)."""

    def test_pause_holds_writer_counts_overflow_and_resumes(self):
        d = self.dev("qui")
        d.cmd("trace_on", expect=True)
        d.init()
        for i in range(3):
            d.log(i)
        d.step(12)
        a0 = d.acct()["acct"]
        d.cmd("pause", expect=True)
        self.assertEqual(d.cmd("paused", expect=True)["paused"], 1)
        ops0 = d.opcount()
        for i in range(3, 160):                                 # ~32 KiB >> 8 KiB ring
            d.log(i, 200)
        d.step(20)
        self.assertEqual(d.opcount(), ops0)                     # no FS op while paused
        self.assertEqual(d.cmd("paused", expect=True)["paused"], 1)
        a1 = d.acct()["acct"]
        d.cmd("resume")
        d.step(30)
        d.cmd("trace_drain", expect=True)
        d.finish()
        ents = _entries(d.trace)
        dropped = [e for e in ents if e[1] == "P" and e[6] == "dropped"]
        self.assertTrue(dropped)
        self.assertEqual(sum(int(e[4]) for e in dropped), a1["dropped_ring_bytes"] - a0["dropped_ring_bytes"])
        # writer events after the pause: CLOSE (pause drain) ... then OPEN on resume
        w = _writer(ents)
        closes = [i for i, e in enumerate(w) if e[1] == "CLOSE"]
        self.assertTrue(closes)
        self.assertTrue(any(e[1] == "OPEN" for e in w[closes[-1]:]), w[-6:])

    def test_quiesce_resumes_on_every_exit_path(self):
        """Static single-exit proof for evq_hil_sdlog_cmd / hil_sdl_quiesce."""
        src = (ROOT / "components/evq_hil/evq_hil_sdlog.c").read_text()
        q = src.split("static int hil_sdl_quiesce(", 1)[1].split("\n}\n", 1)[0]
        # the only post-pause failure path resumes before returning
        self.assertRegex(q, r"if \(!sd_logger_hil_paused\(\)\) \{\s*sd_logger_resume\(\);")
        self.assertEqual(q.count("sd_logger_pause()"), 1)
        body = src.split("int evq_hil_sdlog_cmd(", 1)[1]
        blk = body.split("if (hil_sdl_quiesce(&gen) == 0) {", 1)[1].split("}", 1)[0]
        self.assertIn("hil_sdl_resume();", blk)
        self.assertNotIn("return", blk)                          # no early exit between pause and resume


class Sde1(unittest.TestCase):
    """SDE-1: the device pad generator (evq_hil_pad) and the host twin agree."""

    def test_pad_twins(self):
        import hilpay
        cc = L.cc()
        with tempfile.TemporaryDirectory() as d:
            main = Path(d) / "m.c"
            main.write_text('#include <stdio.h>\n#include "evq_hil_payload.h"\n'
                            'int main(void){ const char *runs[]={"s2-20260927-L0-01","x","a.b_c-9"};\n'
                            ' char out[200]; for(int r=0;r<3;r++) for(unsigned k=100000;k<100050;k++) for(unsigned p=0;p<=160;p+=40){\n'
                            '  if(evq_hil_pad(runs[r],k,p,out,sizeof out)!=p) return 1; printf("%s %u %u %s\\n",runs[r],k,p,out);} return 0; }\n')
            exe = Path(d) / "m"
            subprocess.run([cc, "-std=gnu11", "-O1", f"-I{ROOT / 'components/evq_hil/include'}",
                            str(ROOT / "components/evq_hil/evq_hil_payload.c"), str(main), "-o", str(exe)], check=True)
            out = subprocess.run([str(exe)], capture_output=True, text=True, check=True).stdout.splitlines()
        self.assertEqual(len(out), 3 * 50 * 5)
        for ln in out:
            run, k, p, *rest = ln.split(" ")
            self.assertEqual(rest[0] if rest else "", hilpay.pad(run, int(k), int(p)), ln)


class Sde2(_Base):
    """SDE-2: R(u) = 21 + 3 + u + 12 + len(run) + 1 + 6 + 1 + pad + 1 is the exact
    framed length the production sink produces for `W (<u digits>) HILSDLOG: ...`."""

    def test_record_length_formula(self):
        d = self.dev("sde2")
        d.cmd("trace_on", expect=True)
        d.init()
        run, k, pad = "s2-20260927-L0-01", 100000, 160
        import hilpay
        p = hilpay.pad(run, k, pad)
        for u in (7, 8, 9):
            uptime = "1" + "0" * (u - 1)
            d.logf(f"W ({uptime}) HILSDLOG: %s\n".encode(), f"{run} {k} {p}".encode())
        d.step(12)
        d.cmd("trace_drain", expect=True)
        d.finish()
        lens = [int(e[4]) for e in _entries(d.trace) if e[1] == "P" and e[7] == "1"]
        want = [21 + 3 + u + 12 + len(run) + 1 + 6 + 1 + pad + 1 for u in (7, 8, 9)]
        self.assertEqual(lens, want)


class _TracedDev(L.Dev):
    """A harness device whose trace records from before the writer's first OPEN and is
    drained after every step (the host analogue of the 20 s rolling drains)."""

    def start(self) -> None:
        logs = self.state / "sdcard" / "logs"
        self.i0 = {p.name: p.read_bytes() for p in logs.iterdir()} if logs.exists() else {}
        super().start()
        self.cmd("trace_on", expect=True)

    def step(self, n: int = 1) -> None:
        super().step(n)
        self.cmd("trace_drain", expect=True)

    def finish(self) -> None:
        if self.proc and self.proc.poll() is None:
            self.cmd("trace_drain", expect=True)
        super().finish()


def _shim_fired(dev: L.Dev) -> list[dict]:
    """The harness shim's own injections, in the HIL_FAULT-fired shape the replay takes
    as step/mode hints (on the device these come from the fired lines)."""
    out = []
    for o in dev.ops():
        inj = o.get("inj") or ""
        op = {"ftruncate": "truncate", "rename": "rename", "remove": "remove"}.get(o["op"])
        if op and inj in ("eio", "enospc", "applied_eio"):
            out.append({"op": op, "path": o["path"], "mode": inj})
    return out


class Slt1(_Base):
    """SLT-1: for every Sprint-1 sd_logger scenario SL1..SL21, the replay model applied
    to the recorded trace reproduces every log file the harness left, byte for byte."""

    def test_replay_reproduces_every_scenario_byte_exact(self):
        import sdlog_check as C
        import sdlog_scenarios as S
        results = {}
        for name, fn in S.SCENARIOS.items():
            dev = _TracedDev(exe=self.exe, state=self._tmp / f"slt1-{name}", variant="hil")
            if dev.state.exists():
                shutil.rmtree(dev.state)
            dev.state.mkdir(parents=True)
            fn(dev)
            logs = dev.state / "sdcard" / "logs"
            actual = {p.name: p.read_bytes() for p in logs.iterdir()} if logs.exists() else {}
            try:
                pred, _ev = C.replay_model(dev.i0, dev.trace, fired=_shim_fired(dev))
            except ValueError as e:
                results[name] = f"VOID {e}"
                continue
            diff = sorted(n for n in set(pred) | set(actual) if pred.get(n) != actual.get(n))
            results[name] = "OK" if not diff else f"DIFF {diff}"
        bad = {k: v for k, v in results.items() if v != "OK"}
        self.assertEqual(len(results), 21)
        self.assertEqual(bad, {}, results)


if __name__ == "__main__":
    unittest.main()
