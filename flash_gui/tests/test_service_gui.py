# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only
"""Production serial layer + procedure against physical console/service bytes.

Only the device and serial OS boundary are fake. Capability selection, JSON
discovery, framing, journaling, transfer, reconnect and activation are real.
"""
import base64
from dataclasses import replace
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import threading
from types import SimpleNamespace

import pytest

from flash_gui import ambyte_serial, config, procedure, release_fetch
from flash_gui.service_client import OperationJournal, schedule_descriptor
from flash_gui.tests.service_device import Clock, Device, Serial, response

MAC = "AA:BB:CC:DD:EE:FF"
WB = "11111111-2222-4333-8444-555555555555"
BLOB = b"schema: jii.ambyte-schedule/v1-draft\n" + b"# fixture\n" * 150
SHA = hashlib.sha256(BLOB).hexdigest()
OLD_SHA = "b" * 64
CAPS = ["identity", "schedule.release", "schedule.begin", "schedule.put",
        "schedule.commit", "schedule.abort"]


def source(**changes):
    return replace(procedure.ScheduleSource("workbook", "fixture.yaml", BLOB,
        SHA, "test-v1", "gui-test", "2.6.2", None, WB), **changes)


class GuiDevice(Device):
    def __init__(self, *, svc=True, identity=True):
        super().__init__()
        self.svc, self.has_identity = svc, identity
        self.mac = MAC
        self.active_sha, self.active_version, self.active_wb = OLD_SHA, "old-v1", None
        self.loaded_source, self.running = "INSTALLED", True
        self.release_sha = None
        self.wrong_workbook = False
        self.legacy_data = b""
        self.plain = []

    def answer(self, body):
        value = super().answer(body)
        if body.startswith("svc.result ") and value.get("state") == "succeeded":
            self.active_sha, self.active_version = value["sha256"], value["version"]
            self.active_wb = None if self.wrong_workbook else value["workbook"]
        return value

    def console(self, command):
        self.plain.append(command)
        if not command:
            return b"ambyte> "
        if command == "identity":
            if not self.has_identity:
                # Exact A2 bench fixture, not a newly invented old-image reply.
                return (Path(__file__).parent / "fixtures/identity-2.5.0.raw").read_bytes()
            value = dict(mac=self.mac, thing_name="AMBYTE_" + self.mac, fw="2.6.2", contract=1,
                         schedule=dict(source=self.loaded_source, sha256=self.active_sha,
                                       workbookVersionId=self.active_wb, script_version=self.active_version),
                         capabilities=CAPS + (["svc"] if self.svc else []))
            return b"identity\r\n" + json.dumps(value).encode() + b"\r\r\nambyte> "
        if command == "schedule release":
            return (f"{command}\r\nschedule release: sha256={self.release_sha or self.active_sha} "
                    f"version={self.active_version} built_against_fw=2.6.2 installed_on_fw=2.6.2 "
                    f"verified=true running={str(self.running).lower()}\r\nambyte> ").encode()
        if command == "schedule status":
            return f"id=test version={self.active_version} workbook={self.active_wb or '-'} name=test\r\nambyte> ".encode()
        if command == "schedule begin":
            self.legacy_data = b""
            value = "schedule begin: ready"
        elif command.startswith("schedule put "):
            self.legacy_data += base64.b64decode(command.split()[2], validate=True)
            value = f"schedule put: {len(self.legacy_data)} bytes"
        elif command.startswith("schedule commit "):
            args = command.split()
            assert hashlib.sha256(self.legacy_data).hexdigest() == args[2]
            self.active_sha, self.active_version = args[2], args[4]
            self.active_wb = WB if self.legacy_data == BLOB else None
            value = "schedule commit queued: id=gui-test"
        else:
            raise AssertionError(f"Unexpected plain command: {command}")
        return f"{command}\r\n{value}\r\nambyte> ".encode()


class GuiSerial(Serial):
    def __init__(self, device, clock, **kwargs):
        super().__init__(device, clock, **kwargs)
        self.is_open = False
        self.timeout, self.write_timeout = .2, None
        self.dtr = self.rts = None
        self.port = None

    def open(self):
        assert self.dtr is False and self.rts is False
        self.is_open = True

    def reset_input_buffer(self):
        self.output.clear()

    def read(self, size):
        self.clock.sleep(.005)
        return super().read(size)

    def write(self, line):
        if not self.is_open:
            raise OSError("disconnected")
        if line == b"svc\n" or re.match(rb"[0-9a-f]{8} ", line):
            return super().write(line)
        if self.device.mode:
            self.output.extend(response(0, dict(ok=False, store=self.device.store, reason="bad_frame")))
        else:
            self.output.extend(self.device.console(line.decode().strip()))
        return len(line)


@pytest.fixture
def bench(monkeypatch, tmp_path):
    clock = Clock()
    monkeypatch.setattr(ambyte_serial.time, "time", clock)
    monkeypatch.setattr(ambyte_serial.time, "monotonic", clock)
    monkeypatch.setattr(ambyte_serial.time, "sleep", clock.sleep)
    monkeypatch.setattr(ambyte_serial, "esp_jtag_ports", lambda: ["bench"])
    monkeypatch.setattr(config, "SERVICE_OPERATIONS_DIR", tmp_path)
    monkeypatch.setattr(procedure, "SCHEDULE_INSTALL_DEADLINE_S", 12)
    monkeypatch.setattr(procedure, "SCHEDULE_INSTALL_SETTLE_S", 0)
    handles, logs = [], []

    def run(device=None, *, cuts=(), mutate=None, raw=None, selected=None, cancelled=lambda: False,
            start=lambda: None, expected_mac=MAC, new_request=False):
        device = device or GuiDevice()
        iterator = iter(cuts)
        def factory():
            serial = GuiSerial(device, clock, cut=next(iterator, None), mutate=mutate, raw=raw)
            handles.append(serial)
            return serial
        monkeypatch.setattr(ambyte_serial.serial, "Serial", factory)
        state = SimpleNamespace(port="bench", preflight=SimpleNamespace(mac=expected_mac))
        result = procedure.install_schedule_source(selected or source(), state, log=logs.append,
            cancelled=cancelled, on_service_start=start, new_request=new_request)
        return result, device, state

    return SimpleNamespace(run=run, handles=handles, logs=logs, clock=clock, path=tmp_path)


def requests(bench):
    return [line for ser in bench.handles for line in ser.sent]


def test_gui_service_selects_real_client_and_checks_json_after_exit(bench):
    result, device, state = bench.run()
    assert result.passed and state.console_identity.mac == MAC
    assert device.data == BLOB and device.applies == device.commits == 1
    assert not any(c.startswith(("schedule begin", "schedule put", "schedule commit", "schedule install")) for c in device.plain)
    assert device.plain[-3:] == ["identity", "schedule release", "identity"]
    assert OperationJournal(bench.path, MAC).load()["phase"] == "complete"
    assert all(not ser.is_open for ser in bench.handles)


@pytest.mark.parametrize("identity", [True, False])
def test_gui_no_svc_uses_existing_legacy_push(bench, identity):
    result, device, _ = bench.run(GuiDevice(svc=False, identity=identity))
    assert result.passed and device.legacy_data == BLOB
    assert "schedule begin" in device.plain and device.commits == 0
    assert not requests(bench)
    assert not list(bench.path.glob("*.json"))


@pytest.mark.parametrize("action,occurrence,point", [
    ("schedule.begin", 1, "before"), ("schedule.begin", 1, "after"),
    ("p", 2, "before"), ("p", 2, "after"),
    ("p", (len(BLOB) + 143) // 144, "after"),
    ("schedule.commit", 1, "before"), ("schedule.commit", 1, "after"),
    ("svc.result", 4, "after"),
])
def test_gui_reconnect_resumes_id_and_bytes_without_duplicate_apply(bench, action, occurrence, point):
    result, device, _ = bench.run(cuts=[(action, occurrence, point)])
    assert result.passed and device.data == BLOB
    assert len(device.records) == 1 and device.applies == device.commits == 1
    assert len(bench.handles) == 2
    assert all(not ser.is_open for ser in bench.handles)


@pytest.mark.parametrize("outcome", ["failed", "uncertain"])
def test_gui_never_turns_failure_or_uncertainty_into_success(bench, outcome):
    device = GuiDevice(); device.outcome = outcome
    with pytest.raises(procedure.ProcedureError, match="compile_failed"):
        bench.run(device)
    assert device.applies == 1
    assert not any(c.startswith("schedule install") for c in device.plain)


def test_service_url_without_bytes_is_actionable_and_never_mutates(bench):
    device = GuiDevice()
    with pytest.raises(procedure.ProcedureError, match="Verified bytes.*unavailable"):
        bench.run(device, selected=source(blob=None, asset_url="https://example.test/immutable.yaml"))
    assert not device.records and not device.legacy_data


@pytest.mark.parametrize("bad", ["crc", "correlation", "json", "actions"])
def test_gui_bad_service_reply_never_downgrades_to_legacy(bench, bad):
    device = GuiDevice()
    def mutate(action, value):
        if bad == "actions" and action == "svc.version": value["actions"] = []
        return value
    def raw(action, frame):
        if action != "svc.version": return frame
        if bad == "crc": return frame[:-2] + b"0\n"
        if bad == "correlation": return response(99, dict(ok=True, store=device.store))
        if bad == "json": return response(int(frame[:8], 16), dict(ok=True, store=device.store, service="wrong"))
        return frame
    with pytest.raises(procedure.ProcedureError): bench.run(device, mutate=mutate, raw=raw)
    assert not device.records and not device.legacy_data


def test_gui_foreign_active_reports_kind_and_state_without_touching_it(bench):
    device = GuiDevice(); device.mode = device.bound = True
    device.answer(f"schedule.begin 0000000000000001 {len(BLOB):08x} {SHA} test-v1 2.6.2 {WB}")
    before = dict(device.records["0000000000000001"])
    with pytest.raises(procedure.ProcedureError, match="schedule.*staging"):
        bench.run(device)
    assert device.records["0000000000000001"] == before
    assert not device.commits and not device.aborts


@pytest.mark.parametrize("field", ["workbook", "release", "source", "running"])
def test_gui_independent_activation_mismatch_never_succeeds(bench, field):
    device = GuiDevice()
    if field == "workbook": device.wrong_workbook = True
    if field == "release":
        # Only disagree after the operation, so discovery itself is consistent.
        original = device.answer
        def answer(body):
            value = original(body)
            if value.get("state") == "succeeded": device.release_sha = OLD_SHA
            return value
        device.answer = answer
    if field == "source": device.loaded_source = "EMBEDDED_DEFAULT"
    if field == "running": device.running = False
    with pytest.raises(procedure.ProcedureError, match="not verified"):
        bench.run(device)
    assert device.applies == 1


@pytest.mark.parametrize("committed", [False, True])
def test_gui_cancel_only_aborts_own_precommit_transfer(bench, committed):
    device = GuiDevice()
    def cancelled(): return bool(device.commits if committed else len(device.data))
    with pytest.raises(procedure.ProcedureError, match="cancelled before commit|operation continues"):
        bench.run(device, cancelled=cancelled)
    assert device.aborts == (0 if committed else 1)
    assert device.commits == (1 if committed else 0)
    entry = OperationJournal(bench.path, MAC).load()
    assert entry["phase"] == ("committing" if committed else "complete")


def test_gui_rejects_wrong_gateway_before_any_admission(bench):
    device = GuiDevice()
    with pytest.raises(procedure.ProcedureError, match="differs from the selected board"):
        bench.run(device, expected_mac="11:22:33:44:55:66")
    assert not device.records and not requests(bench)


@pytest.mark.parametrize("problem", ["store_changed", "expired", "conflict"])
def test_gui_preserves_unresolved_intent_without_admission_or_downgrade(bench, problem):
    device = GuiDevice()
    descriptor = schedule_descriptor(BLOB, SHA, "test-v1", "2.6.2", WB)
    journal = OperationJournal(bench.path, MAC)
    journal.save(dict(schema=1, mac=MAC, store=device.store, op="0000000000000001",
                      descriptor=descriptor, phase="admitted"))
    if problem == "store_changed": device.store = "fedcba9876543210"
    else:
        device.highest = 1
        if problem == "conflict":
            device.mode = device.bound = True
            device.highest = 0
            device.answer(f"schedule.begin 0000000000000001 {len(BLOB):08x} {OLD_SHA} test-v1 2.6.2 {WB}")
            device.records["0000000000000001"]["state"] = "failed"
    before = journal.path.read_bytes()
    with pytest.raises(procedure.ProcedureError): bench.run(device)
    assert journal.path.read_bytes() == before
    assert not device.commits and not device.aborts and not device.legacy_data


@pytest.mark.parametrize("problem", ["store_changed", "expired"])
def test_explicit_new_request_preserves_previous_intent_after_readback(bench, monkeypatch, problem):
    device = GuiDevice()
    journal = OperationJournal(bench.path, MAC)
    journal.save(dict(schema=1, mac=MAC, store=device.store, op="0000000000000001",
        descriptor=schedule_descriptor(BLOB, SHA, "test-v1", "2.6.2", WB), phase="admitted"))
    before = journal.path.read_bytes()
    if problem == "store_changed": device.store = "fedcba9876543210"
    else: device.highest = 1
    monkeypatch.setattr(ambyte_serial.serial, "Serial", lambda: GuiSerial(device, bench.clock))
    observed = procedure.service_recovery_identity(
        SimpleNamespace(port="bench", preflight=SimpleNamespace(mac=MAC)), log=lambda _: None)
    assert observed.schedule.sha256 == OLD_SHA and journal.path.read_bytes() == before
    result, _, _ = bench.run(device, new_request=True)
    assert result.passed and device.applies == 1
    assert journal.path.with_suffix(".previous.json").read_bytes() == before
    assert journal.load()["op"] == ("0000000000000001" if problem == "store_changed" else "0000000000000002")


def test_gui_service_download_failure_does_not_use_url_installer(bench, monkeypatch):
    device = GuiDevice()
    selected = source(blob=None, asset_url="https://example.test/immutable.yaml")
    monkeypatch.setattr(procedure, "schedule_source", lambda *a, **kw: selected)
    def unavailable(*a, **kw): raise release_fetch.ReleaseError("offline and not cached")
    monkeypatch.setattr(release_fetch, "script_bytes", unavailable)
    # Use bench's real serial factory for the install_schedule_script entrypoint.
    serial = GuiSerial(device, bench.clock)
    monkeypatch.setattr(ambyte_serial.serial, "Serial", lambda: serial)
    with pytest.raises(procedure.ProcedureError, match="download/cache"):
        procedure.install_schedule_script(SimpleNamespace(schedule_script=None, wifi_ssid=""),
                                           SimpleNamespace(port="bench"), log=lambda _: None)
    assert not device.records and not device.legacy_data


@pytest.mark.parametrize("reader", ["identity", "schedule_release", "status_mac"])
def test_legacy_parsers_fail_cleanly_when_device_is_still_in_service(bench, monkeypatch, reader):
    device = GuiDevice(); device.mode = True
    serial = GuiSerial(device, bench.clock)
    monkeypatch.setattr(ambyte_serial.serial, "Serial", lambda: serial)
    with ambyte_serial.AmbyteConsole("bench") as con:
        with pytest.raises(ambyte_serial.ConsoleError, match="got no prompt"):
            getattr(con, reader)()
    assert not device.records and not device.applies


@pytest.mark.parametrize("close_at", ["chunk", "terminal"])
def test_new_gui_process_resumes_after_abrupt_app_close(tmp_path, close_at):
    peer = tmp_path / "peer.json"
    journal = tmp_path / "journal"
    program = r'''
import json, os, sys
from pathlib import Path
from types import SimpleNamespace
from flash_gui import ambyte_serial, config, procedure
from flash_gui.tests.test_service_gui import GuiDevice, GuiSerial, source, MAC
from flash_gui.tests.service_device import Clock
peer, journal, phase, close_at = sys.argv[1:]
clock = Clock()
ambyte_serial.time.time = clock
ambyte_serial.time.monotonic = clock
ambyte_serial.time.sleep = clock.sleep
ambyte_serial.esp_jtag_ports = lambda: ['bench']
config.SERVICE_OPERATIONS_DIR = Path(journal)
device = GuiDevice()
if Path(peer).exists():
    saved = json.loads(Path(peer).read_text())
    saved['data'] = bytes.fromhex(saved['data'])
    saved['legacy_data'] = bytes.fromhex(saved['legacy_data'])
    device.__dict__.update(saved)
def save():
    saved = dict(vars(device), data=device.data.hex(), legacy_data=device.legacy_data.hex())
    Path(peer).write_text(json.dumps(saved))
handles = []
def factory():
    serial = GuiSerial(device, clock)
    handles.append(serial)
    write = serial.write
    def wrapped(line):
        count = write(line)
        hit = close_at == 'chunk' and serial.counts.get('p') == 2
        hit |= close_at == 'terminal' and any(r['state'] == 'succeeded' for r in device.records.values())
        if phase == 'close' and hit:
            save()
            os._exit(73)  # no worker finally, close handler, abort or journal cleanup
        return count
    serial.write = wrapped
    return serial
ambyte_serial.serial.Serial = factory
result = procedure.install_schedule_source(source(), SimpleNamespace(port='bench'), log=lambda _: None)
assert result.passed
save()
print(json.dumps(dict(puts=sum(s.counts.get('p', 0) for s in handles),
                      commits=sum(s.counts.get('schedule.commit', 0) for s in handles))))
'''
    first = subprocess.run([sys.executable, "-c", program, str(peer), str(journal), "close", close_at],
                           capture_output=True, text=True, timeout=15)
    assert first.returncode == 73, first.stderr
    before = json.loads(peer.read_text())
    entry = OperationJournal(journal, MAC).load()
    assert entry["phase"] == ("admitted" if close_at == "chunk" else "committing")
    assert len(bytes.fromhex(before["data"])) == (288 if close_at == "chunk" else len(BLOB))
    second = subprocess.run([sys.executable, "-c", program, str(peer), str(journal), "resume", close_at],
                            capture_output=True, text=True, timeout=15)
    assert second.returncode == 0, second.stderr
    counts, after = json.loads(second.stdout), json.loads(peer.read_text())
    assert counts["puts"] == ((len(BLOB) + 143) // 144 - 2 if close_at == "chunk" else 0)
    assert counts["commits"] == (1 if close_at == "chunk" else 0)
    assert bytes.fromhex(after["data"]) == BLOB and after["applies"] == 1
    assert len(after["records"]) == 1 and OperationJournal(journal, MAC).load()["phase"] == "complete"


def test_gui_worker_transfer_and_cancel_keep_tk_on_main_thread(bench, monkeypatch):
    from flash_gui import gui
    main_thread = threading.get_ident()
    entered, release, finished = threading.Event(), threading.Event(), threading.Event()
    posts, threads, logs = [], [], []
    device = GuiDevice()
    serial = GuiSerial(device, bench.clock)
    write = serial.write
    def blocking_write(line):
        if b" p " in line and not entered.is_set():
            entered.set()
            assert release.wait(3)
        return write(line)
    serial.write = blocking_write
    monkeypatch.setattr(ambyte_serial.serial, "Serial", lambda: serial)
    monkeypatch.setattr(procedure, "schedule_source", lambda *a, **kw: source())
    monkeypatch.setattr(procedure, "provision_and_verify_with_repair",
                        lambda *a, **kw: [procedure.VerifyItem("Configuration", True, "ok")])
    class Widget:
        def configure(self, options=None, **kw): assert threading.get_ident() == main_thread
        def set(self, value): assert threading.get_ident() == main_thread
    app = object.__new__(gui.App)
    app.busy, app.schedule_catalog = False, None
    app._schedule_cancel = threading.Event()
    app.current_run = SimpleNamespace(port="bench", plan=object(), name="test")
    app._make_context = lambda: SimpleNamespace(schedule_script=None, wifi_ssid="")
    app._post = lambda function, *args: posts.append((function, args))
    app.log = logs.append
    app._set_step = lambda *a: None
    app.result_var = Widget()
    for name in ("onboard_btn", "schedule_script_box", "schedule_refresh_btn",
                 "cancel_schedule_btn", "retry_prov_btn", "retry_flash_btn", "new_schedule_btn"):
        setattr(app, name, Widget())
    def fail(exc):
        logs.append(str(exc)); finished.set()
    app._fail_step = fail
    real_thread = threading.Thread
    def thread(*args, **kwargs):
        worker = real_thread(*args, **kwargs); threads.append(worker); return worker
    monkeypatch.setattr(gui.threading, "Thread", thread)
    app._retry_provision()
    assert entered.wait(3), logs
    assert threads[0].ident != main_thread and threads[0].is_alive()
    app._cancel_schedule()  # Tk callback only sets the signal; worker owns USB
    release.set()
    assert finished.wait(3), logs
    threads[0].join(3)
    assert not threads[0].is_alive() and device.aborts == 1 and device.commits == 0
    assert any("cancelled before commit" in item for item in logs)
    for function, args in posts: function(*args)


@pytest.mark.parametrize("approved", [False, True])
def test_new_request_ui_reads_current_identity_before_explicit_choice(monkeypatch, approved):
    from flash_gui import gui
    order, done = [], threading.Event()
    identity = SimpleNamespace(mac=MAC, schedule=ambyte_serial.ScheduleIdentity("INSTALLED", OLD_SHA, None, "old-v1"))
    def readback(*a, **kw): order.append("readback"); return identity
    def ask(title, message, **kw):
        assert order == ["readback"] and OLD_SHA in message and MAC in message
        order.append("choice"); return approved
    def install(*a, **kw):
        assert order == ["readback", "choice"] and kw["new_request"] is True
        order.append("admission"); return procedure.VerifyItem("Schedule", True, "verified")
    monkeypatch.setattr(procedure, "service_recovery_identity", readback)
    monkeypatch.setattr(gui.messagebox, "askyesno", ask)
    monkeypatch.setattr(procedure, "install_schedule_script", install)
    app = object.__new__(gui.App)
    app.current_run, app.busy = object(), False
    app._schedule_cancel = threading.Event()
    app._make_context = lambda: object()
    app.log = lambda _: None
    app._set_busy = lambda busy: done.set() if not busy else None
    app._ask_on_main = lambda fn: fn()
    app._set_step = lambda *args: None
    app._post = lambda *args: None
    app._enable_schedule_cancel = lambda: None
    app.new_schedule_btn = SimpleNamespace(configure=lambda *a: None)
    app._fail_step = lambda exc: pytest.fail(str(exc))
    app._retry_new_schedule()
    assert done.wait(3)
    assert order == ["readback", "choice"] + (["admission"] if approved else [])


def test_plain_notice_then_prompt_and_identity_work_on_same_open_port(bench, monkeypatch):
    board = GuiDevice()
    port = GuiSerial(board, bench.clock)
    monkeypatch.setattr(ambyte_serial.serial, 'Serial', lambda: port)
    con = ambyte_serial.AmbyteConsole('bench')
    from flash_gui.service_client import encode_frame, decode_frame
    port.write_timeout = 5
    port.write(encode_frame(1, 'svc.version'))
    notice = port.read(4096)
    assert notice.startswith(b'ambyte> \r\n00000000 ')
    lines = notice.splitlines(keepends=True)
    frame = next(line for line in lines if line.startswith(b'00000000 '))
    assert decode_frame(frame)[1]['reason'] == 'mode_required'
    assert con.wait_prompt(timeout=2)
    assert con.identity().mac == MAC
    assert not board.mode
    con.close()
