# SPDX-License-Identifier: GPL-3.0-only
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import zlib

import pytest

from flash_gui import service_client as svc
from service_device import Clock, Device, Serial, STORE, response

BLOB = b"schedule bytes\n" + bytes(range(128)) * 4
SHA = hashlib.sha256(BLOB).hexdigest()
MAC = "02:00:00:00:00:01"


def client(tmp_path, device=None, clock=None, *, reconnect=None, **kwargs):
    device, clock = device or Device(), clock or Clock()
    port = Serial(device, clock, **kwargs)
    journal = svc.OperationJournal(tmp_path, MAC)
    return svc.ServiceClient(port, journal, reconnect=reconnect, clock=clock, sleep=clock.sleep), device, clock


def upload(c, **kwargs):
    return c.upload_schedule(BLOB, SHA, "a3", "test", **kwargs)


def test_real_journal_precedes_admission_and_wire_bounds(tmp_path):
    c, board, _ = client(tmp_path, split=1)
    def before(a):
        entry = c.journal.load()
        assert entry["store"] == STORE and entry["op"] == a[1]
        assert entry["descriptor"]["sha256"] == SHA
    board.before_admit = before
    result = upload(c)
    assert result["state"] == "succeeded" and result["applied"] is True
    assert board.data == BLOB and board.applies == board.commits == 1
    assert c.journal.load()["phase"] == "complete"
    frames = [line for line in c.transport.sent if line != b"svc\n"]
    assert max(map(len, frames)) == 248
    assert all(line.endswith(b"\n") and b"\r" not in line for line in frames)
    assert c.transport.write_timeout is None  # caller's plain-console settings restored


@pytest.mark.parametrize("action,nth", [("schedule.begin", 1), ("p", 1), ("p", 2), ("p", 4), ("schedule.commit", 1), ("svc.result", 5)])
@pytest.mark.parametrize("where", ["before", "after"])
def test_interrupted_upload_resumes_exact_bytes_and_single_apply(tmp_path, action, nth, where):
    board, clock, ports = Device(), Clock(), []
    def reconnect(remaining):
        assert remaining > 0
        board.mode = board.bound = False  # reopening may reboot the gateway
        port = Serial(board, clock)
        ports.append(port)
        return port
    c, _, _ = client(tmp_path, board, clock, reconnect=reconnect, cut=(action, nth, where))
    first = c.transport
    assert upload(c)["state"] == "succeeded"
    assert board.data == BLOB and board.applies == 1
    assert len(ports) <= 1
    assert all(not line.startswith(b"schedule ") for p in [first, *ports] for line in p.sent)


def test_lost_final_response_recovers_stored_outcome_without_new_commit(tmp_path):
    board, clock = Device(), Clock()
    board.finish_after = 1
    def cut_final(action, value):
        if action == "svc.result" and value.get("state") == "succeeded":
            raise OSError("response lost after terminal durability")
        return value
    c, _, _ = client(tmp_path, board, clock, mutate=cut_final)
    with pytest.raises(svc.TransportError): upload(c)
    assert board.applies == 1
    board.mode = board.bound = False
    resumed, _, _ = client(tmp_path, board, clock)
    assert upload(resumed)["state"] == "succeeded"
    assert board.commits == 1 and board.applies == 1


def test_journal_write_failure_prevents_admission(tmp_path, monkeypatch):
    c, board, _ = client(tmp_path)
    def fail(_fd): raise OSError("disk full")
    monkeypatch.setattr(os, "fsync", fail)
    with pytest.raises(svc.ServiceClientError, match="persist"): upload(c)
    assert board.highest == 0 and board.applies == 0
    assert not any(b"schedule.begin" in line for line in c.transport.sent)


def test_foreign_active_operation_is_reported_without_mutation(tmp_path):
    owner, board, clock = client(tmp_path / "owner", cut=("p", 1, "before"))
    with pytest.raises(svc.TransportError): upload(owner)
    foreign, _, _ = client(tmp_path / "foreign", board, clock)
    with pytest.raises(svc.BusyOperation) as error: upload(foreign)
    assert (error.value.kind, error.value.state) == ("schedule", "staging")
    assert board.commits == board.applies == board.aborts == 0
    assert not foreign.journal.path.exists()


def test_store_change_and_expiry_keep_journal_and_never_readmit(tmp_path):
    c, board, clock = client(tmp_path, cut=("p", 1, "before"))
    with pytest.raises(svc.TransportError): upload(c)
    original = c.journal.path.read_bytes()
    board.store = "2222222222222222"
    changed, _, _ = client(tmp_path, board, clock)
    with pytest.raises(svc.UnknownOutcome, match="store changed"): upload(changed)
    assert changed.journal.path.read_bytes() == original
    board.store = STORE
    board.records.clear()  # watermark remains; result now expired
    expired, _, _ = client(tmp_path, board, clock)
    with pytest.raises(svc.UnknownOutcome, match="expired"): upload(expired)
    assert expired.journal.path.read_bytes() == original
    assert not any(b"schedule.begin" in line for line in expired.transport.sent)


def test_new_mac_never_inherits_port_journal(tmp_path):
    journal = svc.OperationJournal(tmp_path, MAC)
    other = svc.OperationJournal(tmp_path, "02:00:00:00:00:02")
    assert journal.path != other.path and other.load() is None


def test_cancel_before_commit_aborts_own_staging_only(tmp_path):
    c, board, _ = client(tmp_path)
    with pytest.raises(svc.TransferCancelled, match="before commit"):
        upload(c, cancelled=lambda: len(board.data) >= 144)
    assert board.aborts == 1 and board.commits == board.applies == 0
    assert c.journal.load()["phase"] == "complete"


def test_cancel_after_commit_preserves_intent_for_next_client(tmp_path):
    c, board, clock = client(tmp_path)
    with pytest.raises(svc.TransferCancelled, match="continues"):
        upload(c, cancelled=lambda: board.commits > 0)
    assert board.aborts == 0 and c.journal.load()["phase"] == "committing"
    next_client, _, _ = client(tmp_path, board, clock)
    assert upload(next_client)["state"] == "succeeded"
    assert board.applies == board.commits == 1


def test_startup_wait_reuses_staged_operation_until_ready(tmp_path):
    c, board, clock = client(tmp_path)
    board.boot_checks_remaining = 3
    messages = []
    assert upload(c, log=messages.append)["state"] == "succeeded"
    assert len(board.records) == 1 and board.applies == 1
    assert board.commits == 4 and clock() >= .6
    assert sum("startup" in message.lower() for message in messages) == 1


def test_startup_wait_deadline_preserves_full_staging_for_resume(tmp_path):
    c, board, clock = client(tmp_path)
    board.boot_checks_remaining = 100
    with pytest.raises(svc.ServiceClientError, match="startup"):
        upload(c, deadline_s=.5)
    entry = c.journal.load()
    assert entry["phase"] != "complete"
    assert board.records[entry["op"]]["state"] == "staging"
    assert board.data == BLOB and board.applies == board.aborts == 0
    board.boot_checks_remaining = 0
    resumed, _, _ = client(tmp_path, board, clock)
    assert upload(resumed)["op"] == entry["op"]
    assert len(board.records) == 1 and board.applies == 1


def test_deadline_after_last_ack_does_not_blame_startup(tmp_path):
    c, board, clock = client(tmp_path)
    def slow_progress(message):
        if f"Pushed {len(BLOB)}/{len(BLOB)}" in message:
            clock.sleep(.5)
    with pytest.raises(svc.UnknownOutcome, match="Transfer deadline reached"):
        upload(c, deadline_s=.5, log=slow_progress)
    assert board.commits == board.applies == 0 and board.data == BLOB
    assert c.journal.load()["phase"] != "complete"


def test_cancel_while_waiting_for_boot_aborts_staging(tmp_path):
    c, board, _ = client(tmp_path)
    board.boot_checks_remaining = 100
    with pytest.raises(svc.TransferCancelled, match="before commit"):
        upload(c, cancelled=lambda: board.commits > 0)
    assert board.applies == 0 and board.aborts == 1
    assert c.journal.load()["phase"] == "complete"


def test_disconnect_while_booting_reconciles_same_complete_prefix(tmp_path):
    board, clock = Device(), Clock()
    board.boot_checks_remaining = 3
    def reconnect(_remaining):
        board.mode = board.bound = False
        return Serial(board, clock)
    c, _, _ = client(tmp_path, board, clock, reconnect=reconnect, cut=("schedule.commit", 1, "after"))
    assert upload(c)["state"] == "succeeded"
    assert len(board.records) == 1 and board.applies == 1 and board.data == BLOB
    assert c.transport.counts.get("p", 0) == 0  # reconnect reads the full prefix


def test_terminal_busy_is_not_retried_as_startup(tmp_path):
    c, board, _ = client(tmp_path)
    board.outcome, board.error_reason = "failed", "busy"
    with pytest.raises(svc.ServiceError, match="busy"):
        upload(c)
    assert board.commits == 1 and len(board.records) == 1
    assert c.journal.load()["phase"] == "complete"


@pytest.mark.parametrize("state", ["failed", "aborted", "uncertain"])
def test_terminal_nonsuccess_never_becomes_gui_success(tmp_path, state):
    c, board, _ = client(tmp_path)
    board.outcome = state
    with pytest.raises(svc.ServiceClientError): upload(c)
    assert board.applies == 1
    assert c.journal.load()["phase"] == ("uncertain" if state == "uncertain" else "complete")


@pytest.mark.parametrize("line", [
    b'00000001 {"ok":true,"store":"0123456789abcdef"}*00000000\n',
    response(1, dict(ok=True, store=STORE)).replace(b"\n", b"\r\n"),
    b"log " + response(1, dict(ok=True, store=STORE)),
    b"x" * 1044 + b"\n",
])
def test_invalid_physical_frames_are_not_salvaged(line):
    with pytest.raises(svc.ProtocolError): svc.decode_frame(line)


@pytest.mark.parametrize("body", [b'{"ok":true,"ok":false,"store":null}', b'{"ok":NaN,"store":null}',
                                  b'{"ok":1,"store":null}', b'{"ok":true}', b'{"ok":true,"store":123}'])
def test_strict_json_envelope(body):
    prefix = b"00000001 " + body
    with pytest.raises(svc.ProtocolError): svc.decode_frame(prefix + b"*%08x\n" % zlib.crc32(prefix))


def test_invalid_ack_and_descriptor_disagreement_never_commits(tmp_path):
    def wrong(action, value):
        if action == "p": value["offset"] = True
        return value
    c, board, _ = client(tmp_path, mutate=wrong)
    with pytest.raises(svc.ProtocolError): upload(c)
    assert board.commits == board.applies == board.aborts == 0


def test_credential_join_is_once_and_never_logged_or_persisted(tmp_path):
    c, board, _ = client(tmp_path)
    assert c.join_wifi("A3Bench", "wrongpass")["saved"] is True
    assert board.joins == 1
    wire = b"".join(c.transport.sent)
    assert wire.count(b"svc.wifi.join ") == 1
    for path in tmp_path.iterdir():
        assert b"wrongpass" not in path.read_bytes() and b"d3JvbmdwYXNz" not in path.read_bytes()


def test_lost_wifi_admission_response_recovers_without_credential_retry(tmp_path):
    board, clock = Device(), Clock()
    def reconnect(_remaining):
        board.mode = board.bound = False
        return Serial(board, clock)
    c, _, _ = client(tmp_path, board, clock, reconnect=reconnect, cut=("svc.wifi.join", 1, "after"))
    assert c.join_wifi("A3Bench", "wrongpass")["state"] == "succeeded"
    assert board.joins == 1


@pytest.mark.parametrize("bad", ["plain", "crc", "timeout", "reopen"])
def test_credential_probe_failure_prevents_secret_write(tmp_path, bad):
    def raw(action, encoded):
        if action == "svc.bind":
            if bad == "plain": return b"ambyte> plain reply\n"
            if bad == "crc": return encoded[:-3] + b"00\n"
            if bad == "timeout": return response(0, dict(ok=False, store=STORE, reason="session_timeout"))
        return encoded
    c, board, _ = client(tmp_path, raw=raw)
    if bad == "reopen": c.transport.close()
    with pytest.raises(svc.TransportError): c.join_wifi("A3Bench", "wrongpass")
    assert board.joins == 0
    assert not any(b"d3JvbmdwYXNz" in line for line in c.transport.sent)


def test_new_process_reads_saved_intent(tmp_path):
    c, _, _ = client(tmp_path, cut=("p", 1, "before"))
    with pytest.raises(svc.TransportError): upload(c)
    code = "from flash_gui.service_client import OperationJournal; from pathlib import Path; import sys,json; print(json.dumps(OperationJournal(Path(sys.argv[1]),sys.argv[2]).load()))"
    run = subprocess.run([sys.executable, "-c", code, str(tmp_path), MAC], check=True, capture_output=True, text=True)
    entry = json.loads(run.stdout)
    assert entry["op"] == "0000000000000001" and entry["descriptor"]["sha256"] == SHA


def test_corrupt_journal_is_not_replaced(tmp_path):
    c, board, _ = client(tmp_path)
    c.journal.path.write_text('{"schema":1,"schema":1}')
    before = c.journal.path.read_bytes()
    with pytest.raises(svc.ServiceClientError, match="saved transfer intent"): upload(c)
    assert c.journal.path.read_bytes() == before and board.highest == 0


def test_password_reflected_as_unknown_error_reason_is_never_shown(tmp_path):
    def reflection(action, value):
        if action == "svc.wifi.join": return dict(ok=False, store=STORE, reason="wrongpass")
        return value
    c, _, _ = client(tmp_path, mutate=reflection)
    # The join may have been admitted. Its read-only reconciliation is safe;
    # if it succeeds, there is still exactly one credential transmission.
    c.join_wifi("A3Bench", "wrongpass")
    assert sum(b"svc.wifi.join " in line for line in c.transport.sent) == 1


def test_success_without_wifi_evidence_is_rejected(tmp_path):
    def false_success(action, value):
        if action == "svc.result" and value.get("kind") == "wifi": value["saved"] = False
        return value
    c, _, _ = client(tmp_path, mutate=false_success)
    with pytest.raises(svc.ProtocolError, match="evidence"):
        c.join_wifi("A3Bench", "wrongpass")


def test_slow_journal_reprobes_immediately_before_credentials(tmp_path):
    c, board, clock = client(tmp_path)
    save = c.journal.save
    def slow(entry):
        save(entry); clock.sleep(2)
    c.journal.save = slow
    def before(_a):
        assert c._permission[:2] == (c.generation, STORE)
        assert clock() - c._permission[2] <= 1
    board.before_admit = before
    assert c.join_wifi("A3Bench", "wrongpass")["saved"] is True
    assert c.transport.counts["svc.bind"] >= 2


def test_unhashable_result_state_fails_as_protocol_error(tmp_path):
    def bad_state(action, value):
        if action == "svc.result" and value.get("kind"): value["state"] = []
        return value
    c, board, _ = client(tmp_path, mutate=bad_state)
    with pytest.raises(svc.ProtocolError): upload(c)
    assert board.commits == 0


def test_second_process_cannot_replace_pending_intent(tmp_path):
    c, board, _ = client(tmp_path)
    other, _, _ = client(tmp_path, board)
    with c.journal.lease():
        with pytest.raises(svc.ServiceClientError, match="another GUI"):
            upload(other)
        code = """from flash_gui.service_client import OperationJournal,ServiceClientError
from pathlib import Path
import sys
try:
    with OperationJournal(Path(sys.argv[1]),sys.argv[2]).lease():
        raise SystemExit(9)
except ServiceClientError:
    print('locked')
"""
        run = subprocess.run([sys.executable, "-c", code, str(tmp_path), MAC], capture_output=True, text=True)
        assert run.returncode == 0 and run.stdout.strip() == "locked"
    assert board.highest == 0
    assert upload(other)["state"] == "succeeded"


def test_abort_requires_owned_journal_and_repeat_is_idempotent(tmp_path):
    owner, board, clock = client(tmp_path / "owner")
    with pytest.raises(svc.TransferCancelled): upload(owner, cancelled=lambda: len(board.data) >= 144)
    owner.connect()
    owner.abort(owner.journal.load())  # reads the stored aborted outcome
    owner.exit()
    assert board.aborts == 1
    foreign, _, _ = client(tmp_path / "foreign", board, clock)
    foreign.connect()
    with pytest.raises(svc.ServiceClientError, match="owned"):
        foreign.abort(owner.journal.load())
    foreign.exit()
    assert board.aborts == 1


def test_sampled_budget_counters_are_typed_and_preserved(tmp_path, monkeypatch):
    c, _, _ = client(tmp_path)
    c.connect()
    counters = dict(ok=True, store=STORE, free_heap=64000, largest_block=32000, min_heap=11000,
                    cli_stack=4000, worker_stack=5000, runner_stack=3000, dropped_bytes=0,
                    dropped_calls=0, logical_bytes=21536, records=9, log_buffer_bytes=0,
                    min_free_heap=12000, min_largest_block=5000, samples=800, fs_total=1000000, fs_used=100000)
    monkeypatch.setattr(c, "_exchange", lambda _: dict(counters))
    assert c.command("svc.stats") == counters
    counters["min_largest_block"] = True
    with pytest.raises(svc.ProtocolError): c.command("svc.stats")
    c.active = True
    del counters["min_largest_block"]
    with pytest.raises(svc.ProtocolError): c.command("svc.stats")

@pytest.mark.parametrize('kind', ['schedule', 'wifi'])
def test_pending_old_store_never_binds_a_reformatted_instance(tmp_path, kind):
    c, board, clock = client(tmp_path)
    if kind == 'schedule':
        c.transport.cut = ('p', 1, 'before')
        with pytest.raises(svc.TransportError): upload(c)
    else:
        c.transport.cut = ('svc.wifi.join', 1, 'after')
        with pytest.raises(svc.TransportError): c.join_wifi('A3Bench', 'wrongpass')
    journal = c.journal.path.read_bytes()
    board.store, board.mode, board.bound = '2222222222222222', False, False
    board.records.clear(); board.highest = 0
    resumed, _, _ = client(tmp_path, board, clock)
    with pytest.raises(svc.UnknownOutcome, match='store changed'):
        resumed.connect()
    assert resumed.journal.path.read_bytes() == journal
    assert not any(b'svc.bind ' in line or b'svc.wifi.join ' in line or b'schedule.begin ' in line for line in resumed.transport.sent)


@pytest.mark.parametrize('active', [False, {}, {'op': '0' * 16, 'kind': 'schedule', 'state': 'staging'},
    {'op': '1' * 16, 'kind': 'schedule', 'state': 'persisting'},
    {'op': '1' * 16, 'kind': [], 'state': 'staging'},
    {'op': '1' * 16, 'kind': 'wifi', 'state': []},
    {'op': '1' * 16, 'kind': 'wifi', 'state': 'succeeded'},
    {'op': '1' * 16, 'kind': 'wifi', 'state': 'connecting', 'secret': 'synthetic'}])
def test_active_metadata_is_strict_and_does_not_allocate_intent(tmp_path, active):
    def mutate(action, value):
        if action == 'svc.active': value['active'] = active
        return value
    c, board, _ = client(tmp_path, mutate=mutate)
    with pytest.raises(svc.ProtocolError): upload(c)
    assert board.highest == 0 and not c.journal.path.exists()


def test_active_lookup_finds_foreign_op_below_watermark(tmp_path):
    owner, board, clock = client(tmp_path / 'owner', cut=('p', 1, 'before'))
    with pytest.raises(svc.TransportError): upload(owner)
    board.highest = 9
    other, _, _ = client(tmp_path / 'other', board, clock)
    with pytest.raises(svc.BusyOperation) as error: upload(other)
    assert error.value.operation == '0000000000000001'
    assert board.commits == board.aborts == 0 and not other.journal.path.exists()


def test_not_bound_invalidates_session_and_preserves_pending_journal(tmp_path):
    c, board, _ = client(tmp_path, cut=('p', 1, 'before'))
    with pytest.raises(svc.TransportError): upload(c)
    saved = c.journal.path.read_bytes()
    c.transport.is_open = True; c.transport.cut = None
    c.connect()
    c.transport.mutate = lambda a, v: dict(ok=False, store=STORE, reason='not_bound') if a == 'svc.result' else v
    with pytest.raises(svc.ProtocolError, match='not bound'): c.result(c.journal.load()['op'])
    assert not c.active and c._permission is None and c.journal.path.read_bytes() == saved


@pytest.mark.parametrize('name', ['b01_a2_svc_framed.raw', 'r4_v270_svc_framed.raw',
                                'b01_a2_svc_plain.raw', 'r4_v270_svc_plain.raw'])
def test_captured_older_probe_echo_is_unsupported_not_malformed(tmp_path, name):
    capture = (Path(__file__).parent / 'fixtures' / name).read_bytes()
    c, _, _ = client(tmp_path, raw=lambda a, value: capture)
    with pytest.raises(svc.ServiceError) as error:
        if name.endswith('_plain.raw'):
            c.transport.output.extend(capture)
            c._receive(1, allow_plain=True, probe=b'svc\n')
        else:
            c.connect()
    assert error.value.reason == 'unsupported'
    assert c.transport.sent == ([] if name.endswith('_plain.raw') else [svc.encode_frame(1, 'svc.version')])


def test_prompt_prefixed_frame_is_not_classified_as_plain_console():
    line = b'ambyte> ' + response(0, dict(ok=False, store=STORE, reason='mode_required'))
    with pytest.raises(svc.ProtocolError) as error: svc.decode_frame(line)
    assert type(error.value) is svc.ProtocolError


def test_older_service_without_active_keeps_existing_bounded_discovery(tmp_path):
    def old_version(action, value):
        if action == 'svc.version': value['actions'].remove('svc.active')
        return value
    c, board, _ = client(tmp_path, mutate=old_version)
    assert upload(c)['state'] == 'succeeded'
    assert not any(b' svc.active*' in line for line in c.transport.sent)
