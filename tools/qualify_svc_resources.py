#!/usr/bin/env python3
"""Build-time A3 F6 evidence; runtime heap/stack/flash allocation remain bench gates."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from elf_budget import metrics
from qualify_svc_interposition import tool


def qualify(build, *, static_ram_baseline=107204):
    elf, binary = build / "firmware.elf", build / "firmware.bin"
    measured = metrics(str(elf))
    size = binary.stat().st_size
    if size + 8192 >= 2764800:
        raise ValueError("A3 image plus conservative signing envelope exceeds F601")
    # F601 baseline includes both DRAM and IRAM data in PlatformIO RAM. The
    # ELF's loadable section metric is reported separately, never conflated.
    listing = subprocess.check_output([tool("size"), "-A", str(elf)], text=True)
    sections = {m[0]: int(m[1]) for m in re.findall(r"^(\.[^\s]+)\s+(\d+)\s+\d+\s*$", listing, re.M)}
    static_ram = sum(n for name, n in sections.items() if name.startswith((".dram0.data", ".dram0.bss", ".iram0.data", ".iram0.bss")))
    if static_ram - static_ram_baseline > 8192:
        raise ValueError("A3 static RAM delta exceeds F601")
    frames = []
    for source in sorted((build / "components/usb_service").glob("*.su")):
        for line in source.read_text().splitlines():
            location, amount, kind = line.split("\t")
            frames.append(dict(location=location, bytes=int(amount), kind=kind))
    required = ("svc_session_run", "svc_protocol_dispatch", "usb_service_command", "__wrap_xRingbufferSendFromISR")
    if any(not any(row["location"].endswith(":" + name) for row in frames) for name in required):
        raise ValueError("Missing target -fstack-usage records")
    symbols = subprocess.check_output([tool("nm"), "-S", str(elf)], text=True)
    if not re.search(r"^[0-9a-f]+ [0-9a-f]+ T sched_resource_sample$", symbols, re.M):
        raise ValueError("Resource observer is not strongly linked")
    return dict(build=str(build), sha256=hashlib.sha256(binary.read_bytes()).hexdigest(), unsigned_bytes=size,
                conservative_signing_envelope=8192, slot_budget=2764800, **measured,
                static_ram=static_ram, static_ram_delta=static_ram-static_ram_baseline, target_function_frames=frames,
                service_logical_workspace_max=21536, record_bytes=512, new_nvs_namespaces=0, new_nvs_keys=0,
                new_tasks=0, capture_storage_overhead_bytes=0,
                runtime="NOT YET EVALUATED: heap, largest block, task margins and physical littlefs allocation require bench",
                stack_note="Per-function compiler frames only; not call-chain or runtime task high-water margins",
                acquisition_jitter="not measured")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = json.dumps(qualify(args.build), indent=2) + "\n"
    if args.output:
        args.output.write_text(report)
        print(f"Build resource evidence: {args.output}")
    else:
        print(report, end="")


if __name__ == "__main__": main()
