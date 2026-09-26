#!/usr/bin/env python3
"""J3: production values of every host-overridable constant, and proof that
the host fault seam never reaches the firmware.

Parses the #ifndef defaults straight from the sources (the host harness only
ever changes them with -D), compares them with the documented production
values, checks the partition tables are byte-identical to b3f9b8a, and - if a
production build exists - that firmware.elf contains no evq_fault_point symbol.
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

EXPECT = {
    ("components/event_log/include/event_log.h", "EVSTORE_MOUNT"): '"/evstore"',
    ("components/event_log/include/event_log.h", "EVSTORE_PARTITION"): '"storage"',
    ("components/event_log/include/event_log.h", "EVLOG_LINE_CAP_NORMAL"): "65568U",
    ("components/event_log/include/event_log.h", "EVLOG_LINE_CAP_FALLBACK"): "12288U",
    ("components/event_log/include/event_log.h", "EVLOG_RECORD_GUARD_BYTES"): "16U",
    ("components/sd_card/sd_card.h", "SD_MOUNT_POINT"): '"/sdcard"',
    ("components/event_log/event_log.c", "EVQ_SD_ROOT"): "SD_MOUNT_POINT",
    ("components/event_log/event_log.c", "EVLOG_ROTATE_BYTES"): "(256 * 1024)",
    ("components/event_log/event_log.c", "EVLOG_ARCHIVE_EVERY_N"): "1000",
    ("components/event_log/event_log.c", "EVLOG_MIN_FREE_BYTES"): "(256 * 1024)",
    ("components/event_log/event_log.c", "EVLOG_EVICT_TARGET"): "(512 * 1024)",
    ("components/event_log/event_log.c", "EVQ_PRESSURE_PCT"): "25",
    ("components/event_log/event_log.c", "EVQ_RECLAIM_PCT"): "40",
    ("components/event_log/event_log.c", "EVQ_SD_RESERVE_BYTES"): "(64ULL * 1024 * 1024)",
    ("components/event_log/event_log.c", "EVQ_INDEX_CAP"): "4096",
    ("components/event_log/event_log.c", "EVQ_INDEX_COMPACT_BYTES"): "(48 * 1024)",
    ("components/event_log/event_log.c", "EVQ_KEEPER_PERIOD_MS"): "60000",
    ("components/event_log/event_log.c", "EVQ_SD_BACKOFF_MIN_MS"): "60000",
    ("components/event_log/event_log.c", "EVQ_SD_BACKOFF_MAX_MS"): "(30 * 60000)",
    ("components/domain/include/persistence_port.h", "PUBLISH_WINDOW_SLOTS"): "16U",
    ("components/domain/include/persistence_port.h", "PUBLISH_WINDOW_BYTES"): "65536U",
}


def define_value(path: str, name: str) -> str | None:
    text = (ROOT / path).read_text()
    m = re.search(rf"^#define\s+{re.escape(name)}\s+(.+?)\s*(?:/\*.*)?$", text, re.M)
    return m.group(1).strip() if m else None


def main() -> int:
    bad = []
    for (path, name), want in EXPECT.items():
        got = define_value(path, name)
        status = "ok" if got == want else "MISMATCH"
        if got != want:
            bad.append(name)
        print(f"{status:8} {name:28} {got!s:24} (expected {want})  [{path}]")
    # the host seam must not be enabled anywhere in the firmware build
    for f in ["platformio.ini", "CMakeLists.txt", "sdkconfig.defaults", "main/CMakeLists.txt",
              "components/event_log/CMakeLists.txt"]:
        p = ROOT / f
        if p.exists() and "EVQ_HOST_FAULTS" in p.read_text():
            bad.append(f"EVQ_HOST_FAULTS in {f}")
            print(f"MISMATCH EVQ_HOST_FAULTS referenced by {f}")
    parts = subprocess.run(["git", "diff", "--quiet", "b3f9b8a", "--", "partitions*.csv", "*.csv"], cwd=ROOT)
    print(f"{'ok' if parts.returncode == 0 else 'MISMATCH':8} partition tables identical to b3f9b8a")
    if parts.returncode != 0:
        bad.append("partitions")
    elf = ROOT / ".pio/build/esp32-s3-devkitm-1/firmware.elf"
    if elf.exists():
        nm = next((c for c in [Path.home() / ".platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-nm"] if c.exists()), None)
        data = elf.read_bytes()
        has = b"evq_fault_point" in data
        print(f"{'ok' if not has else 'MISMATCH':8} firmware.elf has no evq_fault_point symbol")
        if has:
            bad.append("evq_fault_point in firmware")
        _ = nm
    else:
        print("skip     firmware.elf not built (run C6 first for the symbol check)")
    print("FAIL: " + ", ".join(bad) if bad else "all production constants as documented")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
