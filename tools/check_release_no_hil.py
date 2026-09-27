#!/usr/bin/env python3
"""Release-image HIL exclusion + evq-hil wrapper presence (contract C-36, C-37).

The bench-only fault command (`CONFIG_AMBYTE_EVQ_HIL`, env `evq-hil`) can make SD writes
fail, short-write, or reset the CPU on demand. It must never ship: a fleet image that
still links it is one stray console line away from destroying data on a field card.
So the RELEASE image is checked three independent ways, any one of which fails it:

  * ELF .symtab has no symbol containing a HIL-only name part (`evq_hil`, `evq_arm`,
    `evq_tr_`, plus the Sprint 2 sd_logger trace/inventory parts `hil_sdl`, `sdl_hil`,
    `slt_`, `sd_logger_hil`);
  * firmware.bin has none of the HIL commands' / markers' literal strings, and - the
    Sprint 2 naming rule (contract r4 §2.2) - not even the byte PREFIXES `SDL_` / `SLT_`
    anywhere: every sd_logger HIL marker starts with one of them, so a new marker that
    leaks into the release image is caught without having to be listed here;
  * the build's effective generated config ($REL/config/sdkconfig.json, recorded and
    consumed by hash in the same evidence run) has AMBYTE_EVQ_HIL absent or false. The
    mutable source-worktree sdkconfig is never read: it can differ from what was built.

`--expect-wrapped` is the converse for the evq-hil image (C-36): the HIL build must
actually route the new writers' SD calls through the `evq_hil_io_*` wrappers, proved
per object by an UNDEFINED reference (the call site exists in that TU and is resolved
to the wrapper at link time); and (Sprint 2 §2.2) the HIL ELF must carry every Sprint 2
command and marker string - the converse proof that the strings the release scan bans
are the ones the verification build really prints (a renamed marker would otherwise
make the release scan vacuous).

`--self-test` builds synthetic ELF/bin/json fixtures and proves the checks catch
pollution (C-37 "`--self-test` fails on a polluted fixture").

Pure-Python ELF parser, stdlib only: see tools/elf_budget.py for why no nm/pyelftools.
The parser is duplicated here on purpose so each evidence tool runs standalone.
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import tempfile

SHT_SYMTAB = 2
SHT_STRTAB = 3
SHT_NOBITS = 8
SHN_UNDEF = 0

FORBIDDEN_SYMBOL_PARTS = ("evq_hil", "evq_arm", "evq_tr_",
                          # Sprint 2 (contract r4 §2.2): sd_logger trace / quiesce / inventory.
                          # ISO-NAMES (tests/test_iso_names.py) makes every function or object
                          # the Sprint 2 diff defines in HIL-only code carry one of these, so
                          # this substring scan binds the whole HIL surface - including the
                          # generic single-token trace labels (P, POP, WR, ...) that a string
                          # scan cannot ban, because they are only emitted from such functions.
                          "hil_sdl", "sdl_hil", "slt_", "sd_logger_hil")
# Sprint 1 strings, then the Sprint 2 commands and markers (§2.2). The SDL_*/SLT_*
# markers are also covered by the prefix ban below; they stay listed so a hit names
# the marker instead of only the prefix.
FORBIDDEN_BIN_STRINGS = (b"HIL_FAULT", b"evq_hil", b"fault io", b"power_cut", b"slot=",
                         b"sdlog_emit", b"sdlog_inv", b"sdlog_dump", b"sdlog_trace", b"HILSDLOG",
                         b"SDL_BEGIN", b"SDL_END", b"SDL_FF", b"SDL_FL", b"SDL_STATE", b"SDL_MORE",
                         b"SDL_INVALID", b"SDL_TIMEOUT", b"SDL_B64", b"SDL_DUMP_END", b"SDL_Q",
                         b"SLT_HDR", b"SLT_DRAIN", b"SLT_WM", b"SLT_ERR")
# Byte prefixes banned ANYWHERE in the release image (naming rule, §2.2 ISO-NAMES).
# A clean release build at 4ec9cef has no occurrence (checked against the Sprint 1
# evidence run's build_rel/firmware.bin); if a future production string ever needed
# one, the rule says rename the HIL marker, never relax this list.
FORBIDDEN_BIN_PREFIXES = (b"SDL_", b"SLT_")
# Exact PRODUCTION contexts of a forbidden string that predate the HIL marker and must
# not be renamed (BLD-1 forbids touching production code for isolation). `slot=` is the
# H1 fired-line field (` slot=<A|B>`), but event_log.c's window-frontier error already
# prints "... slot=%u:%ld id=%lld ..." in every release image since 1.0.6. Only that
# exact byte context is exempt; any other `slot=` (e.g. the HIL "slot=%c") still fails.
ALLOWED_BIN_CONTEXTS = {b"slot=": (b"slot=%u:%ld",)}
# --expect-wrapped: the HIL image must carry these (commands + markers, §2.2).
REQUIRED_HIL_STRINGS = (b"HIL_FAULT", b"slot=", b"sdlog_emit", b"sdlog_inv", b"sdlog_dump", b"sdlog_trace",
                        b"HILSDLOG", b"SDL_BEGIN", b"SDL_END", b"SDL_FF", b"SDL_FL", b"SDL_STATE",
                        b"SDL_MORE", b"SDL_INVALID", b"SDL_TIMEOUT", b"SDL_B64", b"SDL_DUMP_END", b"SDL_Q",
                        b"SLT_HDR", b"SLT_DRAIN", b"SLT_WM", b"SLT_ERR")
HIL_CONFIG_KEYS = ("AMBYTE_EVQ_HIL", "CONFIG_AMBYTE_EVQ_HIL")
# The three new SD writers that must be wrapped in the evq-hil image (§1 W2..W4).
WRAPPED_OBJECTS = ("sd_logger.c", "ambit_stage.c", "ambit_flash_preflight.c")
OBJ_SUFFIXES = (".obj", ".o")
WRAPPER_PREFIX = "evq_hil_io_"


class ElfError(Exception):
    pass


# --------------------------------------------------------------------------- ELF parsing

def _sections(data: bytes) -> tuple[str, int, list[dict]]:
    if len(data) < 16 or data[:4] != b"\x7fELF":
        raise ElfError("not an ELF file (bad magic)")
    ei_class, ei_data = data[4], data[5]
    if ei_class not in (1, 2) or ei_data not in (1, 2):
        raise ElfError(f"bad EI_CLASS/EI_DATA {ei_class}/{ei_data}")
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
    if shoff == 0 or shentsize != want:
        raise ElfError("missing or malformed section header table")

    def hdr(i: int) -> tuple:
        off = shoff + i * shentsize
        if off + shentsize > len(data):
            raise ElfError(f"section header {i} beyond end of file")
        return struct.unpack_from(shfmt, data, off)

    if shnum == 0:
        shnum = hdr(0)[5]
    if shstrndx == 0xFFFF:
        shstrndx = hdr(0)[6]
    if shnum == 0 or shnum > 1_000_000 or shstrndx >= shnum:
        raise ElfError("implausible section count / shstrndx")
    secs = []
    for i in range(shnum):
        n, t, fl, _a, off, size, link, info, _al, ent = hdr(i)
        secs.append({"name_off": n, "type": t, "flags": fl, "offset": off, "size": size,
                     "link": link, "info": info, "entsize": ent})
    return e, ei_class, secs


def _cstr(blob: bytes, off: int) -> str:
    end = blob.find(b"\0", off)
    if off > len(blob) or end < 0:
        raise ElfError(f"bad string offset {off}")
    return blob[off:end].decode("utf-8", "replace")


def elf_symbols(path: str) -> list[tuple[str, int]]:
    """Return [(name, st_shndx)] for every named .symtab entry. Raises ElfError if the
    file is not an ELF or has no .symtab (a stripped image cannot prove absence)."""
    with open(path, "rb") as f:
        data = f.read()
    e, cls, secs = _sections(data)
    symfmt, symsize = ((e + "IIIBBH", 16) if cls == 1 else (e + "IBBHQQ", 24))
    out: list[tuple[str, int]] = []
    found = False
    for s in secs:
        if s["type"] != SHT_SYMTAB:
            continue
        found = True
        if s["link"] >= len(secs):
            raise ElfError("symtab sh_link out of range")
        st = secs[s["link"]]
        if st["offset"] + st["size"] > len(data) or s["offset"] + s["size"] > len(data):
            raise ElfError("symtab/strtab beyond end of file")
        strtab = data[st["offset"]:st["offset"] + st["size"]]
        for off in range(s["offset"], s["offset"] + s["size"] - symsize + 1, symsize):
            f_ = struct.unpack_from(symfmt, data, off)
            name_off, shndx = (f_[0], f_[5]) if cls == 1 else (f_[0], f_[3])
            if name_off:
                out.append((_cstr(strtab, name_off), shndx))
    if not found:
        raise ElfError("no .symtab (stripped?) - cannot prove symbol absence")
    return out


# --------------------------------------------------------------------------- fixture builder

def build_elf(sections: list[tuple], symbols: list[tuple[str, int]] | None = None,
              ei_class: int = 1, endian: str = "<", e_type: int = 2) -> bytes:
    """Build a minimal, valid ELF by hand for fixtures.

    sections: [(name, sh_type, sh_flags, payload_bytes_or_size_for_NOBITS)].
    symbols:  [(name, st_shndx)] -> emitted as .symtab/.strtab when not None.
    Section indices in `symbols` refer to the final table (0 = NULL/UNDEF, then the
    caller's sections in order starting at 1).
    """
    e = endian
    secs = list(sections)
    if symbols is not None:
        strtab = b"\0"
        offs = []
        for name, _ in symbols:
            offs.append(len(strtab))
            strtab += name.encode() + b"\0"
        if ei_class == 1:
            sym = b"\0" * 16 + b"".join(struct.pack(e + "IIIBBH", o, 0, 0, 0x12, 0, shn)
                                        for o, (_, shn) in zip(offs, symbols))
        else:
            sym = b"\0" * 24 + b"".join(struct.pack(e + "IBBHQQ", o, 0x12, 0, shn, 0, 0)
                                        for o, (_, shn) in zip(offs, symbols))
        symtab_idx = len(secs) + 1
        secs.append((".symtab", SHT_SYMTAB, 0, sym))
        secs.append((".strtab", SHT_STRTAB, 0, strtab))
    else:
        symtab_idx = None
    shstr = b"\0"
    name_offs = []
    for name, *_ in secs + [(".shstrtab",)]:
        name_offs.append(len(shstr))
        shstr += name.encode() + b"\0"
    secs.append((".shstrtab", SHT_STRTAB, 0, shstr))

    ehsize = 52 if ei_class == 1 else 64
    body = b""
    placed = []  # (offset, size)
    for _name, typ, _fl, payload in secs:
        if typ == SHT_NOBITS:
            placed.append((ehsize + len(body), int(payload)))
            continue
        while (ehsize + len(body)) % 4:
            body += b"\0"
        placed.append((ehsize + len(body), len(payload)))
        body += payload
    while (ehsize + len(body)) % 8:
        body += b"\0"
    shoff = ehsize + len(body)
    shnum = len(secs) + 1
    shstrndx = shnum - 1
    ident = b"\x7fELF" + bytes([ei_class, 1 if e == "<" else 2, 1]) + b"\0" * 9
    if ei_class == 1:
        hdr = ident + struct.pack(e + "HHIIIIIHHHHHH", e_type, 94, 1, 0, 0, shoff, 0,
                                  ehsize, 0, 0, 40, shnum, shstrndx)
        shfmt = e + "IIIIIIIIII"
    else:
        hdr = ident + struct.pack(e + "HHIQQQIHHHHHH", e_type, 62, 1, 0, 0, shoff, 0,
                                  ehsize, 0, 0, 64, shnum, shstrndx)
        shfmt = e + "IIQQQQIIQQ"
    shdrs = struct.pack(shfmt, *([0] * 10))
    for i, ((_name, typ, fl, _p), (off, size)) in enumerate(zip(secs, placed)):
        link = entsize = info = 0
        if typ == SHT_SYMTAB:
            link, entsize, info = symtab_idx + 1, (16 if ei_class == 1 else 24), 1
        shdrs += struct.pack(shfmt, name_offs[i], typ, fl, 0, off, size, link, info, 4, entsize)
    return hdr + body + shdrs


# --------------------------------------------------------------------------- checks

def _count_forbidden(blob: bytes, needle: bytes) -> int:
    """Occurrences of `needle` in `blob` minus the exempt production contexts."""
    n = blob.count(needle)
    for ctx in ALLOWED_BIN_CONTEXTS.get(needle, ()):
        n -= blob.count(ctx)
    return n


def forbidden_bin_hits(blob: bytes) -> list[str]:
    hits = [s.decode() for s in FORBIDDEN_BIN_STRINGS if _count_forbidden(blob, s) > 0]
    hits += [f"prefix {p.decode()}" for p in FORBIDDEN_BIN_PREFIXES if p in blob]
    return hits


def check_release(elf: str, binf: str, cfg: str) -> list[tuple[bool, str]]:
    res: list[tuple[bool, str]] = []
    try:
        bad = sorted({n for n, _ in elf_symbols(elf) if any(p in n for p in FORBIDDEN_SYMBOL_PARTS)})
        res.append((not bad, "ELF symbols: no " + "/".join(FORBIDDEN_SYMBOL_PARTS)
                    + ("" if not bad else f" (found {', '.join(bad[:10])})")))
    except (OSError, ElfError) as e:
        res.append((False, f"ELF symbols: cannot read {elf}: {e}"))
    try:
        with open(binf, "rb") as f:
            blob = f.read()
        hits = forbidden_bin_hits(blob)
        res.append((not hits, f"bin strings: none of {len(FORBIDDEN_BIN_STRINGS)} HIL strings "
                    f"(HIL_FAULT/evq_hil/fault io/power_cut/slot=/sdlog_*/SDL_*/SLT_*), no "
                    + "/".join(p.decode() for p in FORBIDDEN_BIN_PREFIXES) + " prefix"
                    + ("" if not hits else f" (found {', '.join(hits)})")))
    except OSError as e:
        res.append((False, f"bin strings: cannot read {binf}: {e}"))
    try:
        with open(cfg, encoding="utf-8") as f:
            obj = json.load(f)
        if not isinstance(obj, dict):
            res.append((False, f"sdkconfig.json: {cfg} is not a JSON object"))
        else:
            # Only literal false / 0 / absence mean "off". Anything else (true, 1, "y",
            # null, "n"...) is either on or not a value Kconfig emits for a bool: fail.
            bad = {k: obj[k] for k in HIL_CONFIG_KEYS
                   if k in obj and not (obj[k] is False or (type(obj[k]) is int and obj[k] == 0))}
            res.append((not bad, "sdkconfig.json: AMBYTE_EVQ_HIL absent or false"
                        + ("" if not bad else f" (found {bad})")))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as e:
        res.append((False, f"sdkconfig.json: cannot read/parse {cfg}: {e}"))
    return res


def _find_objects(objdir: str, base: str) -> list[str]:
    names = {base + s for s in OBJ_SUFFIXES} | {base[:-2] + ".o"}
    hits = []
    for root, _dirs, files in os.walk(objdir):
        hits.extend(os.path.join(root, f) for f in files if f in names)
    return sorted(hits)


def check_wrapped(elf: str, objdir: str) -> list[tuple[bool, str]]:
    res: list[tuple[bool, str]] = []
    try:
        hil = [n for n, _ in elf_symbols(elf) if "evq_hil" in n]
        res.append((bool(hil), f"HIL ELF links evq_hil code ({len(hil)} symbols)"))
        # The ELF carries .rodata verbatim, so the command/marker literals are found in
        # its raw bytes (no separate .bin needed; keeps the frozen CLI unchanged).
        with open(elf, "rb") as f:
            blob = f.read()
        missing = [s.decode() for s in REQUIRED_HIL_STRINGS if s not in blob]
        res.append((not missing, f"HIL ELF carries all {len(REQUIRED_HIL_STRINGS)} Sprint 2 command/marker strings"
                    + ("" if not missing else f" (missing {', '.join(missing)})")))
    except (OSError, ElfError) as e:
        res.append((False, f"HIL ELF: cannot read {elf}: {e}"))
    if not os.path.isdir(objdir):
        res.append((False, f"object dir {objdir} does not exist"))
        return res
    for base in WRAPPED_OBJECTS:
        objs = _find_objects(objdir, base)
        if not objs:
            searched = ", ".join(sorted({base + s for s in OBJ_SUFFIXES} | {base[:-2] + ".o"}))
            res.append((False, f"{base}: object not found under {objdir} (searched {searched})"))
            continue
        # Every copy must be wrapped: a second, unwrapped build of the same TU linked
        # anywhere would bypass the fault seam.
        for o in objs:
            try:
                refs = sorted({n for n, shn in elf_symbols(o)
                               if shn == SHN_UNDEF and n.startswith(WRAPPER_PREFIX)})
                res.append((bool(refs), f"{os.path.relpath(o, objdir)}: undefined "
                            f"{WRAPPER_PREFIX}* refs: {', '.join(refs) if refs else 'NONE'}"))
            except (OSError, ElfError) as e:
                res.append((False, f"{o}: cannot read: {e}"))
    return res


# --------------------------------------------------------------------------- self-test

def self_test() -> list[tuple[bool, str]]:
    out: list[tuple[bool, str]] = []

    def expect(name: str, results: list[tuple[bool, str]], want_pass: bool) -> None:
        got = all(ok for ok, _ in results)
        out.append((got == want_pass, f"self-test {name}: expected {'PASS' if want_pass else 'FAIL'}, "
                    f"got {'PASS' if got else 'FAIL'}"))

    text = (".flash.text", 1, 0x6, b"\x90" * 32)
    with tempfile.TemporaryDirectory(prefix="nohil-st-") as td:
        def w(name: str, data: bytes | str) -> str:
            p = os.path.join(td, name)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "wb") as f:
                f.write(data.encode() if isinstance(data, str) else data)
            return p

        clean_elf = w("clean.elf", build_elf([text], [("app_main", 1), ("sd_logger_init", 1)]))
        dirty_elf = w("dirty.elf", build_elf([text], [("app_main", 1), ("evq_hil_fopen", 1)]))
        clean_bin = w("clean.bin", b"\xe9" + b"ambyte release image" + b"\0" * 64)
        dirty_bin = w("dirty.bin", b"\xe9" + b"xx HIL_FAULT fired kind=short" + b"\0" * 8)
        cfg_off = w("off.json", json.dumps({"AMBYTE_EVQ_HIL": False, "IDF_TARGET": "esp32s3"}))
        cfg_abs = w("absent.json", json.dumps({"IDF_TARGET": "esp32s3"}))
        cfg_on = w("on.json", json.dumps({"AMBYTE_EVQ_HIL": True}))
        cfg_bad = w("bad.json", "{not json")
        cfg_list = w("list.json", "[]")
        missing = os.path.join(td, "nope.json")

        # One polluted fixture per forbidden pattern (contract r4 §2.2): each must fail alone.
        for part in FORBIDDEN_SYMBOL_PARTS:
            sym = f"x_{part}probe"
            p_elf = w(f"sym-{part}.elf", build_elf([text], [("app_main", 1), (sym, 1)]))
            expect(f"symbol containing {part!r}", check_release(p_elf, clean_bin, cfg_off), False)
        for needle in FORBIDDEN_BIN_STRINGS + FORBIDDEN_BIN_PREFIXES:
            tag = needle.decode().strip().replace(" ", "_").replace("=", "eq")
            p_bin = w(f"str-{tag}.bin", b"\xe9" + b"release " + needle + b"probe" + b"\0" * 8)
            expect(f"bin string {needle.decode()!r}", check_release(clean_elf, p_bin, cfg_off), False)
        # The exempt production context passes; the HIL form of the same field fails.
        prod_bin = w("slot-prod.bin", b"\xe9 window frontier mismatch: cursor=%u:%ld slot=%u:%ld id=%lld\0")
        expect("production 'slot=%u:%ld' context", check_release(clean_elf, prod_bin, cfg_off), True)
        both_bin = w("slot-both.bin", b"\xe9 slot=%u:%ld id \0 nth=%u slot=%c\0")
        expect("HIL 'slot=%c' beside the production context", check_release(clean_elf, both_bin, cfg_off), False)
        expect("clean release", check_release(clean_elf, clean_bin, cfg_off), True)
        expect("clean release, key absent", check_release(clean_elf, clean_bin, cfg_abs), True)
        expect("evq_hil_fopen symbol", check_release(dirty_elf, clean_bin, cfg_off), False)
        expect("HIL_FAULT in bin", check_release(clean_elf, dirty_bin, cfg_off), False)
        expect("AMBYTE_EVQ_HIL true", check_release(clean_elf, clean_bin, cfg_on), False)
        expect("sdkconfig.json missing", check_release(clean_elf, clean_bin, missing), False)
        expect("sdkconfig.json malformed", check_release(clean_elf, clean_bin, cfg_bad), False)
        expect("sdkconfig.json not an object", check_release(clean_elf, clean_bin, cfg_list), False)

        hil_rodata = (".flash.rodata", 1, 0x2, b"\0".join(REQUIRED_HIL_STRINGS) + b"\0")
        hil_elf = w("hil.elf", build_elf([text, hil_rodata], [("app_main", 1), ("evq_hil_io_fopen_w", 1)]))
        hil_elf_bare = w("hil-bare.elf", build_elf([text], [("app_main", 1), ("evq_hil_io_fopen_w", 1)]))
        wrapped = build_elf([text], [("writer", 1), ("evq_hil_io_fopen_w", SHN_UNDEF)], e_type=1)
        bare = build_elf([text], [("writer", 1), ("fopen", SHN_UNDEF)], e_type=1)
        good = os.path.join(td, "good")
        w("good/esp-idf/sd_logger/CMakeFiles/x.dir/sd_logger.c.obj", wrapped)
        w("good/esp-idf/ambit_ota/CMakeFiles/x.dir/ambit_stage.c.obj", wrapped)
        w("good/esp-idf/ambit_flash/CMakeFiles/x.dir/ambit_flash_preflight.c.obj", wrapped)
        bad = os.path.join(td, "bad")
        w("bad/sd_logger.c.obj", wrapped)
        w("bad/ambit_stage.c.obj", bare)
        w("bad/ambit_flash_preflight.c.obj", wrapped)
        partial = os.path.join(td, "partial")
        w("partial/sd_logger.c.obj", wrapped)
        w("partial/ambit_stage.c.obj", wrapped)

        expect("expect-wrapped all wrapped", check_wrapped(hil_elf, good), True)
        expect("expect-wrapped one object unwrapped", check_wrapped(hil_elf, bad), False)
        expect("expect-wrapped one object missing", check_wrapped(hil_elf, partial), False)
        expect("expect-wrapped ELF without evq_hil", check_wrapped(clean_elf, good), False)
        expect("expect-wrapped ELF without Sprint 2 strings", check_wrapped(hil_elf_bare, good), False)
        for needle in REQUIRED_HIL_STRINGS:
            rod = (".flash.rodata", 1, 0x2, b"\0".join(x for x in REQUIRED_HIL_STRINGS if x != needle) + b"\0")
            tag = needle.decode().replace("=", "eq")
            e_ = w(f"hil-no-{tag}.elf", build_elf([text, rod], [("app_main", 1), ("evq_hil_io_fopen_w", 1)]))
            expect(f"expect-wrapped missing {needle.decode()!r}", check_wrapped(e_, good), False)
    return out


# --------------------------------------------------------------------------- cli

def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="check_release_no_hil.py",
        description="Default: fail if the release ELF/bin/effective sdkconfig.json carry the "
                    "bench-only HIL fault command (C-37). --expect-wrapped: pass only if the "
                    "evq-hil image routes the new SD writers through evq_hil_io_* (C-36). "
                    "--self-test: prove the checks catch polluted fixtures.",
        allow_abbrev=False)
    mode = p.add_mutually_exclusive_group()
    mode.add_argument("--expect-wrapped", action="store_true",
                      help="check the evq-hil ELF + object dir (needs --elf, --objdir)")
    mode.add_argument("--self-test", action="store_true", help="run built-in fixture self-test")
    p.add_argument("--elf")
    p.add_argument("--bin")
    p.add_argument("--sdkconfig-json", help="effective generated config (build/config/sdkconfig.json)")
    p.add_argument("--objdir", help="(--expect-wrapped) build directory to search for objects")
    a = p.parse_args(argv)

    if a.self_test:
        if any(v is not None for v in (a.elf, a.bin, a.sdkconfig_json, a.objdir)):
            p.error("--self-test takes no other arguments")
        results = self_test()
    elif a.expect_wrapped:
        if a.elf is None or a.objdir is None:
            p.error("--expect-wrapped requires --elf and --objdir")
        if a.bin is not None or a.sdkconfig_json is not None:
            p.error("--expect-wrapped takes only --elf and --objdir")
        results = check_wrapped(a.elf, a.objdir)
    else:
        absent = [k for k, v in (("--elf", a.elf), ("--bin", a.bin),
                                 ("--sdkconfig-json", a.sdkconfig_json)) if v is None]
        if absent:
            p.error("release check requires " + ", ".join(absent))
        if a.objdir is not None:
            p.error("--objdir is only valid with --expect-wrapped")
        results = check_release(a.elf, a.bin, a.sdkconfig_json)

    ok = all(r for r, _ in results)
    for r, msg in results:
        print(f"{'PASS' if r else 'FAIL'} {msg}")
    print(("SELF-TEST " if a.self_test else "CHECK ") + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
