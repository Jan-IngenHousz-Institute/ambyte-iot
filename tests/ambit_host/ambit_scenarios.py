"""AMBIT scenarios (contract §5.4 C-22..C-26, §5.4a C-55..C-57).

Each returns (checks, base_red). base_red is True when the baseline reproduces
the defect (the pre-existing image at /sdcard/ambit_fw.bin no longer reachable,
or an SD op outside the io gate), None when the contract claims nothing for it."""
from __future__ import annotations

import hashlib
import os

import ambit_lib as L

IMG = 5000
EINVAL_STATE = 0x103


def _sentinel(dev) -> str:
    data = os.urandom(3000)
    (dev.state / "sdcard" / "ambit_fw.bin").write_bytes(data)
    return hashlib.sha256(data).hexdigest()


def _final_sha(dev) -> str | None:
    p = dev.state / "sdcard" / "ambit_fw.bin"
    return hashlib.sha256(p.read_bytes()).hexdigest() if p.exists() else None


def _stages(dev) -> list[str]:
    return sorted(n for n in os.listdir(dev.state / "sdcard") if n.startswith("ambit_fw.stg-"))


def _status(dev) -> dict:
    return dev.cmd("status")


def _fault(op: str, mode: str, sub: str | None = None, expect_mismatch: bool = False, diag_op: str | None = None):
    def run(dev):
        h0 = _sentinel(dev)
        dev.start()
        dev.cmd(f"source {IMG}")
        arm = dev.cmd(f"arm {op} {mode} 1 1" + (f" {sub}" if sub else ""))
        assert arm["arm"] == "ok", arm
        r = dev.cmd("download")
        st = _status(dev)
        dev.finish()
        base = "why" not in r
        final = _final_sha(dev)
        base_red = final != h0 if base else None
        if base:
            return [("download ran", "rc" in r, r)], base_red
        c = [("C-22 no success reported", r["rc"] != 0, r),
             ("C-22 H0 still at /sdcard/ambit_fw.bin", final == h0, (final, h0)),
             ("C-12 every SD op inside the io gate", st["violations"] == 0 and st["refs"] == 0, st)]
        if expect_mismatch:
            c.append(("C-22 mismatch evidence kept", r["why"] == "readback_mismatch" and len(_stages(dev)) == 1,
                      (r["why"], _stages(dev))))
        else:
            c.append(("C-22 no stage left behind", _stages(dev) == [], _stages(dev)))
        if diag_op:
            c.append(("C-29 diag counted", st["diag"]["faults"].get(f"ambit_ota.{diag_op}", 0) >= 1, st["diag"]))
        return c, base_red
    return run


def _reset(op: str, mode: str, sub: str | None = None):
    """CPU reset (process exit 86) at a write-path boundary, then reboot and a
    fresh healthy attempt: H0 intact throughout; the stale stage is never opened
    for streaming and is cleaned by the next attempt."""
    def run(dev):
        h0 = _sentinel(dev)
        dev.start()
        src = dev.cmd(f"source {IMG}")
        dev.cmd(f"arm {op} {mode} 1 1" + (f" {sub}" if sub else ""))
        r = dev.cmd("download")
        crashed = "crashed" in r
        after_crash = _final_sha(dev)
        stale = _stages(dev)
        dev.start()                                   # reboot
        dev.cmd(f"source {IMG}")
        r2 = dev.cmd("download")
        st = _status(dev)
        dev.finish()
        base = "why" not in r2
        if base:
            return [("reset fired", crashed, r)], after_crash != h0
        ops = dev.ops()
        ci = max(i for i, o in enumerate(ops) if o["op"] == "crash")
        created = set()
        bad_reads = []
        for o in ops[ci + 1:]:
            if o["op"] == "fopen" and "stg-" in o["path"]:
                if o["inj"] == "wx" and o["ret"] == 0:
                    created.add(o["path"])
                elif o["inj"] == "rb" and o["path"] not in created:
                    bad_reads.append(o)
        c = [("C-22 reset fired", crashed and dev.exits[-1] == 86, (r, dev.exits)),
             ("C-22 H0 intact after the reset", after_crash == h0, (after_crash, h0)),
             ("C-22 stale stage never opened for streaming", not bad_reads, bad_reads[:3]),
             ("C-22 next attempt cleans stale stages then succeeds",
              r2["rc"] == 0 and r2["stale_removed"] == len(stale) and r2["sha"] == src["source_sha"], (r2, stale)),
             ("C-22 H0 intact at the end", _final_sha(dev) == h0, None),
             ("C-12 gate", st["violations"] == 0 and st["refs"] == 0, st)]
        return c, None
    return run


def ao8(dev):
    """success: stage == download == read-back hash, final untouched, stage removed after confirm"""
    h0 = _sentinel(dev)
    dev.start()
    src = dev.cmd(f"source {IMG}")
    r = dev.cmd("download")
    if "why" not in r:
        dev.finish()
        return [("download ran", r.get("rc") == 0, r)], None
    s = dev.cmd("stream")
    rm = dev.cmd("remove_stage")
    st = _status(dev)
    dev.finish()
    return [("C-23 verified stage hash == source", r["rc"] == 0 and r["sha"] == src["source_sha"] and r["len"] == IMG, r),
            ("C-23 streamed and ended", s["stream"] == 1 and s["end"] == 1 and s["abort"] == 0 and s["bytes"] == IMG, s),
            ("C-23 final path untouched", _final_sha(dev) == h0, None),
            ("C-23 stage removed", rm["removed"] == 1 and _stages(dev) == [], _stages(dev)),
            ("C-12 gate", st["violations"] == 0 and st["refs"] == 0, st)], None


def _stream_fault(mode: str):
    def run(dev):
        h0 = _sentinel(dev)
        dev.start()
        dev.cmd(f"source {IMG}")
        r = dev.cmd("download")
        if "why" not in r:
            dev.finish()
            return [("n/a on base", True, None)], None
        dev.cmd(f"arm fread {mode} 2 1 stg-")
        s = dev.cmd("stream")
        dev.finish()
        return [("C-24 abort sent, never OTA_END", s["stream"] == 0 and s["abort"] == 1 and s["end"] == 0, s),
                ("C-24 H0 intact", _final_sha(dev) == h0, None)], None
    return run


def _stale(n: int, dev) -> None:
    for k in range(n):
        (dev.state / "sdcard" / f"ambit_fw.stg-{k}.bin").write_bytes(b"stale" * 10)


def c25a(dev):
    """8 stale stages → all removed first, stg-0 allocated, success"""
    h0 = _sentinel(dev)
    _stale(8, dev)
    dev.start()
    dev.cmd(f"source {IMG}")
    r = dev.cmd("download")
    dev.finish()
    if "why" not in r:
        return [("n/a on base", True, None)], None
    return [("C-25a all stale removed then success", r["rc"] == 0 and r["stale_removed"] == 8 and
             r["path"].endswith("ambit_fw.stg-0.bin"), r),
            ("C-25 H0 intact", _final_sha(dev) == h0, None)], None


def c25bc(dev):
    """every remove fails → refused, nothing created; removes healthy again → success"""
    h0 = _sentinel(dev)
    _stale(8, dev)
    dev.start()
    dev.cmd(f"source {IMG}")
    dev.cmd("arm remove eio 1 8 stg-")
    r = dev.cmd("download")
    if "why" not in r:
        dev.finish()
        return [("n/a on base", True, None)], None
    before = _stages(dev)
    r2 = dev.cmd("download")
    dev.finish()
    return [("C-25b refused staging_slots_blocked", r["rc"] != 0 and r["why"] == "staging_slots_blocked", r),
            ("C-25b nothing created or overwritten", before == [f"ambit_fw.stg-{k}.bin" for k in range(8)], before),
            ("C-25c next attempt succeeds (no permanent deadlock)", r2["rc"] == 0 and r2["stale_removed"] == 8, r2),
            ("C-25 H0 intact", _final_sha(dev) == h0, None)], None


def c25d(dev):
    """3 of 8 removes fail → lowest freed slot used"""
    _sentinel(dev)
    _stale(8, dev)
    dev.start()
    dev.cmd(f"source {IMG}")
    dev.cmd("arm remove eio 1 3 stg-")
    r = dev.cmd("download")
    dev.finish()
    if "why" not in r:
        return [("n/a on base", True, None)], None
    ops = [o for o in dev.ops() if o["op"] == "remove" and "stg-" in o["path"]]
    freed = sorted(int(o["path"].rsplit("-", 1)[1].split(".")[0]) for o in ops if o["ret"] == 0)
    return [("C-25d lowest freed slot allocated", r["rc"] == 0 and freed and
             r["path"].endswith(f"ambit_fw.stg-{freed[0]}.bin") and r["stale_remove_err"] == 3, (r, freed))], None


def ao12(dev):
    """C-57: stale-stage remove applied_eio (removed, reported failed) → proceeds, slot reused"""
    _sentinel(dev)
    _stale(1, dev)
    dev.start()
    dev.cmd(f"source {IMG}")
    dev.cmd("arm remove applied_eio 1 1 stg-")
    r = dev.cmd("download")
    st = _status(dev)
    dev.finish()
    if "why" not in r:
        return [("n/a on base", True, None)], None
    return [("C-57 proceeds and reuses the slot", r["rc"] == 0 and r["path"].endswith("stg-0.bin"), r),
            ("C-57 remove error counted once", r["stale_remove_err"] == 1 and
             st["diag"]["faults"].get("ambit_ota.remove") == 1, (r, st["diag"]))], None


# ── ambit_flash preflight (C-26) ────────────────────────────────────────────

DIR = "./sdcard/ambit_fw/0.0.9"


def _regions(dev) -> None:
    d = dev.state / "sdcard" / "ambit_fw" / "0.0.9"
    d.mkdir(parents=True)
    for n in L.REGIONS:
        (d / n).write_bytes(b"\xaa" * 64)


def _af(teardown: str | None):
    def run(dev):
        _regions(dev)
        dev.start()
        if teardown == "now":
            dev.cmd("teardown_now")
        elif teardown:
            dev.cmd(f"teardown_at {teardown}")
        r = dev.cmd(f"flash_image {DIR}")
        st = _status(dev)
        dev.finish()
        base_red = st["violations"] > 0
        if teardown is None:
            return [("C-26 healthy preflight proceeds to the bus", r["session"] == 1 and st["violations"] == 0, (r, st))], \
                base_red
        c = [("C-26 every FS call held a ref", st["violations"] == 0, st),
             ("C-26 refs balance to 0", r["refs"] == 0 and st["refs"] == 0, (r, st)),
             ("C-26 error before the UART bus / target", r["session"] == 0 and r["rc"] == EINVAL_STATE, r)]
        if teardown == "now":
            n_fs = sum(1 for o in dev.ops() if o["op"] in ("mkdir", "fopen", "stat", "fseek", "ftell"))
            c.append(("C-26 gate refusal → zero FS calls", n_fs == 0, n_fs))
        return c, base_red
    return run


def af_mkdir_fail(dev):
    """mkdir failure: attributed in sd_diag, error before the bus"""
    dev.start()
    dev.cmd("arm mkdir eio 1 1")
    r = dev.cmd(f"flash_image {DIR}")
    st = _status(dev)
    dev.finish()
    if dev.exe.name.startswith("ambit_base"):
        return [("n/a on base", True, None)], None
    return [("C-29 ambit_flash.mkdir counted", st["diag"]["faults"].get("ambit_flash.mkdir") == 1, st),
            ("error before the bus", r["session"] == 0 and r["rc"] != 0, r)], None


SCENARIOS = {
    # write-path faults: base destroys H0 by truncating the final name at open
    "AO1": _fault("fwrite", "short", diag_op="write"),
    "AO2": _fault("fwrite", "eio", diag_op="write"),
    "AO3": _fault("fflush", "eio", diag_op="flush"),
    "AO4": _fault("fsync", "eio", diag_op="fsync"),
    "AO5": _fault("fclose", "eio", diag_op="close"),
    "AO6": _fault("fopen", "eio", sub="stg-", diag_op="open"),
    "AO7": _fault("fread", "flip_read", sub="stg-", expect_mismatch=True, diag_op="verify"),
    "AO8": ao8,
    "AO9a": _stream_fault("flip_read"),
    "AO9b": _stream_fault("eio"),
    "AO10": _fault("fsync", "applied_eio", diag_op="fsync"),
    "AO11": _fault("fclose", "applied_eio", diag_op="close"),
    "AO12": ao12,
    "AOR1": _reset("fopen", "crash", "stg-"), "AOR2": _reset("fopen", "crash_after", "stg-"),
    "AOR3": _reset("fwrite", "crash"), "AOR4": _reset("fwrite", "crash_after"), "AOR5": _reset("fwrite", "crash_mid"),
    "AOR6": _reset("fflush", "crash"), "AOR7": _reset("fflush", "crash_after"),
    "AOR8": _reset("fsync", "crash"), "AOR9": _reset("fsync", "crash_after"),
    "AOR10": _reset("fclose", "crash"), "AOR11": _reset("fclose", "crash_after"),
    "C25a": c25a, "C25bc": c25bc, "C25d": c25d,
    "AF0": _af(None), "AF1": _af("now"), "AF2": _af("mkdir 1"), "AF3": _af("mkdir 2"),
    "AF4a": _af("fopen 1"), "AF4b": _af("fopen 2"), "AF4c": _af("fopen 3"), "AF4d": _af("fopen 4"),
    "AF5": af_mkdir_fail,
}
BASE_RED_EXPECTED = {"AO1", "AO2", "AO3", "AO4", "AO5", "AF1", "AF2", "AF3", "AF4a", "AF4b", "AF4c", "AF4d"}
