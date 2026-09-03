#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only
"""Generate the firmware's IANA -> POSIX-TZ table from the IANA tzdata.

WHY A GENERATOR AT ALL
----------------------
ESP-IDF ships no tzdata files, so components/timezone must carry its own
IANA-name -> POSIX-TZ-rule map (the POSIX rule embeds the DST transition dates,
which is all the on-device scheduler needs). That table used to be hand-written
and covered Europe only; a Bolivian on-boarding (`cfg set timezone
America/La_Paz` -> ESP_ERR_INVALID_ARG) is what finally forced the whole world
into it. Hand-maintaining ~600 zones is not an option, so we generate.

WHERE THE RULES COME FROM
-------------------------
Every TZif v2+ binary ends with a newline-delimited POSIX TZ string -- the
"footer" -- which upstream tzdata computes as the best POSIX approximation of
that zone's *current* rules. That is exactly the artefact we want, so we read it
straight out of the compiled zoneinfo instead of re-deriving anything from the
tzdata source. Consequence: the table describes the rules in force at
generation time. Zones whose governments change the rules (or that observe
rules POSIX cannot express -- see CLAMPING below) need a regeneration plus a
firmware release, exactly as the hand-written table did.

WHAT IS EMITTED
---------------
  components/timezone/tz_zone_table.inc   C tables (included by timezone.c)
  flash_gui/tz_zone_table.py              name set for the on-boarding GUI

Both are checked in so neither the firmware build nor a frozen flash_gui needs
Python tzdata at build time. tests/test_timezone_config.py fails if they drift
from each other or from a fresh run of this script.

USAGE
    python tools/gen_tz_table.py            # rewrite both artefacts
    python tools/gen_tz_table.py --check    # exit 1 if they are out of date
"""

from __future__ import annotations

import argparse
import collections
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
C_OUT = ROOT / "components/timezone/tz_zone_table.inc"
PY_OUT = ROOT / "flash_gui/tz_zone_table.py"

# NVS caps the stored timezone string at 47 chars and timezone.c canonicalizes
# through a char[48]; refuse to emit anything that could not round-trip.
MAX_NAME_LEN = 47


# -- tzdata access -----------------------------------------------------------
def _zone_names() -> list[str]:
    import zoneinfo
    return sorted(zoneinfo.available_timezones())


def _zone_bytes(name: str) -> bytes:
    """Raw TZif bytes for `name`, from pip tzdata or the system zoneinfo tree."""
    try:
        import importlib.resources as ir
        path = ir.files("tzdata") / "zoneinfo"
        for part in name.split("/"):
            path = path / part
        return path.read_bytes()
    except (ImportError, ModuleNotFoundError, FileNotFoundError, OSError):
        return (Path("/usr/share/zoneinfo") / name).read_bytes()


def _tzdata_version() -> str:
    try:
        import tzdata
        return str(getattr(tzdata, "IANA_VERSION", None) or tzdata.__version__)
    except Exception:
        return "unknown"


def _posix_footer(data: bytes) -> str:
    """The POSIX TZ string trailing a TZif v2+ file.

    The footer is always the final newline-delimited line, so we take it from
    the end rather than walking the two data blocks -- fewer ways to be wrong,
    and version-agnostic.
    """
    if not data.endswith(b"\n"):
        return ""
    start = data.rfind(b"\n", 0, len(data) - 1)
    if start < 0:
        return ""
    return data[start + 1:-1].decode("ascii", "strict")


# -- POSIX rule validation / clamping ----------------------------------------
# The conservative POSIX.1 subset newlib's tzset() actually parses: an
# abbreviation (3+ letters, or <> quoted so it may hold digits and a sign), a
# signed offset, and optionally a DST abbreviation plus two transition dates.
_ABBR = r"(?:[A-Za-z]{3,}|<[+-]?[A-Za-z0-9]+>)"
_OFF = r"[+-]?\d{1,3}(?::\d{1,2}(?::\d{1,2})?)?"
_DATE = r"(?:J\d{1,3}|\d{1,3}|M\d{1,2}\.[1-5]\.[0-6])"
_TIME = r"(?:/[+-]?\d{1,3}(?::\d{1,2}(?::\d{1,2})?)?)"
_RULE_RE = re.compile(
    rf"^(?P<std_abbr>{_ABBR})(?P<std_off>{_OFF})"
    rf"(?:{_ABBR}(?:{_OFF})?,{_DATE}{_TIME}?,{_DATE}{_TIME}?)?$"
)

# CLAMPING
# Transition times outside 00:00..24:00 are a TZif v3 extension; tzdata uses
# them for rules that fire before midnight or a day or two into the next date
# (Greenland's /-1, Jerusalem's /26, Gaza's /50). newlib scans that field with
# %hu, so a negative value aborts the parse mid-string and silently drops DST
# for the entire year. Clamping into the POSIX.1 range keeps such a zone
# DST-correct except within a day or so of its transition -- a much smaller
# error, and one that only ever moves the scheduler (stored and published
# timestamps are UTC regardless).
_TIME_FIELD_RE = re.compile(r"/(?P<sign>[+-]?)(?P<h>\d{1,3})(?P<rest>(?::\d{1,2}){0,2})")


def _clamp_times(rule: str) -> tuple[str, bool]:
    """Force every /time field into 00:00..24:00. Returns (rule, clamped?)."""
    changed = False

    def repl(m: re.Match[str]) -> str:
        nonlocal changed
        hours = int(m.group("h"))
        if m.group("sign") == "-":
            hours = -hours
        if 0 <= hours <= 24:
            return m.group(0)
        changed = True
        return "/0" if hours < 0 else "/24"

    return _TIME_FIELD_RE.sub(repl, rule), changed


def _std_offset_minutes(rule: str) -> int:
    """The rule's own standard-time offset, in minutes EAST of UTC.

    POSIX writes the offset with the opposite sign to everyone else ("CET-1" is
    UTC+1), so this flips it. Only a fallback for zones tzdata cannot be asked
    about directly -- see _fallback_offset_minutes.
    """
    m = _RULE_RE.match(rule)
    if m is None:
        raise ValueError(f"unparseable POSIX rule: {rule!r}")
    text = m.group("std_off")
    sign = -1 if text.startswith("-") else 1
    parts = text.lstrip("+-").split(":")
    hours = int(parts[0])
    minutes = int(parts[1]) if len(parts) > 1 else 0
    return -sign * (hours * 60 + minutes)


# Any year works; pinned so the generator's output is reproducible.
REFERENCE_YEAR = 2026


def _fallback_offset_minutes(name: str, rule: str) -> int:
    """The single fixed offset the firmware should schedule on if libc ever
    refuses this zone's rule.

    Taken from tzdata rather than from the rule's "std" field, because the POSIX
    approximation swaps std and DST for negative-DST zones: Africa/Casablanca is
    written as +00 standard with a "+01 DST" that covers everything except
    Ramadan, so the rule's std would be an hour off for most of the year.
    Sampling tzdata twice a month across a reference year and taking the most
    common offset gives the best single number for any modelling style.
    """
    try:
        import datetime
        import zoneinfo
        tz = zoneinfo.ZoneInfo(name)
    except Exception:
        return _std_offset_minutes(rule)
    counts: collections.Counter[int] = collections.Counter()
    for month in range(1, 13):
        for day in (1, 15):
            moment = datetime.datetime(REFERENCE_YEAR, month, day, 12,
                                       tzinfo=datetime.timezone.utc).astimezone(tz)
            offset = moment.utcoffset() or datetime.timedelta(0)
            counts[int(offset.total_seconds()) // 60] += 1
    # Sorted, not Counter.most_common, so a 50/50 zone is still deterministic.
    return sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))[0][0]


# -- table construction ------------------------------------------------------
class Zone:
    __slots__ = ("name", "rule", "std_min", "clamped")

    def __init__(self, name: str, rule: str, std_min: int, clamped: bool):
        self.name = name
        self.rule = rule
        self.std_min = std_min
        self.clamped = clamped


def build_zones() -> tuple[list[Zone], list[str], list[str]]:
    """Every accepted zone, the distinct rule pool, and skip warnings."""
    zones: list[Zone] = []
    warnings: list[str] = []
    for name in _zone_names():
        if not name.isascii() or len(name) > MAX_NAME_LEN:
            warnings.append(
                f"skipped {name!r}: non-ASCII or longer than {MAX_NAME_LEN} chars")
            continue
        rule = _posix_footer(_zone_bytes(name))
        if not rule:
            warnings.append(f"skipped {name!r}: no POSIX footer (TZif v1?)")
            continue
        rule, clamped = _clamp_times(rule)
        if _RULE_RE.match(rule) is None:
            warnings.append(
                f"skipped {name!r}: rule {rule!r} outside the supported POSIX subset")
            continue
        zones.append(Zone(name, rule, _fallback_offset_minutes(name, rule), clamped))
    # Sorted bytewise (ASCII-only, so Python's order equals strcmp order)
    # because timezone.c binary-searches this array.
    zones.sort(key=lambda z: z.name.encode("ascii"))
    rules = sorted({z.rule for z in zones})
    return zones, rules, warnings


# Repo convention since the licensing pass (#34): every host-side source file
# carries the SPDX pair, generated ones included. Firmware sources do not (see
# render_c) -- components/ is CERN-OHL-S v2, tools/ and flash_gui/ are GPL-3.0.
SPDX_LINES = (
    "SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute",
    "SPDX-License-Identifier: GPL-3.0-only",
)

BANNER_LINES = (
    "GENERATED FILE -- DO NOT EDIT BY HAND.",
    "Regenerate with `python tools/gen_tz_table.py` (that script documents why",
    "this table is generated and what the POSIX rules mean).",
)


def render_c(zones: list[Zone], rules: list[str], tzdata_version: str) -> str:
    index = {rule: i for i, rule in enumerate(rules)}
    clamped = [z.name for z in zones if z.clamped]
    # No SPDX header here on purpose: this file is firmware (components/ is
    # CERN-OHL-S v2, not the GPL-3.0 host-side tree) and no first-party firmware
    # source carries one. The Python artefact below does.
    lines = ["/*"]
    lines += [f" * {line}" for line in BANNER_LINES]
    lines += [
        " *",
        f" * Source: IANA tzdata {tzdata_version} (the POSIX TZ footer of each compiled TZif).",
        f" * {len(zones)} zone names -- canonical names plus their historical aliases, so a",
        ' * host reporting "Asia/Calcutta" or "US/Pacific" is accepted as-is -- sharing',
        f" * {len(rules)} distinct POSIX rules.",
        " *",
        " * Transition times were clamped into the POSIX.1 00:00..24:00 range for"
        f" {len(clamped)} zone(s):",
    ]
    lines += [f" *   {name}" for name in clamped]
    lines += [
        " * Those stay DST-correct except within a day or so of a transition; see",
        " * tools/gen_tz_table.py (CLAMPING) for why that beats letting newlib reject",
        " * the rule and drop DST for the whole year.",
        " */",
        "",
        f'#define TZ_TABLE_TZDATA_VERSION "{tzdata_version}"',
        f"#define TZ_TABLE_RULE_COUNT {len(rules)}",
        f"#define TZ_TABLE_ZONE_COUNT {len(zones)}",
        "",
        '/* Distinct POSIX TZ rules, handed to setenv("TZ", ...) verbatim. */',
        "static const char *const k_tz_rules[TZ_TABLE_RULE_COUNT] = {",
    ]
    lines += [f'    "{rule}",' for rule in rules]
    lines += [
        "};",
        "",
        "/* One entry per accepted IANA name, sorted bytewise for binary search.",
        " * fallback_min is the offset (minutes east of UTC) that zone spends most of",
        " * the year on -- the fixed offset timezone.c schedules on if libc ever",
        f" * refuses the rule. Sampled from tzdata across {REFERENCE_YEAR}. */",
        "typedef struct {",
        "    const char *iana;",
        "    uint16_t    rule;            /* index into k_tz_rules */",
        "    int16_t     fallback_min;",
        "} tz_zone_t;",
        "",
        "static const tz_zone_t k_tz_zones[TZ_TABLE_ZONE_COUNT] = {",
    ]
    width = max(len(z.name) for z in zones) + 3
    for z in zones:
        name = f'"{z.name}",'
        lines.append(f"    {{ {name:<{width}} {index[z.rule]:>3}, {z.std_min:>5} }},")
    lines += ["};", ""]
    return "\n".join(lines)


def render_py(zones: list[Zone], rules: list[str], tzdata_version: str) -> str:
    lines = [f"# {SPDX_LINES[0]}", f"# {SPDX_LINES[1]}", ""]
    lines += ['"""IANA zone names this firmware accepts (generated).', ""]
    lines += list(BANNER_LINES)
    lines += [
        "",
        f"Mirrors k_tz_zones in components/timezone/tz_zone_table.inc: {len(zones)} names",
        f"from IANA tzdata {tzdata_version} sharing {len(rules)} POSIX rules. Kept as its own",
        "module so the frozen flash_gui executable needs no tzdata at runtime.",
        '"""',
        "",
        "from __future__ import annotations",
        "",
        f'TZDATA_VERSION = "{tzdata_version}"',
        "",
        "ZONES = frozenset({",
    ]
    lines += [f'    "{z.name}",' for z in zones]
    lines += ["})", ""]
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="verify the checked-in artefacts are up to date")
    args = parser.parse_args(argv)

    zones, rules, warnings = build_zones()
    version = _tzdata_version()
    for warning in warnings:
        print(f"warning: {warning}", file=sys.stderr)
    if not zones:
        print("error: no zones found -- is the tzdata package installed?",
              file=sys.stderr)
        return 1

    artefacts = {
        C_OUT: render_c(zones, rules, version),
        PY_OUT: render_py(zones, rules, version),
    }
    stale = [p for p, text in artefacts.items()
             if not p.exists() or p.read_text(encoding="utf-8") != text]
    if args.check:
        for path in stale:
            print(f"out of date: {path.relative_to(ROOT)}", file=sys.stderr)
        return 1 if stale else 0
    for path, text in artefacts.items():
        path.write_text(text, encoding="utf-8")
    print(f"wrote {len(zones)} zones / {len(rules)} rules from tzdata {version}")
    for path in artefacts:
        print(f"  {path.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
