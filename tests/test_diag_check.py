"""tools/evq_hil/diag_check.py (Sprint 2 contract r4 H6, §6.5 DG-1..5).

The decoder's layout is cross-checked against the real C header by compiling
sd_diag.h with the host compiler (x86-64 and the ESP32-S3's xtensa ABI align
uint16/uint32/int64 identically: natural alignment, int64 on 8), and the
decoder's reading of a block is cross-checked against sd_diag_core.c itself
(a block produced by the production core decodes to exactly what the core's own
JSON renderer says). NVS fixtures are built page by page in the ESP-IDF v2
format and read back through ESP-IDF's nvs_partition_tool, as on real dumps.
"""
from __future__ import annotations

import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "evq_hil"))
import diag_check as D  # noqa: E402

TOOL = ROOT / "tools" / "evq_hil" / "diag_check.py"
CC = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc") or "cc"
HAVE_NVS_TOOL = (D.IDF_NVS_TOOL / "nvs_parser.py").is_file()


# --------------------------------------------------------------------------- NVS v2 page builder

def _crc(b: bytes) -> int:
    return zlib.crc32(b, 0xFFFFFFFF)


def _entry(ns: int, typ: int, span: int, chunk: int, key: str, data: bytes) -> bytes:
    k = key.encode().ljust(16, b"\0")
    body = bytes([ns, typ, span, chunk]) + b"\0\0\0\0" + k + data
    crc = _crc(body[:4] + body[8:])
    return body[:4] + struct.pack("<I", crc) + body[8:]


def nvs_region(items: list[tuple[str, str, bytes | int]], size: int = 0x6000) -> bytes:
    """items: (namespace, key, value) with value an int (u32) or bytes (blob, NVS v2
    blob_data chunk 0 + blob_index). One active page, the rest erased (0xFF)."""
    ns_ids: dict[str, int] = {}
    ents: list[bytes] = []
    for ns, _k, _v in items:
        if ns not in ns_ids:
            ns_ids[ns] = len(ns_ids) + 1
            ents.append(_entry(0, 0x01, 1, 0xFF, ns, bytes([ns_ids[ns]]) + b"\xff" * 7))
    for ns, key, val in items:
        i = ns_ids[ns]
        if isinstance(val, int):
            ents.append(_entry(i, 0x04, 1, 0xFF, key, struct.pack("<I", val) + b"\xff" * 4))
            continue
        n = len(val)
        span = 1 + (n + 31) // 32
        head = _entry(i, 0x42, span, 0, key, struct.pack("<HHI", n, 0xFFFF, _crc(val)))
        ents.append(head + val.ljust((span - 1) * 32, b"\xff"))
        ents.append(_entry(i, 0x48, 1, 0xFF, key, struct.pack("<IBBH", n, 1, 0, 0xFFFF)))
    body = b"".join(ents)
    nent = len(body) // 32
    bitmap = bytearray(b"\xff" * 32)
    for e in range(nent):
        bitmap[e // 4] &= ~(0b01 << ((e % 4) * 2)) & 0xFF      # 0b11 -> 0b10 (Written)
    hdr = struct.pack("<II", 0xFFFFFFFE, 0) + bytes([0xFE]) + b"\xff" * 19
    hdr += struct.pack("<I", _crc(hdr[4:28]))
    page = (hdr + bytes(bitmap) + body).ljust(4096, b"\xff")
    return page + b"\xff" * (size - 4096)


def flash_image_with_nvs(region: bytes) -> bytes:
    """Partition table at 0x8000 with an `nvs` entry at 0x9000 (as flashimg.parse_pt reads)."""
    img = bytearray(b"\xff" * (0x9000 + len(region)))
    ent = b"\xaa\x50" + struct.pack("<BBII", 1, 2, 0x9000, len(region)) + b"nvs".ljust(16, b"\0") + b"\0" * 4
    img[0x8000:0x8000 + 32] = ent
    img[0x9000:] = region
    return bytes(img)


class Layout(unittest.TestCase):
    def test_offsets_match_the_c_header(self):
        fields = ["magic", "version", "size", "epoch", "boot_seq", "gen", "exact", "floor_pending", "cnt",
                  "last.writer", "last.op", "last.err", "last.uptime_ms", "last.boot_seq", "refused",
                  "ref_first_id", "ref_last_id", "ref_last.reason", "ref_last.blocked", "ref_last.sd_state",
                  "ref_last.err", "ref_last.uptime_ms", "ref_last.wall_ms", "crc"]
        prog = "#include <stdio.h>\n#include <stddef.h>\n#include \"sd_diag.h\"\nint main(void){\n"
        prog += '  printf("sizeof %zu\\n", sizeof(sd_diag_block_t));\n'
        for f in fields:
            prog += f'  printf("{f} %zu\\n", offsetof(sd_diag_block_t, {f}));\n'
        prog += "  return 0;\n}\n"
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "lay.c"
            src.write_text(prog)
            exe = Path(d) / "lay"
            r = subprocess.run([CC, "-std=gnu11", f"-I{ROOT / 'components/sd_card'}", str(src), "-o", str(exe)],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            got = dict(ln.split() for ln in subprocess.run([str(exe)], capture_output=True, text=True,
                                                           check=True).stdout.splitlines())
        self.assertEqual(int(got["sizeof"]), D.SIZE)
        want = {name: off for name, off, _c in D.LAYOUT}
        for f in fields:
            self.assertEqual(int(got[f]), want[f], f)
        self.assertLessEqual(D.SIZE, 256)             # the RTC budget the contract names
        self.assertEqual(D.CRC_OFF, want["crc"])

    def test_crc_is_the_c_loop_and_zlib(self):
        b = D.encode_block(gen=3, epoch=2, boot=9)
        self.assertEqual(D.sd_diag_crc(b), zlib.crc32(b[:D.CRC_OFF]))

    def test_block_from_the_production_core_decodes_to_its_own_json(self):
        prog = r'''
#include <stdio.h>
#include "sd_diag.h"
int main(void) {
    sd_diag_block_t b, floor;
    sd_diag_core_boot(&floor, false); sd_diag_core_merge_floor(&floor, NULL);
    sd_diag_core_fault(&floor, SD_DIAG_W_EVLOG, SD_DIAG_OP_RENAME, 5, 777);
    sd_diag_core_boot(&b, false);
    sd_diag_core_fault(&b, SD_DIAG_W_SDLOG, SD_DIAG_OP_FSYNC, 5, 1234);
    sd_diag_core_merge_floor(&b, &floor);
    sd_diag_core_fault(&b, SD_DIAG_W_SDLOG, SD_DIAG_OP_FSYNC, 5, 2345);
    sd_diag_core_fault(&b, SD_DIAG_W_AMBIT_OTA, SD_DIAG_OP_WRITE, 28, 3456);
    sd_diag_core_refusal(&b, SD_DIAG_REF_FULL, 145751, 12, 3, 2, 1790000000123LL, 4567);
    const unsigned char *p = (const unsigned char *)&b;
    for (size_t i = 0; i < sizeof b; i++) printf("%02x", p[i]);
    printf("\n");
    char js[2048];
    sd_diag_render_json(&b, js, sizeof js);
    printf("%s\n%u\n", js, (unsigned)b.gen);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "core.c"
            src.write_text(prog)
            exe = Path(d) / "core"
            r = subprocess.run([CC, "-std=gnu11", f"-I{ROOT / 'components/sd_card'}", str(src),
                                str(ROOT / "components/sd_card/sd_diag_core.c"), "-o", str(exe)],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            hexline, js, gen = subprocess.run([str(exe)], capture_output=True, text=True,
                                              check=True).stdout.splitlines()
        dec = D.decode_block(bytes.fromhex(hexline))
        self.assertTrue(dec["valid"], dec["reasons"])
        live = json.loads(js)
        self.assertEqual(D.compare(live, dec["decoded"], int(gen))["status"], D.PASS)
        self.assertEqual(dec["decoded"]["faults"], {"evlog.rename": 1, "sdlog.fsync": 2, "ambit_ota.write": 1})
        self.assertEqual(dec["decoded"]["epoch"], 2)
        self.assertFalse(dec["decoded"]["exact"])


@unittest.skipUnless(HAVE_NVS_TOOL, "ESP-IDF nvs_partition_tool not installed")
class NvsFixtures(unittest.TestCase):
    def cli(self, path: Path, *extra: str) -> tuple[int, dict]:
        r = subprocess.run([sys.executable, TOOL, "nvs", str(path), *extra], capture_output=True, text=True)
        return r.returncode, json.loads(r.stdout)

    def write(self, d: str, name: str, data: bytes) -> Path:
        p = Path(d) / name
        p.write_bytes(data)
        return p

    def test_fixture_matrix(self):
        valid = D.encode_block(gen=11, epoch=3, boot=4, cnt=[0] * 15 + [2] + [0] * 32, last=(1, 3, 5, 999, 4))
        cases = {
            "missing_ns": ([("evlog", "rd_seq", 5)], 0, False),
            "missing_key": ([("sd_diag", "other", b"x" * 8)], 0, False),
            "wrong_length": ([("sd_diag", "snap", valid[:-1])], 1, True),
            "bad_magic": ([("sd_diag", "snap", D.encode_block(magic=0x12345678))], 1, True),
            "bad_version": ([("sd_diag", "snap", D.encode_block(version=2))], 1, True),
            "bad_size_field": ([("sd_diag", "snap", D.encode_block(size=256))], 1, True),
            "bad_crc": ([("sd_diag", "snap", valid[:-8] + bytes([valid[-8] ^ 0xFF]) + valid[-7:])], 1, True),
            "valid": ([("evlog", "rd_seq", 5), ("sd_diag", "snap", valid)], 0, True),
        }
        with tempfile.TemporaryDirectory() as d:
            for name, (items, rc, present) in cases.items():
                p = self.write(d, f"{name}.bin", nvs_region(items))
                got_rc, out = self.cli(p)
                self.assertEqual((got_rc, out["present"]), (rc, present), (name, out))
            rc, out = self.cli(Path(d) / "valid.bin")
            self.assertEqual(out["decoded"]["gen"], 11)
            self.assertEqual(out["decoded"]["faults"], {"sdlog.fsync": 2})
            self.assertEqual(out["decoded"]["last"], {"w": "sdlog", "op": "fsync", "errno": 5, "uptime_ms": 999,
                                                      "boot": 4})
            self.assertEqual(self.cli(Path(d) / "missing_ns.bin", "--expect", "present")[0], 1)
            self.assertEqual(self.cli(Path(d) / "valid.bin", "--expect", "absent")[0], 1)
            img = self.write(d, "img.bin", flash_image_with_nvs(nvs_region(cases["valid"][0])))
            rc, out = self.cli(img)
            self.assertEqual((rc, out["decoded"]["gen"]), (0, 11))

    def test_compare_dg4(self):
        blk = D.encode_block(gen=21, epoch=1, boot=2, cnt=[0] * 13 + [1] + [0] * 34, last=(1, 1, 5, 50, 2))
        live = {"exact": False, "epoch": 1, "boot": 2, "faults": {"sdlog.write": 1},
                "last": {"w": "sdlog", "op": "write", "errno": 5, "uptime_ms": 50, "boot": 2},
                "refused": {"full": 0, "media": 0, "too_large": 0, "unavailable": 0, "first_id": 0, "last_id": 0}}
        with tempfile.TemporaryDirectory() as d:
            nvs = self.write(d, "n.bin", nvs_region([("sd_diag", "snap", blk)]))
            cap = self.write(d, "evlog.log", ("evlog: available=1\r\n  sd_diag=" + json.dumps(live) + "\r\n").encode())
            run = lambda *x: subprocess.run([sys.executable, TOOL, "compare", "--live", str(cap), "--nvs", str(nvs),
                                             *x], capture_output=True, text=True)  # noqa: E731
            self.assertEqual(run("--live-gen", "21").returncode, 0)
            self.assertEqual(run().returncode, 2)                       # gen unavailable -> incomplete
            self.assertEqual(run("--live-gen", "20").returncode, 1)
            live["faults"] = {"sdlog.write": 2}
            cap.write_bytes(("  sd_diag=" + json.dumps(live) + "\n").encode())
            self.assertEqual(run("--live-gen", "21").returncode, 1)


class Dg(unittest.TestCase):
    def fired(self, writer, op, kind="injected_errno", errno=5):
        return {"kind": kind, "writer": writer, "op": op, "errno": str(errno)}

    def test_dg2_exact_accounting(self):
        before = {"faults": {"evlog.rename": 1}, "refused": {"full": 0}}
        after = {"faults": {"evlog.rename": 1, "sdlog.write": 1}, "last": {"w": "sdlog", "op": "write", "errno": 5},
                 "refused": {"full": 0}}
        f = [self.fired("sdlog", "write", "short_write", 0)]
        self.assertEqual(D.dg2(f, before, after, {})["status"], D.PASS)
        self.assertEqual(D.dg2(f + f, before, after, {})["status"], D.FAIL)              # count mismatch
        organic = dict(after, faults={"evlog.rename": 2, "sdlog.write": 1})
        self.assertEqual(D.dg2(f, before, organic, {})["status"], D.FAIL)                # unexplained organic
        self.assertEqual(D.dg2(f, before, organic, {"evlog.rename": 1})["status"], D.PASS)
        wrong_last = dict(after, last={"w": "sdlog", "op": "write", "errno": 28})
        self.assertEqual(D.dg2(f, before, wrong_last, {})["status"], D.FAIL)
        reset = [self.fired("sdlog", "write", "cpu_reset", 0)]
        r = D.dg2(reset, before, before, {})
        self.assertEqual((r["status"], len(r["reset_firings"])), (D.PASS, 1))
        refused = dict(after, refused={"full": 1})
        self.assertEqual(D.dg2(f, before, refused, {})["status"], D.FAIL)

    def test_dg2_cli_reads_fired_lines(self):
        with tempfile.TemporaryDirectory() as d:
            cap = Path(d) / "c.log"
            cap.write_bytes(b"noise\r\n\r\nHIL_FAULT fired kind=injected_errno mode=eio writer=sdlog op=fsync "
                            b"path=/sdcard/logs/ambyte.log errno=5 nth=1 slot=A\r\n")
            b = Path(d) / "b.json"
            a = Path(d) / "a.json"
            b.write_text(json.dumps({"faults": {}}))
            a.write_text(json.dumps({"faults": {"sdlog.fsync": 1}, "last": {"w": "sdlog", "op": "fsync", "errno": 5}}))
            r = subprocess.run([sys.executable, TOOL, "dg2", "--capture", str(cap), "--before", str(b), "--after",
                                str(a)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout)

    def test_dg1_dg3_dg5(self):
        self.assertEqual(D.dg1({"exact": False, "epoch": 1}, None, True)["status"], D.PASS)
        self.assertEqual(D.dg1({"exact": True, "epoch": 1}, None, True)["status"], D.FAIL)
        self.assertEqual(D.dg1({"exact": False, "epoch": 2}, None, True)["status"], D.FAIL)
        floor = {"epoch": 3, "faults": {"sdlog.write": 2}}
        self.assertEqual(D.dg1({"exact": False, "epoch": 4, "faults": {"sdlog.write": 2}}, floor, False)["status"],
                         D.PASS)
        self.assertEqual(D.dg1({"exact": False, "epoch": 4, "faults": {"sdlog.write": 1}}, floor, False)["status"],
                         D.FAIL)
        b = {"exact": False, "epoch": 2, "boot": 5, "faults": {"sdlog.write": 1}}
        r = D.dg3(b, dict(b, boot=6), 3)
        self.assertEqual((r["status"], r["path"]), (D.PASS, "rtc"))
        self.assertEqual(D.dg3(b, dict(b, boot=6, faults={}), 3)["status"], D.FAIL)
        self.assertEqual(D.dg3(b, dict(b, boot=6, exact=True), 3)["status"], D.FAIL)
        self.assertEqual(D.dg3(b, dict(b, epoch=3, boot=1), 1)["path"], "floor_merge")
        before = {"refused": {"full": 0, "first_id": 0, "last_id": 0}}
        after = {"refused": {"full": 3, "first_id": 100, "last_id": 102}}
        self.assertEqual(D.dg5(before, after, 3, 100, 102)["status"], D.PASS)
        self.assertEqual(D.dg5(before, after, 2, 100, 102)["status"], D.FAIL)
        self.assertEqual(D.dg5(before, after, 3, 101, 102)["status"], D.FAIL)


if __name__ == "__main__":
    unittest.main()
