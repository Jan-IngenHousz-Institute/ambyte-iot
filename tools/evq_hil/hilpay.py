"""Host twin of components/evq_hil/evq_hil_payload.c (contract §1.4, PAY-1).

payload(run, k, pad_len) reproduces the device payload byte for byte, and
stored_line() reproduces the exact event_log line a synthetic fill writes, so
every EVQ_ACC hash can be recomputed from (run, k, pad_len, id, start, end).
"""
from __future__ import annotations

import hashlib
import re

_SET = "abcdefghijklmnopqrstuvwxyz0123456789"
_M64 = (1 << 64) - 1
RUN_RE = re.compile(r"^[A-Za-z0-9._-]+$")


def fnv1a64(s: str) -> int:
    h = 1469598103934665603
    for b in s.encode():
        h ^= b
        h = (h * 1099511628211) & _M64
    return h


def payload(run: str, k: int, pad_len: int) -> str:
    if not RUN_RE.match(run):
        raise ValueError(f"bad run id {run!r}")
    x = fnv1a64(run) ^ ((k * 0x9E3779B97F4A7C15) & _M64)
    if x == 0:
        x = 1
    out = []
    for _ in range(pad_len):
        x ^= (x << 13) & _M64
        x ^= x >> 7
        x ^= (x << 17) & _M64
        out.append(_SET[x % 36])
    return '{"evq_hil":"%s","k":%u,"pad":"%s"}' % (run, k, "".join(out))


def stored_line(measure_id: int, run: str, start_ms: int, end_ms: int, pl: str) -> bytes:
    """event_log_store_impl line: id, channel, device, tag, cmd_raw, start,
    end, metadata, payload - tab separated, newline terminated."""
    return (f"{measure_id}\t\tevq_hil\tMEASUREMENT\tevq_hil {run}\t{start_ms}\t{end_ms}\t\t{pl}\n").encode()


def sha(b: bytes | str) -> str:
    return hashlib.sha256(b.encode() if isinstance(b, str) else b).hexdigest()
