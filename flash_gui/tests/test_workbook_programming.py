# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Workbook programming: resolving the pinned version's cells, and stamping
the schedule header with the workbook provenance the platform joins on.

HTTP is mocked throughout — no API keys, no network. The compile-validation
tests use an injectable checker; one integration test exercises the real
sched_host build when a C toolchain and the source tree are present.
"""

import hashlib
import shutil
from dataclasses import replace
from pathlib import Path

import pytest

from flash_gui import schedule_stamp
from flash_gui.openjii_client import (MacroCondition, OpenJIIClient,
                                      OpenJIIError, WorkbookMacro,
                                      WorkbookProgramming)
from flash_gui.schedule_stamp import ScheduleStampError

EXPERIMENT_ID = "665b6b18-3cfe-4d0a-85c7-3e84fa2f7834"
WORKBOOK_ID = "2ec3531a-d938-408b-ab77-6cd47ad86e59"
VERSION_ID = "1c7b82b5-0000-1111-2222-333333333333"
MACRO_ID = "47b03f78-1111-2222-3333-444455556666"

SCHEDULE_YAML = (
    "schema: jii.ambyte-schedule/v1-draft\n"
    "id: urn:jii:schedule:baseline\n"
    "version: 0.1.0\n"
    "name: baseline\n"
    "description: SS every minute.\n"
    "\n"
    "# A comment that must survive stamping.\n"
    "jobs:\n"
    "  health:\n"
    "    schedule:\n"
    "      - boot\n"
    "    steps:\n"
    "      - uses: device/status-report\n"
)

MACRO = WorkbookMacro(id=MACRO_ID, name="ambyte-trace",
                      filename="macro_8feac276a118")


def _client(responses):
    """An OpenJIIClient whose _request answers from a path → (status, body)."""
    client = OpenJIIClient.__new__(OpenJIIClient)
    client.env = None
    client.api_key = "test-key"
    calls = []

    def _request(method, path, body=None, timeout=60):
        calls.append((method, path))
        if path not in responses:
            return 404, {"message": f"no mock for {path}"}
        return responses[path]

    client._request = _request
    client.calls = calls
    return client


def _version_payload(cells):
    return {"id": VERSION_ID, "versionNumber": 3, "cells": cells}


def _command_cell(content, fmt="yaml", name=None):
    payload = {"format": fmt, "content": content}
    if name:
        payload["name"] = name
    return {"id": "cell-1", "type": "command", "payload": payload}


def _macro_cell(macro_id=MACRO_ID):
    return {"id": "cell-2", "type": "macro",
            "payload": {"macroId": macro_id, "language": "python"}}


# Branch-cell fixtures: the web app routes on branch paths
# ({conditions: [{sourceCellId, field, operator, value}], gotoCellId}); the
# command cell is "cell-1", the macro cell "cell-2" (see the helpers above).
COMMAND_CELL_ID = "cell-1"
MACRO_CELL_ID = "cell-2"

MACRO_PAYLOAD = {"id": MACRO_ID, "name": "ambyte-trace",
                 "filename": "macro_8feac276a118", "language": "python"}


def _branch_cell(paths, cell_id="cell-3"):
    return {"id": cell_id, "type": "branch", "payload": {"paths": paths}}


def _condition(field="schema", operator="eq", value="ambit.trace/3",
               source=COMMAND_CELL_ID):
    return {"sourceCellId": source, "field": field,
            "operator": operator, "value": value}


def _programming_client(cells, macro_payload=MACRO_PAYLOAD):
    """A client for one experiment pinning a version with the given cells."""
    responses = {
        f"/api/v1/experiments/{EXPERIMENT_ID}": (
            200, {"workbookId": WORKBOOK_ID, "workbookVersionId": VERSION_ID}),
        f"/api/v1/workbooks/{WORKBOOK_ID}/versions/{VERSION_ID}": (
            200, _version_payload(cells)),
    }
    if macro_payload is not None:
        responses[f"/api/v1/macros/{MACRO_ID}"] = (200, macro_payload)
    return _client(responses)


# ── resolve_programming ──────────────────────────────────────────────────────
def test_resolves_programming_cell_and_macro_identities():
    client = _client({
        f"/api/v1/experiments/{EXPERIMENT_ID}": (
            200, {"id": EXPERIMENT_ID, "workbookId": WORKBOOK_ID,
                  "workbookVersionId": VERSION_ID}),
        f"/api/v1/workbooks/{WORKBOOK_ID}/versions/{VERSION_ID}": (
            200, _version_payload([
                {"id": "cell-0", "type": "markdown", "payload": {}},
                _command_cell(SCHEDULE_YAML),
                _macro_cell(),
            ])),
        f"/api/v1/macros/{MACRO_ID}": (
            200, {"id": MACRO_ID, "name": "ambyte-trace",
                  "filename": "macro_8feac276a118", "language": "python"}),
    })

    prog = client.resolve_programming(EXPERIMENT_ID)

    assert isinstance(prog, WorkbookProgramming)
    assert prog.yaml_text == SCHEDULE_YAML
    assert prog.workbook_id == WORKBOOK_ID
    assert prog.workbook_version_id == VERSION_ID
    assert prog.workbook_version_number == 3
    assert prog.macros == (WorkbookMacro(id=MACRO_ID, name="ambyte-trace",
                                         filename="macro_8feac276a118"),)


def test_no_pinned_workbook_returns_none_without_further_requests():
    client = _client({
        f"/api/v1/experiments/{EXPERIMENT_ID}": (
            200, {"id": EXPERIMENT_ID, "workbookId": None,
                  "workbookVersionId": None}),
    })
    assert client.resolve_programming(EXPERIMENT_ID) is None
    assert len(client.calls) == 1


def test_pinned_version_without_a_schedule_cell_returns_none():
    for cell in (
        _command_cell(SCHEDULE_YAML, fmt="string"),          # not yaml
        _command_cell("schema: jii.other/v1\njobs: {}\n"),   # other schema
        {"id": "cell-9", "type": "markdown", "payload": {}},
    ):
        client = _client({
            f"/api/v1/experiments/{EXPERIMENT_ID}": (
                200, {"workbookId": WORKBOOK_ID,
                      "workbookVersionId": VERSION_ID}),
            f"/api/v1/workbooks/{WORKBOOK_ID}/versions/{VERSION_ID}": (
                200, _version_payload([cell])),
        })
        assert client.resolve_programming(EXPERIMENT_ID) is None, cell


def test_http_failures_raise_openjii_error():
    client = _client({
        f"/api/v1/experiments/{EXPERIMENT_ID}": (403, {"message": "nope"}),
    })
    with pytest.raises(OpenJIIError, match="403"):
        client.resolve_programming(EXPERIMENT_ID)

    client = _client({
        f"/api/v1/experiments/{EXPERIMENT_ID}": (
            200, {"workbookId": WORKBOOK_ID, "workbookVersionId": VERSION_ID}),
        f"/api/v1/workbooks/{WORKBOOK_ID}/versions/{VERSION_ID}": (
            404, {"message": "gone"}),
    })
    with pytest.raises(OpenJIIError, match="404"):
        client.resolve_programming(EXPERIMENT_ID)


def test_a_macro_without_filename_is_an_error_not_a_silent_drop():
    # The platform keys each macro table on the filename; stamping a wrong
    # one would misroute every row, so this is louder than skipping the macro.
    client = _client({
        f"/api/v1/experiments/{EXPERIMENT_ID}": (
            200, {"workbookId": WORKBOOK_ID, "workbookVersionId": VERSION_ID}),
        f"/api/v1/workbooks/{WORKBOOK_ID}/versions/{VERSION_ID}": (
            200, _version_payload([_command_cell(SCHEDULE_YAML),
                                   _macro_cell()])),
        f"/api/v1/macros/{MACRO_ID}": (200, {"id": MACRO_ID,
                                             "name": "ambyte-trace"}),
    })
    with pytest.raises(OpenJIIError, match="filename"):
        client.resolve_programming(EXPERIMENT_ID)


# ── branch cells → per-macro when: routing ───────────────────────────────────
def test_branch_path_compiles_to_a_when_block():
    client = _programming_client([
        _command_cell(SCHEDULE_YAML),
        _macro_cell(),
        _branch_cell([{"conditions": [_condition()],
                       "gotoCellId": MACRO_CELL_ID}]),
    ])
    prog = client.resolve_programming(EXPERIMENT_ID)
    assert prog.macros == (WorkbookMacro(
        id=MACRO_ID, name="ambyte-trace", filename="macro_8feac276a118",
        when=(MacroCondition(field="schema", op="eq", value="ambit.trace/3"),)),)


def test_branch_conditions_are_anded_in_order_including_dotted_fields():
    conditions = [
        _condition(),
        _condition(field="protocol.name", operator="neq", value="modbus"),
    ]
    client = _programming_client([
        _command_cell(SCHEDULE_YAML),
        _macro_cell(),
        _branch_cell([{"conditions": conditions, "gotoCellId": MACRO_CELL_ID}]),
    ])
    prog = client.resolve_programming(EXPERIMENT_ID)
    assert prog.macros[0].when == (
        MacroCondition(field="schema", op="eq", value="ambit.trace/3"),
        MacroCondition(field="protocol.name", op="neq", value="modbus"),
    )


def test_branch_routing_to_a_non_macro_cell_is_ignored():
    # Web-side control flow between other cells is none of the device's
    # business; only paths landing on a macro cell compile to when:.
    client = _programming_client([
        _command_cell(SCHEDULE_YAML),
        _macro_cell(),
        _branch_cell([{"conditions": [_condition()], "gotoCellId": "cell-9"}]),
    ])
    prog = client.resolve_programming(EXPERIMENT_ID)
    assert prog.macros[0].when == ()


def test_branch_condition_on_an_unknown_field_is_an_error():
    client = _programming_client([
        _command_cell(SCHEDULE_YAML),
        _macro_cell(),
        _branch_cell([{"conditions": [_condition(field="owner")],
                       "gotoCellId": MACRO_CELL_ID}]),
    ])
    with pytest.raises(OpenJIIError,
                       match="branch cell cell-3 path 0.*owner"):
        client.resolve_programming(EXPERIMENT_ID)


def test_branch_ordering_operator_is_an_error_not_a_silent_drop():
    client = _programming_client([
        _command_cell(SCHEDULE_YAML),
        _macro_cell(),
        _branch_cell([{"conditions": [_condition(operator="gt", value="2")],
                       "gotoCellId": MACRO_CELL_ID}]),
    ])
    with pytest.raises(OpenJIIError, match="'gt'"):
        client.resolve_programming(EXPERIMENT_ID)


def test_branch_condition_must_read_the_command_cells_output():
    client = _programming_client([
        _command_cell(SCHEDULE_YAML),
        _macro_cell(),
        _branch_cell([{"conditions": [_condition(source="cell-9")],
                       "gotoCellId": MACRO_CELL_ID}]),
    ])
    with pytest.raises(OpenJIIError, match="'cell-9'"):
        client.resolve_programming(EXPERIMENT_ID)


def test_two_paths_to_the_same_macro_are_an_error():
    # The device expresses ONE AND-ed condition set per macro; web-side OR
    # routing (two paths landing on the same macro) is not representable.
    path = {"conditions": [_condition()], "gotoCellId": MACRO_CELL_ID}
    client = _programming_client([
        _command_cell(SCHEDULE_YAML),
        _macro_cell(),
        _branch_cell([dict(path), dict(path)]),
    ])
    with pytest.raises(OpenJIIError, match="ambyte-trace"):
        client.resolve_programming(EXPERIMENT_ID)


def test_more_than_four_conditions_is_an_error():
    client = _programming_client([
        _command_cell(SCHEDULE_YAML),
        _macro_cell(),
        _branch_cell([{"conditions": [_condition(value=f"v{i}")
                                      for i in range(5)],
                       "gotoCellId": MACRO_CELL_ID}]),
    ])
    with pytest.raises(OpenJIIError, match="5 conditions"):
        client.resolve_programming(EXPERIMENT_ID)


def test_no_branch_cells_keeps_macros_unrouted_and_byte_compatible():
    client = _programming_client([_command_cell(SCHEDULE_YAML), _macro_cell()])
    prog = client.resolve_programming(EXPERIMENT_ID)
    assert prog.macros == (MACRO,)  # when defaults to (): always applies
    stamped = schedule_stamp.stamp_header(
        prog.yaml_text, prog.workbook_version_id, prog.macros, checker=OK)
    today = schedule_stamp.stamp_header(SCHEDULE_YAML, VERSION_ID, [MACRO],
                                        checker=OK)
    assert stamped == today
    assert "when:" not in stamped


def test_authored_macros_block_in_the_command_cell_is_an_error():
    # The replaced design let the authored YAML declare macros; routing now
    # comes from the workbook's branch cells, so the block is rejected.
    yaml_with_macros = SCHEDULE_YAML + "macros:\n  - id: old-style\n"
    client = _programming_client(
        [_command_cell(yaml_with_macros), _macro_cell()])
    with pytest.raises(OpenJIIError, match="remove the macros: block"):
        client.resolve_programming(EXPERIMENT_ID)


# ── stamp_header ─────────────────────────────────────────────────────────────
OK = lambda _text: None  # checker: compiles


def test_inserts_after_description_and_preserves_everything_else():
    stamped = schedule_stamp.stamp_header(SCHEDULE_YAML, VERSION_ID, [MACRO],
                                          checker=OK)
    lines = stamped.splitlines()
    assert lines[4].startswith("description:")
    assert lines[5] == f'workbookVersionId: "{VERSION_ID}"'
    assert lines[6] == "macros:"
    # The entire original document survives in order.
    original = [line for line in SCHEDULE_YAML.splitlines()]
    remaining = [line for line in lines
                 if not line.startswith(("workbookVersionId:", "macros:"))
                 and not line.startswith(("  - id:", "    name:",
                                          "    filename:"))]
    assert remaining == original


def test_anchors_after_schema_when_no_description():
    text = SCHEDULE_YAML.replace("description: SS every minute.\n", "")
    stamped = schedule_stamp.stamp_header(text, VERSION_ID, [], checker=OK)
    lines = stamped.splitlines()
    assert lines[0].startswith("schema:")
    assert lines[1] == f'workbookVersionId: "{VERSION_ID}"'
    assert "macros:" not in stamped  # empty list omits the key entirely


def test_block_style_only():
    stamped = schedule_stamp.stamp_header(SCHEDULE_YAML, VERSION_ID, [MACRO],
                                          checker=OK)
    block = stamped.split("macros:\n", 1)[1].split("\n\n", 1)[0]
    assert "[" not in block and "{" not in block and "]" not in block
    assert f"  - id: \"{MACRO_ID}\"" in block
    assert "    name: ambyte-trace" in block
    assert "    filename: macro_8feac276a118" in block


def test_idempotent_and_restamps_replace():
    once = schedule_stamp.stamp_header(SCHEDULE_YAML, VERSION_ID, [MACRO],
                                       checker=OK)
    twice = schedule_stamp.stamp_header(once, VERSION_ID, [MACRO], checker=OK)
    assert twice == once
    other = schedule_stamp.stamp_header(once, "ffffffff-ffff-ffff-ffff-ffffffffffff",
                                        [], checker=OK)
    assert VERSION_ID not in other
    assert "macros:" not in other
    # UUIDs with a letter first char are safe plain scalars; ones starting
    # with a digit get quoted so they can never be read back as a number.
    assert "workbookVersionId: ffffffff-ffff-ffff-ffff-ffffffffffff" in other


def test_comments_and_body_survive_byte_for_byte():
    stamped = schedule_stamp.stamp_header(SCHEDULE_YAML, VERSION_ID, [MACRO],
                                          checker=OK)
    assert "# A comment that must survive stamping." in stamped
    body = stamped.split("# A comment that must survive stamping.", 1)[1]
    original_body = SCHEDULE_YAML.split(
        "# A comment that must survive stamping.", 1)[1]
    assert body == original_body


def test_values_that_would_change_type_are_quoted():
    # Reserved-word values pass the macro contract but must not come back as
    # a YAML bool; stamp_header quotes them.
    macro = WorkbookMacro(id=MACRO_ID, name="true", filename="macro_x")
    stamped = schedule_stamp.stamp_header(SCHEDULE_YAML, VERSION_ID, [macro],
                                          checker=OK)
    assert 'name: "true"' in stamped
    # Values outside the contract never reach stamp_header (see the contract
    # tests); the emitter itself still quotes anything non-plain, e.g. spaces.
    block = "".join(schedule_stamp.stamped_block(
        VERSION_ID, [WorkbookMacro(id=MACRO_ID, name="ambyte trace",
                                   filename="macro_x")]))
    assert 'name: "ambyte trace"' in block


def test_macro_contract_is_enforced_before_stamping():
    def expect(field, macros=(MACRO,), version_id=VERSION_ID):
        with pytest.raises(ValueError, match=field):
            schedule_stamp.stamp_header(SCHEDULE_YAML, version_id, macros,
                                        checker=OK)

    expect("workbookVersionId", version_id="not-a-uuid")
    expect("id", macros=(WorkbookMacro(id="not-a-uuid", name="m",
                                       filename="macro_x"),))
    expect("name", macros=(WorkbookMacro(id=MACRO_ID, name="has space",
                                         filename="macro_x"),))
    expect("name", macros=(WorkbookMacro(id=MACRO_ID, name="x" * 48,
                                         filename="macro_x"),))
    expect("filename", macros=(WorkbookMacro(id=MACRO_ID, name="m",
                                             filename="macro/x"),))
    expect("macros", macros=tuple(
        WorkbookMacro(id=f"47b03f78-1111-2222-3333-4444555{i:07d}",
                      name=f"m{i}", filename=f"macro_{i}")
        for i in range(9)))
    # The boundary values are accepted.
    schedule_stamp.stamp_header(
        SCHEDULE_YAML, VERSION_ID,
        [WorkbookMacro(id=MACRO_ID, name="x" * 47, filename="macro_x")],
        checker=OK)


def test_when_block_is_emitted_block_style_and_restamps():
    routed = WorkbookMacro(
        id=MACRO_ID, name="ambyte-trace", filename="macro_8feac276a118",
        when=(MacroCondition(field="schema", op="eq", value="ambit.trace/3"),
              MacroCondition(field="protocol.name", op="neq", value="modbus")))
    stamped = schedule_stamp.stamp_header(SCHEDULE_YAML, VERSION_ID, [routed],
                                          checker=OK)
    block = stamped.split("macros:\n", 1)[1].split("\n\n", 1)[0]
    # The device-side parser is block-only: no flow collections anywhere.
    assert "[" not in block and "{" not in block and "]" not in block
    assert "    when:" in block
    assert "      - field: schema" in block
    assert "        op: eq" in block
    # '/' is outside the plain-scalar set, so the value is quoted.
    assert '        value: "ambit.trace/3"' in block
    assert "      - field: protocol.name" in block
    # The nested when: block is indented, so a re-stamp replaces it whole.
    twice = schedule_stamp.stamp_header(stamped, VERSION_ID, [routed],
                                        checker=OK)
    assert twice == stamped


def test_when_contract_is_enforced_before_stamping():
    def macro_with(when):
        return WorkbookMacro(id=MACRO_ID, name="m", filename="macro_x",
                             when=when)

    def expect(needle, when):
        with pytest.raises(ValueError, match=needle):
            schedule_stamp.stamp_header(SCHEDULE_YAML, VERSION_ID,
                                        [macro_with(when)], checker=OK)

    good = MacroCondition(field="schema", op="eq", value="ambit.trace/3")
    expect("field", (MacroCondition(field="owner", op="eq", value="x"),))
    expect("op", (MacroCondition(field="schema", op="gt", value="x"),))
    expect("value", (MacroCondition(field="schema", op="eq",
                                    value="has space"),))
    expect("value", (MacroCondition(field="schema", op="eq", value="x" * 48),))
    expect("repeats", (good, good))
    expect("exceed", tuple(replace(good, value=f"v{i}") for i in range(5)))
    # The boundary values are accepted: '/' is deliberately in the charset
    # (schema ids need it), 47 chars pass, and the same field+op with a
    # different value is a distinct condition, not a duplicate.
    schedule_stamp.stamp_header(
        SCHEDULE_YAML, VERSION_ID,
        [macro_with((good, replace(good, value="x" * 47),
                     MacroCondition(field="tag", op="neq", value="pilot")))],
        checker=OK)


def test_macro_when_firmware_gate():
    assert schedule_stamp.firmware_supports_macro_when("2.2.0")
    assert schedule_stamp.firmware_supports_macro_when("v2.2.0")
    assert schedule_stamp.firmware_supports_macro_when("2.10.0")
    assert not schedule_stamp.firmware_supports_macro_when("2.1.9")
    # Same fail-closed semantics as the macros-header gate.
    assert not schedule_stamp.firmware_supports_macro_when("2.2.0-rc1")
    assert not schedule_stamp.firmware_supports_macro_when("")
    assert not schedule_stamp.firmware_supports_macro_when("dev-build")


def test_rejects_non_schedule_text():
    with pytest.raises(ScheduleStampError, match="schema"):
        schedule_stamp.stamp_header("hello: world\n", VERSION_ID, [MACRO],
                                    checker=OK)
    with pytest.raises(ScheduleStampError):
        schedule_stamp.stamp_header(SCHEDULE_YAML, "", [MACRO], checker=OK)


def test_compile_failure_is_loud():
    def boom(_text):
        return "jobs:12:3: unknown action 'device/nope'"

    with pytest.raises(ScheduleStampError, match="unknown action"):
        schedule_stamp.stamp_header(SCHEDULE_YAML, VERSION_ID, [MACRO],
                                    checker=boom)


def test_pre_stream_a_compiler_is_tolerated_for_macros_only():
    # A host compiler built from a checkout without stream A rejects exactly
    # `unknown top-level key 'macros'`; the macros-stripped remainder must
    # still compile for the stamp to be accepted.
    def pre_a_compiler(text):
        if "\nmacros:" in "\n" + text:
            return "doc:7:1: unknown top-level key 'macros'"
        return None

    stamped = schedule_stamp.stamp_header(SCHEDULE_YAML, VERSION_ID, [MACRO],
                                          checker=pre_a_compiler)
    assert "macros:" in stamped

    def broken_even_without_macros(text):
        if "\nmacros:" in "\n" + text:
            return "doc:7:1: unknown top-level key 'macros'"
        return "doc:20:5: something else is broken"

    with pytest.raises(ScheduleStampError, match="something else"):
        schedule_stamp.stamp_header(SCHEDULE_YAML, VERSION_ID, [MACRO],
                                    checker=broken_even_without_macros)


def test_firmware_gate():
    assert schedule_stamp.firmware_supports_macros("2.1.0")
    assert schedule_stamp.firmware_supports_macros("v2.1.0")
    assert schedule_stamp.firmware_supports_macros("2.2.0")
    assert schedule_stamp.firmware_supports_macros("v2.10.0")
    assert not schedule_stamp.firmware_supports_macros("2.0.9")
    assert not schedule_stamp.firmware_supports_macros("1.9.9")
    # A prerelease of exactly the floor version predates the final (semver).
    assert not schedule_stamp.firmware_supports_macros("2.1.0-rc1")
    # Unparseable fails closed: never stamp macros onto an unknown firmware.
    assert not schedule_stamp.firmware_supports_macros("")
    assert not schedule_stamp.firmware_supports_macros("dev-build")


# ── real compiler (skipped when no toolchain/source tree) ───────────────────
HAVE_TOOLCHAIN = (shutil.which("cc") or shutil.which("clang")
                  or shutil.which("gcc")) and (
    Path(__file__).resolve().parents[2] / "tools" / "sched_host.c").is_file()


@pytest.mark.skipif(not HAVE_TOOLCHAIN, reason="no C toolchain / source tree")
def test_real_compiler_validates_default_schedule_stamp():
    yaml_text = (Path(__file__).resolve().parents[2]
                 / "schedule" / "default.yaml").read_text(encoding="utf-8")
    # workbookVersionId alone must always validate, on any compiler vintage.
    stamped = schedule_stamp.stamp_header(yaml_text, VERSION_ID, [])
    assert f'workbookVersionId: "{VERSION_ID}"' in stamped
    # This checkout predates stream A, so macros exercise the tolerated path;
    # once stream A lands this same call validates directly.
    schedule_stamp.stamp_header(yaml_text, VERSION_ID, [MACRO])


@pytest.mark.skipif(not HAVE_TOOLCHAIN, reason="no C toolchain / source tree")
def test_real_compiler_accepts_the_stamped_when_block():
    # The seam between the two halves of this feature: the block-style `when:`
    # the stamper writes has to be exactly what the device compiler parses.
    # A Python-side reconstruction of the grammar would not prove that.
    routed = replace(MACRO, when=(
        MacroCondition(field="schema", op="eq", value="ambit.trace/3"),
        MacroCondition(field="protocol.name", op="neq", value="SS"),
    ))
    stamped = schedule_stamp.stamp_header(SCHEDULE_YAML, VERSION_ID, [routed])
    assert "    when:\n      - field: schema\n" in stamped
    # '/' is not plain-safe, so the scalar quotes; the device parser unquotes
    assert '        value: "ambit.trace/3"\n' in stamped
    assert "        value: SS\n" in stamped
    # And prove the compiler actually ran rather than silently no-opping (a
    # missing binary reports "no error"): the same document with a field the
    # device cannot address has to come back with a compile error.
    assert schedule_stamp._check_with_sched_host(stamped) is None
    unaddressable = stamped.replace("      - field: schema\n",
                                    "      - field: measure_id\n")
    error = schedule_stamp._check_with_sched_host(unaddressable)
    assert error is not None and "measure_id" in error


@pytest.mark.skipif(not HAVE_TOOLCHAIN, reason="no C toolchain / source tree")
def test_real_compiler_rejects_a_broken_schedule():
    broken = SCHEDULE_YAML.replace("      - uses: device/status-report",
                                   "      - uses: device/nope")
    with pytest.raises(ScheduleStampError):
        schedule_stamp.stamp_header(broken, VERSION_ID, [])


# ── identity helpers used by procedure/fleet ─────────────────────────────────
def test_stamped_sha_is_over_the_stamped_bytes():
    stamped = schedule_stamp.stamp_header(SCHEDULE_YAML, VERSION_ID, [MACRO],
                                          checker=OK)
    assert hashlib.sha256(stamped.encode("utf-8")).hexdigest() != \
        hashlib.sha256(SCHEDULE_YAML.encode("utf-8")).hexdigest()
