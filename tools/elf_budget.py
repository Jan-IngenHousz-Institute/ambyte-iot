#!/usr/bin/env python3
"""Release-image size budget: head ELF vs base ELF (contract §3 "Budget", C-39).

The SD write-integrity fixes must stay cheap on the release image: flash <= +8 KiB,
internal DRAM (.dram0 data+bss) <= +1 KiB, RTC_NOINIT <= +256 B. The limits are DELTAS
(head - base) in bytes, never absolute sizes: the base image is whatever `e1ca6ee` builds
to with the same toolchain in the same invocation, so toolchain drift cancels out.

Pure-Python ELF section-header parser (ELF32/ELF64, little/big endian), stdlib only:
pyelftools / xtensa binutils are not guaranteed on the evidence host, and a size check
that silently "passes" because a tool is missing is worse than none. Any parse failure
is a FAIL.
"""
from __future__ import annotations

import argparse
import struct
import sys

SHT_NOBITS = 8
SHF_ALLOC = 0x2


class ElfError(Exception):
    pass


def parse_sections(data: bytes) -> list[dict]:
    """Return [{name, type, flags, size, offset, link, entsize}] from the section headers."""
    if len(data) < 16 or data[:4] != b"\x7fELF":
        raise ElfError("not an ELF file (bad magic)")
    ei_class, ei_data = data[4], data[5]
    if ei_class not in (1, 2):
        raise ElfError(f"bad EI_CLASS {ei_class}")
    if ei_data not in (1, 2):
        raise ElfError(f"bad EI_DATA {ei_data}")
    e = "<" if ei_data == 1 else ">"
    try:
        if ei_class == 1:
            (shoff,) = struct.unpack_from(e + "I", data, 0x20)
            shentsize, shnum, shstrndx = struct.unpack_from(e + "HHH", data, 0x2E)
            shfmt, want = e + "IIIIIIIIII", 40
        else:
            (shoff,) = struct.unpack_from(e + "Q", data, 0x28)
            shentsize, shnum, shstrndx = struct.unpack_from(e + "HHH", data, 0x3A)
            shfmt, want = e + "IIQQQQIIQQ", 64
    except struct.error as ex:
        raise ElfError(f"truncated ELF header ({ex})") from None
    if shoff == 0:
        raise ElfError("no section header table")
    if shentsize != want:
        raise ElfError(f"unexpected e_shentsize {shentsize} (want {want})")

    def hdr(i: int) -> tuple:
        off = shoff + i * shentsize
        if off + shentsize > len(data):
            raise ElfError(f"section header {i} beyond end of file")
        return struct.unpack_from(shfmt, data, off)

    # Extended numbering (ELF gABI): >= 0xff00 sections store the real count in
    # section 0's sh_size and the real shstrndx (SHN_XINDEX) in its sh_link.
    if shnum == 0:
        shnum = hdr(0)[5]
    if shstrndx == 0xFFFF:
        shstrndx = hdr(0)[6]
    if shnum == 0 or shnum > 1_000_000:
        raise ElfError(f"implausible section count {shnum}")

    raw = [hdr(i) for i in range(shnum)]
    if shstrndx >= shnum:
        raise ElfError(f"e_shstrndx {shstrndx} out of range")
    stroff, strsize = raw[shstrndx][4], raw[shstrndx][5]
    if stroff + strsize > len(data):
        raise ElfError("section name table beyond end of file")
    strtab = data[stroff:stroff + strsize]

    out = []
    for h in raw:
        name_off, typ, flags, _addr, offset, size, link, _info, _align, entsize = h
        end = strtab.find(b"\0", name_off)
        if name_off > len(strtab) or end < 0:
            raise ElfError(f"bad section name offset {name_off}")
        out.append({"name": strtab[name_off:end].decode("utf-8", "replace"), "type": typ,
                    "flags": flags, "size": size, "offset": offset, "link": link,
                    "entsize": entsize})
    return out


def metrics(path: str) -> dict[str, int]:
    with open(path, "rb") as f:
        secs = parse_sections(f.read())
    # flash: everything the loader places from the image (ALLOC and has file bytes).
    # .dram0.data counts here AND in dram: its initialiser lives in flash, the live copy
    # in DRAM.
    flash = sum(s["size"] for s in secs if s["flags"] & SHF_ALLOC and s["type"] != SHT_NOBITS)
    dram = sum(s["size"] for s in secs
               if s["name"].startswith(".dram0.data") or s["name"].startswith(".dram0.bss"))
    rtc = sum(s["size"] for s in secs if s["name"].startswith(".rtc_noinit"))
    return {"flash": flash, "dram": dram, "rtc_noinit": rtc}


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="elf_budget.py",
        description="Compare section sizes of a head ELF against a base ELF. All limits are "
                    "DELTAS (head - base) in bytes, not absolute sizes. "
                    "flash = sum of SHF_ALLOC non-NOBITS sections; dram = .dram0.data* + "
                    ".dram0.bss*; rtc_noinit = .rtc_noinit*. Exits non-zero if any delta "
                    "exceeds its limit or either ELF cannot be parsed.",
        allow_abbrev=False)
    p.add_argument("--base", required=True, help="baseline ELF")
    p.add_argument("--head", required=True, help="candidate ELF")
    p.add_argument("--max-flash", type=int, required=True, help="max flash delta, bytes (head-base)")
    p.add_argument("--max-dram", type=int, required=True, help="max .dram0 data+bss delta, bytes (head-base)")
    p.add_argument("--max-rtc-noinit", type=int, required=True,
                   help="max .rtc_noinit delta, bytes (head-base)")
    a = p.parse_args(argv)

    try:
        base, head = metrics(a.base), metrics(a.head)
    except (OSError, ElfError) as e:
        print(f"FAIL cannot parse ELF: {e}")
        return 1

    limits = {"flash": a.max_flash, "dram": a.max_dram, "rtc_noinit": a.max_rtc_noinit}
    ok = True
    for k in ("flash", "dram", "rtc_noinit"):
        d = head[k] - base[k]
        good = d <= limits[k]
        ok = ok and good
        print(f"{'PASS' if good else 'FAIL'} {k}: base={base[k]} head={head[k]} "
              f"delta={d:+d} limit=+{limits[k]}")
    print("BUDGET " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
