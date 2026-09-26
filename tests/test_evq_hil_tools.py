"""Sprint 02 host tests for the on-device verification layer
(docs/evq-sd-overflow-hil-contract.md §1.6): PAY-1, MAN-1, OTA-1, IO-1, ISO-1.
HOLD-1 lives in the evq_host scenario suite (tests/test_evq_storage.py).
"""
from __future__ import annotations

import binascii
import json
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import types
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "evq_hil"))
import hilpay  # noqa: E402
import manifest  # noqa: E402
import ota_select  # noqa: E402

CC = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc") or "cc"
SAN = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]


def _build(srcs: list[str], incs: list[str], defs: list[str], out: Path) -> Path:
    cmd = [CC, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", *SAN, *defs,
           *[f"-I{ROOT / i}" for i in incs], *[str(ROOT / s) for s in srcs], "-o", str(out)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise AssertionError(f"compile failed: {' '.join(cmd)}\n{r.stderr}")
    return out


class Pay1(unittest.TestCase):
    """PAY-1: device C generator == host Python twin over 1000 cases, canonical JSON."""

    def test_c_equals_python(self):
        with tempfile.TemporaryDirectory() as d:
            exe = _build(["components/evq_hil/evq_hil_payload.c", "tests/evq_hil_host/pay_main.c"],
                         ["components/evq_hil/include"], [], Path(d) / "pay")
            rng = random.Random(20260926)
            cases = [("hil-20260926-P4-01", 0, 4000), ("r", 0, 0), ("A.b_c-9", 4294967295, 1)]
            while len(cases) < 1000:
                cases.append((f"hil-{rng.randrange(10**6)}-{rng.choice('ab')}", rng.randrange(1 << 32),
                              rng.randrange(0, 5000)))
            stdin = "".join(f"{r} {k} {p}\n" for r, k, p in cases)
            out = subprocess.run([str(exe)], input=stdin, capture_output=True, text=True, check=True).stdout
            lines = out.split("\n")[:-1]
            self.assertEqual(len(lines), len(cases))
            for (r, k, p), got in zip(cases, lines):
                want = hilpay.payload(r, k, p)
                self.assertEqual(got, want, (r, k, p))
                obj = json.loads(got)
                self.assertEqual(json.dumps(obj, separators=(",", ":")), got)   # canonical compact
                self.assertEqual((obj["evq_hil"], obj["k"], len(obj["pad"])), (r, k, p))

    def test_bad_run_rejected(self):
        with self.assertRaises(ValueError):
            hilpay.payload('x"y', 0, 4)


def _capture(run: str, recs: list[tuple[int, int]], pad: int = 64, t0: int = 1790384000000) -> str:
    out = [f"EVQ_BEGIN {run} {recs[0][0]} {len(recs)} {pad} {recs[0][1]}"]
    for k, mid in recs:
        pl = hilpay.payload(run, k, pad)
        line = hilpay.stored_line(mid, run, t0, t0, pl)
        out.append(f"EVQ_ACC {run} {k} {mid} {t0} {t0} {len(line)} {hilpay.sha(line)} {hilpay.sha(pl)}")
    out.append(f"EVQ_END {run} {recs[-1][0]} {len(recs)} 0 {recs[-1][1] + 1}")
    return "\n".join(out) + "\n"


class Man1(unittest.TestCase):
    """MAN-1: parser accepts a consistent capture and rejects every tamper class."""

    def test_valid(self):
        cap = _capture("hil-t-01", [(k, 1000 + k) for k in range(20)])
        s = manifest.verify(manifest.parse(["noise\n" + cap]))
        self.assertEqual(s["accepted"], 20)

    def _bad(self, cap: str):
        with self.assertRaises(manifest.ManifestError):
            manifest.verify(manifest.parse([cap]))

    def test_tampered_payload_hash(self):
        cap = _capture("hil-t-02", [(0, 5), (1, 6)])
        self._bad(cap.replace(hilpay.sha(hilpay.payload("hil-t-02", 1, 64)), "0" * 64))

    def test_tampered_line_hash(self):
        cap = _capture("hil-t-03", [(0, 5)])
        pl = hilpay.payload("hil-t-03", 0, 64)
        self._bad(cap.replace(hilpay.sha(hilpay.stored_line(5, "hil-t-03", 1790384000000, 1790384000000, pl)), "f" * 64))

    def test_duplicate_run_k(self):
        cap = _capture("hil-t-04", [(0, 5), (1, 6)])
        dup = [x for x in cap.splitlines() if x.startswith("EVQ_ACC hil-t-04 1 ")][0]
        self._bad(cap + dup.replace(" 6 ", " 9 ", 1) + "\n")

    def test_id_reused_with_other_line(self):
        a = _capture("hil-t-05", [(0, 77)])
        b = _capture("hil-t-06", [(0, 77)])
        self._bad(a + b)

    def test_refused_and_accepted(self):
        cap = _capture("hil-t-07", [(0, 5)]) + "EVQ_REF hil-t-07 1 5 ESP_ERR_NO_MEM flash_full_sd_full\n"
        self._bad(cap)

    def test_truncated(self):
        self._bad("EVQ_BEGIN r 0 1 8 1\nEVQ_ACC r 0 1 2\n")


def _otadata(entries: list[tuple[int, int] | None]) -> bytes:
    out = b""
    for e in entries:
        if e is None:
            out += b"\xff" * 0x1000
            continue
        seq, state = e
        ent = struct.pack("<I", seq) + b"\xff" * 20 + struct.pack("<I", state) + \
            struct.pack("<I", binascii.crc32(struct.pack("<I", seq), 0xFFFFFFFF))
        out += ent + b"\xff" * (0x1000 - 32)
    return out


def _otatool():
    p = Path("/home/dv/.platformio/packages/framework-espidf/components/app_update")
    if not (p / "otatool.py").exists():
        return None
    sys.path.insert(0, str(p))
    sys.path.insert(0, str(p.parent / "partition_table"))
    try:
        import otatool  # type: ignore
        return otatool
    except Exception:  # noqa: BLE001
        return None


class Ota1(unittest.TestCase):
    """OTA-1: the rendered sector selects the slot with state NEW and parses
    identically under ESP-IDF otatool's own reader."""

    def test_select_ota0_from_running_ota1(self):
        cur = _otadata([(1, 2), (2, 2)])           # sector1 seq 2 -> ota_1 VALID (bench state)
        off, sector, meta = ota_select.select(cur, 0)
        self.assertEqual(off, 0xF000)               # writes the sector NOT holding the newest entry
        new = sector + cur[0x1000:]
        info = ota_select.parse(new)
        self.assertTrue(info[0]["valid"])
        self.assertEqual((info[0]["seq"], info[0]["state"], info[0]["slot"]), (3, 0, 0))
        tool = _otatool()
        if tool is not None:
            fake = types.SimpleNamespace(otadata=new, spi_flash_sec_size=0x1000)
            got = tool.OtatoolTarget._get_otadata_info(fake)
            self.assertEqual(got[0].seq, 3)
            self.assertEqual(got[0].crc, binascii.crc32(struct.pack("<I", 3), 0xFFFFFFFF))

    def test_select_ota1_back(self):
        cur = _otadata([(3, 2), (2, 2)])
        off, sector, meta = ota_select.select(cur, 1)
        self.assertEqual(off, 0x10000)
        self.assertEqual(meta["seq"], 4)
        self.assertEqual(ota_select.parse(cur[:0x1000] + sector)[1]["slot"], 1)

    def test_blank(self):
        off, sector, meta = ota_select.select(_otadata([None, None]), 0)
        self.assertEqual((off, meta["seq"]), (0xF000, 1))

    def test_already_selected(self):
        with self.assertRaises(ValueError):
            ota_select.select(_otadata([(1, 2), (2, 2)]), 1)


class Io1(unittest.TestCase):
    """IO-1: relevant-op classification, lossless drains with contiguous seqs,
    exact lost accounting on overflow, and one-shot fault arming."""

    def test_trace_and_arming(self):
        with tempfile.TemporaryDirectory() as d:
            exe = _build(["components/event_log/evq_hil_trace.c", "tests/evq_hil_host/trace_main.c"],
                         ["components/event_log"], ["-DEVQ_HIL_TRACE_HOST"], Path(d) / "trace")
            out = subprocess.run([str(exe)], capture_output=True, text=True, check=True).stdout
            rows = [json.loads(x) for x in out.splitlines()]
            self.assertEqual(rows[0]["cls"], [1, 1, 1, 0, 1, 1, 0])
            d1, d2, d3, d4 = rows[1:5]
            self.assertEqual((d1["n"], d1["lost"], d1["first"], d1["last"]), (100, 0, 1, 100))
            cap = 8192
            self.assertEqual(d2["lost"], 50)
            self.assertEqual((d2["first"], d2["last"], d2["n"]), (151, 100 + cap + 50, cap))
            self.assertEqual((d3["first"], d3["n"], d3["lost"]), (d2["last"] + 1, 10, 0))
            self.assertEqual((d4["n"], d4["lost"]), (0, 0))
            self.assertEqual(d3["gaps"], 1)          # the only discontinuity is the reported overflow
            # [a1, io1, a2, io2, io3, a3, r1, r2, reset_inside]
            self.assertEqual(rows[5]["arm"], [0, 0, 0, 5, 0, 0, 0, 1, -1])


class Iso1(unittest.TestCase):
    """ISO-1: no verification control in the release image; present in evq-hil."""

    PAT_SYM = ("evq_hil", "evq_fault_point", "hil_")
    PAT_STR = (b"EVQ_ACC", b"HIL_TRACE", b"evq_hil")

    def _nm(self) -> str | None:
        for p in Path("/home/dv/.platformio/packages").glob("toolchain-xtensa-esp*/bin/*-nm"):
            return str(p)
        return shutil.which("xtensa-esp32s3-elf-nm")

    def _check(self, env: str) -> tuple[int, int]:
        elf = ROOT / ".pio" / "build" / env / "firmware.elf"
        if not elf.exists():
            self.skipTest(f"{elf} not built (run pio run -e {env})")
        nm = self._nm()
        if nm is None:
            self.skipTest("xtensa nm not found")
        syms = subprocess.run([nm, str(elf)], capture_output=True, text=True, check=True).stdout
        nsym = sum(1 for ln in syms.splitlines() if any(p in ln for p in self.PAT_SYM))
        data = elf.read_bytes()
        nstr = sum(data.count(p) for p in self.PAT_STR)
        return nsym, nstr

    def test_release_clean(self):
        self.assertEqual(self._check("esp32-s3-devkitm-1"), (0, 0))

    def test_hil_present(self):
        nsym, nstr = self._check("evq-hil")
        self.assertGreater(nsym, 0)
        self.assertGreater(nstr, 0)


if __name__ == "__main__":
    unittest.main()
