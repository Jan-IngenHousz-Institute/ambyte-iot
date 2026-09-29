"""Bench identity for the Sprint 2 hardware tools (replacement amendment A2).

Why there is no default: the original bench (28:37:2F:FF:E7:04) was replaced
mid-sprint by E8:F6:0A:B1:1F:34. A hard-coded board meant every tool would have
silently talked to - or reconciled warehouse rows of - the wrong device. The
identity is now REQUIRED and explicit: AMBYTE_BENCH_MAC (and, for warehouse
reconciliation, AMBYTE_BENCH_EXPERIMENT); every evidence JSON records it.
"""
from __future__ import annotations

import os
import re

MAC_RE = re.compile(r"^[0-9A-F]{2}(:[0-9A-F]{2}){5}$")
UUID_RE = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")


def mac() -> str:
    v = os.environ.get("AMBYTE_BENCH_MAC", "").strip().upper()
    if not MAC_RE.match(v):
        raise SystemExit("AMBYTE_BENCH_MAC must name the bench board (AA:BB:CC:DD:EE:FF); there is no default")
    return v


def port() -> str:
    return f"/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_{mac()}-if00"


def client() -> str:
    return f"ambyte_{mac().lower()}"


def experiment() -> str:
    v = os.environ.get("AMBYTE_BENCH_EXPERIMENT", "").strip().lower()
    if not UUID_RE.match(v):
        raise SystemExit("AMBYTE_BENCH_EXPERIMENT must name the bench experiment uuid; there is no default")
    return v


def device_id() -> str:
    """The envelope `device_id` the unit is CONFIGURED with (cfg), which need not be
    its MAC: E8:F6:0A:B1:1F:34 reports device_id 03:25:07:04 (replacement review 4)."""
    v = os.environ.get("AMBYTE_BENCH_DEVICE_ID", "").strip()
    if not v:
        raise SystemExit("AMBYTE_BENCH_DEVICE_ID must be the unit's configured envelope device_id; there is no default")
    return v


def device_name() -> str:
    v = os.environ.get("AMBYTE_BENCH_DEVICE_NAME", "").strip()
    if not v:
        raise SystemExit("AMBYTE_BENCH_DEVICE_NAME must be the unit's configured device_name; there is no default")
    return v


def identity() -> dict:
    return {"mac": mac(), "client": client(), "port": port()}
