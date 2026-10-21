#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Record linked hooks and direct/literal-indirect wrapped callers.

Run against the final firmware. This catches a weak/internal SDK reference
silently bypassing --wrap; it does not replace the device transcript or host
traffic tests. Addresses and caller lists are evidence for the pinned S3 image.
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess

WRAPPED = (
    "usb_serial_jtag_driver_install", "xRingbufferCreate", "xRingbufferSendFromISR",
    "xRingbufferReceiveUpTo", "vRingbufferReturnItem", "usb_serial_jtag_write_bytes",
)


def tool(name):
    candidate = shutil.which("xtensa-esp32s3-elf-" + name)
    if candidate:
        return candidate
    candidate = Path.home() / ".platformio/packages/toolchain-xtensa-esp-elf/bin" / ("xtensa-esp32s3-elf-" + name)
    if not candidate.exists():
        raise SystemExit(f"Target tool unavailable: {candidate}")
    return str(candidate)


def qualify(elf):
    listing = subprocess.check_output([tool("nm"), "-S", str(elf)], text=True)
    symbols = {}
    for line in listing.splitlines():
        match = re.fullmatch(r"([0-9a-f]+) ([0-9a-f]+) (\w) (.+)", line)
        if match:
            address, size, kind, name = match.groups()
            symbols[name] = {"address": address, "size": int(size, 16), "kind": kind}
    disassembly = subprocess.check_output([tool("objdump"), "-d", str(elf)], text=True)
    names = ["__wrap_" + name for name in WRAPPED] + ["linenoiseSetReadCharacteristics"]
    callers = {name: set() for name in names}
    current = None
    indirect = None
    for line in disassembly.splitlines():
        label = re.fullmatch(r"[0-9a-f]+ <(.+)>:", line)
        if label:
            current = label.group(1)
            indirect = None
        call = re.search(r"\bcall\w*\s+[0-9a-f]+ <([^>]+)>", line)
        if call and call.group(1) in callers:
            callers[call.group(1)].add(current)
        literal = re.search(r"\bl32r\s+(a\d+),.*\([0-9a-f]+ <([^>]+)>\)", line)
        if literal:
            indirect = (literal.group(1), literal.group(2))
        register_call = re.search(r"\bcallx\w*\s+(a\d+)\s*$", line)
        if register_call:
            if indirect and register_call.group(1) == indirect[0] and indirect[1] in callers:
                callers[indirect[1]].add(current)
            indirect = None
    # Xtensa mapping symbols can make objdump render a whole live function as
    # words rather than instructions. Cross-reference the final link map and
    # compiler relocation records as well; do not treat missing decoding as a
    # missing hook or silently assume the weak reference was intercepted.
    map_path = elf.parent / "ambyte-iot.map"
    if not map_path.exists():
        raise SystemExit(f"Link map unavailable: {map_path}")
    object_members = {name: set() for name in names}
    active = None
    for line in map_path.read_text().split("Cross Reference Table", 1)[-1].splitlines():
        if line and not line[0].isspace():
            active = line.split()[0]
        if active in object_members:
            member = re.search(r"\.a\(([^)]+)\)", line)
            if member and member.group(1) != "svc_usb.c.o":
                object_members[active].add(member.group(1))
    for member in set().union(*object_members.values()):
        matches = list(elf.parent.rglob(member))
        if len(matches) != 1:
            raise SystemExit(f"Cannot resolve linked object {member}: {matches}")
        relocations = subprocess.check_output([tool("objdump"), "-r", str(matches[0])], text=True)
        caller = None
        for line in relocations.splitlines():
            section = re.match(r"RELOCATION RECORDS FOR \[\.(literal|text)\.([^]]+)\]", line)
            if line.startswith("RELOCATION RECORDS"):
                caller = section.group(2) if section else None
            relocation = re.match(r"[0-9a-f]+\s+R_XTENSA_\w+\s+(\w+)\s*$", line)
            if caller in symbols and relocation:
                name = relocation.group(1)
                wrapped = "__wrap_" + name if name in WRAPPED else name
                if wrapped in callers and member in object_members[wrapped]:
                    callers[wrapped].add(caller)
    result = {}
    for name in names:
        if name not in symbols or not callers[name]:
            raise SystemExit(f"Hook missing or bypassed in linked firmware: {name}")
        result[name] = {**symbols[name], "callers": sorted(callers[name]),
                        "linked_caller_objects": sorted(object_members[name])}
    isr = result["__wrap_xRingbufferSendFromISR"]
    if not isr["address"].startswith("403"):
        raise SystemExit("USB ISR wrapper is not in ESP32-S3 internal executable RAM")
    if result["linenoiseSetReadCharacteristics"]["kind"] != "T" or "editor_read" not in symbols:
        raise SystemExit("Strong linenoise customization and editor filter are not linked")
    expected = {
        "__wrap_usb_serial_jtag_driver_install": "esp_console_new_repl_usb_serial_jtag",
        "__wrap_xRingbufferCreate": "usb_serial_jtag_driver_install",
        "__wrap_xRingbufferSendFromISR": "usb_serial_jtag_isr_handler_default",
        "__wrap_xRingbufferReceiveUpTo": "usb_serial_jtag_read_bytes",
        "__wrap_vRingbufferReturnItem": "usb_serial_jtag_read_bytes",
        "__wrap_usb_serial_jtag_write_bytes": "usbjtag_tx_char_via_driver",
        "linenoiseSetReadCharacteristics": "linenoiseProbe",
    }
    for name, caller in expected.items():
        if caller not in result[name]["callers"]:
            raise SystemExit(f"Pinned SDK path bypassed {name}: missing {caller}")
    return {"elf": str(elf), "hooks": result}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    rendered = json.dumps(qualify(args.elf), indent=2) + "\n"
    if args.output:
        args.output.write_text(rendered)
        print(f"Qualified six wrap symbols and strong linenoise hook: {args.output}")
    else:
        print(rendered, end="")


if __name__ == "__main__":
    main()
