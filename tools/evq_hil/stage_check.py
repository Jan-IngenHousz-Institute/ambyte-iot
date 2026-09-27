#!/usr/bin/env python3
"""AMBIT writer rows AO-1..5 / AF-1..3 (Sprint 2 contract r4 §6.2): console-pattern and
SD-residue checks around `ambit_ota 1 U` / `ambit_flash 1 /sdcard/hils2afw`.

    stage_check.py ao --row AO-1 --capture CAP [--from N --to M] [--sd-before CAP2 --sd-after CAP3]
                      [--expect-versions AMBIT1=1.4.0,AMBIT3=1.4.0]
    stage_check.py af --row AF-1 --capture CAP [--from --to] [--sd-before CAP2 --sd-after CAP3] [...]

The capture range must hold, in order: an `ambit_versions` block, the armed case
(fired line + the command's log), and a second `ambit_versions` block. SD residue
comes from `evq_hil sd_inv /` captures (EVQ_FF / EVQ_DIR lines, tools/evq_hil/hillog.py).
Exit 0 PASS, 1 FAIL, 2 VOID (evidence missing), 3 STOP (a stream/flash line for the
live channels ch0/ch2 = AMBIT1/AMBIT3, or `ping_uart 1` connected - the contract's
STOP condition, never merely a FAIL).
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import hillog  # noqa: E402

PASS, FAIL, VOID, STOP = "PASS", "FAIL", "VOID", "STOP"
EXIT = {PASS: 0, FAIL: 1, VOID: 2, STOP: 3}

STAGE_CLASS = "/sdcard/ambit_fw.stg-"          # AMBIT OTA stages: single-named ambit_fw.stg-<k>.bin
AF_DIR = "/sdcard/hils2afw"
# Channel contact = any UART/ROM exchange with an AMBIT (ambit_ota.c / ambit_flash.c log lines):
# the per-channel OTA path (ambit_ota_one: fw before/after reads, OTA_BEGIN/DATA/END, streaming),
# ROM probes, and ping. AO-1..4 must fail BEFORE any of these (download/stage failure).
CONTACT_RE = re.compile(r"ambit_ota_one|AMBIT\d+ fw (?:before|after)|OTA_(?:BEGIN|DATA|END|ABORT)|streaming \d+%|"
                        r"stream aborted|ROM OK|no ROM response|(?i:\bping(?:_uart)?\b.*ambit|ambit.*\bping\b)")
# AMBIT reset / bootloader entry (ambit_flash must return before touching the AMBIT).
RESET_RE = re.compile(r"hard-reset|ROM OK|no ROM response|entering (?:ROM|bootloader)|AMBIT\d+: ROM", re.I)
# STOP: any stream or flash line for the live channels (index 0 and 2 print as AMBIT1 / AMBIT3).
LIVE_STOP_RE = re.compile(r"AMBIT[13]\b.*(?:stream|OTA_|flash|ROM OK|rebooting)|ch[02]\b.*(?:flash|stream)", re.I)
PING_CONNECTED_RE = re.compile(r"(?:ping_uart\s+1|AMBIT2|ch(?:annel)?\s*1)\b.*(?<!not )(?<!dis)\bconnected\b", re.I)
VERS_RE = re.compile(r"^\s*(AMBIT\d+): (v(\d+\.\d+\.\d+)|absent|present, no version.*)")
FIRED_RE = re.compile(r"HIL_FAULT fired (.*)$")


def _lines(path: str, lo: int | None, hi: int | None) -> list[str]:
    data = Path(path).read_bytes()[lo or 0:hi]
    return [ln.replace("\r", "") for ln in data.decode(errors="replace").splitlines()]


def version_blocks(lines: list[str]) -> list[dict]:
    blocks, cur = [], None
    for ln in lines:
        if "AMBIT firmware versions:" in ln:
            cur = {}
            blocks.append(cur)
            continue
        m = VERS_RE.match(ln)
        if m and cur is not None:
            cur[m.group(1)] = m.group(3) or m.group(2)
        elif cur is not None and ln.strip() and not m:
            cur = None
    return blocks


def fired(lines: list[str]) -> list[dict]:
    out = []
    for ln in lines:
        m = FIRED_RE.search(ln)
        if m:
            out.append(dict(t.split("=", 1) for t in m.group(1).split() if "=" in t))
    return out


def sd_files(path: str | None) -> tuple[dict, list[str]] | None:
    if path is None:
        return None
    invs = hillog.sd_inventories(Path(path).read_text(errors="replace"))
    if not invs:
        raise SystemExit(f"{path}: no complete EVQ_SDSNAP..EVQ_SDEND inventory")
    inv = invs[-1]
    return inv["files"], inv["dirs"]


class Check:
    def __init__(self) -> None:
        self.rows: list[dict] = []

    def need(self, ok: bool, what: str, status_if_not: str = FAIL, **kw) -> None:
        self.rows.append({"check": what, "ok": bool(ok), **({} if ok else {"status": status_if_not}), **kw})

    @property
    def status(self) -> str:
        bad = [r["status"] for r in self.rows if not r["ok"]]
        for s in (STOP, FAIL, VOID):
            if s in bad:
                return s
        return PASS


def common(ck: Check, lines: list[str], expect_versions: dict[str, str]) -> None:
    stops = [ln for ln in lines if LIVE_STOP_RE.search(ln)]
    ck.need(not stops, "no stream/flash line for ch0/ch2 (AMBIT1/AMBIT3)", STOP, lines=stops[:5])
    pc = [ln for ln in lines if PING_CONNECTED_RE.search(ln)]
    ck.need(not pc, "ping_uart 1 not connected (AMBIT2 slot empty)", STOP, lines=pc[:3])
    vb = version_blocks(lines)
    if len(vb) < 2:
        ck.need(False, "ambit_versions before and after the case", VOID, found=len(vb))
        return
    ck.need(vb[0] == vb[-1], "AMBIT versions unchanged", before=vb[0], after=vb[-1])
    for ch, ver in expect_versions.items():
        ck.need(vb[-1].get(ch) == ver, f"{ch} at {ver}", got=vb[-1].get(ch))


def residue(ck: Check, before, after, row: str) -> None:
    if before is None or after is None:
        ck.need(False, "SD inventories before/after supplied", VOID)
        return
    bf, _bd = before
    af, ad = after
    protected = [p for p in set(bf) | set(af) if p == "/sdcard/ambit_fw.bin" or p.startswith("/sdcard/ambit_fw/")]
    changed = sorted(p for p in protected if bf.get(p) != af.get(p))
    ck.need(not changed, "/sdcard/ambit_fw.bin and /sdcard/ambit_fw/** byte-identical", changed=changed[:10])
    stages = sorted(p for p in af if p.startswith(STAGE_CLASS))
    if row in ("AO-4", "AO-5"):
        ck.need(not stages, "no stage file left (fixed-name cleanup / stage removed)", stages=stages)
    else:
        ck.rows.append({"check": "stages kept as evidence (removed only by the next attempt's cleanup)",
                        "ok": True, "stages": stages})
    ck.rows.append({"check": "synthetic residue", "ok": True,
                    "hils2afw": sorted(d for d in ad if d.startswith(AF_DIR)) +
                    sorted(p for p in af if p.startswith(AF_DIR + "/"))})


def check_ao(row: str, lines: list[str], before, after, expect_versions) -> dict:
    ck = Check()
    common(ck, lines, expect_versions)
    fl = [f for f in fired(lines) if f.get("writer") == "ambit_ota"]
    other = [f for f in fired(lines) if f.get("writer") != "ambit_ota"]
    ck.need(not other, "no firing of another writer", fired=other[:3])
    contact = [ln for ln in lines if CONTACT_RE.search(ln)]
    if row in ("AO-1", "AO-2", "AO-3"):
        ck.need(len(fl) == 1, "exactly one ambit_ota firing", fired=len(fl))
        ck.need(all(f.get("path", "").startswith(STAGE_CLASS) for f in fl), f"fired path class {STAGE_CLASS}*",
                paths=[f.get("path") for f in fl])
        want_op = {"AO-1": "write", "AO-2": "fsync", "AO-3": "read"}[row]
        ck.need(all(f.get("op") == want_op for f in fl), f"fired op {want_op}")
        ck.need(any("download failed" in ln for ln in lines), "'download failed' logged")
        ck.need(not contact, "no channel contact (no ambit_ota_one / OTA_* / ping line)", lines=contact[:5])
    elif row == "AO-4":
        ck.need(len(fl) == 1 and fl[0].get("kind") == "cpu_reset" and
                fl[0].get("path", "").startswith(STAGE_CLASS), "one cpu_reset firing on a stage path")
        ck.need(any(hillog_boot(ln) for ln in lines), "boot banner after the reset")
    elif row == "AO-5":
        ck.need(not fl, "no firing (unarmed)", fired=len(fl))
        ck.need(any("OTA_BEGIN failed" in ln for ln in lines), "OTA_BEGIN fails on the absent ch1")
        ck.need(not any(re.search(r"AMBIT[13]\b", ln) and CONTACT_RE.search(ln) for ln in lines),
                "contact only with ch1 (AMBIT2)")
    residue(ck, before, after, row)
    return {"row": row, "status": ck.status, "checks": ck.rows}


def hillog_boot(ln: str) -> bool:
    return "ESP-ROM:" in ln or "rst:0x" in ln


def check_af(row: str, lines: list[str], before, after, expect_versions) -> dict:
    ck = Check()
    common(ck, lines, expect_versions)
    fl = [f for f in fired(lines) if f.get("writer") == "ambit_flash"]
    resets = [ln for ln in lines if RESET_RE.search(ln)]
    ck.need(not resets, "no AMBIT reset / ROM line (returns before touching the AMBIT)", lines=resets[:5])
    if row == "AF-1":
        ck.need(len(fl) == 1 and fl[0].get("op") == "mkdir" and fl[0].get("path", "").startswith(AF_DIR),
                f"one ambit_flash mkdir firing on {AF_DIR}", fired=fl[:2])
        ck.need(any("cannot create" in ln for ln in lines), "'cannot create' logged")
        if after is not None:
            ck.need(not any(d.startswith(AF_DIR) for d in after[1]) and
                    not any(p.startswith(AF_DIR) for p in after[0]), f"no {AF_DIR} directory created")
    elif row == "AF-2":
        ck.need(not fl, "no firing (unarmed)")
        ck.need(any("ESP_ERR_NOT_FOUND" in ln for ln in lines), "returns ESP_ERR_NOT_FOUND (regions missing)")
    elif row == "AF-3":
        ck.need(len(fl) == 1 and fl[0].get("op") == "open" and fl[0].get("path", "").startswith(AF_DIR + "/"),
                f"one ambit_flash open firing under {AF_DIR}/", fired=fl[:2])
        ck.need(any(re.search(r"ESP_ERR_\w+|cannot|failed", ln) for ln in lines), "fails closed with an error")
    if before is not None and after is not None:
        residue(ck, before, after, row)
    return {"row": row, "status": ck.status, "checks": ck.rows}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="stage_check.py", description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name, rows in (("ao", ("AO-1", "AO-2", "AO-3", "AO-4", "AO-5")), ("af", ("AF-1", "AF-2", "AF-3"))):
        p = sub.add_parser(name)
        p.add_argument("--row", required=True, choices=rows)
        p.add_argument("--capture", required=True)
        p.add_argument("--from", dest="lo", type=lambda s: int(s, 0))
        p.add_argument("--to", dest="hi", type=lambda s: int(s, 0))
        p.add_argument("--sd-before")
        p.add_argument("--sd-after")
        p.add_argument("--expect-versions", default="AMBIT1=1.4.0,AMBIT3=1.4.0")
    a = ap.parse_args(argv)
    ev = dict(x.split("=", 1) for x in a.expect_versions.split(",") if x)
    lines = _lines(a.capture, a.lo, a.hi)
    fn = check_ao if a.cmd == "ao" else check_af
    out = fn(a.row, lines, sd_files(a.sd_before), sd_files(a.sd_after), ev)
    print(json.dumps(out, indent=1))
    return EXIT[out["status"]]


if __name__ == "__main__":
    sys.exit(main())
