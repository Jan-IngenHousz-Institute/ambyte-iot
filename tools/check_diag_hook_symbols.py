#!/usr/bin/env python3
"""Prove the error-log capture hook cannot reach the heap or a blocking lock.

components/diag/diag_log_hook.c runs on whatever task logged an error, including
the esp-mqtt task and holders of event_log's s_mtx (repo CLAUDE.md task
boundaries). It and its formatter (diag_fmt.c) are compiled as their own
objects so this can be checked mechanically: every UNDEFINED symbol of those
objects must be on an allow-list (spinlock, esp_timer, task handle, string
primitives, libgcc arithmetic helpers, the shared ring state, the formatter,
the downstream-sink fallback) and none may match the deny-list (allocators,
the printf family, newlib reentrant `_r` routines such as _dtoa_r, FreeRTOS
queues/semaphores, event_log, libc time). An object that references nothing
forbidden cannot call it: a call needs a relocation against the symbol.

Works on the Xtensa objects of a firmware build (CI, after `pio run`) and on
host objects (tests/test_diag.py). Pure Python ELF reader, stdlib only.
"""
import argparse
import re
import struct
import sys
from pathlib import Path

ALLOWED = {
    # ring state, formatter, downstream fallback
    "g_dl", "g_diag_log_mux", "diag_vsnformat", "diag_log_fallback_sink",
    # FreeRTOS critical section + task identity + ISR probe (never block)
    "vPortEnterCritical", "vPortExitCritical", "xPortEnterCriticalTimeout", "xPortInIsrContext",
    "xTaskGetCurrentTaskHandle",
    # lock-free monotonic clock
    "esp_timer_get_time",
    # string / memory primitives (no allocation, no locks)
    "memcpy", "memmove", "memset", "memcmp", "strcmp", "strncmp", "strlen", "strnlen", "strncpy", "strstr",
    # stack protector
    "__stack_chk_fail", "__stack_chk_guard",
    # host port (tests/diag_host): pthread mutex stands in for the spinlock
    "diag_port_lock", "diag_port_unlock", "diag_port_uptime_us", "diag_port_in_isr", "diag_port_task",
}
# libgcc integer / soft-float helpers: __divdf3, __udivdi3, __fixdfsi,
# __fixunsdfdi, __floatsidf, __floatundidf, ... (pure arithmetic, no state)
LIBGCC = re.compile(r"^__[a-z]+(?:[0-9]|[sdt]f[sdt]i|[sdt]i[sdt]f)$")
DENIED = re.compile(
    r"malloc|calloc|realloc|(^|_)free$|printf|scanf|_dtoa|Balloc|_r$|Semaphore|Queue|event_log|"
    r"_lock_|^time$|gettimeofday|localtime|strftime|vTaskDelay|esp_log")


def undefined_symbols(path):
    data = Path(path).read_bytes()
    if data[:4] != b"\x7fELF" or data[5] != 1:
        raise ValueError(f"{path}: not a little-endian ELF object")
    is64 = data[4] == 2
    if is64:
        (_, _, _, _, _, _, shoff, _, _, _, _, shentsize, shnum, _) = struct.unpack_from("<16sHHIQQQIHHHHHH", data)
        sh_fmt = "<IIQQQQIIQQ"
    else:
        (_, _, _, _, _, _, shoff, _, _, _, _, shentsize, shnum, _) = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
        sh_fmt = "<IIIIIIIIII"
    sections = [struct.unpack_from(sh_fmt, data, shoff + i * shentsize) for i in range(shnum)]
    found = set()
    for sec in sections:
        sh_type, offset, size, link, entsize = sec[1], sec[4], sec[5], sec[6], sec[9]
        if sh_type != 2:                       # SHT_SYMTAB
            continue
        strtab = sections[link]
        str_off = strtab[4]
        for i in range(1, size // entsize):
            if is64:
                name, info, _, shndx, _, _ = struct.unpack_from("<IBBHQQ", data, offset + i * entsize)
            else:
                name, _, _, info, _, shndx = struct.unpack_from("<IIIBBH", data, offset + i * entsize)
            bind = info >> 4
            if shndx != 0 or bind not in (1, 2) or name == 0:   # undefined GLOBAL/WEAK
                continue
            end = data.index(b"\0", str_off + name)
            found.add(data[str_off + name:end].decode())
    return found


def violations(symbols):
    bad = []
    for sym in sorted(symbols):
        if DENIED.search(sym):
            bad.append(f"{sym}: forbidden in the logging-task hook")
        elif sym not in ALLOWED and not LIBGCC.match(sym):
            bad.append(f"{sym}: not on the hook allow-list (review it, then add it)")
    return bad


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("objects", type=Path, nargs="+")
    args = ap.parse_args(argv)
    failures = []
    for obj in args.objects:
        failures += [f"{obj}: {v}" for v in violations(undefined_symbols(obj))]
    if failures:
        print("DIAG HOOK CHECK FAIL\n" + "\n".join(failures))
        return 1
    print(f"DIAG HOOK CHECK PASS: {len(args.objects)} object(s) reference no allocator, printf or blocking lock")
    return 0


if __name__ == "__main__":
    sys.exit(main())
