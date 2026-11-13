# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only
"""Actual signed-image parsing and console bytes, with no physical flash."""
from pathlib import Path
from contextlib import redirect_stdout
from types import SimpleNamespace
from unittest.mock import Mock

import pytest
from cryptography.hazmat.primitives.serialization import load_pem_private_key

from flash_gui import ambyte_serial, esptool_ops, firmware_update, gui
from flash_gui.firmware_metadata import key_id, sign_image
from flash_gui.tests.test_signed_images import image
from flash_gui.tests.test_wifi_settings import usb  # noqa: F401 - shared byte-level USB fixture
from flash_gui.tests.test_service_gui import MAC

ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture(scope="module")
def signer():
    return load_pem_private_key((ROOT / "tools/keys/UNSAFE_BENCH_ONLY_firmware.pem").read_bytes(), None)


@pytest.fixture
def bench(usb, tmp_path, monkeypatch, signer):
    device, handles, _ = usb
    device.version = "2.8.0"
    values = dict(mqtt_uri="mqtts://example.test:8883", mqtt_client_id="AMBYTE_{MAC}",
                  mqtt_topic_root="experiment/data_ingest/v1/example/ambyte/v1.0/AMBYTE_{MAC}",
                  command_topic="device/scripts/v1/Ambyte/2/AMBYTE_{MAC}",
                  status_topic="example/status", device_name="Field_{MAC}", timezone="Europe/Amsterdam")
    original = device.console
    def console(command):
        if command.startswith("cfg get "):
            key = command.split()[2]
            device.plain.append(command)
            return f"{key} = {values[key]}\r\nambyte> ".encode()
        value = original(command)
        if command == "identity":
            value = value.replace(b'"fw": "2.6.2"', f'"fw": "{device.version}"'.encode())
        return value
    device.console = console
    monkeypatch.setattr(firmware_update, "RELEASE_KEY_ID", key_id(signer))
    files = []
    for offset, filename, data in [
            (0, "bootloader.bin", b"bootloader"), (0x8000, "partitions.bin", b"partition table"),
            (0xF000, "ota.bin", b"ota selection"),
            (0x20000, "app.bin", sign_image(image(version="2.12.0"), signer))]:
        path = tmp_path / filename
        path.write_bytes(data)
        files.append((offset, path))
    release = SimpleNamespace(version="2.12.0", flash_files=files, flash_settings={})
    writes, logs = [], []
    def flash(port, images, settings, **options):
        writes.append((port, images, options))
        device.version = release.version
    monkeypatch.setattr(esptool_ops, "flash_images", flash)
    return SimpleNamespace(device=device, handles=handles, values=values, release=release,
                           writes=writes, logs=logs, signer=signer)


def test_default_update_preserves_identity_and_filesystems_without_openjii(bench):
    result = firmware_update.update_firmware("bench", bench.release, log=bench.logs.append)
    assert result.changed and result.mac == MAC and result.firmware == "2.12.0"
    assert len(bench.writes) == 1
    _, images, options = bench.writes[0]
    assert {off for off, _ in images} == {0, 0x8000, 0xF000, 0x20000}
    assert options["expected_mac"] == MAC and options["expected_partition_table"] == b"partition table"
    assert not any(cmd.startswith(("cfg set", "wifi_join", "schedule ", "rtc set")) for cmd in bench.device.plain)
    assert not bench.device.commits and not bench.device.joins
    assert all(not h.is_open for h in bench.handles)


def test_current_version_skips_flash_and_certificate_work(bench):
    bench.device.version = "2.12.0"
    result = firmware_update.update_firmware("bench", bench.release, log=bench.logs.append)
    assert not result.changed and not bench.writes


@pytest.mark.parametrize("problem", ["nvs", "littlefs", "oversize", "version", "signer", "unsigned", "downgrade", "unconfigured"])
def test_refusals_never_write_or_fall_back_to_provisioning(bench, monkeypatch, problem):
    if problem in {"nvs", "littlefs"}:
        bench.release.flash_files.append((0x9000 if problem == "nvs" else 0x620000, bench.release.flash_files[0][1]))
    elif problem == "oversize":
        bench.release.flash_files[0][1].write_bytes(b"x" * (0x8000 + 1))
    elif problem == "version":
        bench.release.version = "2.12.1"
    elif problem == "signer":
        monkeypatch.setattr(firmware_update, "RELEASE_KEY_ID", "untrusted")
    elif problem == "unsigned":
        dict(bench.release.flash_files)[0x20000].write_bytes(image(version="2.12.0"))
    elif problem == "downgrade":
        bench.device.version = "2.13.0"
    elif problem == "unconfigured":
        bench.values["mqtt_client_id"] = ""
    with pytest.raises(firmware_update.FirmwareUpdateError):
        firmware_update.update_firmware("bench", bench.release, log=bench.logs.append)
    assert not bench.writes
    assert not any(cmd.startswith("cfg set") for cmd in bench.device.plain)


def test_post_boot_configuration_drift_is_reported_without_repair(bench, monkeypatch):
    def flash(*args, **kwargs):
        bench.device.version = "2.12.0"
        bench.values["status_topic"] = "changed"
    monkeypatch.setattr(esptool_ops, "flash_images", flash)
    with pytest.raises(firmware_update.FirmwareUpdateError, match="status_topic"):
        firmware_update.update_firmware("bench", bench.release, log=bench.logs.append)
    assert not any(cmd.startswith("cfg set") for cmd in bench.device.plain)


def test_packaged_release_pin_matches_reviewed_repository_pin():
    assert firmware_update.RELEASE_KEY_ID == (ROOT / "tools/keys/release_firmware.keyid").read_text().strip()
    assert firmware_update.FIRST_SIGNED_RELEASE == tuple(int(v) for v in (ROOT / "tools/keys/first_signed_release.version").read_text().strip().split("."))


@pytest.mark.parametrize("mismatch", ["mac", "partitions", None])
def test_chip_checks_run_on_the_writing_session_before_any_erase(tmp_path, monkeypatch, mismatch):
    path = tmp_path / "boot.bin"
    path.write_bytes(b"boot")
    esp = SimpleNamespace(read_mac=lambda *_: bytes.fromhex("001122334455") if mismatch == "mac" else bytes.fromhex("AABBCCDDEEFF"),
                          read_flash=lambda *_: b"different" if mismatch == "partitions" else b"table")
    write, reset, close = Mock(), Mock(), Mock()
    monkeypatch.setattr(esptool_ops, "_session", lambda *a: esp)
    monkeypatch.setattr(esptool_ops, "attach_flash", lambda *a: None)
    monkeypatch.setattr(esptool_ops, "write_flash", write)
    monkeypatch.setattr(esptool_ops, "_reset_to_app", reset)
    monkeypatch.setattr(esptool_ops, "_close_port", close)
    if mismatch:
        with pytest.raises(esptool_ops.EsptoolError):
            esptool_ops.flash_images("bench", [(0, path)], {}, expected_mac=MAC, expected_partition_table=b"table")
        write.assert_not_called()
    else:
        esptool_ops.flash_images("bench", [(0, path)], {}, expected_mac=MAC, expected_partition_table=b"table")
        write.assert_called_once()
    reset.assert_called_once_with(esp)
    close.assert_called_once_with(esp)


def test_primary_firmware_action_needs_no_api_key_experiment_or_schedule(monkeypatch):
    calls, posted = [], []
    app = SimpleNamespace(busy=False, client=None, _confirm_timezone=lambda status: False, release=object(), _selected_port=lambda: "bench",
                          _reset_steps=lambda: None, _set_busy=calls.append,
                          result_var=SimpleNamespace(set=calls.append), log=calls.append,
                          _post=lambda fn, *args: posted.append((fn, args)))
    class Thread:
        def __init__(self, *, target, daemon): self.target = target
        def start(self): self.target()
    monkeypatch.setattr(gui.threading, "Thread", Thread)
    monkeypatch.setattr(firmware_update, "update_firmware", lambda *a, **kw: firmware_update.FirmwareUpdateResult(MAC, "2.12.0", False))
    gui.App._update_firmware(app)
    assert app.busy and app.current_run is None and len(posted) == 1
    fn, args = posted[0]
    fn(*args)
    assert False in calls and any("no flash needed" in v for v in calls if isinstance(v, str))


def test_terminal_logger_does_not_reenter_esptool_capture(capsys):
    tee = esptool_ops._Tee(print)
    with redirect_stdout(tee):
        print("Connecting...")
    assert capsys.readouterr().out == "Connecting...\n"
    assert tee.getvalue() == "Connecting...\n"


def test_pre_flash_discovery_waits_for_loaded_schedule_identity(bench):
    original = bench.device.console
    observed = []
    def console(command):
        if command == "identity":
            observed.append(command)
            bench.device.loaded_source = "NONE" if len(observed) < 3 else "INSTALLED"
        return original(command)
    bench.device.console = console
    firmware_update.update_firmware("bench", bench.release, log=bench.logs.append)
    assert len(bench.writes) == 1
    assert any("Pre-update schedule: INSTALLED" in line for line in bench.logs)


def test_unavailable_pre_flash_schedule_never_writes_firmware(bench):
    bench.device.loaded_source = "NONE"
    with pytest.raises(firmware_update.FirmwareUpdateError, match="No firmware was written"):
        firmware_update.update_firmware("bench", bench.release, log=bench.logs.append)
    assert not bench.writes


def automatic_timezone_pair():
    from dataclasses import replace
    from flash_gui.tests.test_identity import IDENTITY
    before = firmware_update.ConfiguredDevice('bench', IDENTITY,
        {key: 'Europe/Berlin' if key == 'timezone' else key for key in firmware_update.CONFIG_KEYS})
    zone = ambyte_serial.DeviceTimezone('Australia/Darwin','Australia/Darwin','auto','Australia/Darwin',34200)
    after = replace(before, settings=dict(before.settings, timezone='Australia/Darwin'),
        identity=replace(IDENTITY, location=ambyte_serial.DeviceLocation(True,-25.3444,131.0369), timezone=zone))
    return before, after


def test_verified_firmware_auto_timezone_change_is_separate_success():
    before,after=automatic_timezone_pair()
    changed,note=firmware_update._configuration_changes(before,after)
    assert changed==[] and 'Timezone updated by the Ambyte: Europe/Berlin → Australia/Darwin' in note


@pytest.mark.parametrize('field,value', [('source','manual'), ('configured','Europe/Berlin'),
    ('active','Europe/Berlin'), ('resolved','Europe/Berlin'), ('resolved',None)])
def test_unproven_timezone_change_is_a_visible_warning(field,value):
    from dataclasses import replace
    before,after=automatic_timezone_pair()
    after=replace(after,identity=replace(after.identity,timezone=replace(after.identity.timezone,**{field:value})))
    changed,note=firmware_update._configuration_changes(before,after)
    assert not changed and 'Timezone needs attention: Europe/Berlin → Australia/Darwin' in note


@pytest.mark.parametrize('location', [None, ambyte_serial.DeviceLocation(False,None,None)])
def test_missing_effective_location_makes_timezone_drift_a_warning(location):
    from dataclasses import replace
    before,after=automatic_timezone_pair()
    after=replace(after,identity=replace(after.identity,location=location))
    changed,note=firmware_update._configuration_changes(before,after)
    assert not changed and 'Timezone needs attention: Europe/Berlin → Australia/Darwin' in note


def test_auto_timezone_does_not_excuse_other_config_changes():
    from dataclasses import replace
    before,after=automatic_timezone_pair()
    after=replace(after,settings=dict(after.settings, mqtt_topic_root='unexpected'))
    changes,note=firmware_update._configuration_changes(before,after)
    assert changes==['mqtt_topic_root'] and 'Timezone updated by the Ambyte' in note


def test_verified_device_change_is_informational_independent_of_the_host_map(monkeypatch):
    from flash_gui import tz_geo
    before,after=automatic_timezone_pair()
    monkeypatch.setattr(tz_geo,'zone_for',lambda *a:'Australia/Adelaide')
    changed,note=firmware_update._configuration_changes(before,after)
    assert not changed and 'Timezone updated by the Ambyte' in note


def test_workbook_check_is_after_successful_postboot_verify_and_cannot_change_update_success(bench,monkeypatch):
    from flash_gui import maintenance
    calls=[]
    def check(port,mac,client,**kw):
        assert bench.writes and bench.device.version=='2.12.0'
        calls.append((port,mac,client))
        return maintenance.MaintenanceResult('Workbook: not checked: inaccessible experiment')
    monkeypatch.setattr(maintenance,'run',check)
    client=object()
    result=firmware_update.update_firmware('bench',bench.release,client=client,log=bench.logs.append)
    assert result.changed and 'inaccessible experiment' in result.maintenance.text
    assert calls==[('bench',MAC,client)]
