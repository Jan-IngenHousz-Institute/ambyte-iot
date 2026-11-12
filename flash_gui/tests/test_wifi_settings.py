# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only
"""Local Wi-Fi changes use physical USB bytes, without cloud provisioning."""
import hashlib

import pytest

from flash_gui import ambyte_serial, config, wifi_settings
from flash_gui.openjii_client import OpenJIIClient
from flash_gui.service_client import OperationJournal, schedule_descriptor
from flash_gui.tests.service_device import Clock, Device, STORE
from flash_gui.tests.test_service_gui import GuiDevice, GuiSerial, MAC


class WifiDevice(GuiDevice):
    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.legacy_result = "ESP_OK"

    def answer(self, body):
        value = Device.answer(self, body)
        if value.get("kind") == "wifi":
            value.update(getattr(self, "wifi_evidence", {}))
        return value

    def console(self, command):
        if command == "cfg get device_name":
            self.plain.append(command)
            return b"device_name = Field_{MAC}\r\nambyte> "
        if command == "status":
            self.plain.append(command)
            return f" - MAC: {self.mac}\r\n - Wi-Fi: disconnected (provisioned: yes)\r\nambyte> ".encode()
        if command.startswith("wifi_join "):
            self.plain.append(command)
            # Use the exact console success/failure syntax, including the echo
            # that must never reach a log or a GUI exception.
            return (f'{command}\r\nwifi_join "Field hub 1": {self.legacy_result}\r\n'
                    + (ambyte_serial.FAILURE_MARK + "\r\n" if self.legacy_result != "ESP_OK" else "")
                    + "ambyte> ").encode()
        return super().console(command)


@pytest.fixture
def usb(monkeypatch, tmp_path):
    clock, handles = Clock(), []
    monkeypatch.setattr(ambyte_serial.time, "time", clock)
    monkeypatch.setattr(ambyte_serial.time, "monotonic", clock)
    monkeypatch.setattr(ambyte_serial.time, "sleep", clock.sleep)
    monkeypatch.setattr(config, "SERVICE_OPERATIONS_DIR", tmp_path)
    device = WifiDevice()
    def open_serial():
        handle = GuiSerial(device, clock)
        handles.append(handle)
        return handle
    monkeypatch.setattr(ambyte_serial.serial, "Serial", open_serial)
    def no_cloud(*args, **kwargs):
        pytest.fail("Local Wi-Fi settings must not contact openJII")
    monkeypatch.setattr(OpenJIIClient, "_request", no_cloud)
    return device, handles, tmp_path


def test_registered_board_can_update_wifi_without_account_or_reflash(usb):
    device, handles, directory = usb
    info = wifi_settings.read_device("bench")
    assert info.mac == MAC and info.name == "Field_" + MAC
    result = wifi_settings.update_wifi(info, "Field hub 1", "synthetic-password")
    assert result.connected and device.joins == 1
    assert device.commits == device.applies == 0
    assert not any(cmd.startswith(("cfg set", "reboot", "schedule", "wifi_join")) for cmd in device.plain)
    assert all(not handle.is_open for handle in handles)
    assert "synthetic-password" not in "".join(p.read_text() for p in directory.glob("*.json"))


@pytest.mark.parametrize("identity", [True, False])
def test_older_firmware_uses_persistent_wifi_join(usb, identity):
    device, handles, _ = usb
    device.svc, device.has_identity = False, identity
    info = wifi_settings.read_device("bench")
    result = wifi_settings.update_wifi(info, "Field hub 1", "synthetic-password")
    assert result.connected
    assert device.plain.count('wifi_join "Field hub 1" "synthetic-password"') == 1
    assert not device.joins
    assert all(not handle.is_open for handle in handles)


def test_board_swap_is_rejected_before_credentials(usb):
    device, _, _ = usb
    info = wifi_settings.read_device("bench")
    device.mac = "00:11:22:33:44:55"
    with pytest.raises(wifi_settings.WifiSettingsError, match="changed"):
        wifi_settings.update_wifi(info, "Field hub 1", "synthetic-password")
    assert not device.joins
    assert not any(cmd.startswith("wifi_join") for cmd in device.plain)


@pytest.mark.parametrize("ssid,password", [("", "synthetic-password"), ("x" * 32, "synthetic-password"),
    ("Field\nhub", "synthetic-password"), ("Field hub 1", "short"),
    ("Field hub 1", "secret\ncommand"), ("Field hub 1", "x" * 64)])
def test_invalid_credentials_never_open_usb(usb, ssid, password):
    _, handles, _ = usb
    info = wifi_settings.read_device("bench")
    before = len(handles)
    with pytest.raises(wifi_settings.WifiSettingsError):
        wifi_settings.update_wifi(info, ssid, password)
    assert len(handles) == before


def test_legacy_error_does_not_expose_password(usb):
    device, _, _ = usb
    device.svc, device.legacy_result = False, "WIFI_MANAGER_ERR_AUTH_REJECTED"
    info = wifi_settings.read_device("bench")
    with pytest.raises(wifi_settings.WifiSettingsError, match="AUTH_REJECTED") as error:
        wifi_settings.update_wifi(info, "Field hub 1", "synthetic-password")
    assert "synthetic-password" not in str(error.value)


def test_legacy_timeout_hides_command_and_never_retries(usb, monkeypatch):
    device, _, _ = usb
    device.svc = False
    info = wifi_settings.read_device("bench")
    original = ambyte_serial.AmbyteConsole.command
    sent = []
    def command(self, cmd, *args, **kwargs):
        if cmd.startswith("wifi_join"):
            sent.append(cmd)
            raise ambyte_serial.ConsoleError(cmd + " got no prompt back")
        return original(self, cmd, *args, **kwargs)
    monkeypatch.setattr(ambyte_serial.AmbyteConsole, "command", command)
    with pytest.raises(wifi_settings.WifiSettingsError, match="unknown") as error:
        wifi_settings.update_wifi(info, "Field hub 1", "synthetic-password")
    assert "synthetic-password" not in str(error.value) and len(sent) == 1


@pytest.mark.parametrize("password,argument", [("", '""'),
    ('q"u\\ote pass', '"q\\"u\\\\ote pass"')])
def test_legacy_quotes_password_and_supports_open_network(usb, password, argument):
    device, _, _ = usb
    device.svc = False
    info = wifi_settings.read_device("bench")
    assert wifi_settings.update_wifi(info, "Field hub 1", password).connected
    assert f'wifi_join "Field hub 1" {argument}' in device.plain


def test_legacy_physical_crcrlf_result_is_recognized(usb):
    device, _, _ = usb
    device.svc = False
    original = device.console
    device.console = lambda command: original(command).replace(b"\r\n", b"\r\r\n")
    info = wifi_settings.read_device("bench")
    assert wifi_settings.update_wifi(info, "Field hub 1", "synthetic-password").connected


def test_legacy_password_echo_cannot_be_mistaken_for_a_failure(usb):
    device, _, _ = usb
    device.svc = False
    info = wifi_settings.read_device("bench")
    assert wifi_settings.update_wifi(info, "Field hub 1", ambyte_serial.FAILURE_MARK).connected


def test_lost_service_reply_recovers_by_id_without_resending_password(usb, monkeypatch):
    device, handles, _ = usb
    info = wifi_settings.read_device("bench")
    factory = ambyte_serial.serial.Serial
    def interrupted():
        handle = factory()
        handle.cut = ("svc.wifi.join", 1, "after")
        return handle
    monkeypatch.setattr(ambyte_serial.serial, "Serial", interrupted)
    with pytest.raises(wifi_settings.WifiSettingsError):
        wifi_settings.update_wifi(info, "Field hub 1", "synthetic-password")
    assert device.joins == 1
    monkeypatch.setattr(ambyte_serial.serial, "Serial", factory)
    assert wifi_settings.recover_wifi(info).connected
    assert device.joins == 1
    assert all(not h.is_open for h in handles)


def test_failed_service_never_downgrades_to_console_join(usb, monkeypatch):
    device, _, _ = usb
    info = wifi_settings.read_device("bench")
    factory = ambyte_serial.serial.Serial
    def corrupt():
        handle = factory()
        handle.raw = lambda action, frame: frame[:-2] + b"x\n" if action == "svc.version" else frame
        return handle
    monkeypatch.setattr(ambyte_serial.serial, "Serial", corrupt)
    with pytest.raises(wifi_settings.WifiSettingsError):
        wifi_settings.update_wifi(info, "Field hub 1", "synthetic-password")
    assert not device.joins
    assert not any(cmd.startswith("wifi_join") for cmd in device.plain)


def test_service_failed_connection_is_not_reported_as_success(usb):
    device, _, _ = usb
    device.outcome, device.error_reason = "failed", "wifi_auth_rejected"
    info = wifi_settings.read_device("bench")
    with pytest.raises(wifi_settings.WifiSettingsError, match="credentials"):
        wifi_settings.update_wifi(info, "Field hub 1", "synthetic-password")
    assert device.joins == 1


def test_unknown_saved_result_requires_explicit_new_attempt(usb):
    device, _, directory = usb
    device.outcome, device.error_reason = "uncertain", "interrupted"
    info = wifi_settings.read_device("bench")
    with pytest.raises(wifi_settings.WifiSettingsError) as error:
        wifi_settings.update_wifi(info, "Field hub 1", "synthetic-password")
    assert error.value.uncertain
    with pytest.raises(wifi_settings.WifiSettingsError):
        wifi_settings.update_wifi(info, "Field hub 1", "different-password")
    assert device.joins == 1
    device.outcome = "succeeded"
    assert wifi_settings.update_wifi(info, "Field hub 1", "different-password", new_request=True).connected
    assert device.joins == 2
    assert list(directory.glob("*.previous.json"))


def test_discovery_failure_does_not_scan_other_ports(usb, monkeypatch):
    _, handles, _ = usb
    monkeypatch.setattr(ambyte_serial.AmbyteConsole, "wait_prompt", lambda *a, **kw: False)
    monkeypatch.setattr(ambyte_serial, "esp_jtag_ports", lambda: pytest.fail("must not scan other devices"))
    with pytest.raises(wifi_settings.WifiSettingsError, match="selected port"):
        wifi_settings.read_device("operator-selected")
    assert len(handles) == 1 and not handles[0].is_open


@pytest.mark.parametrize("phase", ["admitted", "committing", "uncertain"])
@pytest.mark.parametrize("action", ["join", "new_join", "recover"])
def test_wifi_controls_preserve_unresolved_schedule_intent(usb, phase, action):
    device, _, directory = usb
    info = wifi_settings.read_device("bench")
    journal = OperationJournal(directory, MAC)
    entry = dict(schema=1, mac=MAC, store=STORE, op="0000000000000001",
                 descriptor=schedule_descriptor(b"test", hashlib.sha256(b"test").hexdigest(), "test-v1", "2.6.2", None), phase=phase)
    journal.save(entry)
    before = journal.path.read_bytes()
    with pytest.raises(wifi_settings.WifiSettingsError, match="schedule controls") as error:
        if action == "recover":
            wifi_settings.recover_wifi(info)
        else:
            wifi_settings.update_wifi(info, "Field hub 1", "synthetic-password", new_request=action == "new_join")
    assert not error.value.uncertain
    assert journal.path.read_bytes() == before
    assert not list(directory.glob("*.previous.json")) and not device.joins


def test_failed_trial_shows_rollback_evidence_and_allows_correction(usb):
    device, _, _ = usb
    device.outcome, device.error_reason = "failed", "wifi_auth_rejected"
    device.wifi_evidence = dict(saved=False, connected=False, restored=True,
                               retrying=True, restoration_connected=False)
    info = wifi_settings.read_device("bench")
    with pytest.raises(wifi_settings.WifiSettingsError) as error:
        wifi_settings.update_wifi(info, "Field hub 1", "synthetic-password")
    message = str(error.value)
    assert "Previous network restored" in message
    assert "retrying" in message and "not saved" in message
    assert "Check saved result" not in message and "wifi_auth_rejected" not in message
    assert not error.value.uncertain


def test_missing_saved_result_has_no_circular_recovery_advice(usb):
    info = wifi_settings.read_device("bench")
    with pytest.raises(wifi_settings.WifiSettingsError) as error:
        wifi_settings.recover_wifi(info)
    assert "No saved Wi-Fi" in str(error.value)
    assert "Check saved result" not in str(error.value)


def _pending_schedule(usb, state):
    device, _, directory = usb
    info = wifi_settings.read_device("bench")
    descriptor = schedule_descriptor(b"test", hashlib.sha256(b"test").hexdigest(), "test-v1", "2.6.2", None)
    entry = dict(schema=1, mac=MAC, store=STORE, op="0000000000000001", descriptor=descriptor, phase="committing")
    journal = OperationJournal(directory, MAC)
    journal.save(entry)
    device.bound = True
    device.answer(f"schedule.begin {entry['op']} 00000004 {descriptor['sha256']} test-v1 2.6.2 -")
    record = device.records[entry["op"]]
    record.update(state=state, offset=4, applied=True,
                  reason="interrupted" if state == "uncertain" else "compile_failed" if state == "failed" else "")
    return info, journal, entry


@pytest.mark.parametrize("state", ["succeeded", "failed", "aborted"])
def test_schedule_result_can_be_reconciled_after_app_restart_without_onboarding(usb, state):
    info, journal, _ = _pending_schedule(usb, state)
    result = wifi_settings.check_saved_result(info)
    assert result.state == state and result.action is None
    assert journal.load()["phase"] == "complete"
    assert "not rechecked" in result.message
    assert usb[0].joins == usb[0].commits == 0


def test_own_staged_schedule_can_be_explicitly_cancelled(usb):
    info, journal, _ = _pending_schedule(usb, "staging")
    result = wifi_settings.check_saved_result(info)
    assert result.action == "cancel" and not usb[0].aborts
    resolved = wifi_settings.resolve_saved_schedule(info, result)
    assert resolved.state == "aborted" and resolved.action is None
    assert journal.load()["phase"] == "complete" and usb[0].aborts == 1


@pytest.mark.parametrize("changed_store", [False, True])
def test_uncertain_or_changed_store_schedule_requires_explicit_archive(usb, changed_store):
    info, journal, entry = _pending_schedule(usb, "uncertain")
    if changed_store:
        usb[0].store = "fedcba9876543210"
    before = journal.path.read_bytes()
    result = wifi_settings.check_saved_result(info)
    assert result.action == "archive" and journal.path.read_bytes() == before
    resolved = wifi_settings.resolve_saved_schedule(info, result)
    assert resolved.state == "archived" and journal.load() is None
    assert journal.path.with_suffix(".previous.json").read_bytes() == before


def test_recovery_of_expired_wifi_result_points_to_explicit_new_attempt(usb):
    device, _, _ = usb
    device.outcome, device.error_reason = "uncertain", "interrupted"
    info = wifi_settings.read_device("bench")
    with pytest.raises(wifi_settings.WifiSettingsError):
        wifi_settings.update_wifi(info, "Field hub 1", "synthetic-password")
    device.records.clear()
    with pytest.raises(wifi_settings.WifiSettingsError) as error:
        wifi_settings.check_saved_result(info)
    assert "Start new Wi-Fi attempt" in str(error.value)
    assert "Use Check saved result" not in str(error.value)
