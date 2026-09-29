#!/usr/bin/env python3
"""sd_logger hardware-qualification checkers (Sprint 2 contract r4, H6).

Wire grammar: docs/sdlog-hil-trace.md (H2 trace, H4 inventory, H5 dump, H7
quiesce, H8 reset witness). Oracles: contract §5 LOG-REPLAY / LOG-LEDGER /
LOG-PRESERVE / SL-BACKOFF, §6.2 L-rows (incl. L-8), §6.6 REL-LOG.

    sdlog_check.py parse      --capture CAP [--from N --to M] [--out parsed.json]
    sdlog_check.py undump     --capture CAP [--from --to] (--index K | --all) --out PATH
    sdlog_check.py continuity --capture CAP [--from --to] [--cap BYTES]
    sdlog_check.py replay     --capture CAP --i0 A:B [--i1 C:D] [--exports E.json] [--from --to]
    sdlog_check.py verify     --capture CAP --i0 A:B --i1 C:D --exports E.json [--from --to]
    sdlog_check.py ledger     --capture CAP --i0 A:B --i1 C:D --exports E.json [--from --to]
    sdlog_check.py preserve   --events EVENTS.json | --rel REL.json [--rel-console LOG ...]
    sdlog_check.py backoff    --capture CAP [--from --to] [--row L5|L6] [--tol-us US]
    sdlog_check.py l8post     --capture CAP --i0 A:B [--i1 C:D] [--exports E.json]
    sdlog_check.py rlen       --run RUN --u U [--pad 160] [--f HZ] [--s-active N] [--rho BPS]

Every command prints one JSON object and exits 0 PASS, 1 FAIL, 2 VOID (the
evidence cannot support a verdict - re-run with a new run id; never a pass),
3 STOP (evidence capacity not maintained: the phase stops). Byte offsets
(`--from/--to`, `A:B` ranges) are offsets into the raw capture file, as the
serial_daemon `.idx` records them.

Trust boundary: the prediction is built ONLY from host-held bytes (the trace's
base64 producer records and the host-undumped I0 file bytes) and the writer
events' numbers; device-reported hashes (SDL_FF, SDL_DUMP_END) are
cross-checks, never the witness (contract r3, critique-02 #1).

Window definition (resolves the contract's "P pushed records with seq in the
window" exactly, without slack): sd_logger's pause handshake drains the RAM
ring until a pop returns 0 before it reports paused, so at every quiesce point
the ring holds exactly the producer records pushed after the last traced POP.
The replay therefore starts its producer stream right after the last POP
before the I0 pause (the anchor) and applies file effects only after the I0
pause; W = every stream byte consumed up to the last I1 pause. Records pushed
after the final drain-loop POP are carried out of the window (not in W).
"""
from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
import json
import math
import os
import re
import sys
from pathlib import Path

PASS, FAIL, VOID, STOP = "PASS", "FAIL", "VOID", "STOP"
EXIT = {PASS: 0, FAIL: 1, VOID: 2, STOP: 3}

LOG_NAMES = ["ambyte.log"] + [f"ambyte.{i}.log" for i in range(1, 6)]
ACTIVE = "ambyte.log"
OLDEST = "ambyte.5.log"
FILE_CAP = 1024 * 1024             # SD_LOGGER_FILE_BYTES
HARD_CAP = 2 * FILE_CAP            # SD_LOGGER_HARD_BYTES: growth ceiling while rotation is blocked
LINE_MAX = 256                     # SD_LOGGER_LINE_MAX, incl. the '\n'
BACKOFF_US = 60_000_000            # SD_LOGGER_ROT_BACKOFF_MS
# The rotation decision (`now` in ensure_room) is taken BEFORE commit/close/remove/
# renames, but the ROT trace entry is stamped AFTER rotate_files returns. A slow first
# attempt followed by a fast retry can therefore show a recorded gap a few ms under
# 60 s although the firmware honoured the backoff exactly. Default tolerance 250 ms,
# reported together with the minimum observed gap so the reader sees how close it came.
BACKOFF_TOL_US = 250_000
SYN_TAG = b" HILSDLOG: "           # the H3 emitter tag as IDF log format v1 renders it

ANSI = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")
BOOT_RE = re.compile(rb"ESP-ROM:|rst:0x")
ACCT_RE = re.compile(rb'\{"quar":[^{}]*\}')
FILE_RE = re.compile(rb"- file: \S*ambyte\.log \((\d+) bytes\)")
DUMP_CMD_RE = re.compile(rb"sdlog_dump\s+(ambyte(?:\.[1-5])?\.log)\b")
ACCT_KEYS = ("quar", "backoff", "rb", "indet", "unwr", "ring", "rot", "unav", "evict", "trunc", "torn", "err")
TAGS = (b"SLT_RESET_WITNESS", b"SLT_AUTOARM", b"SLT_DRAIN", b"SLT_STAT", b"SLT_HDR", b"SLT_OFF", b"SLT_ERR",
        b"SLT_ON", b"SLT_WM", b"SLT_E", b"SDL_DUMP_END", b"SDL_INV_END", b"SDL_INVALID", b"SDL_TIMEOUT",
        b"SDL_BEGIN", b"SDL_STATE", b"SDL_MORE", b"SDL_B64", b"SDL_END", b"SDL_ERR", b"SDL_FF", b"SDL_FL",
        b"SDL_Q", b"HIL_FAULT_LAST", b"HIL_FAULT")
TAG_RE = re.compile(rb"(?<![A-Za-z0-9_])(" + b"|".join(TAGS) + rb")(?= |$)(.*)$")
# Field counts after `SLT_E <kind>` (seq, us, then the kind's own fields).
ENTRY_NFIELDS = {"P": 7, "POP": 3, "WR": 5, "COMMIT": 3, "RB": 5, "ROT": 5, "OPEN": 5, "CLOSE": 3, "DROP": 4,
                 "QUI": 3}


def sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def worst(*statuses: str) -> str:
    for s in (FAIL, STOP, VOID):
        if s in statuses:
            return s
    return PASS


class Verdict:
    """Accumulates reasons; the verdict is the worst status seen."""

    def __init__(self) -> None:
        self.reasons: list[dict] = []
        self.void_at: int | None = None     # index of the first "model cannot apply" reason

    def add(self, status: str, msg: str, **kw) -> None:
        self.reasons.append({"status": status, "msg": msg, **kw})

    def model_void(self, msg: str) -> None:
        if self.void_at is None:
            self.void_at = len(self.reasons)
        self.add(VOID, msg)

    @property
    def status(self) -> str:
        """Worst status - except that once the model could not apply an event, every
        later comparison is against a wrong prediction: only findings recorded BEFORE
        that point may still turn the verdict into FAIL/STOP; otherwise it is VOID."""
        if not self.reasons:
            return PASS
        if self.void_at is None:
            return worst(*[r["status"] for r in self.reasons])
        before = worst(*[r["status"] for r in self.reasons[:self.void_at]])
        return before if before in (FAIL, STOP) else VOID

    def json(self) -> list[dict]:
        return self.reasons[:200]


# =========================================================================== parse

def iter_lines(data: bytes, lo: int = 0, hi: int | None = None):
    """(offset, line) for each line in data[lo:hi]; `\\r`, ANSI colour and the `\\n`
    stripped. Offsets are raw capture offsets (line start)."""
    hi = len(data) if hi is None else min(hi, len(data))
    pos = max(0, lo)
    while pos < hi:
        nl = data.find(b"\n", pos, hi)
        end = hi if nl < 0 else nl
        yield pos, ANSI.sub(b"", data[pos:end]).replace(b"\r", b"")
        pos = end + 1


def _kv(tokens: list[str]) -> dict:
    return dict(t.split("=", 1) for t in tokens if "=" in t)


def _int(s: str) -> int:
    return int(s, 10)


def parse_entry(tok: list[str]) -> dict:
    """`<kind> <seq> <us> ...` (the tokens after `SLT_E`) -> dict. ValueError if malformed."""
    if not tok:
        raise ValueError("empty entry")
    kind, f = tok[0], tok[1:]
    want = ENTRY_NFIELDS.get(kind)
    if want is None:
        raise ValueError(f"unknown entry kind {kind!r}")
    if len(f) < want or (kind != "ROT" and len(f) != want):
        raise ValueError(f"{kind}: {len(f)} fields, want {want}")
    e = {"kind": kind, "seq": _int(f[0]), "us": _int(f[1])}
    a = f[2:]
    if kind == "P":
        if a[2] not in ("pushed", "dropped") or a[3] not in ("0", "1") or len(a[1]) != 64:
            raise ValueError("P: bad state/syn/sha")
        try:
            data = base64.b64decode(a[4], validate=True)
        except (binascii.Error, ValueError) as ex:
            raise ValueError(f"P: bad base64 ({ex})") from None
        e.update(len=_int(a[0]), sha=a[1], state=a[2], syn=int(a[3]), data=data)
    elif kind == "POP":
        e.update(n=_int(a[0]))
    elif kind == "WR":
        e.update(n=_int(a[0]), written=_int(a[1]), before=_int(a[2]))
    elif kind == "COMMIT":
        e.update(committed=_int(a[0]))
    elif kind == "RB":
        if a[2] not in ("ok", "quar_trunc", "quar_fsync"):
            raise ValueError(f"RB: bad result {a[2]!r}")
        e.update(to=_int(a[0]), since=_int(a[1]), result=a[2])
    elif kind == "ROT":
        if a[0] not in ("ok", "fail"):
            raise ValueError(f"ROT: bad result {a[0]!r}")
        e.update(ok=a[0] == "ok", op=a[1], errno=_int(a[2]) if a[2].lstrip("-").isdigit() else a[2],
                 extra=a[3:])
    elif kind == "OPEN":
        if a[2] not in ("0", "1"):
            raise ValueError("OPEN: bad torn")
        e.update(name=a[0], size=_int(a[1]), torn=int(a[2]))
    elif kind == "CLOSE":
        if a[0] not in ("ok", "err", "abandon"):
            raise ValueError(f"CLOSE: bad result {a[0]!r}")
        e.update(result=a[0])
    elif kind == "DROP":
        if a[0] not in ("rotate_blocked", "unavailable"):
            raise ValueError(f"DROP: bad reason {a[0]!r}")
        e.update(why=a[0], n=_int(a[1]))
    elif kind == "QUI":
        e.update(state=a[0])
    return e


def _new_session(off: int, state: str) -> dict:
    return {"off": off, "end": off if state != "paused" else None, "state": state, "complete": state != "paused",
            "ff": [], "fl": [], "state_line": None, "dumps": [], "invalid": [], "timeout": [], "inv_end": None}


def parse_capture(data: bytes, lo: int = 0, hi: int | None = None) -> dict:
    """Structure a console capture (bytes, possibly with unrelated interleaved lines)."""
    cp: dict = {"range": [lo, len(data) if hi is None else min(hi, len(data))],
                "slt_on": [], "slt_off": [], "slt_err": [], "wm": [], "stat": [], "autoarm": [],
                "drains": [], "orphans": [], "malformed": [], "witness": [], "sessions": [], "dumps": [],
                "ff": [], "fl": [], "state": [], "invalid": [], "timeout": [], "begin": [], "end": [],
                "fired": [], "fault_last": [], "log_status": [], "boots": [], "dump_cmds": []}
    drain = None
    sess = None
    chunks: list[dict] = []
    pending_file_bytes = None
    # Echoed `sdlog_dump <name>` commands bind, in order, to the dumps of the NEXT
    # quiesce session (each dump command runs its own quiesce; a refused or invalid
    # session consumes its names without producing a dump, so they cannot shift).
    echo_names: list[str] = []
    sess_names: list[str] = []
    for off, ln in iter_lines(data, lo, hi):
        if BOOT_RE.search(ln):
            cp["boots"].append(off)
        m = DUMP_CMD_RE.search(ln)
        if m and b"SDL_" not in ln:
            echo_names.append(m.group(1).decode())
            cp["dump_cmds"].append({"off": off, "name": echo_names[-1]})
        fm = FILE_RE.search(ln)
        if fm:
            pending_file_bytes = int(fm.group(1))
        am = ACCT_RE.search(ln)
        if am:
            try:
                acct = json.loads(am.group(0))
                if all(k in acct for k in ACCT_KEYS):
                    cp["log_status"].append({"off": off, "acct": acct, "file_bytes": pending_file_bytes})
                else:
                    cp["malformed"].append({"off": off, "why": "log_status: acct missing keys"})
            except json.JSONDecodeError:
                cp["malformed"].append({"off": off, "why": "log_status: acct not JSON"})
            pending_file_bytes = None
        tm = TAG_RE.search(ln)
        if not tm:
            continue
        tag = tm.group(1).decode()
        try:
            tok = tm.group(2).decode("ascii").split()
        except UnicodeDecodeError:
            cp["malformed"].append({"off": off, "why": f"{tag}: non-ascii"})
            continue
        try:
            if tag == "SLT_E":
                e = parse_entry(tok)
                e["off"] = off
                (drain["entries"] if drain is not None else cp["orphans"]).append(e)
            elif tag == "SLT_DRAIN":
                if drain is not None:
                    drain["complete"] = False
                    cp["drains"].append(drain)
                drain = {"off": off, "first": _int(tok[0]), "last": _int(tok[1]), "entries": [], "hdr": None,
                         "complete": False, "end": None}
            elif tag == "SLT_HDR":
                h = {"total": _int(tok[0]), "lost": _int(tok[1]), "first": _int(tok[2]), "last": _int(tok[3]),
                     "fill": _int(tok[4])}
                if drain is None:
                    cp["malformed"].append({"off": off, "why": "SLT_HDR: without SLT_DRAIN"})
                else:
                    drain.update(hdr=h, end=off, complete=True)
                    cp["drains"].append(drain)
                    drain = None
            elif tag == "SLT_ON":
                cp["slt_on"].append({"off": off, "cap": _int(tok[0]), "next_seq": _int(tok[1]),
                                     "autoarm": "autoarm" in tok[2:]})
            elif tag == "SLT_OFF":
                cp["slt_off"].append({"off": off, "next_seq": _int(tok[0])})
            elif tag == "SLT_ERR":
                cp["slt_err"].append({"off": off, "what": " ".join(tok)})
            elif tag == "SLT_WM":
                cp["wm"].append({"off": off, "fill": _int(tok[0])})
            elif tag == "SLT_STAT":
                cp["stat"].append({"off": off, "fields": tok})
            elif tag == "SLT_AUTOARM":
                cp["autoarm"].append({"off": off, "what": " ".join(tok)})
            elif tag == "SLT_RESET_WITNESS":
                raw = base64.b64decode(tok[5], validate=True)
                cp["witness"].append({"off": off, "path": tok[0], "before": _int(tok[1]), "n": _int(tok[2]),
                                      "half": _int(tok[3]), "sha": tok[4], "data": raw})
            elif tag == "SDL_Q":
                st = tok[0] if tok else "?"
                if st == "paused":
                    if sess is not None:
                        sess["complete"] = False
                    sess = _new_session(off, "paused")
                    cp["sessions"].append(sess)
                    chunks = []
                    sess_names, echo_names = echo_names, []
                elif st == "resumed":
                    if sess is not None:
                        sess.update(end=off, complete=True)
                        sess = None
                else:
                    cp["sessions"].append(_new_session(off, st))
                    echo_names = []
            elif tag == "SDL_FF":
                ff = {"off": off, "name": tok[0], "size": _int(tok[1]), "sha": tok[2], "lines": _int(tok[3]),
                      "torn": _int(tok[4]), "maxline": _int(tok[5])}
                cp["ff"].append(ff)
                if sess is not None:
                    sess["ff"].append(ff)
            elif tag == "SDL_FL":
                fl = {"off": off, "name": tok[0], "foff": _int(tok[1]), "len": _int(tok[2]), "sha": tok[3]}
                cp["fl"].append(fl)
                if sess is not None:
                    sess["fl"].append(fl)
            elif tag == "SDL_STATE":
                st = {"off": off, "cur": tok[0], "committed": _int(tok[1]), "quar": _int(tok[2]),
                      "backoff_ms_left": _int(tok[3])}
                cp["state"].append(st)
                if sess is not None:
                    sess["state_line"] = st
            elif tag == "SDL_INV_END":
                if sess is not None:
                    sess["inv_end"] = _int(tok[0])
            elif tag == "SDL_B64":
                chunks.append({"off": off, "foff": _int(tok[0]), "data": base64.b64decode(tok[1], validate=True)})
            elif tag == "SDL_DUMP_END":
                d = _finish_dump(off, tok, chunks, sess, sess_names.pop(0) if sess_names else None)
                chunks = []
                cp["dumps"].append(d)
                if sess is not None:
                    sess["dumps"].append(d)
            elif tag == "SDL_INVALID":
                cp["invalid"].append({"off": off, "what": " ".join(tok)})
                if sess is not None:
                    sess["invalid"].append(" ".join(tok))
            elif tag == "SDL_TIMEOUT":
                cp["timeout"].append({"off": off, "what": " ".join(tok)})
                if sess is not None:
                    sess["timeout"].append(" ".join(tok))
            elif tag == "SDL_BEGIN":
                cp["begin"].append({"off": off, "run": tok[0], "k0": _int(tok[1]), "n": _int(tok[2]),
                                    "hz": _int(tok[3]), "pad": _int(tok[4]), "us": _int(tok[5])})
            elif tag == "SDL_END":
                cp["end"].append({"off": off, "run": tok[0], "k_last": _int(tok[1]), "us": _int(tok[2])})
            elif tag == "HIL_FAULT":
                if tok and tok[0] == "fired":
                    cp["fired"].append({"off": off, **_kv(tok[1:]), "raw": " ".join(tok)})
            elif tag == "HIL_FAULT_LAST":
                cp["fault_last"].append({"off": off, **_kv(tok), "raw": " ".join(tok)})
        except (IndexError, ValueError, binascii.Error) as ex:
            cp["malformed"].append({"off": off, "why": f"{tag}: {ex}"})
    if drain is not None:
        drain["complete"] = False
        cp["drains"].append(drain)
    return cp


def _finish_dump(off: int, tok: list[str], chunks: list[dict], sess: dict | None, name: str | None) -> dict:
    d = {"off": off, "from_off": _int(tok[0]), "size": _int(tok[1]), "sha_whole": tok[2], "sha_range": tok[3],
         "name": name, "session_off": sess["off"] if sess else None, "ok": True, "why": [],
         "first_off": chunks[0]["off"] if chunks else off}
    pos = d["from_off"]
    buf = bytearray()
    for c in chunks:
        if c["foff"] != pos:
            d["ok"] = False
            d["why"].append(f"chunk at {c['foff']} expected {pos}")
            break
        buf += c["data"]
        pos += len(c["data"])
    if d["ok"] and pos != d["size"]:
        d["ok"] = False
        d["why"].append(f"chunks end at {pos}, size {d['size']}")
    if d["ok"] and sha(bytes(buf)) != d["sha_range"]:
        d["ok"] = False
        d["why"].append("range sha mismatch")
    if d["ok"] and d["from_off"] == 0 and sha(bytes(buf)) != d["sha_whole"]:
        d["ok"] = False
        d["why"].append("whole-file sha mismatch")
    d["data"] = bytes(buf)
    return d


def entries_of(cp: dict) -> list[dict]:
    return [e for d in cp["drains"] for e in d["entries"]]


def load_capture(path: str, lo: int | None, hi: int | None) -> tuple[bytes, dict]:
    data = Path(path).read_bytes()
    return data, parse_capture(data, lo or 0, hi)


def _rng(s: str | None) -> tuple[int, int] | None:
    if s is None:
        return None
    a, b = s.split(":", 1)
    return int(a, 0), int(b, 0)


# =========================================================================== continuity

def continuity(cp: dict, cap_bytes: int | None = None) -> dict:
    """H2 drain-set rules: global contiguous seq across drains, lost == 0 at every
    header, fill <= 50 % of cap at every header, headers consistent with their
    entries, P length/sha recomputed from the carried bytes, producer framing."""
    v = Verdict()
    drains = cp["drains"]
    if not drains:
        v.add(VOID, "no trace drains in range")
    cap = cap_bytes
    if cap is None:
        full = [d for d in drains if d["entries"]]
        ons = [o for o in cp["slt_on"] if not full or o["off"] < full[0]["off"]]
        cap = ons[-1]["cap"] if ons else (cp["slt_on"][0]["cap"] if cp["slt_on"] else None)
    if cap is None:
        v.add(VOID, "trace capacity unknown (no SLT_ON in range; pass --cap)")
    for err in cp["slt_err"]:
        v.add(STOP, f"SLT_ERR {err['what']} at {err['off']} (no evidence capacity: L-rows STOP)")
    for m in cp["malformed"]:
        if m["why"].startswith(("SLT_", "SDL_")):
            v.add(VOID, f"malformed line at {m['off']}: {m['why']}")
    if cp["orphans"]:
        v.add(VOID, f"{len(cp['orphans'])} SLT_E line(s) outside a drain", first_off=cp["orphans"][0]["off"])
    expected = None
    spans = []
    p_count = 0
    syn_mismatch = []
    for d in drains:
        ents = d["entries"]
        spans.append({"off": d["off"], "end": d.get("end"), "first": d["first"], "last": d["last"], "n": len(ents)})
        if not d["complete"]:
            v.add(VOID, f"drain at {d['off']} has no SLT_HDR (truncated or interleaved)")
            continue
        h = d["hdr"]
        if h["lost"] != 0:
            v.add(VOID, f"drain at {d['off']}: lost={h['lost']} (ring overflow: window void, re-run)")
        if (h["first"], h["last"]) != (d["first"], d["last"]):
            v.add(VOID, f"drain at {d['off']}: SLT_HDR range {h['first']}..{h['last']} != SLT_DRAIN "
                        f"{d['first']}..{d['last']}")
        if ents:
            if ents[0]["seq"] != d["first"] or ents[-1]["seq"] != d["last"] or len(ents) != d["last"] - d["first"] + 1:
                v.add(VOID, f"drain at {d['off']}: {len(ents)} entries do not fill {d['first']}..{d['last']}")
        elif d["first"] <= d["last"]:
            v.add(VOID, f"drain at {d['off']}: announces {d['first']}..{d['last']} but carries no entries")
        if cap is not None and h["fill"] * 2 > cap:
            v.add(STOP, f"drain at {d['off']}: fill {h['fill']} > 50 % of {cap} (evidence capacity not maintained)")
        for e in ents:
            if expected is not None and e["seq"] != expected:
                v.add(VOID, f"seq gap: {e['seq']} after {expected - 1} (at {e['off']})")
            expected = e["seq"] + 1
            if e["kind"] == "P":
                p_count += 1
                b = e["data"]
                if len(b) != e["len"]:
                    v.add(VOID, f"P {e['seq']}: len {e['len']} != {len(b)} carried bytes")
                if sha(b) != e["sha"]:
                    v.add(VOID, f"P {e['seq']}: sha != sha256(carried bytes)")
                if e["syn"] != (1 if SYN_TAG in b else 0):
                    syn_mismatch.append(e["seq"])      # informational: recomputed from the bytes
                # F-L0 framing is fixed at the producer: a violation is a firmware defect.
                if not b or not b.endswith(b"\n") or b.count(b"\n") != 1 or len(b) > LINE_MAX:
                    v.add(FAIL, f"P {e['seq']}: record not framed (1..{LINE_MAX} B ending in exactly one \\n)")
    return {"status": v.status, "cap": cap, "drains": spans,
            "entries": sum(len(d["entries"]) for d in drains), "p_entries": p_count,
            "syn_flag_mismatch": syn_mismatch[:50], "reasons": v.json()}


# =========================================================================== replay model

class MFile:
    """One file identity on the modelled card. `data` None = opaque (size+sha only)."""

    def __init__(self, origin: str, data: bytes | None = None, size: int | None = None, sha256: str | None = None,
                 new: bool = False):
        self.origin = origin
        self.data = bytearray(data) if data is not None else None
        self._size = size
        self._sha = sha256
        self.base = len(data) if data is not None else (size or 0)   # the I0 size (prefix boundary)
        self.new = new
        self.segs: list[list] = []      # [file_off, stream_off, length, indet]: window bytes in this file
        self.quarantined = False
        self.mod = 0                    # bumps on every content change
        self.verified: dict | None = None

    @property
    def size(self) -> int:
        return len(self.data) if self.data is not None else int(self._size or 0)

    def digest(self) -> str | None:
        return sha(bytes(self.data)) if self.data is not None else self._sha


class Model:
    """The LOG-REPLAY state machine: exactly the effects sd_logger.c's writer applies,
    driven by the trace's writer events (it never re-decides the writer's policy -
    the events are the ground truth, the model only checks they are applicable)."""

    CODES = {"live": 1, "rolled_back": 2, "unwritten": 3, "dropped_rotate_blocked": 4, "dropped_unavailable": 5,
             "indeterminate_absent": 6, "retained_evicted": 7, "surviving": 8, "indeterminate_present": 9}

    def __init__(self, files: dict[str, MFile], fired: list[dict], rot_steps: dict[int, int], v: Verdict):
        self.files = files
        self.v = v
        self.stream = bytearray()
        self.bounds = {0}
        self.owner = bytearray()        # per stream byte: which bucket it currently belongs to
        self.pos = 0
        self.pending: tuple[int, int, int] | None = None     # (stream_off, n, seq) of the popped chunk
        self.open = False
        self.committed = 0
        self.evicted: list[dict] = []
        self.fired = {"rename": [f for f in fired if f.get("op") == "rename"],
                      "remove": [f for f in fired if f.get("op") == "remove"],
                      "truncate": [f for f in fired if f.get("op") == "truncate"]}
        self.rot_steps = rot_steps
        self.ctr = {"rb": 0, "indet": 0, "unwr": 0, "rot": 0, "unav": 0, "evict": 0, "torn": 0}
        self.rot_log: list[dict] = []
        self.dropped_p: list[dict] = []
        self.last_pop_seq: int | None = None
        self.p_seq_ends: list[tuple[int, int]] = []          # (seq, stream_end)

    # ---------------------------------------------------------------- helpers
    def _void(self, e: dict, msg: str) -> None:
        self.v.model_void(f"seq {e.get('seq')} {e.get('kind')}: {msg}")

    def _mark(self, a: int, n: int, code: str) -> None:
        if n > 0:
            self.owner[a:a + n] = bytes([self.CODES[code]]) * n

    def _truncate(self, f: MFile, to: int, code: str | None, indet_only: bool = False) -> int:
        """Drop (or, indet_only, only flag as indeterminate) file bytes >= `to`."""
        n = f.size - to
        keep = []
        for s in f.segs:
            fo, so, ln, ind = s
            if fo + ln <= to:
                keep.append(s)
                continue
            cut = max(0, to - fo)
            if cut:
                keep.append([fo, so, cut, ind])
            if indet_only:
                keep.append([fo + cut, so + cut, ln - cut, True])
            elif code:
                self._mark(so + cut, ln - cut, code)
        f.segs = keep
        if not indet_only:
            del f.data[to:]
            f.mod += 1
        return n

    def _seg_cover(self, f: MFile, a: int, b: int) -> int:
        return sum(max(0, min(fo + ln, b) - max(fo, a)) for fo, _so, ln, _i in f.segs)

    def _active(self, e: dict) -> MFile | None:
        f = self.files.get(ACTIVE)
        if f is None or not self.open:
            self._void(e, "no open ambyte.log in the model")
            return None
        return f

    def _rename(self, i: int, e: dict) -> bool:
        a, b = LOG_NAMES[i], LOG_NAMES[i + 1]
        if a not in self.files:
            return True                                   # ENOENT: harmless, rotate continues
        if b in self.files:
            self._void(e, f"rename {a} -> {b} over an existing target (FAT EEXIST) but the trace says applied")
            return False
        self.files[b] = self.files.pop(a)
        return True

    def _remove_oldest(self, e: dict) -> None:
        f = self.files.pop(OLDEST, None)
        if f is None:
            return                                        # ENOENT: harmless
        for _fo, so, ln, _i in f.segs:
            self._mark(so, ln, "retained_evicted")
        if f.size > 0:
            self.ctr["evict"] += f.size                   # rotate_files counts only a non-empty file
        self.evicted.append({"seq": e["seq"], "us": e["us"], "name": OLDEST, "origin": f.origin, "size": f.size,
                             "sha256": f.digest(), "window_bytes": sum(s[2] for s in f.segs), "file": f})

    # ---------------------------------------------------------------- events
    def push(self, e: dict) -> None:
        if e["state"] == "dropped":
            self.dropped_p.append(e)
            return
        self.stream += e["data"]
        self.owner += b"\0" * len(e["data"])
        self.bounds.add(len(self.stream))
        self.p_seq_ends.append((e["seq"], len(self.stream)))

    def apply(self, e: dict) -> None:
        fn = getattr(self, "ev_" + e["kind"], None)
        if fn is None:
            self._void(e, "unknown writer event")
            return
        fn(e)

    def ev_POP(self, e: dict) -> None:
        n = e["n"]
        if self.pending is not None:
            self._void(e, f"previous chunk (POP at seq {self.pending[2]}) neither written nor dropped")
        if n <= 0 or self.pos + n > len(self.stream):
            self._void(e, f"POP {n} beyond the traced pushed stream ({len(self.stream) - self.pos} B available): "
                          "untraced or lost producer bytes")
            self.pending = None
            return
        if self.pos + n not in self.bounds or self.stream[self.pos + n - 1] != 0x0A:
            self._void(e, f"POP {n} does not end on a producer record boundary")
        self.pending = (self.pos, n, e["seq"])
        self.pos += n
        self.last_pop_seq = e["seq"]

    def ev_WR(self, e: dict) -> None:
        f = self._active(e)
        if self.pending is None or self.pending[1] != e["n"]:
            self._void(e, f"WR n={e['n']} does not match the pending popped chunk {self.pending}")
            return
        if f is None:
            return
        if f.data is None:
            self._void(e, "write to an opaque (undumped) ambyte.log: active bytes are required")
            return
        if e["before"] != f.size:
            self._void(e, f"WR before={e['before']} != modelled size {f.size}")
            return
        if not 0 <= e["written"] <= e["n"]:
            self._void(e, f"WR written={e['written']} outside 0..{e['n']}")
            return
        so, n, _s = self.pending
        w = e["written"]
        f.data += self.stream[so:so + w]
        if w:
            f.segs.append([e["before"], so, w, False])
            self._mark(so, w, "live")
            f.mod += 1
        if w < n:
            self._mark(so + w, n - w, "unwritten")
            self.ctr["unwr"] += n - w
        self.pending = None

    def ev_DROP(self, e: dict) -> None:
        if self.pending is None or self.pending[1] != e["n"]:
            self._void(e, f"DROP n={e['n']} does not match the pending popped chunk {self.pending}")
            return
        so, n, _s = self.pending
        blocked = e["why"] == "rotate_blocked"
        self._mark(so, n, "dropped_rotate_blocked" if blocked else "dropped_unavailable")
        self.ctr["rot" if blocked else "unav"] += n
        self.pending = None

    def ev_COMMIT(self, e: dict) -> None:
        f = self._active(e)
        if f is None:
            return
        if e["committed"] != f.size:
            self._void(e, f"COMMIT {e['committed']} != modelled size {f.size}")
            return
        self.committed = e["committed"]

    def ev_RB(self, e: dict) -> None:
        f = self._active(e)
        if f is None:
            return
        if e["to"] != self.committed:
            self._void(e, f"RB to={e['to']} != modelled committed offset {self.committed}")
            return
        if e["since"] != f.size - e["to"]:
            self._void(e, f"RB since={e['since']} != modelled size {f.size} - to {e['to']}")
            return
        if self._seg_cover(f, e["to"], f.size) != f.size - e["to"]:
            self._void(e, "bytes beyond the commit point were not written in the window")
            return
        if e["result"] == "ok":
            self.ctr["rb"] += self._truncate(f, e["to"], "rolled_back")
            return
        # Quarantine: the handle is fclose'd inside rollback() (no CLOSE entry follows).
        self.ctr["indet"] += e["since"]
        applied = False
        if e["result"] == "quar_trunc" and self.fired.get("truncate"):
            # The k-th quar_trunc is the k-th truncate firing: its mode says whether the
            # truncate ran (applied_eio) - the trace alone only says "truncate failed".
            applied = self.fired["truncate"].pop(0).get("mode") == "applied_eio"
        if e["result"] == "quar_trunc" and not applied:
            # the injected truncate EIO is NOT performed: the tail stays in the FS view
            self._truncate(f, e["to"], None, indet_only=True)
        else:
            # truncate performed (then the injected fsync EIO, or an applied-then-failed
            # truncate): FS view truncated
            self._truncate(f, e["to"], "indeterminate_absent")
        f.quarantined = True
        self.open = False

    def ev_CLOSE(self, e: dict) -> None:
        f = self.files.get(ACTIVE)
        if not self.open:
            self._void(e, "CLOSE with no open file in the model")
            return
        extra = (f.size - self.committed) if f is not None else 0
        if e["result"] == "err" and extra > 0:
            self.ctr["indet"] += extra
            self._truncate(f, self.committed, None, indet_only=True)
        elif e["result"] == "abandon" and extra > 0:
            self._void(e, f"abandoned handle with {extra} uncommitted bytes: card state unknowable")
        self.open = False

    def ev_OPEN(self, e: dict) -> None:
        if e["name"] != ACTIVE:
            self._void(e, f"OPEN of {e['name']!r} (the writer only opens {ACTIVE})")
            return
        if self.open:
            self._void(e, "OPEN while the model already has an open handle")
        f = self.files.get(ACTIVE)
        if f is None:
            f = MFile(f"new@{e['seq']}", b"", new=True)
            self.files[ACTIVE] = f
        if f.data is None:
            self._void(e, "active file opaque: I0 needs a whole-file dump of ambyte.log")
            return
        if e["size"] != f.size:
            self._void(e, f"OPEN size {e['size']} != modelled size {f.size}")
            return
        torn = 1 if f.size > 0 and f.data[-1] != 0x0A else 0
        if torn != e["torn"]:
            self._void(e, f"OPEN torn={e['torn']} but the modelled tail says {torn}")
            return
        if torn:
            self.ctr["torn"] += 1
            f.quarantined = True
        self.open = True
        self.committed = f.size

    def ev_ROT(self, e: dict) -> None:
        if self.open:
            self._void(e, "rotation with the file still open (close_log precedes rotate_files)")
            return
        rec = {"seq": e["seq"], "us": e["us"], "ok": e["ok"], "op": e["op"], "errno": e["errno"]}
        self.rot_log.append(rec)
        if e["ok"]:
            self._remove_oldest(e)
            for i in range(len(LOG_NAMES) - 2, -1, -1):   # 4->5, 3->4, 2->3, 1->2, 0->1
                if not self._rename(i, e):
                    return
            return
        op = e["op"]
        step = None
        mode = "eio"
        explicit = [x for x in e.get("extra", []) if x.startswith("step=")]
        if e["seq"] in self.rot_steps:
            step = self.rot_steps[e["seq"]]
        elif explicit:
            step = int(explicit[0].split("=", 1)[1])
        elif self.fired.get(op):
            # The k-th traced failure of this op is the k-th HIL_FAULT fired line of it;
            # the fired path is the rename SOURCE (evq_hil_io_rename_w passes `a`), which
            # names the step - the trace itself only says "rename failed".
            fl = self.fired[op].pop(0)
            mode = fl.get("mode", "eio")
            path = fl.get("path", "")
            name = path.rsplit("/", 1)[-1]
            rec["fired_path"] = path
            if op == "rename":
                if name not in LOG_NAMES[:-1]:
                    self._void(e, f"fired rename path {path!r} is not a chain source")
                    return
                step = LOG_NAMES.index(name)
            elif name != OLDEST:
                self._void(e, f"fired remove path {path!r} is not {OLDEST}")
                return
        if op == "remove":
            if mode not in ("eio", "enospc"):
                self._void(e, f"remove fault mode {mode!r} (applied) is not modelled")
            return                                        # not performed: nothing changed
        if op != "rename":
            self._void(e, f"ROT fail op {op!r} unknown")
            return
        if step is None:
            self._void(e, "organic rename failure: failing step unknown (no HIL_FAULT fired line; "
                          "pass --rot-step SEQ=SRC_INDEX)")
            return
        rec["step_src"] = step
        self._remove_oldest(e)                            # remove precedes the renames
        for i in range(len(LOG_NAMES) - 2, step, -1):     # the renames before the failing step
            if not self._rename(i, e):
                return
        if mode == "applied_eio":
            self._rename(step, e)                         # the op ran, then reported failure
        elif mode not in ("eio", "enospc"):
            self._void(e, f"rename fault mode {mode!r} not modelled")

    def ev_QUI(self, e: dict) -> None:
        pass


def replay_model(i0_files: dict[str, bytes], entries: list, fired: list[dict] | None = None,
                 rot_steps: dict[int, int] | None = None) -> tuple[dict[str, bytes], list[dict]]:
    """Importable core for host tests (SLT-1): apply a whole trace, recorded from an
    EMPTY producer ring (trace on before the writer's first event), to the I0 files.

    entries: parsed entry dicts, or raw console lines (non-`SLT_E` lines are skipped,
    so a harness's collected trace list can be passed as is).
    fired: optional [{"op": "rename"|"remove", "path": ..., "mode": "eio"}] naming the
    failing step of each `ROT fail` in order. If the first writer event is a write,
    commit, rollback or close (the trace was switched on with the file already open),
    the model starts from an open ambyte.log at its I0 size. Returns (predicted bytes per file,
    evicted [{name, origin, size, sha256, seq}]); raises ValueError with the reasons
    when an event cannot be applied (the VOID condition)."""
    ents = []
    for x in entries:
        if isinstance(x, dict):
            ents.append(x)
            continue
        s = x.decode() if isinstance(x, (bytes, bytearray)) else str(x)
        i = s.find("SLT_E ")
        if i >= 0:
            ents.append(parse_entry(s[i + 6:].split()))
        elif "SLT_HDR " in s and s.split("SLT_HDR ", 1)[1].split()[1] != "0":
            raise ValueError(f"trace lost entries: {s.strip()}")
    seqs = sorted(e["seq"] for e in ents)
    if seqs and seqs != list(range(seqs[0], seqs[0] + len(seqs))):
        raise ValueError("trace seq not contiguous (lost or duplicated entries)")
    v = Verdict()
    m = Model({n: MFile(f"I0:{n}", b) for n, b in i0_files.items()}, fired or [], rot_steps or {}, v)
    ents = sorted(ents, key=lambda z: z["seq"])
    # A trace switched on while the writer already held the file open (no OPEN before
    # its first write/commit/close) starts from that handle at the I0 size. On the
    # device, windows always start at a quiesce (file closed), so replay() never needs this.
    first = next((e for e in ents if e["kind"] not in ("P", "QUI", "POP")), None)
    if first is not None and first["kind"] in ("WR", "COMMIT", "RB", "CLOSE"):
        m.files.setdefault(ACTIVE, MFile(f"I0:{ACTIVE}", b""))    # created by that earlier open
        m.open = True
        m.committed = m.files[ACTIVE].size
    for e in ents:
        if e["kind"] == "P":
            m.push(e)
        else:
            m.apply(e)
    if v.status != PASS:
        raise ValueError("; ".join(r["msg"] for r in v.reasons[:10]))
    pred = {n: bytes(f.data) for n, f in m.files.items() if f.data is not None}
    ev = [{k: x[k] for k in ("name", "origin", "size", "sha256", "seq")} for x in m.evicted]
    return pred, ev


def _resolve_dump_names(cp: dict) -> None:
    """A dump's file name: the echoed `sdlog_dump <name>` command, else a SDL_FF with
    the same size and whole-file sha in the same quiesce session (a device report,
    used only to NAME the dump; its content is still compared as bytes)."""
    for d in cp["dumps"]:
        if d["name"] is not None:
            continue
        cands = {ff["name"] for s in cp["sessions"] if s["off"] == d["session_off"] for ff in s["ff"]
                 if ff["size"] == d["size"] and ff["sha"] == d["sha_whole"]}
        if len(cands) == 1:
            d["name"] = cands.pop()


def _sessions_after(cp: dict, off: int) -> list[dict]:
    return [s for s in cp["sessions"] if s["state"] == "paused" and s["off"] > off]


def _in(off: int, r: tuple[int, int] | None) -> bool:
    return r is not None and r[0] <= off < r[1]


def _status_pair(cp: dict, before_off: int, after_off: int) -> tuple[dict | None, dict | None]:
    b = [x for x in cp["log_status"] if x["off"] < before_off]
    a = [x for x in cp["log_status"] if x["off"] >= after_off]
    return (b[-1] if b else None), (a[0] if a else None)


def _counter_moving(e: dict) -> bool:
    k = e["kind"]
    return (k in ("RB", "DROP") or (k == "WR" and e["written"] < e["n"]) or (k == "ROT" and e["ok"])
            or (k == "OPEN" and e["torn"]) or (k == "CLOSE" and e["result"] != "ok")
            or (k == "P" and e["state"] == "dropped"))


def replay(cp: dict, i0: tuple[int, int], i1: tuple[int, int] | None, exports: list[dict] | None,
           rot_steps: dict[int, int] | None = None, end_mode: str = "i1") -> dict:
    """LOG-REPLAY. end_mode 'i1' stops at the last I1 quiesce; 'trace' runs to the end
    of the trace (L-8 pre-reset: the chunk being written stays pending)."""
    v = Verdict()
    cont = continuity(cp)
    if cont["status"] != PASS:
        v.add(cont["status"], "continuity: " + cont["status"], detail=cont["reasons"][:5])
    _resolve_dump_names(cp)
    ents = entries_of(cp)
    out: dict = {"continuity": cont["status"]}

    def done() -> dict:
        return {**out, "status": v.status, "reasons": v.json()}

    if not ents:
        v.add(VOID, "no trace entries")
        return done()
    first_full = next(d for d in cp["drains"] if d["entries"])
    on = [o for o in cp["slt_on"] if o["off"] < first_full["off"]]
    on = on[-1] if on else None
    if on is None:
        v.add(VOID, "no SLT_ON before the first drained entry: cannot bind quiesce sessions to QUI entries")
        return done()
    if on["next_seq"] != ents[0]["seq"]:
        v.add(VOID, f"first drained seq {ents[0]['seq']} != SLT_ON next_seq {on['next_seq']}")
    qui = [e for e in ents if e["kind"] == "QUI" and e["state"] == "paused"]
    sessions = _sessions_after(cp, on["off"])
    # The k-th console `SDL_Q paused` after `on` IS the k-th traced `QUI paused`: both
    # are totally ordered and complete (lossless drains, whole capture), so the order
    # binds each inventory/dump to its exact point in the writer's event stream.
    if len(sessions) != len(qui):
        v.add(VOID, f"{len(sessions)} console quiesce sessions after SLT_ON vs {len(qui)} QUI paused entries")
        return done()
    for s, q in zip(sessions, qui):
        s["qui_seq"] = q["seq"]
        if not s["complete"]:
            v.add(VOID, f"quiesce session at {s['off']} never resumed in the capture")
        for why in s["invalid"] + s["timeout"]:
            v.add(VOID, f"quiesce session at {s['off']}: {why} (snapshot void, retake)")
    i0s = [s for s in sessions if _in(s["off"], i0)]
    if not i0s:
        v.add(VOID, "no quiesce session inside the I0 range")
        return done()
    inv0 = [s for s in i0s if s["ff"]]
    act = [(s, d) for s in i0s for d in s["dumps"] if d["name"] == ACTIVE]
    if not inv0:
        v.add(VOID, "I0 has no SDL_FF inventory")
    if not act:
        v.add(VOID, "I0 has no whole-file dump of ambyte.log (active bytes are required)")
    if not inv0 or not act:
        return done()
    inv0s = inv0[-1]
    s0, d0 = act[-1]
    if not d0["ok"] or d0["from_off"] != 0:
        v.add(VOID, f"I0 ambyte.log dump invalid ({'; '.join(d0['why']) or 'not from offset 0'})")
        return done()
    lo_q, hi_q = sorted((inv0s["qui_seq"], s0["qui_seq"]))
    if any(e["kind"] == "ROT" and lo_q < e["seq"] < hi_q for e in ents):
        v.add(VOID, "rotation between the I0 inventory and the I0 dump: inventory stale")
    files: dict[str, MFile] = {}
    for ff in inv0s["ff"]:
        if ff["name"] not in LOG_NAMES:
            v.add(FAIL, f"unexpected file {ff['name']!r} in /sdcard/logs at I0")
            continue
        files[ff["name"]] = MFile(f"I0:{ff['name']}", size=ff["size"], sha256=ff["sha"])
    files[ACTIVE] = MFile(f"I0:{ACTIVE}", d0["data"])
    ff_act = [ff for ff in inv0s["ff"] if ff["name"] == ACTIVE]
    if inv0s["qui_seq"] < s0["qui_seq"] and ff_act:
        ff = ff_act[0]
        if ff["size"] > len(d0["data"]) or sha(d0["data"][:ff["size"]]) != ff["sha"]:
            v.add(FAIL, "I0 SDL_FF of ambyte.log is not a prefix of the later I0 dump")
    for s in i0s:                                     # other I0 dumps give bytes to opaque files
        for d in s["dumps"]:
            if d["name"] and d["name"] != ACTIVE and d["ok"] and d["from_off"] == 0 and d["name"] in files:
                f = files[d["name"]]
                if f.digest() != sha(d["data"]):
                    v.add(FAIL, f"I0 dump of {d['name']} != its SDL_FF sha")
                else:
                    files[d["name"]] = MFile(f.origin, d["data"])
    i1s = [s for s in sessions if _in(s["off"], i1)] if i1 else []
    if end_mode == "i1":
        if not i1s:
            v.add(VOID, "no quiesce session inside the I1 range")
            return done()
        end_seq = i1s[-1]["qui_seq"]
    else:
        end_seq = ents[-1]["seq"] + 1
    start_seq = s0["qui_seq"]
    anchor = [e for e in ents if e["kind"] == "POP" and e["seq"] < start_seq]
    anchor_seq = anchor[-1]["seq"] if anchor else on["next_seq"] - 1
    fired = [f for f in cp["fired"] if f.get("writer") == "sdlog" and f["off"] > on["off"]]
    m = Model(files, fired, rot_steps or {}, v)
    by_qui = {s["qui_seq"]: s for s in sessions}
    checks: list[dict] = []
    outside: list[dict] = []
    for e in ents:
        s = e["seq"]
        if s <= anchor_seq or s > end_seq:
            if _counter_moving(e):
                outside.append({"seq": s, "kind": e["kind"]})
            continue
        if e["kind"] == "P":
            m.push(e)
            continue
        if s <= start_seq:
            continue                                  # between the anchor POP and the I0 pause: in I0
        if e["kind"] == "QUI" and e["state"] == "paused":
            _checkpoint(m, by_qui.get(s), s, v, checks, i1s)
            if s == end_seq:
                break
            continue
        m.apply(e)
    # Ring-empty invariant at the final pause: every producer record pushed before the
    # drain loop's last POP has been popped (the loop exits only on an empty ring).
    if end_mode == "i1" and m.last_pop_seq is not None:
        want = max([end for sq, end in m.p_seq_ends if sq < m.last_pop_seq], default=0)
        if m.pos < want:
            v.add(VOID, f"at the I1 pause {m.pos} stream bytes popped but {want} pushed before the last POP")
    if end_mode == "i1" and m.pending is not None:
        v.add(VOID, f"chunk popped at seq {m.pending[2]} never written or dropped")
    out.update({"anchor_seq": anchor_seq, "start_seq": start_seq, "end_seq": end_seq,
                "window_bytes": m.pos, "checks": checks})
    out.update(_finish(m, v, i1s, exports, end_mode))
    status_b, status_a = _status_pair(cp, i0[0], (i1s[-1]["end"] or i1s[-1]["off"]) if i1s else 1 << 62)
    out["status_before"] = status_b and status_b["acct"]
    out["status_after"] = status_a and status_a["acct"]
    out["outside_window_counter_events"] = outside
    out["model"] = m
    if end_mode == "i1" and exports is not None:
        _check_evict_delta(m, v, status_b, status_a)
    out["status"] = v.status
    out["reasons"] = v.json()
    return out


def _checkpoint(m: Model, sess: dict | None, seq: int, v: Verdict, checks: list[dict], i1s: list[dict]) -> None:
    """At a quiesce point every SDL_FF and every dump must agree with the model."""
    if sess is None:
        return
    is_i1 = sess in i1s
    for ff in sess["ff"]:
        f = m.files.get(ff["name"])
        ok = f is not None and f.size == ff["size"] and (f.digest() is None or f.digest() == ff["sha"])
        checks.append({"seq": seq, "what": "SDL_FF", "name": ff["name"], "ok": ok})
        if not ok:
            v.add(FAIL, f"SDL_FF {ff['name']} at seq {seq} disagrees with the model "
                        f"({'absent' if f is None else f'{f.size} B'} vs {ff['size']} B)")
    if sess["inv_end"] is not None and not sess["fl"]:
        listed = {ff["name"] for ff in sess["ff"]}
        for name in m.files:
            if name not in listed:
                v.add(FAIL, f"model has {name} but the inventory at seq {seq} does not list it")
    for d in sess["dumps"]:
        if d["name"] is None:
            v.add(VOID, f"dump at {d['off']}: file name unresolved (no echoed command, no matching SDL_FF)")
            continue
        if not d["ok"]:
            v.add(VOID, f"dump of {d['name']} at {d['off']} invalid: {'; '.join(d['why'])}")
            continue
        f = m.files.get(d["name"])
        rec = {"seq": seq, "what": "dump", "name": d["name"], "i1": is_i1, "size": d["size"]}
        if f is None:
            rec["ok"] = False
            v.add(FAIL, f"dump of {d['name']} at seq {seq}: the model has no such file")
        elif f.data is None:
            rec["ok"] = d["from_off"] == 0 and sha(d["data"]) == f.digest() and len(d["data"]) == f.size
            if rec["ok"]:
                f.data = bytearray(d["data"])          # an opaque file's bytes become known
            else:
                v.add(FAIL, f"dump of {d['name']} at seq {seq} != its identity sha")
            f.verified = {"mod": f.mod, "seq": seq, "i1": is_i1, "ok": rec["ok"]}
        else:
            got = d["data"]
            fo = d["from_off"]
            want = bytes(f.data[fo:])
            cut = max(0, f.base - fo)
            rec["prefix_ok"] = got[:cut] == want[:cut]
            rec["append_ok"] = got[cut:] == want[cut:]
            rec["ok"] = got == want
            if not rec["ok"]:
                v.add(FAIL, f"dump of {d['name']} at seq {seq}: {len(got)} B != predicted {len(want)} B"
                            f" (I0 prefix {'ok' if rec['prefix_ok'] else 'CHANGED'}, appended "
                            f"{'ok' if rec['append_ok'] else 'MISMATCH'})")
            if fo == 0:
                f.verified = {"mod": f.mod, "seq": seq, "i1": is_i1, "ok": rec["ok"]}
        checks.append(rec)


def _finish(m: Model, v: Verdict, i1s: list[dict], exports: list[dict] | None, end_mode: str) -> dict:
    surviving = []
    for name, f in sorted(m.files.items()):
        touched = bool(f.segs) or f.new or f.quarantined or f.origin == f"I0:{ACTIVE}"
        row = {"name": name, "origin": f.origin, "size": f.size, "sha256": f.digest(), "touched": touched,
               "window_bytes": sum(s[2] for s in f.segs), "quarantined": f.quarantined,
               "i0_size": 0 if f.new else f.base}
        if end_mode == "i1" and touched:
            ver = f.verified
            if ver is None or not ver["i1"]:
                v.add(VOID, f"S file {name} ({f.origin}) has no whole-file dump at I1")
            elif ver["mod"] != f.mod:
                v.add(VOID, f"S file {name} changed after its I1 dump (dump the active file last)")
            row["dump_verified"] = bool(ver and ver["i1"] and ver["mod"] == f.mod and ver["ok"])
        surviving.append(row)
    ev = []
    for x in m.evicted:
        row = {k: x[k] for k in ("seq", "us", "name", "origin", "size", "sha256", "window_bytes")}
        if exports is not None:
            match = [ex for ex in exports if ex.get("sha256") == x["sha256"] and int(ex.get("size", -1)) == x["size"]]
            row["export"] = match[0].get("name") if match else None
            if not match:
                v.add(FAIL, f"evicted {x['origin']} ({x['size']} B, seq {x['seq']}) has no matching prior export")
        ev.append(row)
    return {"S": surviving, "E": ev, "rotations": m.rot_log, "model_counters": m.ctr}


def _check_evict_delta(m: Model, v: Verdict, b: dict | None, a: dict | None) -> None:
    if b is None or a is None:
        v.add(VOID, "log_status before I0 / after I1 not found: counter deltas unavailable")
        return
    d = a["acct"]["evict"] - b["acct"]["evict"]
    want = sum(x["size"] for x in m.evicted if x["size"] > 0)
    if d != want:
        v.add(FAIL, f"delta evict {d} != sum of in-window evicted sizes {want}")


def load_exports(path: str | None) -> list[dict] | None:
    if path is None:
        return None
    obj = json.loads(Path(path).read_text())
    rows = obj["exports"] if isinstance(obj, dict) else obj
    out = []
    for r in rows:
        r = dict(r)
        if "bytes_file" in r:
            b = Path(r["bytes_file"]).read_bytes()
            if "sha256" in r and r["sha256"] != sha(b):
                raise SystemExit(f"export {r.get('name')}: bytes_file sha != recorded sha256")
            r.setdefault("sha256", sha(b))
            r.setdefault("size", len(b))
        out.append(r)
    return out


def _parse_rot_steps(items: list[str] | None) -> dict[int, int]:
    return {int(a): int(b) for a, b in (x.split("=", 1) for x in (items or []))}


# =========================================================================== ledger

BUCKETS = ("surviving", "rolled_back", "unwritten", "dropped_rotate_blocked", "dropped_unavailable",
           "indeterminate_absent", "retained_evicted")


def finalize_owner(m: Model) -> None:
    for f in m.files.values():
        for _fo, so, ln, ind in f.segs:
            m._mark(so, ln, "indeterminate_present" if ind else "surviving")


def buckets_of(m: Model) -> dict:
    ow = bytes(m.owner[:m.pos])
    inv = {c: k for k, c in Model.CODES.items()}
    cnt = {k: 0 for k in list(BUCKETS) + ["indeterminate_present", "live", "unassigned"]}
    for code in set(ow):
        cnt[inv.get(code, "unassigned")] += ow.count(bytes([code]))
    cnt["surviving"] += cnt["indeterminate_present"]   # present indeterminate bytes survive
    return cnt


def ledger(cp: dict, i0, i1, exports, rot_steps=None) -> dict:
    """LOG-LEDGER over W: every byte in exactly one bucket, each bucket == its
    log_status delta, no slack."""
    r = replay(cp, i0, i1, exports, rot_steps)
    m: Model | None = r.pop("model", None)
    v = Verdict()
    for x in r.get("reasons", []):
        v.add(x["status"], "replay: " + x["msg"])
    if r.get("status") == VOID:
        v.model_void("replay is VOID: the ledger cannot be closed on an unmodelled window")
    if m is None:
        return {"status": v.status, "reasons": v.json(), "replay": r.get("status")}
    finalize_owner(m)
    b = buckets_of(m)
    W = m.pos
    total = sum(b[k] for k in BUCKETS)
    if b["unassigned"] or b["live"]:
        v.add(VOID, f"{b['unassigned'] + b['live']} window bytes not attributable to a bucket")
    if total != W:
        v.add(FAIL, f"sum of buckets {total} != window bytes {W}")
    sb, sa = r.get("status_before"), r.get("status_after")
    if r.get("outside_window_counter_events"):
        v.add(VOID, "counter-moving events outside the replayed window (their deltas are unattributable)",
              events=r["outside_window_counter_events"][:10])
    deltas = None
    if sb is None or sa is None:
        v.add(VOID, "log_status before I0 / after I1 missing")
    else:
        deltas = {k: sa[k] - sb[k] for k in ACCT_KEYS}
        eq = {"rb": b["rolled_back"], "unwr": b["unwritten"], "rot": b["dropped_rotate_blocked"],
              "unav": b["dropped_unavailable"], "indet": b["indeterminate_present"] + b["indeterminate_absent"],
              "evict": sum(x["size"] for x in m.evicted if x["size"] > 0),
              "ring": sum(e["len"] for e in m.dropped_p), "torn": m.ctr["torn"]}
        for k, want in eq.items():
            if deltas.get(k) != want:
                v.add(FAIL, f"delta {k} {deltas.get(k)} != ledger {want}")
        # the trace's own numbers (RB since, DROP n, WR written) must agree with the bytes
        for k, bk in (("rb", "rolled_back"), ("unwr", "unwritten"), ("rot", "dropped_rotate_blocked"),
                      ("unav", "dropped_unavailable")):
            if m.ctr[k] != b[bk]:
                v.add(FAIL, f"model counter {k} {m.ctr[k]} != bucket {b[bk]}")
    ret = sum(x["window_bytes"] for x in m.evicted)
    if ret != b["retained_evicted"]:
        v.add(FAIL, f"retained_evicted bucket {b['retained_evicted']} != window bytes in evicted files {ret}")
    return {"status": v.status, "window_bytes": W, "buckets": {k: b[k] for k in BUCKETS},
            "indeterminate_present": b["indeterminate_present"], "deltas": deltas,
            "producer_dropped": sum(e["len"] for e in m.dropped_p), "replay": r.get("status"),
            "S": r.get("S"), "E": r.get("E"), "reasons": v.json()}


# =========================================================================== preserve

class Chain:
    """LOG-PRESERVE tracker over inventories, traced rotations and exports."""

    def __init__(self, v: Verdict):
        self.v = v
        self.pos: dict[str, dict] = {}      # chain name -> identity
        self.ids: list[dict] = []
        self.evictions: list[dict] = []
        self.first = True

    def _new(self, name: str, f: dict, at: int) -> dict:
        ident = {"id": f"{name}@{at}", "size": int(f["size"]), "sha256": f["sha256"], "finalized": False,
                 "exports": [], "evicted": False}
        self.ids.append(ident)
        return ident

    def inventory(self, files: dict, at: int) -> None:
        listed = set(files)
        for name in list(self.pos):
            ident = self.pos[name]
            if name not in listed:
                self.v.add(FAIL, f"step {at}: {ident['id']} vanished from {name} without a traced eviction")
                del self.pos[name]
                continue
            f = files[name]
            if name == ACTIVE and not ident["finalized"]:
                # (a) only ambyte.log may grow, append-only: its first old-size bytes unchanged
                if int(f["size"]) < ident["size"]:
                    self.v.add(FAIL, f"step {at}: {name} shrank {ident['size']} -> {f['size']}")
                pre = _prefix_sha(f, ident["size"])
                if pre is None:
                    self.v.add(VOID, f"step {at}: no hash of {name}[0:{ident['size']}] (SDL_FL rehash or bytes)")
                elif pre != ident["sha256"]:
                    self.v.add(FAIL, f"step {at}: {name} prefix [0:{ident['size']}] changed (not append-only)")
                ident["size"], ident["sha256"] = int(f["size"]), f["sha256"]
            elif (int(f["size"]), f["sha256"]) != (ident["size"], ident["sha256"]):
                self.v.add(FAIL, f"step {at}: {name} ({ident['id']}) content changed")
        for name in sorted(listed - set(self.pos)):
            if name not in LOG_NAMES:
                self.v.add(FAIL, f"step {at}: unexpected file {name}")
                continue
            if not self.first and name != ACTIVE:
                self.v.add(FAIL, f"step {at}: {name} appeared without a traced rotation moving it there")
            self.pos[name] = self._new(name, files[name], at)
        self.first = False

    def export(self, x: dict, at: int) -> None:
        cands = [i for i in self.pos.values() if (i["size"], i["sha256"]) == (int(x["size"]), x["sha256"])]
        if x.get("name") in self.pos and self.pos[x["name"]] in cands:
            cands = [self.pos[x["name"]]]
        if not cands:
            self.v.add(FAIL, f"step {at}: export {x.get('name')} ({x['size']} B) matches no current chain file")
            return
        cands[0]["exports"].append({"step": at, **x})

    def quarantine(self, at: int) -> None:
        if ACTIVE in self.pos:
            self.pos[ACTIVE]["finalized"] = True

    def rotation(self, ev: dict, at: int) -> None:
        if ev.get("ok", True):
            steps = ["remove"] + [f"{i}>{i + 1}" for i in range(4, -1, -1)]
        else:
            steps = list(ev.get("applied") or [])
        for st in steps:
            if st == "remove":
                ident = self.pos.pop(OLDEST, None)
                if ident is None:
                    continue
                ident["evicted"] = True
                d = ev.get("evict_delta")
                final = _exported_final(ident)
                rec = {"step": at, "id": ident["id"], "size": ident["size"], "sha256": ident["sha256"],
                       "exported": final, "evict_delta": d}
                self.evictions.append(rec)
                # (c) evicted as the oldest: a prior export of its FINAL bytes (sha and size)
                # AND delta retention_evicted_bytes == its size
                if not final:
                    self.v.add(FAIL, f"step {at}: eviction of {ident['id']} without a matching prior export")
                if d is None:
                    self.v.add(VOID, f"step {at}: eviction of {ident['id']} without a delta retention_evicted_bytes")
                elif int(d) != ident["size"]:
                    self.v.add(FAIL, f"step {at}: delta evict {d} != evicted size {ident['size']}")
            else:
                a, b = (int(x) for x in st.split(">"))
                na, nb = LOG_NAMES[a], LOG_NAMES[b]
                if na not in self.pos:
                    continue
                if nb in self.pos:
                    self.v.add(FAIL, f"step {at}: rename {na} -> {nb} over an existing file")
                    continue
                self.pos[nb] = self.pos.pop(na)
                if nb == "ambyte.1.log":
                    self.pos[nb]["finalized"] = True      # content final: export-on-finalize due


def _exported_final(ident: dict) -> bool:
    return any((int(x["size"]), x["sha256"]) == (ident["size"], ident["sha256"]) for x in ident["exports"])


def _prefix_sha(f: dict, n: int) -> str | None:
    if "bytes_file" in f:
        return sha(Path(f["bytes_file"]).read_bytes()[:n])
    pre = f.get("prefixes") or {}
    if str(n) in pre:
        return pre[str(n)]
    if int(f.get("size", -1)) == n:
        return f.get("sha256")
    return None


def preserve(events: list[dict]) -> dict:
    """LOG-PRESERVE (a)/(b)/(c) + export-on-finalize. events, in order:
      {"t":"inv","files":{name:{"size":N,"sha256":S,"prefixes":{"<len>":sha}|"bytes_file":P}}}
      {"t":"rot","ok":true,"evict_delta":N}          traced ROT ok (delta retention_evicted_bytes)
      {"t":"rot","ok":false,"applied":["remove","4>5"],"evict_delta":N}   partial (traced fail)
      {"t":"export","name":..,"size":..,"sha256":..} {"t":"quarantine"}"""
    v = Verdict()
    ch = Chain(v)
    for i, ev in enumerate(events):
        t = ev.get("t")
        if t == "inv":
            ch.inventory(ev["files"], i)
        elif t == "rot":
            ch.rotation(ev, i)
        elif t == "export":
            ch.export(ev, i)
        elif t == "quarantine":
            ch.quarantine(i)
        else:
            v.add(VOID, f"step {i}: unknown event {t!r}")
    unexported = [i["id"] for i in ch.ids if i["finalized"] and not _exported_final(i) and not i["evicted"]]
    return {"status": v.status, "evictions": ch.evictions, "finalized_unexported": unexported,
            "chain": {k: {"id": x["id"], "size": x["size"], "sha256": x["sha256"], "exported": _exported_final(x)}
                      for k, x in sorted(ch.pos.items())}, "reasons": v.json()}


def rel_log(exp_rel: dict, captures: list[dict]) -> dict:
    """REL-LOG. exp_rel = {"ambyte.5.log": s5, ..., "ambyte.1.log": s1} (EXP-REL sizes);
    captures = ordered [{"kind":"status","evict":N,"file_bytes":M} | {"kind":"boot"}].
    `evict` is a RAM counter that restarts at 0 on every boot, so the cumulative value is
    the sum of each finished boot segment's last capture plus the current one (plus
    across-reset rotation terms). It must always equal one of the strictly increasing
    prefix sums P0 = 0, P1 = s5, P2 = s5+s4, ... - the index is the rotation count j.
    Any other value FAILs at once (an eviction not tied to an export); j >= 3 STOPs."""
    v = Verdict()
    order = [f"ambyte.{i}.log" for i in range(5, 0, -1)]
    for n in order:
        if int(exp_rel.get(n, 0)) <= 0:
            v.add(STOP, f"REL-LOG precondition: {n} missing or empty at EXP-REL")
    P = [0]
    for n in order:
        P.append(P[-1] + int(exp_rel.get(n, 0)))
    closed = 0              # finished boot segments' last evict + across-reset terms
    seg_last = None
    pre_reset_size = None
    after_boot = False
    rows = []
    jmax = 0
    for c in captures:
        if c["kind"] == "boot":
            if seg_last is not None:
                closed += seg_last["evict"]
                pre_reset_size = seg_last.get("file_bytes")
            seg_last = None
            after_boot = True
            continue
        if after_boot and pre_reset_size is not None and c.get("file_bytes") is not None \
                and c["file_bytes"] < pre_reset_size and c["evict"] == 0:
            # Rotated across the reset (after the pre-reset capture, counter lost with the
            # reset): its eviction is the next prefix term (P5 confirms it by the chain shift).
            # With evict > 0 the rotation happened after boot and is already counted.
            j_now = P.index(closed) if closed in P else None
            if j_now is None or j_now + 1 >= len(P):
                v.add(FAIL, f"across-reset rotation at cumulative {closed}: no next prefix term")
            else:
                closed = P[j_now + 1]
                rows.append({"across_reset_rotation": True, "cumulative": closed})
        after_boot = False
        seg_last = c
        cum = closed + c["evict"]
        j = P.index(cum) if cum in P else None
        rows.append({"evict": c["evict"], "file_bytes": c.get("file_bytes"), "cumulative": cum, "j": j})
        if j is None:
            v.add(FAIL, f"cumulative evictions {cum} match no prefix sum {P}: an eviction not tied to an export")
        else:
            jmax = max(jmax, j)
    if jmax >= 3:
        v.add(STOP, f"j = {jmax} >= 3 rotations: stop the soak, run P5 (export) next")
    return {"status": v.status, "prefix_sums": P, "j": jmax, "captures": rows, "reasons": v.json()}


def rel_captures_from_console(paths: list[str]) -> list[dict]:
    out = []
    for p in paths:
        cp = parse_capture(Path(p).read_bytes())
        ev = [(o, {"kind": "boot"}) for o in cp["boots"]]
        ev += [(x["off"], {"kind": "status", "evict": x["acct"]["evict"], "file_bytes": x["file_bytes"]})
               for x in cp["log_status"]]
        out += [e for _o, e in sorted(ev, key=lambda t: t[0])]
    dedup = []                  # the ESP-ROM and rst:0x lines of one boot are one boot
    for e in out:
        if e["kind"] == "boot" and dedup and dedup[-1]["kind"] == "boot":
            continue
        dedup.append(e)
    return dedup


# =========================================================================== backoff

def backoff(cp: dict, row: str | None = None, tol_us: int = BACKOFF_TOL_US) -> dict:
    """SL-BACKOFF from the trace µs timestamps (never from counts)."""
    v = Verdict()
    ents = entries_of(cp)
    rots = [e for e in ents if e["kind"] == "ROT"]
    out: dict = {"tolerance_us": tol_us,
                 "rotations": [{"seq": e["seq"], "us": e["us"], "ok": e["ok"], "op": e["op"]} for e in rots]}
    gaps = []
    for a, b in zip(rots, rots[1:]):
        if not a["ok"]:
            gap = b["us"] - a["us"]
            gaps.append(gap)
            if gap < BACKOFF_US - tol_us:
                v.add(FAIL, f"ROT at seq {b['seq']} only {gap / 1e6:.3f} s after the failed ROT at seq {a['seq']}")
    out["min_gap_after_fail_s"] = min(gaps) / 1e6 if gaps else None
    spans = []
    i = 0
    while i < len(rots):
        if rots[i]["ok"]:
            i += 1
            continue
        j = i
        while j + 1 < len(rots) and not rots[j + 1]["ok"]:
            j += 1
        ended_ok = j + 1 < len(rots)
        end = rots[j + 1] if ended_ok else rots[j]
        T = (end["us"] - rots[i]["us"]) / 1e6
        attempts = (j - i + 1) + (1 if ended_ok else 0)
        spans.append({"first_fail_seq": rots[i]["seq"], "fails": j - i + 1, "T_s": T, "attempts": attempts,
                      "ended_ok": ended_ok})
        if attempts > math.ceil(T / 60) + 1:
            v.add(FAIL, f"{attempts} attempts in a {T:.1f} s blocked span (> ceil(T/60)+1)")
        i = j + 1
    out["blocked_spans"] = spans
    if row == "L5":
        _backoff_l5(ents, rots, v, out, tol_us)
    elif row == "L6":
        _backoff_l6(cp, rots, v, tol_us)
    out["status"] = v.status
    out["reasons"] = v.json()
    return out


def _backoff_l5(ents, rots, v, out, tol_us):
    fails = [r for r in rots if not r["ok"]]
    if not fails:
        v.add(VOID, "L-5: no ROT fail (the rotation boundary was not reached)")
        return
    if any(r["op"] != "rename" for r in fails):
        v.add(FAIL, "L-5: a failed rotation is not a rename failure")
    oks = [r for r in rots if r["ok"] and r["seq"] > fails[0]["seq"]]
    T = ((oks[0]["us"] if oks else fails[-1]["us"]) - fails[0]["us"]) / 1e6
    out["l5_blocked_s"] = T
    if T < 600:
        v.add(VOID, f"L-5: blocked span {T:.1f} s < 600 s (row did not reach its boundary)")
    size = None
    reached = None
    for e in ents:
        if e["kind"] == "WR":
            size = e["before"] + e["written"]
        elif e["kind"] == "OPEN":
            size = e["size"]
        elif e["kind"] == "DROP" and e["why"] == "rotate_blocked" and e["seq"] > fails[0]["seq"]:
            if size is not None and size + e["n"] > HARD_CAP:
                reached = e["seq"]
            else:
                v.add(FAIL, f"L-5: DROP rotate_blocked at seq {e['seq']} before the file reached {HARD_CAP} B")
            break
    out["l5_first_ceiling_drop_seq"] = reached
    if reached is None:
        v.add(VOID, "L-5: no DROP rotate_blocked at the 2 MiB ceiling (row did not reach its boundary)")
    if len(oks) != 1:
        v.add(FAIL, f"L-5: {len(oks)} ROT ok after the failures (exactly one expected in phase 2)")
    elif oks[0]["us"] - fails[-1]["us"] < BACKOFF_US - tol_us:
        v.add(FAIL, "L-5: the ROT ok came before the backoff expiry")


def _backoff_l6(cp, rots, v, tol_us):
    fails = [r for r in rots if not r["ok"]]
    if len(fails) != 3 or any(r["op"] != "remove" for r in fails):
        v.add(FAIL, f"L-6: {len(fails)} failed rotations ({[r['op'] for r in fails]}), want 3 remove failures")
        return
    for a, b in zip(fails, fails[1:]):
        if b["us"] - a["us"] < BACKOFF_US - tol_us:
            v.add(FAIL, "L-6: consecutive failures closer than 60 s")
    after = [r for r in rots if r["ok"] and r["seq"] > fails[-1]["seq"]]
    if not after:
        v.add(VOID, "L-6: no ROT ok after the three failures")
        return
    if after[0]["us"] - fails[0]["us"] < 180_000_000 - tol_us:
        v.add(FAIL, "L-6: ROT ok < 180 s after the first failure")
    if after[0]["us"] - fails[-1]["us"] < BACKOFF_US - tol_us:
        v.add(FAIL, "L-6: ROT ok before the backoff expiry")
    storm = [r for r in rots if fails[0]["seq"] <= r["seq"] <= after[0]["seq"]]
    if len(storm) != 4:
        v.add(FAIL, f"L-6: {len(storm)} rotation attempts from first failure to recovery (4: no EEXIST storm)")
    b = cp["begin"][-1]["us"] if cp["begin"] else None
    e = cp["end"][-1]["us"] if cp["end"] else None
    if b is None or e is None:
        v.add(VOID, "L-6: SDL_BEGIN/SDL_END missing (emission window unknown)")
    elif not all(b <= r["us"] <= e for r in storm):
        v.add(FAIL, "L-6: attempts outside the emission window")


# =========================================================================== L-8

def l8post(data: bytes, cp_all: dict, i0: tuple[int, int], i1: tuple[int, int] | None,
           exports: list[dict] | None, lo: int = 0, hi: int | None = None) -> dict:
    """L-8: pre-reset replay to the witness, torn tail byte-exact from the witness,
    first post-boot writer event OPEN ambyte.log <predicted> torn=1, never appended,
    retired by rotation, later dump byte-identical. The ledger closes at the witness
    (RAM counters restart at the reset): reported without counter deltas."""
    v = Verdict()
    wit = cp_all["witness"]
    if len(wit) != 1:
        v.add(VOID, f"{len(wit)} SLT_RESET_WITNESS lines (exactly one expected)")
        return {"status": v.status, "reasons": v.json()}
    w = wit[0]
    boots = [b for b in cp_all["boots"] if b > w["off"]]
    if not boots:
        v.add(VOID, "no boot banner after the witness")
        return {"status": v.status, "reasons": v.json()}
    boot = boots[0]
    pre = parse_capture(data, lo, boot)
    post = parse_capture(data, boot, hi)
    before_w = [d for d in pre["drains"] if d["complete"] and d["end"] is not None and d["end"] < w["off"]]
    after_w = [d for d in pre["drains"] if d["off"] > w["off"]]
    if len(before_w) < 2:
        v.add(VOID, f"only {len(before_w)} complete drains before the witness (>= 2 required)")
    if not after_w or not after_w[-1]["complete"]:
        v.add(VOID, "the witness's synchronous final drain is missing or incomplete before the boot banner")
    if sha(w["data"]) != w["sha"] or len(w["data"]) != w["n"] or not 0 < w["half"] < w["n"]:
        v.add(VOID, "witness chunk sha/len/half inconsistent")
    fired = [f for f in pre["fired"] if f["off"] > w["off"] and f.get("kind") == "cpu_reset"]
    if not fired or "sd_power=not_interrupted" not in fired[-1]["raw"]:
        v.add(FAIL, "no HIL_FAULT fired kind=cpu_reset ... sd_power=not_interrupted after the witness")
    r = replay(pre, i0, None, exports, end_mode="trace")
    m: Model | None = r.pop("model", None)
    for x in r.get("reasons", []):
        v.add(x["status"], "pre-reset replay: " + x["msg"])
    out: dict = {"pre_replay": r.get("status"), "boot_off": boot}
    if m is None:
        return {**out, "status": v.status, "reasons": v.json()}
    f = m.files.get(ACTIVE)
    if m.pending is None:
        v.add(VOID, "no popped chunk pending at the reset (the reset was not mid-write)")
    elif f is None or f.data is None:
        v.add(VOID, "no modelled ambyte.log at the reset")
    else:
        so, n, _s = m.pending
        chunk = bytes(m.stream[so:so + n])
        if w["n"] != n or w["data"] != chunk:
            v.add(FAIL, "witness chunk bytes != the replayed popped chunk")
        if w["before"] != f.size:
            v.add(FAIL, f"witness offset {w['before']} != replayed file size {f.size}")
        if not w["path"].endswith("/" + ACTIVE):
            v.add(FAIL, f"witness path {w['path']} is not the active log")
        half = w["half"]
        f.data += chunk[:half]
        f.segs.append([w["before"], so, half, False])
        m._mark(so, half, "live")
        m._mark(so + half, n - half, "unwritten")
        m.pending = None
        f.mod += 1
        out["predicted_size"] = f.size
        out["torn_tail_bytes"] = half
    finalize_owner(m)
    out["pre_ledger"] = {k: c for k, c in buckets_of(m).items() if k in BUCKETS}
    on = [o for o in post["slt_on"] if o["autoarm"]]
    if not on:
        v.add(VOID, "no SLT_ON ... autoarm after the boot")
        return {**out, "status": v.status, "reasons": v.json()}
    cont = continuity(post)
    if cont["status"] != PASS:
        v.add(cont["status"], "post-boot continuity: " + cont["status"], detail=cont["reasons"][:5])
    pents = entries_of(post)
    if not pents or pents[0]["seq"] != on[0]["next_seq"]:
        v.add(VOID, "post-boot trace does not start at the auto-arm next_seq")
    wev = [e for e in pents if e["kind"] not in ("P", "POP")]
    if not wev:
        v.add(VOID, "no post-boot writer event")
        return {**out, "status": v.status, "reasons": v.json()}
    first = wev[0]
    want = out.get("predicted_size")
    if not (first["kind"] == "OPEN" and first["name"] == ACTIVE and first["size"] == want and first["torn"] == 1):
        v.add(FAIL, f"first post-boot writer event is {first['kind']} {first.get('name', '')} "
                    f"{first.get('size', '')} torn={first.get('torn', '')}, want OPEN {ACTIVE} {want} torn=1")
    for e in wev[1:]:
        if e["kind"] == "WR":
            v.add(FAIL, f"WR at seq {e['seq']} appended to the torn file before it was rotated away")
            break
        if e["kind"] == "ROT" and e["ok"]:
            out["retired_by_rot_seq"] = e["seq"]
            break
    else:
        v.add(VOID, "no ROT ok retiring the torn file in the post-boot trace")
    if i1 is not None:
        files = dict(m.files)
        torn_file = files.get(ACTIVE)
        pm = Model(files, [x for x in post["fired"] if x.get("writer") == "sdlog"], {}, v)
        sess = _sessions_after(post, on[0]["off"])
        qui = [e for e in pents if e["kind"] == "QUI" and e["state"] == "paused"]
        if len(sess) != len(qui):
            v.add(VOID, "post-boot quiesce sessions do not match QUI entries")
        else:
            _resolve_dump_names(post)
            by_qui = {}
            for s_, q in zip(sess, qui):
                s_["qui_seq"] = q["seq"]
                by_qui[q["seq"]] = s_
            i1s = [s_ for s_ in sess if _in(s_["off"], i1)]
            end_seq = i1s[-1]["qui_seq"] if i1s else None
            checks: list[dict] = []
            for e in pents:
                if e["kind"] == "P":
                    pm.push(e)
                    continue
                if e["kind"] == "QUI" and e["state"] == "paused":
                    _checkpoint(pm, by_qui.get(e["seq"]), e["seq"], v, checks, i1s)
                    if e["seq"] == end_seq:
                        break
                    continue
                pm.apply(e)
            out["post_checks"] = checks
            tv = torn_file.verified if torn_file is not None else None
            if not tv or not tv["ok"]:
                v.add(FAIL if tv else VOID, "the torn file's post-boot dump is missing or not byte-identical")
    out["status"] = v.status
    out["reasons"] = v.json()
    return out


# =========================================================================== rlen

def rlen(run: str, u: int, pad: int = 160, f: float = 20.0, s_active: int | None = None,
         rho: float | None = None) -> dict:
    """Exact synthetic record length R(u) (§6.2; frozen against the host framing by
    SDE-2) and the L-row planning numbers (conservative: R_min at the current u)."""
    R = 21 + 3 + u + 12 + len(run) + 1 + 6 + 1 + pad + 1
    N = lambda t: math.ceil(1.2 * t * f)  # noqa: E731
    out: dict = {"R": R, "u": u, "run_len": len(run), "pad": pad, "f": f}
    if R > LINE_MAX:
        out["error"] = f"R={R} exceeds the {LINE_MAX}-byte record limit (records would be truncated)"
    if s_active is not None:
        B = FILE_CAP - s_active + 256
        out["B_rot"] = B
        t0 = B / (f * R)
        out["L0"] = {"t_rot0_s": t0, "N": N(t0), "deadline_s": N(t0) / f + 180}
        if rho:
            t_rot = B / rho
            Tb = max(600.0, (FILE_CAP + 65536) / rho)
            out["t_rot_s"] = t_rot
            out["N_rot"] = N(t_rot)
            out["L5"] = {"T_b_s": Tb, "phase1_N": N(t_rot + Tb), "phase2_N": N(90),
                         "deadline_s": (N(t_rot + Tb) + N(90)) / f + 180}
            out["L6"] = {"N": N(t_rot + 240), "deadline_s": N(t_rot + 240) / f + 180}
    return out


def record_bytes(when: str, uptime_ms: int, run: str, k: int, pad_str: str) -> bytes:
    """The framed H3 record (log v1, colours off): `<date time>  W (<ms>) HILSDLOG: <msg>\\n`."""
    return f"{when}  W ({uptime_ms}) HILSDLOG: {run} {k} {pad_str}\n".encode()


# =========================================================================== CLI

def _json_default(o):
    if isinstance(o, (bytes, bytearray)):
        return base64.b64encode(bytes(o)).decode()
    return str(o)


def _emit(obj: dict, out: str | None = None) -> int:
    obj = {k: v for k, v in obj.items() if k != "model"}
    text = json.dumps(obj, indent=1, default=_json_default)
    if out:
        fd = os.open(out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as fh:
            fh.write(text)
        print(json.dumps({"status": obj.get("status"), "out": out}))
    else:
        print(text)
    return EXIT.get(obj.get("status", PASS), 1)


def _strip_bytes(cp: dict, with_bytes: bool):
    def clean(x):
        if isinstance(x, dict):
            return {k: clean(v) for k, v in x.items() if with_bytes or k != "data"}
        if isinstance(x, list):
            return [clean(i) for i in x]
        return x
    return clean(cp)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="sdlog_check.py", description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def cap_args(p):
        p.add_argument("--capture", required=True, help="raw serial_daemon capture (private)")
        p.add_argument("--from", dest="lo", type=lambda s: int(s, 0))
        p.add_argument("--to", dest="hi", type=lambda s: int(s, 0))

    p = sub.add_parser("parse")
    cap_args(p)
    p.add_argument("--out")
    p.add_argument("--with-bytes", action="store_true")
    p = sub.add_parser("undump")
    cap_args(p)
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("--index", type=int)
    g.add_argument("--all", action="store_true")
    p.add_argument("--out", required=True, help="file (--index) or directory (--all); written 0600/0700")
    p = sub.add_parser("continuity")
    cap_args(p)
    p.add_argument("--cap", type=int)
    for name in ("replay", "verify", "ledger", "l8post"):
        p = sub.add_parser(name)
        cap_args(p)
        p.add_argument("--i0", required=True, help="capture byte range A:B holding the I0 quiesce sessions")
        p.add_argument("--i1", required=name in ("verify", "ledger"), help="capture byte range of the I1 sessions")
        p.add_argument("--exports", required=name in ("verify", "ledger"),
                       help="JSON [{name,size,sha256[,bytes_file]}] of export-on-finalize dumps")
        p.add_argument("--rot-step", action="append", help="SEQ=SRC_INDEX for an organic rename failure")
        p.add_argument("--out")
    p = sub.add_parser("preserve")
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("--events", help="LOG-PRESERVE events JSON (see preserve())")
    g.add_argument("--rel", help='REL-LOG JSON {"exp_rel":{...},"captures":[...]}')
    p.add_argument("--rel-console", nargs="+", help="with --rel: read the captures from console logs")
    p = sub.add_parser("backoff")
    cap_args(p)
    p.add_argument("--row", choices=("L5", "L6"))
    p.add_argument("--tol-us", type=int, default=BACKOFF_TOL_US)
    p = sub.add_parser("rlen")
    p.add_argument("--run", required=True)
    p.add_argument("--u", type=int, required=True, help="uptime digit count")
    p.add_argument("--pad", type=int, default=160)
    p.add_argument("--f", type=float, default=20.0)
    p.add_argument("--s-active", type=int)
    p.add_argument("--rho", type=float)
    a = ap.parse_args(argv)

    if a.cmd == "rlen":
        r = rlen(a.run, a.u, a.pad, a.f, a.s_active, a.rho)
        return _emit({"status": FAIL if "error" in r else PASS, **r})
    if a.cmd == "preserve":
        if a.events:
            ev = json.loads(Path(a.events).read_text())
            return _emit(preserve(ev["events"] if isinstance(ev, dict) else ev))
        obj = json.loads(Path(a.rel).read_text())
        caps = rel_captures_from_console(a.rel_console) if a.rel_console else obj["captures"]
        return _emit(rel_log(obj["exp_rel"], caps))
    data, cp = load_capture(a.capture, a.lo, a.hi)
    if a.cmd == "parse":
        summ = {"status": PASS, "range": cp["range"], "drains": len(cp["drains"]),
                "entries": len(entries_of(cp)), "orphans": len(cp["orphans"]), "sessions": len(cp["sessions"]),
                "dumps": len(cp["dumps"]), "ff": len(cp["ff"]), "fired": len(cp["fired"]),
                "log_status": [x["acct"] for x in cp["log_status"]], "witness": len(cp["witness"]),
                "boots": cp["boots"], "slt_on": cp["slt_on"], "malformed": cp["malformed"][:20]}
        if a.out:
            fd = os.open(a.out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "w") as fh:
                json.dump(_strip_bytes(cp, a.with_bytes), fh, default=_json_default)
        return _emit(summ)
    if a.cmd == "undump":
        v = Verdict()
        _resolve_dump_names(cp)
        if a.all:
            dumps = cp["dumps"]
        else:
            dumps = [cp["dumps"][a.index]] if 0 <= a.index < len(cp["dumps"]) else []
        if not dumps:
            return _emit({"status": VOID, "reasons": [{"status": VOID, "msg": "no such dump in range"}]})
        if a.all:
            os.makedirs(a.out, mode=0o700, exist_ok=True)
        rows = []
        for k, d in enumerate(dumps):
            path = os.path.join(a.out, f"dump-{k:03d}.bin") if a.all else a.out
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "wb") as fh:
                fh.write(d["data"])
            ff = [x for s in cp["sessions"] if s["off"] == d["session_off"] for x in s["ff"] if x["name"] == d["name"]]
            ffok = None if not ff else (ff[-1]["size"] == d["size"] and ff[-1]["sha"] == d["sha_whole"])
            if not d["ok"]:
                v.add(VOID, f"dump at {d['off']}: {'; '.join(d['why'])}")
            if ffok is False:
                v.add(FAIL, f"dump at {d['off']} != same-quiesce SDL_FF of {d['name']}")
            rows.append({"out": path, "name": d["name"], "from_off": d["from_off"], "size": d["size"],
                         "sha256_whole": d["sha_whole"], "sha256_range": d["sha_range"], "ok": d["ok"],
                         "sdl_ff_match": ffok})
        return _emit({"status": v.status, "dumps": rows, "reasons": v.json()})
    if a.cmd == "continuity":
        return _emit(continuity(cp, a.cap))
    if a.cmd == "backoff":
        return _emit(backoff(cp, a.row, a.tol_us))
    exports = load_exports(a.exports)
    i0, i1 = _rng(a.i0), _rng(a.i1)
    steps = _parse_rot_steps(a.rot_step)
    if a.cmd in ("replay", "verify"):
        return _emit(replay(cp, i0, i1, exports, steps, end_mode="i1" if i1 else "trace"), a.out)
    if a.cmd == "ledger":
        return _emit(ledger(cp, i0, i1, exports, steps), a.out)
    return _emit(l8post(data, cp, i0, i1, exports, a.lo or 0, a.hi), a.out)


if __name__ == "__main__":
    sys.exit(main())
