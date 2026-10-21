"""Versioned discovery through the real serial parser and GUI procedure paths."""
from __future__ import annotations

import json
from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace

import pytest

from flash_gui import ambyte_serial as serial_client, procedure
from flash_gui.ambyte_serial import (AmbyteConsole, ConsoleError, ConsoleIdentity,
    ScheduleIdentity, ScheduleReleaseStatus, UnsupportedConsoleCommand)

MAC = "02:00:00:00:00:01"
SHA = "a" * 64
WB = "11111111-2222-4333-8444-555555555555"
CAPS = frozenset({"identity", "cfg.get", "cfg.set", "reboot", "schedule.status",
    "schedule.release", "schedule.begin", "schedule.put", "schedule.commit", "schedule.abort"})
SCHEDULE = ScheduleIdentity("INSTALLED", SHA, WB, "test-v1")
IDENTITY = ConsoleIdentity(MAC, "Field_é", "2.6.2", 1, SCHEDULE, CAPS)
RELEASE = ScheduleReleaseStatus(SHA, "test-v1", "2.2.0", "2.5.0", True, True)
OLD = (Path(__file__).parent / "fixtures/identity-2.5.0.raw").read_bytes()


def wire(identity=IDENTITY):
    return json.dumps(dict(mac=identity.mac, thing_name=identity.thing_name,
        fw=identity.fw, contract=identity.contract,
        schedule=dict(source=identity.schedule.source, sha256=identity.schedule.sha256,
            workbookVersionId=identity.schedule.workbook_version_id,
            script_version=identity.schedule.script_version),
        capabilities=sorted(identity.capabilities)), ensure_ascii=False).encode()


class Clock:
    def __init__(self): self.now = 0.0
    def time(self): return self.now
    def sleep(self, value): self.now += value


class Transport:
    """Byte-level serial transport, including opens, boot and chunk boundaries."""
    def __init__(self, clock, reply, boot=0):
        self.clock, self.reply, self.boot = clock, reply, boot
        self.dtr = self.rts = True
        self.pending = []
        self.opens = 0
        self.commands = []
        self.closed = False
    def open(self):
        assert not self.dtr and not self.rts
        self.opens += 1
    def close(self): self.closed = True
    def reset_input_buffer(self): self.pending.clear()
    def write(self, data):
        cmd = data.decode().strip()
        self.commands.append(cmd)
        if not cmd:
            if self.clock.now >= self.boot:
                self.pending = [b"ambyte> "]
        elif cmd in ("identity", "ota_status"): self.pending = list(self.reply)
        elif cmd == "status": self.pending = [f" - MAC: {MAC}\r\nambyte> ".encode()]
        elif cmd == "cfg get device_name": self.pending = [b"device_name = raw_{MAC}\r\nambyte> "]
        else: raise AssertionError(cmd)
    def read(self, count):
        self.clock.now += .2
        return self.pending.pop(0) if self.pending else b""


@pytest.fixture
def transport(monkeypatch):
    clock = Clock()
    monkeypatch.setattr(serial_client.time, "time", clock.time)
    monkeypatch.setattr(serial_client.time, "sleep", clock.sleep)
    monkeypatch.setattr(serial_client, "esp_jtag_ports", lambda: ["bench"])
    def make(reply, boot=0, open_console=True):
        ser = Transport(clock, reply, boot)
        monkeypatch.setattr(serial_client.serial, "Serial", lambda: ser)
        return AmbyteConsole("bench") if open_console else None, ser
    return make


def test_strict_json_with_crcrlf_logs_and_split_utf8(transport):
    record = wire()
    split = record.index("é".encode()) + 1
    con, ser = transport([b"identity\r\nI (1) worker: log\r\n", record[:split],
                          record[split:] + b"\r\r\nI (2) worker: done\r\nambyte> "])
    assert con.identity() == IDENTITY
    assert ser.commands == ["identity"]


@pytest.mark.parametrize("reply", [OLD,
    OLD.replace(b"Unrecognized", b"I (11900) APP_MAIN: boot\r\nUnrecognized"),
    OLD + b" I (11950) APP_MAIN: CLI started\r\n"])
def test_real_250_reply_and_boot_logs_select_legacy(transport, reply):
    con, _ = transport([reply])
    with pytest.raises(UnsupportedConsoleCommand): con.identity()


@pytest.mark.parametrize("reply", [b"", b"identity\r\nambyte> ",
    b"identity\r\nCommand returned non-zero error code: 0x1 (ERROR)\r\nambyte> ",
    b"ambyte> " + wire() + b"\r\nambyte> ",
    wire().replace("é".encode(), b"\xff") + b"\r\nambyte> ",
    b"\x1b[0m" + wire() + b"\r\nambyte> ",
    wire() + b" \r\nambyte> ",
    wire() + b"I (2) log tail\r\nambyte> ",
    b'{"contract":true}\r\nambyte> ', b'{"contract":1}\r\nambyte> ',
    wire().replace(b'"contract": 1', b'"contract": 2') + b"\r\nambyte> "])
def test_silence_malformed_and_unsupported_contract_never_downgrade(transport, reply):
    con, _ = transport([reply] if reply else [])
    with pytest.raises(ConsoleError) as raised: con.identity(timeout=1)
    assert not isinstance(raised.value, UnsupportedConsoleCommand)


@pytest.mark.parametrize("field,value", [("capabilities", ["identity", "identity"]),
    ("mac", "bad"), ("thing_name", 7), ("schedule", {"source": "OTHER"}),
    ("capabilities", [1])])
def test_bad_schema_rejected(transport, field, value):
    obj = json.loads(wire()); obj[field] = value
    con, _ = transport([json.dumps(obj).encode() + b"\r\nambyte> "])
    with pytest.raises(ConsoleError): con.identity()


def test_malformed_log_line_ignored_before_response(transport):
    con, _ = transport([b'{not json}\r\n{"log":1}\r\n' + wire() + b"\r\r\nambyte> "])
    assert con.identity() == IDENTITY


def test_probe_keeps_one_descriptor_through_30s_boot_and_uses_json(transport):
    _, ser = transport([wire() + b"\r\r\nambyte> "], boot=30, open_console=False)
    assert serial_client.PROBE_READY_TIMEOUT_S == 45.0
    result = serial_client.probe_device("bench")
    assert ser.opens == 1 and ser.closed
    assert result.identity == IDENTITY and result.mac == MAC
    assert result.device_name == "raw_{MAC}"
    assert "status" not in ser.commands


def test_probe_legacy_status_only_on_explicit_unsupported(transport):
    _, ser = transport([OLD], open_console=False)
    result = serial_client.probe_device("bench")
    assert result.identity is None and result.mac == MAC
    assert ser.commands.count("status") == 1


def test_probe_corrupt_response_does_not_fall_back(transport):
    _, ser = transport([b"identity\r\n{}\r\nambyte> "], open_console=False)
    with pytest.raises(ConsoleError): serial_client.probe_device("bench")
    assert "status" not in ser.commands and ser.closed


class ScheduleConsole:
    def __init__(self, identity=IDENTITY, release=RELEASE, identities=()):
        self.value, self.release = identity, release
        self.identities = list(identities)
        self.calls = []
        self.pushed = self.url_installed = None
        self.closed = False
    def identity(self, timeout=5):
        self.calls.append("identity")
        return self.identities.pop(0) if self.identities else self.value
    def schedule_release(self, timeout=10):
        self.calls.append("release"); return self.release
    def schedule_status_workbook(self): raise AssertionError("JSON path must not parse status")
    def schedule_push(self, *args, **kw):
        self.calls.append("push"); self.pushed = args
        self.value = IDENTITY; self.release = RELEASE
    def schedule_install(self, *args):
        self.calls.append("install"); self.url_installed = args
        self.value = IDENTITY; self.release = RELEASE
    def wifi_connected(self): return True
    def close(self): self.closed = True


def source(**changes):
    return replace(procedure.ScheduleSource("workbook", "test.yaml", b"fixture", SHA,
        "test-v1", "test", "2.2.0", None, WB), **changes)


@pytest.fixture
def install(monkeypatch):
    clock = Clock()
    monkeypatch.setattr(procedure.time, "time", clock.time)
    monkeypatch.setattr(procedure.time, "sleep", clock.sleep)
    monkeypatch.setattr(procedure, "SCHEDULE_INSTALL_DEADLINE_S", 25)
    def run(con, selected=None):
        monkeypatch.setattr(serial_client, "connect_after_boot", lambda *a, **kw: con)
        state = SimpleNamespace(port="bench", console_identity=None)
        return procedure.install_schedule_source(selected or source(), state, log=lambda _: None)
    return run


def test_already_active_json_double_read_is_verified(install):
    con = ScheduleConsole()
    result = install(con)
    assert result.passed and WB in result.detail
    assert con.calls == ["identity", "identity", "release", "identity"]
    assert con.closed and not con.pushed


def test_real_change_uses_push_and_json_post_commit_polling(install):
    stale = replace(IDENTITY, schedule=replace(SCHEDULE, sha256="b" * 64, script_version="old"))
    con = ScheduleConsole(stale, replace(RELEASE, sha256="b" * 64, script_version="old"))
    assert install(con).passed
    assert con.pushed == (b"fixture", SHA, "test", "test-v1", "2.2.0")
    assert con.calls[con.calls.index("push") + 1:] == ["identity", "release", "identity"]


def test_reload_between_identity_reads_and_all_null_poll_without_writes(install):
    other = replace(IDENTITY, schedule=replace(SCHEDULE, sha256="b" * 64))
    empty = replace(IDENTITY, schedule=ScheduleIdentity(None, None, None, None))
    con = ScheduleConsole(identities=[IDENTITY, other, IDENTITY, empty, IDENTITY, IDENTITY])
    assert install(con).passed
    assert not con.pushed and con.calls.count("identity") >= 6


@pytest.mark.parametrize("release", [replace(RELEASE, running=False),
    replace(RELEASE, verified=False), replace(RELEASE, built_against_fw="wrong"),
    replace(RELEASE, sha256="b" * 64), replace(RELEASE, script_version="wrong")])
def test_stopped_unverified_wrong_build_or_torn_release_never_passes(install, release):
    con = ScheduleConsole(release=release)
    # Prevent fake push from repairing the bad sample.
    con.schedule_push = lambda *a, **kw: None
    with pytest.raises(procedure.ProcedureError): install(con)


def test_wrong_json_workbook_is_failure_without_status_fallback(install):
    con = ScheduleConsole(replace(IDENTITY, schedule=replace(SCHEDULE, workbook_version_id="wrong")))
    result = install(con)
    assert not result.passed and "wrong" in result.detail


@pytest.mark.parametrize("missing", ["schedule.begin", "schedule.put", "schedule.commit", "schedule.abort"])
def test_missing_push_capability_stops_workbook_writes(install, missing):
    stale = replace(IDENTITY, capabilities=CAPS - {missing},
                    schedule=replace(SCHEDULE, sha256="b" * 64, script_version="old"))
    con = ScheduleConsole(stale, replace(RELEASE, sha256="b" * 64, script_version="old"))
    with pytest.raises(procedure.ProcedureError, match="capabilities"): install(con)
    assert not con.pushed and "install" not in con.calls


def test_missing_push_capability_uses_catalog_url(install):
    stale = replace(IDENTITY, capabilities=CAPS - {"schedule.put"},
                    schedule=replace(SCHEDULE, sha256="b" * 64, script_version="old"))
    con = ScheduleConsole(stale, replace(RELEASE, sha256="b" * 64, script_version="old"))
    assert install(con, source(asset_url="https://example.test/asset")).passed
    assert con.url_installed[0] == "https://example.test/asset" and not con.pushed


def test_missing_release_capability_cannot_verify_or_write(install):
    con = ScheduleConsole(replace(IDENTITY, capabilities=CAPS - {"schedule.release"}))
    with pytest.raises(procedure.ProcedureError, match="schedule.release"): install(con)
    assert con.calls == ["identity"]


def test_all_null_post_commit_is_polled_until_activation(install):
    stale = replace(IDENTITY, schedule=replace(SCHEDULE, sha256="b" * 64, script_version="old"))
    con = ScheduleConsole(stale, replace(RELEASE, sha256="b" * 64, script_version="old"))
    push = con.schedule_push
    def apply(*args, **kw):
        push(*args, **kw)
        con.identities = [replace(IDENTITY, schedule=ScheduleIdentity(None, None, None, None)),
                          IDENTITY, IDENTITY]
    con.schedule_push = apply
    assert install(con).passed
    assert con.calls[con.calls.index("push") + 1:] == ["identity", "identity", "release", "identity"]


def test_missing_identity_capability_uses_legacy_workbook_reader(install):
    con = ScheduleConsole(replace(IDENTITY, capabilities=CAPS - {"identity"}))
    calls = []
    con.schedule_status_workbook = lambda: calls.append("status") or WB
    assert install(con).passed
    assert calls == ["status"]
    assert con.calls == ["identity", "release"]


@pytest.mark.parametrize("source_name", ["NONE", "EMBEDDED_DEFAULT", "EMBEDDED_FALLBACK"])
def test_noninstalled_source_cannot_pass_even_with_matching_release(install, source_name):
    con = ScheduleConsole(replace(IDENTITY, schedule=replace(SCHEDULE, source=source_name)))
    # Firmware falls back after compile failure; never report activation success.
    con.schedule_push = lambda *a, **kw: None
    with pytest.raises(procedure.ProcedureError): install(con)


def test_json_missing_workbook_is_failure(install):
    con = ScheduleConsole(replace(IDENTITY, schedule=replace(SCHEDULE, workbook_version_id=None)))
    assert not install(con).passed


def test_preflight_carries_discovery_separately_from_stored_name(monkeypatch):
    monkeypatch.setattr(serial_client, "probe_device", lambda *a, **kw:
        serial_client.ProbeResult(True, MAC, "Named_{MAC}", identity=IDENTITY))
    monkeypatch.setattr(procedure.esptool_ops, "read_mac", lambda *a, **kw:
                        pytest.fail("JSON MAC must not invoke esptool"))
    info = procedure.preflight("bench", log=lambda _: None)
    assert info.console_identity == IDENTITY and info.stored_name == f"Named_{MAC}"
    assert info.mac == MAC


@pytest.mark.parametrize("discovered,expected", [(IDENTITY, [True, True, True]),
    (replace(IDENTITY, mac="02:00:00:00:00:02"), [False, True, True]),
    (replace(IDENTITY, thing_name="wrong"), [True, False, True]),
    (replace(IDENTITY, fw="wrong"), [True, True, False])])
def test_post_boot_readback_checks_effective_identity_and_compiled_firmware(monkeypatch, discovered, expected):
    cloud = object()
    clock = Clock(); clock.now = 1000
    monkeypatch.setattr(procedure.time, "time", clock.time)
    class Console:
        def identity(self): return discovered
        def rtc_set(self, now): pass
        def rtc_read(self): return 1000
        def cfg_get(self, key): return {"device_name": "Field", "timezone": "UTC"}[key]
        def sd_mounted(self): return False
        def close(self): pass
    monkeypatch.setattr(serial_client, "connect_after_boot", lambda *a, **kw: Console())
    state = procedure.DeviceRun("bench", procedure.PreflightInfo("bench", MAC, True, None))
    state.identity = cloud
    state.plan = SimpleNamespace(client_id="Field_é", device_name="Field", timezone="UTC")
    ctx = SimpleNamespace(release=SimpleNamespace(version="v2.6.2"))
    results = procedure.provision_and_verify(ctx, state, log=lambda _: None)
    assert [item.passed for item in results[:3]] == expected
    assert state.console_identity == discovered and state.identity is cloud


def test_nullable_json_values_remain_unavailable(transport):
    empty = ConsoleIdentity(None, None, None, 1, ScheduleIdentity(None, None, None, None), CAPS)
    con, _ = transport([wire(empty) + b"\r\r\nambyte> "])
    assert con.identity() == empty


def test_capability_gate_applies_even_when_json_self_token_is_missing(install):
    stale = replace(IDENTITY, capabilities=CAPS - {"identity", "schedule.put"})
    con = ScheduleConsole(stale, replace(RELEASE, sha256="b" * 64, script_version="old"))
    with pytest.raises(procedure.ProcedureError, match="capabilities"): install(con)
    assert not con.pushed


def test_reconnect_discovers_new_capabilities_before_verifying(monkeypatch, install):
    stale = replace(IDENTITY, schedule=replace(SCHEDULE, sha256="b" * 64, script_version="old"))
    first = ScheduleConsole(stale, replace(RELEASE, sha256="b" * 64, script_version="old"))
    replacement = ScheduleConsole(replace(IDENTITY, capabilities=CAPS - {"schedule.release"}))
    push = first.schedule_push
    def disconnect(*args, **kwargs):
        push(*args, **kwargs)
        def lost(timeout=5): raise ConsoleError("port bench went away")
        first.identity = lost
    first.schedule_push = disconnect
    connections = iter([first, replacement])
    # Install fixture wires its own connect; invoke shared entry directly here.
    monkeypatch.setattr(serial_client, "connect_after_boot", lambda *a, **kw: next(connections))
    with pytest.raises(procedure.ProcedureError, match="schedule.release"):
        procedure.install_schedule_source(source(), SimpleNamespace(port="bench"), log=lambda _: None)
    assert first.closed and replacement.closed and replacement.calls == ["identity"]


def test_preflight_probe_never_scans_another_board(transport, monkeypatch):
    _, ser = transport([wire() + b"\r\nambyte> "], open_console=False)
    monkeypatch.setattr(serial_client, "connect_after_boot", lambda *a, **kw:
                        pytest.fail("preflight cannot select another enumerated board"))
    monkeypatch.setattr(serial_client, "esp_jtag_ports", lambda: ["other-board"])
    assert serial_client.probe_device("bench").mac == MAC
    assert ser.port == "bench" and ser.opens == 1


@pytest.mark.parametrize("legacy", [False, True])
def test_ota_policy_uses_real_serial_parser_split_lines_and_legacy_fallback(transport, legacy):
    policy = {"schema": 1, "readable": [1], "written": 1, "fallback": 1,
              "refusal_state": "available", "last_refusal": {
                  "id": "a5-1", "source": "jobs", "reason": "signature_invalid"}}
    record = ("policy: " + json.dumps(policy)).encode()
    old = b"running: ota_0 @ 0x10000 state=VALID\r\r\n"
    reply = [old, b"ambyte> "] if legacy else [old, record[:42], record[42:] + b"\r\r\nambyte> "]
    con, ser = transport(reply)
    assert con.ota_policy() == (None if legacy else policy)
    assert ser.commands == ["ota_status"]


@pytest.mark.parametrize("change", [{"schema": True}, {"readable": [True]},
    {"written": 2}, {"fallback": 0}, {"refusal_state": "invented"},
    {"last_refusal": {"id": "a", "source": "jobs"}}])
def test_malformed_ota_policy_fails_explicitly(transport, change):
    policy = {"schema": 1, "readable": [1], "written": 1, "fallback": 1,
              "refusal_state": "available", "last_refusal": {
                  "id": "a5-1", "source": "jobs", "reason": "signature_invalid"}}
    policy.update(change)
    con, _ = transport([("policy: " + json.dumps(policy)).encode() + b"\r\nambyte> "])
    with pytest.raises(ConsoleError, match="Invalid OTA policy"): con.ota_policy()
