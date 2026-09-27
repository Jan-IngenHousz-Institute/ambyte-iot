"""tools/evq_hil/rel_check.py (REL-ID, REL-EQ, REL-CUR) and tools/evq_hil/stage_check.py
(AO-1..5 / AF-1..3) on synthetic inputs (Sprint 2 contract r4 §6.2, §6.6), plus the
reconcile_warehouse importability the contract requires (H6)."""
from __future__ import annotations

import datetime as dt
import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "evq_hil"))
sys.path.insert(0, str(ROOT / "tests"))
import rel_check as R  # noqa: E402
import stage_check as SC  # noqa: E402
from test_diag_check import HAVE_NVS_TOOL, nvs_region  # noqa: E402

REL = ROOT / "tools" / "evq_hil" / "rel_check.py"
STAGE = ROOT / "tools" / "evq_hil" / "stage_check.py"
T0 = 1790000000000


def line(i: int, payload: dict, start: int) -> bytes:
    return (f"{i}\t\tbme280\tMEASUREMENT\trecord_env\t{start}\t{start}\t\t"
            + json.dumps(payload, separators=(",", ":")) + "\n").encode()


def row(i: int, data: dict, start: int) -> dict:
    iso = dt.datetime.fromtimestamp(start / 1000, dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    pd = {"device_id": "28:37:2F:FF:E7:04", "timestamp": iso,
          "sample": json.dumps([{"v": 2, "measure_id": i, "data": data}])}
    return {"experiment_id": "x", "workbook_version_id": None, "ingestion_timestamp": iso,
            "kinesis_arrival_time": iso, "pd": json.dumps(pd)}


class Fixture:
    def __init__(self, stored: range, delivered: range):
        self.lines, self.raw, self.rows = [], {}, []
        for n, i in enumerate(stored):
            b = line(i, {"t": i}, T0 + i * 1000)
            self.raw[i] = b
            self.lines.append({"seq": 4 + n // 5, "line": n % 5 + 1, "off": (n % 5) * 100, "id": i,
                               "sha256": hashlib.sha256(b).hexdigest(), "bytes": len(b)})
        for i in delivered:
            self.rows.append(row(i, {"t": i}, T0 + i * 1000))

    def write(self, d: str) -> tuple[Path, Path]:
        m = Path(d) / "m.json"
        m.write_text(json.dumps({"storage": {"files": [], "lines": self.lines}}))
        r = Path(d) / "rows.json"
        r.write_text(json.dumps(self.rows))
        return m, r


class RelId(unittest.TestCase):
    def test_pass_gap_and_cli(self):
        fx = Fixture(range(100, 105), range(103, 110))
        rows = R.rw.parse_rows(fx.rows)
        self.assertEqual(R.rel_id(100, 110, fx.lines, rows)["status"], R.PASS)
        r = R.rel_id(100, 111, fx.lines, rows)
        self.assertEqual((r["status"], r["missing"]), (R.FAIL, [110]))
        with tempfile.TemporaryDirectory() as d:
            m, rw_ = fx.write(d)
            p = subprocess.run([sys.executable, REL, "rel-id", "--first", "100", "--next-id", "110", "--manifest",
                                str(m), "--rows", str(rw_)], capture_output=True, text=True)
            self.assertEqual(p.returncode, 0, p.stdout)
            p = subprocess.run([sys.executable, REL, "rel-id", "--first", "99", "--next-id", "110", "--manifest",
                                str(m), "--rows", str(rw_)], capture_output=True, text=True)
            self.assertEqual(p.returncode, 1)


class RelEq(unittest.TestCase):
    def test_equal_missing_different_and_unbound(self):
        fx = Fixture(range(100, 105), range(100, 105))
        rows = R.rw.parse_rows(fx.rows)
        self.assertEqual(R.rel_eq(fx.lines, fx.raw, rows)["status"], R.PASS)
        short = R.rw.parse_rows(fx.rows[:-1])
        self.assertEqual(R.rel_eq(fx.lines, fx.raw, short)["missing"], [104])
        other = {**fx.raw, 102: line(102, {"t": 999}, T0 + 102 * 1000)}
        r = R.rel_eq(fx.lines, other, rows)
        self.assertEqual(r["status"], R.FAIL)          # line does not hash to the manifest
        bad_rows = R.rw.parse_rows(fx.rows[:2] + [row(102, {"t": -1}, T0 + 102 * 1000)] + fx.rows[3:])
        r = R.rel_eq(fx.lines, fx.raw, bad_rows)
        self.assertEqual((r["status"], list(r["failed"])), (R.FAIL, [102]))
        late = R.rw.parse_rows(fx.rows[:4] + [row(104, {"t": 104}, T0 + 104 * 1000 + 5000)])
        self.assertEqual(R.rel_eq(fx.lines, fx.raw, late)["status"], R.FAIL)       # timestamp mismatch


@unittest.skipUnless(HAVE_NVS_TOOL, "ESP-IDF nvs_partition_tool not installed")
class RelCur(unittest.TestCase):
    def nvs(self, d: str, seq: int, off: int, legacy: tuple[int, int] | None = None) -> Path:
        import struct
        items = [("evlog", "cur", struct.pack("<III", seq, off, 0xABCD))]
        if legacy:
            items += [("evlog", "rd_seq", legacy[0]), ("evlog", "rd_off", legacy[1])]
        p = Path(d) / "nvs.bin"
        p.write_bytes(nvs_region(items))
        return p

    def test_cursor_never_beyond_undelivered(self):
        fx = Fixture(range(100, 110), range(100, 107))    # lines: seq 4 off 0..400, seq 5 off 0..400
        rows = R.rw.parse_rows(fx.rows)
        with tempfile.TemporaryDirectory() as d:
            cur = R.nvs_cursor(str(self.nvs(d, 5, 200, (5, 200))))
            self.assertEqual(cur["cur"]["seq"], 5)
            self.assertEqual((cur["rd_seq"], cur["rd_off"]), (5, 200))
            self.assertEqual(R.rel_cur(cur, fx.lines, rows)["status"], R.PASS)     # 107.. sit at/after 5:200
            ahead = R.nvs_cursor(str(self.nvs(d, 5, 300)))
            self.assertEqual(R.rel_cur(ahead, fx.lines, rows)["status"], R.FAIL)   # passes undelivered 107
            m, rw_ = fx.write(d)
            p = subprocess.run([sys.executable, REL, "rel-cur", "--nvs", str(self.nvs(d, 5, 200, (5, 300))),
                                "--manifest", str(m), "--rows", str(rw_)], capture_output=True, text=True)
            self.assertEqual(p.returncode, 1, p.stdout)                         # legacy key checked too


class WarehouseImportable(unittest.TestCase):
    def test_fetch_ids_and_rebuild_line_importable_before_main(self):
        src = (ROOT / "tools" / "evq_hil" / "reconcile_warehouse.py").read_text()
        guard = src.index('if __name__ == "__main__":')
        self.assertLess(src.index("def fetch_ids("), guard)
        self.assertLess(src.index("def rebuild_line("), guard)
        self.assertNotIn("--pre", src)
        self.assertTrue(callable(R.rw.fetch_ids) and callable(R.rw.rebuild_line))


# --------------------------------------------------------------------------- stage_check

VERS = ["AMBIT firmware versions:", "  AMBIT1: v1.4.0  (built Sep  6 2026)", "  AMBIT2: absent",
        "  AMBIT3: v1.4.0  (built Sep  6 2026)", "  AMBIT4: absent", "ambyte> "]


def sd_inv(files: dict[str, str], dirs: tuple[str, ...] = ()) -> str:
    out = ["EVQ_SDSNAP / cid=a4 total=1 free=1 us=1"]
    out += [f"EVQ_DIR {d}" for d in dirs]
    out += [f"EVQ_FF {p} 10 {s} 00000000" for p, s in files.items()]
    out.append(f"EVQ_SDEND {len(files)} 0")
    return "\r\n".join(out) + "\r\n"


BASE = {"/sdcard/ambit_fw.bin": "a" * 64, "/sdcard/ambit_fw/1.1.5/app.bin": "b" * 64}


class Stage(unittest.TestCase):
    def run_row(self, cmd: str, row: str, body: list[str], after: dict | None = None,
                dirs_after: tuple[str, ...] = (), vers_after: list[str] | None = None) -> int:
        with tempfile.TemporaryDirectory() as d:
            cap = Path(d) / "c.log"
            cap.write_text("\r\n".join(VERS + ["I (1) wifi: noise"] + body + (vers_after or VERS)) + "\r\n")
            b = Path(d) / "b.log"
            b.write_text(sd_inv(BASE))
            a = Path(d) / "a.log"
            a.write_text(sd_inv(after if after is not None else BASE, dirs_after))
            p = subprocess.run([sys.executable, STAGE, cmd, "--row", row, "--capture", str(cap), "--sd-before",
                                str(b), "--sd-after", str(a)], capture_output=True, text=True)
            return p.returncode

    FIRED = ("HIL_FAULT fired kind=short_write mode=short writer=ambit_ota op=write "
             "path=/sdcard/ambit_fw.stg-0.bin errno=0 nth=1 slot=A")

    def test_ao1_pass_and_failures(self):
        body = [self.FIRED, "E (22) ambit_ota: download failed (ESP_FAIL)"]
        kept = dict(BASE, **{"/sdcard/ambit_fw.stg-0.bin": "c" * 64})
        self.assertEqual(self.run_row("ao", "AO-1", body, after=kept), 0)
        self.assertEqual(self.run_row("ao", "AO-1", body + ["W (30) ambit_ota: AMBIT2 fw before: no answer"]), 1)
        self.assertEqual(self.run_row("ao", "AO-1", [self.FIRED.replace("ambit_fw.stg-0", "ambit_fw")] + body[1:]), 1)
        self.assertEqual(self.run_row("ao", "AO-1", body[:1]), 1)                      # no 'download failed'
        self.assertEqual(self.run_row("ao", "AO-1", body, after=dict(BASE, **{"/sdcard/ambit_fw.bin": "d" * 64})), 1)
        changed = [x.replace("AMBIT3: v1.4.0", "AMBIT3: v1.1.5") for x in VERS]
        self.assertEqual(self.run_row("ao", "AO-1", body, vers_after=changed), 1)
        self.assertEqual(self.run_row("ao", "AO-1", body + ["W (40) ambit_ota: OTA_END ok — AMBIT1 rebooting"]), 3)
        self.assertEqual(self.run_row("ao", "AO-1", body, vers_after=["no versions here"]), 2)

    def test_ao5_and_ao4(self):
        body = ["I (5) ambit_ota: download ok", "E (6) ambit_ota: OTA_BEGIN failed (ESP_ERR_TIMEOUT, status=0)"]
        self.assertEqual(self.run_row("ao", "AO-5", body), 0)
        leftover = dict(BASE, **{"/sdcard/ambit_fw.stg-1.bin": "e" * 64})
        self.assertEqual(self.run_row("ao", "AO-5", body, after=leftover), 1)
        reset = ["HIL_FAULT fired kind=cpu_reset mode=reset_mid_write writer=ambit_ota op=write "
                 "path=/sdcard/ambit_fw.stg-0.bin errno=0 nth=1 slot=A sd_power=not_interrupted", "ESP-ROM:esp32s3"]
        self.assertEqual(self.run_row("ao", "AO-4", reset), 0)

    def test_af_rows(self):
        f1 = ["HIL_FAULT fired kind=injected_errno mode=eio writer=ambit_flash op=mkdir path=/sdcard/hils2afw "
              "errno=5 nth=1 slot=A", "E (9) ambit_flash: cannot create /sdcard/hils2afw"]
        self.assertEqual(self.run_row("af", "AF-1", f1), 0)
        self.assertEqual(self.run_row("af", "AF-1", f1, dirs_after=("/sdcard/hils2afw",)), 1)
        self.assertEqual(self.run_row("af", "AF-1", f1 + ["W (10) ambit_flash: AMBIT2: ROM OK chip=5"]), 1)
        f2 = ["E (9) ambit_flash: region images missing: ESP_ERR_NOT_FOUND"]
        self.assertEqual(self.run_row("af", "AF-2", f2, dirs_after=("/sdcard/hils2afw",)), 0)
        f3 = ["HIL_FAULT fired kind=injected_errno mode=eio writer=ambit_flash op=open "
              "path=/sdcard/hils2afw/bootloader.bin errno=5 nth=1 slot=A", "E (9) ambit_flash: open failed"]
        self.assertEqual(self.run_row("af", "AF-3", f3, dirs_after=("/sdcard/hils2afw",)), 0)

    def test_version_block_parser(self):
        self.assertEqual(SC.version_blocks(VERS)[0], {"AMBIT1": "1.4.0", "AMBIT2": "absent", "AMBIT3": "1.4.0",
                                                      "AMBIT4": "absent"})


if __name__ == "__main__":
    unittest.main()
