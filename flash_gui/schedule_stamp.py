# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Stamp workbook provenance into an Ambyte schedule's YAML header.

Design rule from the workbook-loop plan: stamp at install, not at authoring.
The workbook's command cell carries a GENERIC schedule (no ids); whatever
installs it onto a device stamps the pinned ``workbookVersionId`` and the
version's macro list into the header so the firmware can publish them with
every row — the platform's gold pipeline only runs a workbook's macros for
rows carrying them.

The module is deliberately text-based: a YAML library round-trip would drop
the authoring comments the released schedules are full of. Everything outside
the stamped keys is preserved byte-for-byte, and stamping is idempotent — a
re-stamp replaces the previously stamped keys at the same position.

Block style only. The device parser (components/sched_spec) rejects flow
``{}``/``[]`` collections anywhere in the document, so the macro list is
emitted as an indented block sequence, never as ``macros: [...]``.

The ``macros:`` key is gated on the target firmware: a compiler predating the
header change (stream A of the plan) rejects the unknown key and the runner
silently falls back to the embedded default (sched_runner.c), so callers stamp
``workbookVersionId`` always and ``macros:`` only when the flashed firmware is
new enough — see MACROS_HEADER_MIN_FW and firmware_supports_macros().

Validation can run the result through the same compiler sources the device
builds (tools/sched_host.c over components/sched_spec). It is OPTIONAL: when
the source tree or a C compiler is unavailable (the packaged GUI ships
neither) the check skips cleanly, and the device's own compile-before-swap in
script_update remains the hard gate on the actual install. One tolerated gap:
a host compiler from a checkout without stream A rejects exactly
``unknown top-level key 'macros'``; that error alone is accepted after the
macros-stripped remainder compiles cleanly. ``workbookVersionId:`` is accepted
by every compiler that has the document header at all and must always
validate.
"""

from __future__ import annotations

import hashlib
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

SCHEMA_PREFIX = "jii.ambyte-schedule/"

# First firmware release whose sched_spec compiler accepts the `macros:`
# header key (stream A of the workbook-loop plan, PR #53, released as
# v2.1.0). workbookVersionId needs no gate: every compiler with the document
# header already accepts it.
MACROS_HEADER_MIN_FW = "2.1.0"

# The macro contract the stream-A device compiler enforces (verified against
# its sched_host build in review D): uuid-shaped ids, names/filenames of
# [A-Za-z0-9_.:-] up to 47 chars, at most 8 entries. Validated host-side so a
# bad workbook fails at install time with a clear message instead of an
# on-device compile rejection after the push.
MAX_MACROS = 8
MACRO_NAME_RE = re.compile(r"^[A-Za-z0-9_.:-]{1,47}$")
UUID_RE = re.compile(
    r"^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
    r"[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$")

# Top-level keys the stamping owns and therefore replaces on a re-stamp.
_STAMPED_KEYS = ("workbookVersionId", "macros")
# Insertion anchors, in preference order: after `description:` when the
# schedule has one, else straight after the `schema:` discriminator line.
_ANCHOR_KEYS = ("description", "schema")

_KEY_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*):")
_SCHEMA_RE = re.compile(r"(?m)^schema:[ \t]*([^#\s]+)")
# The compiler's wording for the one gap we tolerate (see module docstring).
_UNKNOWN_MACROS_RE = re.compile(r"unknown top-level key 'macros'")
# YAML plain scalars we may emit unquoted; anything else gets double-quoted so
# a value can never change type (workbookVersionId: 30m must stay a string) or
# break the line-based device parser.
_PLAIN_RE = re.compile(r"[A-Za-z0-9_.\-]+")
_PLAIN_RESERVED_WORDS = {"true", "false", "null", "yes", "no", "on", "off",
                         "~"}


class ScheduleStampError(RuntimeError):
    """The stamped schedule is not installable (no schema line, or the host
    compiler rejected it)."""


class MacroContractError(ValueError):
    """A macro entry (or the workbookVersionId) violates the device compiler's
    macro contract — caught before anything is stamped or pushed."""


def validate_macro_contract(workbook_version_id: str, macros) -> None:
    """Mirror the stream-A device compiler's header contract, host-side.

    Raises MacroContractError (a ValueError) naming the offending field.
    """
    if not UUID_RE.fullmatch(workbook_version_id or ""):
        raise MacroContractError(
            f"workbookVersionId {workbook_version_id!r} is not uuid-shaped")
    macros = tuple(macros or ())
    if len(macros) > MAX_MACROS:
        raise MacroContractError(
            f"macros: {len(macros)} entries exceed the device cap of "
            f"{MAX_MACROS}")
    for macro in macros:
        if not UUID_RE.fullmatch(macro.id or ""):
            raise MacroContractError(
                f"macro id {macro.id!r} is not uuid-shaped")
        for field_name, value in (("name", macro.name),
                                  ("filename", macro.filename)):
            if not MACRO_NAME_RE.fullmatch(value or ""):
                raise MacroContractError(
                    f"macro {field_name} {value!r} must match "
                    f"{MACRO_NAME_RE.pattern} (macro id {macro.id})")


def is_schedule_yaml(text: str) -> bool:
    """True when the text has a top-level `schema: jii.ambyte-schedule/…` line.

    This is the programming-cell test: a workbook command cell whose YAML is
    not an Ambyte schedule is left alone.
    """
    match = _SCHEMA_RE.search(text or "")
    if match is None:
        return False
    value = match.group(1).strip().strip('"').strip("'")
    return value.startswith(SCHEMA_PREFIX)


def parse_fw_version(text: str) -> tuple[int, int, int, str] | None:
    """'v2.1.0' → (2, 1, 0, ''); '2.1.0-rc1' → (2, 1, 0, 'rc1').

    The fourth element is the prerelease suffix ("" for a final release);
    None when the text is not semver-shaped at all.
    """
    match = re.match(r"^v?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?",
                     (text or "").strip())
    if match is None:
        return None
    return (int(match.group(1)), int(match.group(2)), int(match.group(3)),
            match.group(4) or "")


def firmware_supports_macros(fw_version: str) -> bool:
    """True when the flashed firmware's compiler accepts the macros: header.

    An unparseable version fails closed: silently dropping the macro list is
    exactly the failure this gate exists to prevent. Semver ordering applies:
    a prerelease of exactly the floor version (2.1.0-rc1) predates the final
    and does NOT pass.
    """
    parsed = parse_fw_version(fw_version)
    minimum = parse_fw_version(MACROS_HEADER_MIN_FW)
    if parsed is None:
        return False
    if parsed[:3] != minimum[:3]:
        return parsed[:3] > minimum[:3]
    return parsed[3] == ""


def _yaml_scalar(value: str) -> str:
    """A single-line YAML scalar: plain when provably safe, else quoted."""
    if value and "\n" not in value and "\r" not in value:
        if _PLAIN_RE.fullmatch(value) and not value[0].isdigit() \
                and value.casefold() not in _PLAIN_RESERVED_WORDS:
            return value
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _drop_key_block(lines: list[str], index: int, key: str) -> int:
    """Advance past a stamped key line; for `macros` also its block body.

    The body is the following INDENTED lines. Blank lines are consumed only
    when more block content follows, so the blank separator after the block —
    and any column-0 section comment below it — survives a re-stamp.
    """
    index += 1
    if key != "macros":
        return index
    last_content = None
    scan = index
    while scan < len(lines):
        line = lines[scan]
        if line.strip() and not line[0].isspace():
            break
        if line.strip():
            last_content = scan
        scan += 1
    return (last_content + 1) if last_content is not None else index


def _strip_stamped(lines: list[str]) -> list[str]:
    """Drop previously stamped top-level keys (workbookVersionId, macros)."""
    out: list[str] = []
    i = 0
    while i < len(lines):
        match = _KEY_RE.match(lines[i])
        if match is None or match.group(1) not in _STAMPED_KEYS:
            out.append(lines[i])
            i += 1
            continue
        i = _drop_key_block(lines, i, match.group(1))
    return out


def _without_macros(text: str) -> str:
    """The document with only the top-level `macros:` block removed."""
    lines = text.splitlines(keepends=True)
    out: list[str] = []
    i = 0
    while i < len(lines):
        match = _KEY_RE.match(lines[i])
        if match is not None and match.group(1) == "macros":
            i = _drop_key_block(lines, i, "macros")
            continue
        out.append(lines[i])
        i += 1
    return "".join(out)


def stamped_block(workbook_version_id: str, macros, newline: str = "\n"
                  ) -> list[str]:
    """The inserted header lines: block style, 2-space indent, never flow."""
    lines = [f"workbookVersionId: {_yaml_scalar(workbook_version_id)}{newline}"]
    if macros:
        lines.append(f"macros:{newline}")
        for macro in macros:
            lines.append(f"  - id: {_yaml_scalar(macro.id)}{newline}")
            lines.append(f"    name: {_yaml_scalar(macro.name)}{newline}")
            lines.append(
                f"    filename: {_yaml_scalar(macro.filename)}{newline}")
    return lines


def stamp_header(yaml_text: str, workbook_version_id: str, macros,
                 *, validate: bool = True, checker=None, log=None) -> str:
    """Insert/replace `workbookVersionId:` and the `macros:` block in the header.

    The stamp lands after `description:` when present, else after `schema:`;
    the rest of the document — comments included — is preserved byte-for-byte.
    An empty macro list omits the `macros:` key entirely (``macros: []`` would
    be flow style, which the device parser rejects).

    Raises ScheduleStampError when the text is not an Ambyte schedule or the
    host compiler rejects the result, and MacroContractError (a ValueError
    naming the offending field) when a macro entry or the workbookVersionId
    violates the device compiler's header contract. `checker(text) ->
    str | None` overrides the compiler in tests (returns the error text, None
    when it compiles).
    """
    if not isinstance(yaml_text, str) or not is_schedule_yaml(yaml_text):
        raise ScheduleStampError(
            "not an Ambyte schedule: no top-level "
            f"'schema: {SCHEMA_PREFIX}…' line")
    if not workbook_version_id or not workbook_version_id.strip():
        raise ScheduleStampError("workbook_version_id is required")
    macros = tuple(macros or ())
    validate_macro_contract(workbook_version_id.strip(), macros)
    newline = "\r\n" if "\r\n" in yaml_text.split("\n", 1)[0] else "\n"

    lines = _strip_stamped(yaml_text.splitlines(keepends=True))
    anchor = None
    for want in _ANCHOR_KEYS:
        for index, line in enumerate(lines):
            match = _KEY_RE.match(line)
            if match and match.group(1) == want:
                anchor = index
                break
        if anchor is not None:
            break
    if anchor is None:  # is_schedule_yaml passed, so schema exists — defensive
        raise ScheduleStampError("no header anchor (schema/description) found")

    lines[anchor + 1:anchor + 1] = stamped_block(
        workbook_version_id.strip(), macros, newline)
    stamped = "".join(lines)

    if validate:
        _validate(stamped, checker=checker, log=log)
    return stamped


# ── host-compiler validation (optional; the device remains the hard gate) ────
def _validate(stamped: str, checker, log) -> None:
    if checker is None:
        checker = lambda text: _check_with_sched_host(text, log=log)
    error = checker(stamped)
    if error is None:
        return
    if _UNKNOWN_MACROS_RE.search(error):
        # A host compiler from a checkout without stream A does not know the
        # macros header key yet. Everything else must still compile, so
        # re-check with only that key removed.
        remainder = checker(_without_macros(stamped))
        if remainder is None:
            if log is not None:
                log("note: this checkout's compiler predates the 'macros' "
                    "header key; the rest of the stamped schedule compiles. "
                    "Install it only onto firmware carrying the macros header.")
            return
        raise ScheduleStampError(
            "stamped schedule fails to compile even without 'macros': "
            f"{remainder}")
    raise ScheduleStampError(f"stamped schedule fails to compile: {error}")


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def _sched_host_binary(log=None) -> Path | None:
    """Build (once per source state) the sched_host CLI from device sources.

    Returns None when the source tree or a C compiler is unavailable — the
    packaged GUI ships neither; on-device compile validation in script_update
    remains the gate on the actual install there.
    """
    root = _repo_root()
    main = root / "tools" / "sched_host.c"
    component_dir = root / "components" / "sched_spec"
    includes = [component_dir / "include",
                root / "components" / "time_sync" / "include",
                root / "tests" / "host_stubs"]
    sources = ([main] + sorted(component_dir.glob("*.c"))
               + [root / "components" / "time_sync" / "time_sync.c"])
    if any(not s.is_file() for s in sources) \
            or any(not i.is_dir() for i in includes):
        return None
    compiler = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
    if compiler is None:
        return None

    fingerprint = hashlib.sha1()
    for path in sources:
        stat = path.stat()
        fingerprint.update(
            f"{path.name}:{stat.st_mtime_ns}:{stat.st_size}".encode())
    out_dir = Path(tempfile.gettempdir()) / "ambyte-sched-host"
    binary = out_dir / f"sched_host-{fingerprint.hexdigest()[:16]}"
    if binary.is_file():
        return binary
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [compiler, "-std=c11", "-O1",
           *(f"-I{inc}" for inc in includes),
           *(str(s) for s in sources), "-lm", "-o", str(binary)]
    try:
        subprocess.run(cmd, check=True, cwd=root, capture_output=True,
                       timeout=120)
    except (subprocess.SubprocessError, OSError) as exc:
        if log is not None:
            log(f"note: cannot build sched_host for validation ({exc}); "
                "skipping the host compile check.")
        return None
    return binary


def _check_with_sched_host(text: str, log=None) -> str | None:
    """Compile-check one schedule text; returns the error text or None.

    None ALSO means "could not check" (no toolchain/source tree): validation
    is best-effort by design — the device compiles before swap regardless.
    """
    binary = _sched_host_binary(log=log)
    if binary is None:
        return None
    with tempfile.NamedTemporaryFile(
            "w", suffix=".yaml", delete=False, encoding="utf-8") as handle:
        handle.write(text)
        path = handle.name
    try:
        result = subprocess.run([str(binary), "--check", path],
                                capture_output=True, text=True, timeout=30)
    except (subprocess.SubprocessError, OSError) as exc:
        return f"sched_host failed to run: {exc}"
    finally:
        Path(path).unlink(missing_ok=True)
    if result.returncode == 0:
        return None
    return (result.stderr or result.stdout or "compile failed").strip()
