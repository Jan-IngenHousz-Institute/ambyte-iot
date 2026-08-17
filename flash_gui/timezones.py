# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Host timezone detection + the firmware's supported-zone table.

The device carries no tzdata: components/timezone/timezone.c maps a FIXED list
of IANA names to POSIX-TZ rules. `cfg set timezone` accepts anything (no
validation in the write path) and an unknown name only degrades at boot to a
fixed fallback offset — so the host must do the validating, and warn when the
PC's zone is one the firmware cannot schedule DST for.
"""

from __future__ import annotations

import datetime

# Mirrors k_zones in components/timezone/timezone.c (this firmware tree).
FIRMWARE_SUPPORTED_ZONES = frozenset({
    # CET
    "Europe/Amsterdam", "Europe/Brussels", "Europe/Paris", "Europe/Berlin",
    "Europe/Madrid", "Europe/Rome", "Europe/Vienna", "Europe/Zurich",
    "Europe/Copenhagen", "Europe/Stockholm", "Europe/Oslo", "Europe/Prague",
    "Europe/Warsaw", "Europe/Budapest",
    # WET / GMT
    "Europe/London", "Europe/Dublin", "Europe/Lisbon",
    # EET
    "Europe/Helsinki", "Europe/Athens", "Europe/Bucharest",
    # UTC
    "UTC", "Etc/UTC",
})


def local_iana_zone() -> str:
    """The PC's IANA timezone name.

    tzlocal is the only reliable cross-platform way to get an IANA name (on
    Windows the OS speaks registry names, not IANA). Falls back to UTC with the
    caller expected to surface the degradation.
    """
    try:
        import tzlocal
        name = str(tzlocal.get_localzone_name() or "").strip()
        if name:
            return name
    except Exception:
        pass
    # POSIX fallback: /etc/timezone or a TZ env var that looks IANA-ish.
    import os
    tz = os.environ.get("TZ", "")
    if "/" in tz:
        return tz
    try:
        text = open("/etc/timezone", encoding="utf-8").read().strip()
        if "/" in text:
            return text
    except OSError:
        pass
    return "UTC"


def firmware_supports(zone: str) -> bool:
    return zone in FIRMWARE_SUPPORTED_ZONES


def utc_offset_label(now: datetime.datetime | None = None) -> str:
    """e.g. 'UTC+02:00' for the local clock right now."""
    now = now or datetime.datetime.now().astimezone()
    off = now.utcoffset() or datetime.timedelta(0)
    total = int(off.total_seconds())
    sign = "+" if total >= 0 else "-"
    total = abs(total)
    return f"UTC{sign}{total // 3600:02d}:{(total % 3600) // 60:02d}"
