"""ROMSEQ-1: tools/evq_hil/romseq.py enforces the frozen replacement-amendment
r3.3 §3 sequence against an emulated flash: six replacements in order and slot,
five selections, <= 2 attempts each, confirmation/AMBIT gates, and exit rule (d)
(planned = NEW candidate byte-equal; fallback = previously confirmed VALID
byte-equal; anything else is a refused exit = ROM hold)."""
from __future__ import annotations

import binascii
import hashlib
import json
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "evq_hil"))
os.environ.setdefault("AMBYTE_BENCH_MAC", "E8:F6:0A:B1:1F:34")
os.environ.setdefault("AMBYTE_BENCH_EXPERIMENT", "x")
os.environ.setdefault("AMBYTE_BENCH_DEVICE_ID", "03:25:07:04")
os.environ.setdefault("AMBYTE_BENCH_DEVICE_NAME", "n")
import ota_select  # noqa: E402
import romseq  # noqa: E402

MAC = "e8:f6:0a:b1:1f:34"
OTA0, OTA1, SLOT = 0x20000, 0x320000, 0x300000


def _pt() -> bytes:
    ents = [("nvs", 1, 2, 0x9000, 0x6000), ("otadata", 1, 0, 0xF000, 0x2000),
            ("ota_0", 0, 0x10, OTA0, SLOT), ("ota_1", 0, 0x11, OTA1, SLOT)]
    out = b""
    for lab, t, st, off, size in ents:
        out += b"\xAA\x50" + struct.pack("<BBII", t, st, off, size) + lab.encode().ljust(16, b"\0") + b"\0" * 4
    return out.ljust(0xC00, b"\xff")


def _entry(seq: int, state: int) -> bytes:
    e = struct.pack("<I", seq) + b"\xff" * 20 + struct.pack("<I", state) + \
        struct.pack("<I", binascii.crc32(struct.pack("<I", seq), 0xFFFFFFFF))
    return e + b"\xff" * (0x1000 - 32)


class FakeChip:
    """Flash + ROM/app state; mimics the esptool subcommands romseq uses and the
    bootloader/app behaviour needed between sessions."""

    def __init__(self):
        self.f = bytearray(b"\xff" * 0x700000)
        self.f[0x8000:0x8C00] = _pt()
        self.f[0xF000:0x11000] = _entry(1, 2) + b"\xff" * 0x1000     # PRE: ota_0 VALID
        self.in_rom = False
        self.fail_next_write = 0

    def esptool(self, args, log=None):
        a = list(args)
        before, after, cmd = a[1], a[3], a[4]
        if before == "default_reset":
            self.in_rom = True
        assert self.in_rom, f"{cmd} outside ROM"
        out = f"MAC: {MAC}\n"
        if cmd == "read_flash":
            off, n, path = int(a[5], 16), int(a[6], 16), a[7]
            Path(path).write_bytes(bytes(self.f[off:off + n]))
        elif cmd == "write_flash":
            off, path = int(a[-2], 16), a[-1]
            data = Path(path).read_bytes()
            if self.fail_next_write:
                self.fail_next_write -= 1
                self.f[off:off + len(data) // 2] = data[:len(data) // 2]
                raise SystemExit("esptool failed (2): write_flash (injected)")
            self.f[off:off + len(data)] = data
        elif cmd == "verify_flash":
            off, path = int(a[5], 16), a[6]
            data = Path(path).read_bytes()
            if bytes(self.f[off:off + len(data)]) != data:
                raise SystemExit("esptool failed (2): verify_flash")
        if after == "hard_reset":
            self.in_rom = False
            self.boot()
        return out

    def boot(self, confirm: bool = True):
        """Bootloader pre-pass + NEW->PENDING; the app confirms (VALID) or fails
        (forced restart -> PENDING aborted -> the other entry)."""
        od = bytearray(self.f[0xF000:0x11000])
        info = ota_select.parse(bytes(od))
        for e in info:
            if e["valid"] and e["state"] == 1:
                struct.pack_into("<I", od, e["sector"] * 0x1000 + 24, 4)
        act = ota_select.active(ota_select.parse(bytes(od)))
        if act is not None and act["state"] == 0:
            struct.pack_into("<I", od, act["sector"] * 0x1000 + 24, 2 if confirm else 1)
        self.f[0xF000:0x11000] = od
        return act


class Romseq1(unittest.TestCase):
    def setUp(self):
        self.d = Path(tempfile.mkdtemp(prefix="romseq-"))
        self.imgs = {}
        for name, fill in (("R", b"R"), ("HIL", b"H"), ("REL", b"L")):
            p = self.d / f"{name}.bin"
            p.write_bytes(b"\xe9" + fill * 4095)
            self.imgs[name] = {"path": str(p), "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
        (self.d / "plan.json").write_text(json.dumps({"mac": MAC.upper(), "images": self.imgs}))
        self.chip = FakeChip()

    def seq(self) -> romseq.Seq:
        s = romseq.Seq(self.d / "plan.json", self.d / "ledger.json", self.d / "log")
        s.esptool = self.chip.esptool
        return s

    def slot_bytes(self, off, name):
        n = os.path.getsize(self.imgs[name]["path"])
        return bytes(self.chip.f[off:off + n]) == Path(self.imgs[name]["path"]).read_bytes()

    def test_full_frozen_sequence(self):
        s = self.seq()
        with self.assertRaises(romseq.Refused):
            s.write(1)                                   # not in ROM
        s.enter()
        with self.assertRaises(romseq.Refused):
            s.write(2)                                   # order
        self.assertTrue(s.write(1)["ok"])
        with self.assertRaises(romseq.Refused):
            s.select("S1")                               # needs write 2
        self.assertTrue(s.write(2)["ok"])
        self.assertTrue(s.select("S1")["ok"])
        self.assertTrue(s.exit("planned", "S1")["ok"])    # step 5: R boots from ota_1, confirms
        s.confirmed("R", 1, "w7-r.log")
        s.enter()                                        # step 6
        self.assertTrue(s.write(3)["ok"])
        self.assertTrue(s.select("S2")["ok"])
        self.assertTrue(s.exit("planned", "S2")["ok"])
        s.confirmed("HIL", 0, "w7-hil.log")
        s.enter()                                        # step 8 needs the AMBIT gate
        with self.assertRaises(romseq.Refused):
            s.write(4)
        s.led["in_rom"] = False
        s.ambit_gate("a6-all-1.3.0.log")
        s.enter()
        self.assertTrue(s.write(4)["ok"])
        self.assertTrue(s.select("S3")["ok"])
        self.assertTrue(s.exit("planned", "S3")["ok"])
        s.confirmed("REL", 1, "w7-rel.log")
        s.enter()
        self.assertTrue(s.write(5)["ok"])
        self.assertTrue(s.select("S4")["ok"])
        self.assertTrue(s.exit("planned", "S4")["ok"])
        s.confirmed("HIL", 0, "w7-hil2.log")
        s.enter()
        self.assertTrue(s.write(6)["ok"])
        self.assertTrue(s.select("S5")["ok"])
        self.assertTrue(s.exit("planned", "S5")["ok"])
        # final oracle: ota_1 = REL next boot VALID, ota_0 = HIL
        nb = ota_select.next_boot(ota_select.parse(bytes(self.chip.f[0xF000:0x11000])))
        self.assertEqual((nb["slot"], nb["state_name"]), (1, "VALID"))
        self.assertTrue(self.slot_bytes(OTA1, "REL") and self.slot_bytes(OTA0, "HIL"))
        s.enter()
        for n in range(1, 7):
            with self.assertRaises(romseq.Refused):
                s.write(n)                               # nothing beyond the six
        with self.assertRaises(romseq.Refused):
            s.write(7)

    def _to_step6(self):
        s = self.seq()
        s.enter(); s.write(1); s.write(2); s.select("S1"); s.exit("planned", "S1")
        s.confirmed("R", 1, "w7")
        s.enter()
        return s

    def test_write_cap_and_fallback_exit(self):
        s = self._to_step6()
        self.chip.fail_next_write = 2
        self.assertFalse(s.write(3)["ok"])
        self.assertFalse(s.write(3)["ok"])
        with self.assertRaises(romseq.Refused):
            s.write(3)                                   # exhausted
        with self.assertRaises(romseq.Refused):
            s.select("S2")
        r = s.exit("planned", "S2")
        self.assertIn("hold", r)                         # no planned exit without S2
        r = s.exit("fallback")
        self.assertTrue(r["ok"], r)                      # (d)2: R VALID on ota_1, byte-equal
        self.assertEqual((r["image"], r["slot"]), ("R", "ota_1"))
        with self.assertRaises(romseq.Refused):
            s.enter()                                    # STOP after a fallback exit

    def test_no_exit_before_r_established(self):
        s = self.seq()
        s.enter(); s.write(1)
        r = s.exit("fallback")
        self.assertIn("hold", r)
        self.assertTrue(self.chip.in_rom)

    def test_fallback_refused_when_bytes_differ(self):
        s = self._to_step6()
        self.chip.f[OTA1 + 100] ^= 0xFF                  # R's slot no longer byte-equal
        r = s.exit("fallback")
        self.assertIn("hold", r)
        self.assertTrue(self.chip.in_rom)

    def test_pending_next_boot_refused(self):
        s = self._to_step6()
        od = bytearray(self.chip.f[0xF000:0x11000])
        info = ota_select.parse(bytes(od))
        act = ota_select.active(info)
        struct.pack_into("<I", od, act["sector"] * 0x1000 + 24, 1)   # R entry PENDING
        other = 1 - act["sector"]
        struct.pack_into("<I", od, other * 0x1000 + 24, 3)            # other INVALID
        self.chip.f[0xF000:0x11000] = od
        r = s.exit("fallback")
        self.assertIn("hold", r)                                      # next boot none

    def test_select_retry_same_entry_and_cap(self):
        s = self._to_step6()
        s.write(3)
        self.chip.fail_next_write = 1
        a1 = s.select("S2")
        self.assertFalse(a1["ok"])
        a2 = s.select("S2")
        self.assertTrue(a2["ok"])
        self.assertEqual((a1["offset"], a1["seq"]), (a2["offset"], a2["seq"]))

    def test_write_needs_confirmation(self):
        s = self.seq()
        s.enter(); s.write(1); s.write(2); s.select("S1"); s.exit("planned", "S1")
        s.enter()
        with self.assertRaises(romseq.Refused):
            s.write(3)                                   # R not confirmed yet


if __name__ == "__main__":
    unittest.main()
