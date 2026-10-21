#!/usr/bin/env python3
"""Reject FPU instructions in the target snapshot formatter (task-affinity safety)."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess


def fpu_instructions(disassembly):
    found = []
    for line in disassembly.splitlines():
        match = re.match(r"\s*[0-9a-f]+:\s+(?:[0-9a-f]+\s+)?([a-z0-9_.]+)\s*(.*)", line)
        if match and ((match[1].endswith(".s") or match[1] in {
                "lsi", "lsip", "lsiu", "lsx", "lsxp", "ssi", "ssip", "ssiu", "ssx", "ssxp", "rfr", "wfr"}) and
                re.search(r"\bf(?:[0-9]|1[0-5])\b", match[2]) or
                match[1] in {"rur.fcr", "wur.fcr", "rur.fsr", "wur.fsr"}):
            found.append(line.strip())
    return found


def check_object(path, objdump=None):
    if objdump is None:
        objdump = shutil.which("xtensa-esp32s3-elf-objdump")
    if objdump is None:
        candidates = sorted((Path.home() / ".platformio/packages").glob(
            "toolchain-xtensa-esp-elf*/bin/xtensa-esp32s3-elf-objdump"))
        if candidates:
            objdump = str(candidates[-1])
    if objdump is None:
        raise RuntimeError("Xtensa objdump is required for the target FPU gate")
    result = subprocess.run([objdump, "-d", str(path)], check=True, capture_output=True, text=True)
    return fpu_instructions(result.stdout)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("object", type=Path)
    parser.add_argument("--objdump")
    args = parser.parse_args()
    found = check_object(args.object, args.objdump)
    if found:
        raise SystemExit("FPU CHECK FAIL: snapshot formatting can pin its caller\n" + "\n".join(found))
    print("FPU CHECK PASS: snapshot formatter uses no FPU instructions")
