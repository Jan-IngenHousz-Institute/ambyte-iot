"""ROM-session driver for the Sprint 2 replacement-bench transition sequence
(replacement amendment r3.3 §3, frozen fb829a37…). Every app-slot write, every
otadata selection and every ROM exit of the sequence goes through this tool, so
the frozen caps and the exit rule (d) are enforced mechanically, not by memory.

    python romseq.py --plan PLAN.json --ledger LEDGER.json enter
    python romseq.py ... write <1..6>          # planned app replacement #n (one attempt)
    python romseq.py ... select <S1..S5>       # planned NEW selection (one attempt)
    python romseq.py ... confirmed <image> <slot> --evidence LOG   # after its production W-7
    python romseq.py ... exit planned <S#> | exit fallback
    python romseq.py ... status

PLAN.json (private): {"mac": "...", "images": {"R": {"path":..,"sha256":..},
"HIL": {..}, "REL": {..}}}. The write/selection order, slots and caps are
hard-coded below from the frozen text; the plan only binds image files to SHAs.
Every esptool call stays in the ROM loader (`--after no_reset`) except `exit`,
the only hard reset. Nothing here can erase, or write outside an app slot or
otadata.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench  # noqa: E402
import flashimg  # noqa: E402
import ota_select  # noqa: E402

# (a) the six planned replacements, in order: (image, slot)
WRITES = {1: ("R", 0), 2: ("R", 1), 3: ("HIL", 0), 4: ("REL", 1), 5: ("HIL", 0), 6: ("REL", 1)}
# (c) the five planned selections: (slot, image, prerequisite write)
SELECTS = {"S1": (1, "R", 2), "S2": (0, "HIL", 3), "S3": (1, "REL", 4), "S4": (0, "HIL", 5), "S5": (1, "REL", 6)}
# write n may start only after this selection was verified (S1 needs writes 1 and 2 only)
WRITE_AFTER = {3: "S1", 4: "S2", 5: "S3", 6: "S4"}
# ... and after the image that selection booted passed its production W-7 (steps 5, 7, 8, 9)
CONFIRM_BEFORE = {3: ("R", 1), 4: ("HIL", 0), 5: ("REL", 1), 6: ("HIL", 0)}
MAX_ATTEMPTS = 2
OTADATA = (0xF000, 0x2000)
SLOT_LABEL = {0: "ota_0", 1: "ota_1"}


class Refused(SystemExit):
    pass


def load(p: Path, default):
    return json.loads(p.read_text()) if p.exists() else default


def save(p: Path, obj) -> None:
    tmp = p.with_suffix(".tmp")
    tmp.write_text(json.dumps(obj, indent=1))
    os.chmod(tmp, 0o600)
    os.replace(tmp, p)


def sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


class Seq:
    def __init__(self, plan: Path, ledger: Path, log: Path, dry: bool = False):
        self.plan = json.loads(plan.read_text())
        self.ledger_path = ledger
        self.led = load(ledger, {"writes": {}, "selects": {}, "confirmed": [], "events": [], "in_rom": False})
        self.log = log
        self.dry = dry
        if self.plan.get("mac", "").lower() != bench.mac().lower():
            raise Refused(f"plan MAC {self.plan.get('mac')} != bench MAC {bench.mac()}")

    # ── plumbing ──
    def esptool(self, args: list[str]) -> str:
        if self.dry:
            raise Refused("dry run: no esptool")
        return flashimg.esptool(args, self.log)

    def event(self, kind: str, **kw) -> None:
        self.led["events"].append({"t": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "kind": kind, **kw})
        save(self.ledger_path, self.led)

    def image(self, name: str) -> tuple[bytes, str]:
        im = self.plan["images"][name]
        data = Path(im["path"]).read_bytes()
        if sha(data) != im["sha256"]:
            raise Refused(f"{name}: file sha {sha(data)} != plan {im['sha256']}")
        return data, im["sha256"]

    def read(self, off: int, n: int) -> bytes:
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "r.bin"
            self.esptool(["--before", "no_reset", "--after", "no_reset", "read_flash", hex(off), hex(n), str(out)])
            return out.read_bytes()

    def slots(self) -> dict[int, dict]:
        pt = flashimg.parse_pt(self.read(flashimg.PT_OFFSET, 0xC00), 0)
        apps = {p["subtype"] - 0x10: p for p in pt if p["type"] == 0 and 0x10 <= p["subtype"] <= 0x11}
        if set(apps) != {0, 1}:
            raise Refused(f"partition table has no ota_0/ota_1 pair: {pt}")
        return apps

    def otadata(self) -> bytes:
        return self.read(*OTADATA)

    def need_rom(self) -> None:
        if not self.led.get("in_rom"):
            raise Refused("not in a ROM session (run `enter` first)")

    def ok(self, kind: str, key: str) -> bool:
        return any(a["ok"] for a in self.led[kind].get(key, []))

    def attempts(self, kind: str, key: str) -> int:
        return len(self.led[kind].get(key, []))

    # ── commands ──
    def enter(self) -> dict:
        if self.led.get("halt"):
            raise Refused(f"sequence halted: {self.led['halt']}")
        out = self.esptool(["--before", "default_reset", "--after", "no_reset", "chip_id"])
        mac = next((ln.split("MAC:")[1].strip() for ln in out.splitlines() if "MAC:" in ln), "")
        if mac.lower() != bench.mac().lower():
            raise Refused(f"ROM MAC {mac} != bench {bench.mac()} (still in ROM; hold)")
        self.led["in_rom"] = True
        self.event("enter", mac=mac)
        return {"in_rom": True, "mac": mac}

    def write(self, n: int) -> dict:
        self.need_rom()
        if n not in WRITES:
            raise Refused(f"no planned write #{n}")
        for m in range(1, n):
            if not self.ok("writes", str(m)):
                raise Refused(f"write #{m} is not verified yet; #{n} must wait")
        if self.ok("writes", str(n)):
            raise Refused(f"write #{n} already verified")
        pre = WRITE_AFTER.get(n)
        if pre and not self.ok("selects", pre):
            raise Refused(f"write #{n} needs {pre} verified first")
        if n in CONFIRM_BEFORE:
            img, sl = CONFIRM_BEFORE[n]
            if not any(c["image"] == img and c["slot"] == SLOT_LABEL[sl] for c in self.led["confirmed"]):
                raise Refused(f"write #{n} needs {img} confirmed VALID on {SLOT_LABEL[sl]} (production W-7) first")
        if n == 4 and not self.led.get("ambit_gate"):
            raise Refused("write #4 (step 8) needs the AMBIT gate: every AMBIT at verified 1.3.0 (`ambit_gate`)")
        if self.attempts("writes", str(n)) >= MAX_ATTEMPTS:
            raise Refused(f"write #{n}: {MAX_ATTEMPTS} attempts used (exhausted; ROM hold / (d))")
        name, slot = WRITES[n]
        data, want = self.image(name)
        apps = self.slots()
        off, size = apps[slot]["offset"], apps[slot]["size"]
        if len(data) > size:
            raise Refused(f"{name} ({len(data)} B) > {SLOT_LABEL[slot]} ({size} B)")
        att = {"n": n, "image": name, "slot": SLOT_LABEL[slot], "offset": hex(off), "sha256": want, "ok": False}
        self.led["writes"].setdefault(str(n), []).append(att)
        save(self.ledger_path, self.led)             # the attempt counts BEFORE the chip is touched
        try:
            path = self.plan["images"][name]["path"]
            self.esptool(["--before", "no_reset", "--after", "no_reset", "write_flash", "--flash_mode", "keep",
                          "--flash_size", "keep", hex(off), path])
            self.esptool(["--before", "no_reset", "--after", "no_reset", "verify_flash", hex(off), path])
            rb = sha(self.read(off, len(data)))
            att["readback_sha256"] = rb
            att["ok"] = rb == want
        except SystemExit as e:
            att["error"] = str(e)[-400:]
        save(self.ledger_path, self.led)
        self.event("write", n=n, ok=att["ok"])
        return att

    def select(self, s: str) -> dict:
        self.need_rom()
        if s not in SELECTS:
            raise Refused(f"no planned selection {s}")
        order = list(SELECTS)
        for p in order[:order.index(s)]:
            if not self.ok("selects", p):
                raise Refused(f"{p} is not verified yet; {s} must wait")
        slot, name, prereq = SELECTS[s]
        if not self.ok("writes", str(prereq)):
            raise Refused(f"{s} needs write #{prereq} verified")
        if self.ok("selects", s):
            raise Refused(f"{s} already verified")
        if self.attempts("selects", s) >= MAX_ATTEMPTS:
            raise Refused(f"{s}: {MAX_ATTEMPTS} attempts used (exhausted)")
        prior = self.led["selects"].get(s, [])
        cur = self.otadata()
        # a retry renders from the FIRST attempt's base: the same entry to the same sector
        off, sector, meta = ota_select.select(bytes.fromhex(prior[0]["base_otadata"]) if prior else cur, slot)
        att = {"s": s, "slot": SLOT_LABEL[slot], "offset": hex(off), "seq": meta["seq"],
               "base_otadata": (bytes.fromhex(prior[0]["base_otadata"]) if prior else cur).hex(), "ok": False}
        self.led["selects"].setdefault(s, []).append(att)
        save(self.ledger_path, self.led)
        other = 1 - ((off - OTADATA[0]) // 0x1000)
        base = bytes.fromhex(att["base_otadata"])
        try:
            with tempfile.TemporaryDirectory() as d:
                p = Path(d) / "sector.bin"
                p.write_bytes(sector)
                self.esptool(["--before", "no_reset", "--after", "no_reset", "write_flash", "--flash_mode", "keep",
                              "--flash_size", "keep", hex(off), str(p)])
            new = self.otadata()
            info = ota_select.parse(new)
            act = ota_select.active(info)
            att["parse"] = info
            att["ok"] = (act is not None and act["slot"] == slot and act["state_name"] == "NEW"
                         and act["seq"] == meta["seq"] and act["sector"] == (off - OTADATA[0]) // 0x1000
                         and new[other * 0x1000:(other + 1) * 0x1000] == base[other * 0x1000:(other + 1) * 0x1000])
        except SystemExit as e:
            att["error"] = str(e)[-400:]
        save(self.ledger_path, self.led)
        self.event("select", s=s, ok=att["ok"])
        return att

    def confirmed(self, image: str, slot: int, evidence: str) -> dict:
        if self.led.get("in_rom"):
            raise Refused("a W-7 confirmation is observed on the running app, not in ROM")
        ent = {"image": image, "slot": SLOT_LABEL[slot], "sha256": self.plan["images"][image]["sha256"],
               "evidence": evidence}
        self.led["confirmed"].append(ent)
        self.event("confirmed", **ent)
        return ent

    def ambit_gate(self, evidence: str) -> dict:
        self.led["ambit_gate"] = {"evidence": evidence}
        self.event("ambit_gate", evidence=evidence)
        return {"ambit_gate": True, "evidence": evidence}

    def exit(self, mode: str, s: str | None = None) -> dict:
        """Rule (d): hard reset only if a fresh parse + slot read-back prove a
        permitted next boot; otherwise refuse (ROM hold)."""
        self.need_rom()
        info = ota_select.parse(self.otadata())
        nb = ota_select.next_boot(info)
        apps = self.slots()
        res = {"mode": mode, "next_boot": nb, "ok": False}
        if nb is None:
            return self._hold(res, "next boot is none (no usable entry)")
        slot = nb["slot"]
        if mode == "planned":
            if s not in SELECTS or not self.ok("selects", s):
                return self._hold(res, f"{s} not verified")
            want_slot, name, _ = SELECTS[s]
            if slot != want_slot or nb["state_name"] != "NEW":
                return self._hold(res, f"next boot {SLOT_LABEL[slot]} {nb['state_name']} != {SLOT_LABEL[want_slot]} NEW")
            data, want = self.image(name)
        else:
            if not self.ok("selects", "S1") or not any(c["image"] == "R" and c["slot"] == "ota_1"
                                                       for c in self.led["confirmed"]):
                return self._hold(res, "fallback exits start at step 6 (after S1 and R's step-5 W-7)")
            if nb["state_name"] != "VALID":
                return self._hold(res, f"next boot is {nb['state_name']}, not VALID")
            conf = [c for c in self.led["confirmed"] if c["slot"] == SLOT_LABEL[slot]]
            if not conf:
                return self._hold(res, f"no image confirmed VALID on {SLOT_LABEL[slot]} in this sequence")
            name = conf[-1]["image"]
            data, want = self.image(name)
        rb = sha(self.read(apps[slot]["offset"], len(data)))
        res.update({"image": name, "slot": SLOT_LABEL[slot], "readback_sha256": rb})
        if rb != want:
            return self._hold(res, f"{SLOT_LABEL[slot]} read-back {rb} != {name} {want}")
        self.esptool(["--before", "no_reset", "--after", "hard_reset", "chip_id"])
        self.led["in_rom"] = False
        if mode == "fallback":
            self.led["halt"] = f"fallback exit to {name} on {SLOT_LABEL[slot]}: STOP (r3.3 (d)2)"
        res["ok"] = True
        self.event("exit", mode=mode, s=s, image=name, slot=SLOT_LABEL[slot])
        return res

    def _hold(self, res: dict, why: str) -> dict:
        res["hold"] = why
        self.event("exit_refused", why=why)
        return res


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--plan", required=True, type=Path)
    ap.add_argument("--ledger", required=True, type=Path)
    ap.add_argument("--log", type=Path)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("enter")
    sub.add_parser("status")
    w = sub.add_parser("write")
    w.add_argument("n", type=int)
    s = sub.add_parser("select")
    s.add_argument("s")
    c = sub.add_parser("confirmed")
    c.add_argument("image", choices=["R", "HIL", "REL"])
    c.add_argument("slot", type=int, choices=[0, 1])
    c.add_argument("--evidence", required=True)
    g = sub.add_parser("ambit_gate")
    g.add_argument("--evidence", required=True)
    e = sub.add_parser("exit")
    e.add_argument("mode", choices=["planned", "fallback"])
    e.add_argument("s", nargs="?")
    a = ap.parse_args()
    os.umask(0o077)
    seq = Seq(a.plan, a.ledger, a.log or a.ledger.with_suffix(".esptool.log"))
    if a.cmd == "status":
        out = {k: seq.led[k] for k in ("in_rom", "confirmed")}
        out["writes"] = {k: [x["ok"] for x in v] for k, v in seq.led["writes"].items()}
        out["selects"] = {k: [x["ok"] for x in v] for k, v in seq.led["selects"].items()}
    elif a.cmd == "enter":
        out = seq.enter()
    elif a.cmd == "write":
        out = seq.write(a.n)
    elif a.cmd == "select":
        out = seq.select(a.s)
    elif a.cmd == "ambit_gate":
        out = seq.ambit_gate(a.evidence)
    elif a.cmd == "confirmed":
        out = seq.confirmed(a.image, a.slot, a.evidence)
    else:
        out = seq.exit(a.mode, a.s)
    print(json.dumps({k: v for k, v in out.items() if k not in ("parse", "base_otadata")}, default=str))
    return 0 if out.get("ok", True) and "hold" not in out else 1


if __name__ == "__main__":
    raise SystemExit(main())
