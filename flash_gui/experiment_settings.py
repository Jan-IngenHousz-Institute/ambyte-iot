# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only
"""Existing-device experiment maintenance; never register or issue credentials.

The destination workbook is authorized by its read endpoints. Device registry
visibility/bindings are not an authorization gate for the wildcard ingest
policy. Broker/certificate ownership is preserved, not guessed from API login.
Queued records deliberately follow the current topic/provenance at publication.
"""
from contextlib import contextmanager
from dataclasses import dataclass, replace
from types import SimpleNamespace
from urllib.parse import urlsplit
from uuid import UUID
import re
import time

from . import ambyte_serial, config, firmware_update, procedure, wifi_settings
from .openjii_client import OpenJIIError
from .service_client import OperationJournal, ServiceClient, ServiceClientError, schedule_descriptor


class ExperimentSettingsError(RuntimeError):
    pass


@dataclass(frozen=True)
class DeviceTarget:
    device: firmware_update.ConfiguredDevice
    broker: str
    experiment_id: str | None
    environment: str | None
    timezone_result: str | None = None

    @property
    def mac(self):
        return self.device.identity.mac


_TOPIC = re.compile(r'experiment/data_ingest/v1/([0-9a-fA-F-]{36})/([A-Za-z0-9_.:-]+)/([A-Za-z0-9_.:-]+)/([A-Za-z0-9_.:{}-]+)')


def _uuid(value):
    try:
        canonical = str(UUID(value))
    except (ValueError, TypeError, AttributeError):
        raise ExperimentSettingsError("Experiment ID must be a canonical UUID.") from None
    if canonical != value:
        raise ExperimentSettingsError("Experiment ID must be a canonical lowercase UUID.")
    return canonical


def topic_for_experiment(raw, experiment_id):
    _uuid(experiment_id)
    # This deployed migration rewrites the status topic at every boot. Refuse
    # its old destination rather than report a success that won't survive boot.
    if "a3b865d1" in experiment_id:
        raise ExperimentSettingsError("This experiment ID triggers a legacy firmware status-topic migration and cannot be selected by maintenance.")
    match = _TOPIC.fullmatch(raw or "")
    if not match:
        raise ExperimentSettingsError("The stored topic is not a supported seven-segment ingest topic; no topic was changed.")
    _uuid(match[1])
    topic = f"experiment/data_ingest/v1/{experiment_id}/{'/'.join(match.groups()[1:])}"
    if len(('cfg set status_topic ' + topic + '/status\n').encode()) > 255:
        raise ExperimentSettingsError("The destination topic exceeds the firmware's buffer.")
    return topic


def _target(console, endpoints):
    device = firmware_update._snapshot(console)
    device = firmware_update._settled_schedule(console, device, lambda text: None, before_flash=True)
    broker = urlsplit(device.settings['mqtt_uri']).hostname.lower()
    matches = [env for env, host in endpoints.items() if env in config.ENVIRONMENTS and host == broker]
    match = _TOPIC.fullmatch(device.settings['mqtt_topic_root'] or '')
    experiment = match[1] if match else None
    return DeviceTarget(device, broker, experiment, matches[0] if len(matches) == 1 else None)


def read_target(port, endpoints=None):
    try:
        with wifi_settings._selected_console(port) as console:
            return _target(console, endpoints or {})
    except (OSError, ambyte_serial.ConsoleError, firmware_update.FirmwareUpdateError, wifi_settings.WifiSettingsError) as exc:
        raise ExperimentSettingsError(str(exc)) from None


def cloud_status(target, client):
    """Optional registry evidence only; never a requirement to change topics."""
    device = client.find_device(target.mac)
    if not device or device.get('thingName') != target.device.identity.thing_name:
        return f"{client.env.key} registry: no visible matching client. Device credentials remain unverified."
    connectivity = device.get('connectivity') or {}
    state = {True: 'connected', False: 'disconnected'}.get(connectivity.get('connected'), 'unknown')
    return (f"{client.env.key} registry: {device.get('status', 'unknown')}, connection {state}. "
            "This does not compare the certificate stored on the USB device.")


def _same_device_config(first, second, *, schedule=True):
    """Stable maintenance guards exclude device-managed location/timezone.

    Online estimate age and effective position can change between consecutive
    identity reads. They cannot invalidate workbook or topic maintenance; MAC,
    running image, service capability, the loaded workbook and other persistent
    configuration still guard every write.
    """
    a, b = first.identity, second.identity
    keys = (key for key in firmware_update.CONFIG_KEYS if key != 'timezone')
    return (a.mac == b.mac and a.thing_name == b.thing_name and a.fw == b.fw
            and a.capabilities == b.capabilities and a.storage == b.storage and a.signed == b.signed
            and (not schedule or a.schedule == b.schedule)
            and all(first.settings[key] == second.settings[key] for key in keys))


def _checked(console, expected):
    current = _target(console, {})
    if not _same_device_config(current.device, expected.device):
        # Port stays bound but reads may be separated by an API lookup. Stable
        # configuration and loaded-program fields must still match the preview.
        raise ExperimentSettingsError("Device identity, configuration or workbook changed. Read the device destination again.")
    return current


@contextmanager
def _after_reboot(previous_port, mac):
    # Let the old USB enumeration disappear before opening its replacement.
    # Enumeration is read-only; never open an unrelated port to identify it.
    deadline = time.monotonic() + 180
    time.sleep(2)
    while time.monotonic() < deadline:
        console = None
        try:
            ports = [p.device for p in ambyte_serial.list_ports.comports()
                     if (p.serial_number or '').replace(':', '').upper() == mac.replace(':', '').upper()]
            if len(ports) > 1:
                raise ExperimentSettingsError("More than one USB port reports the expected serial. Select the device again.")
            if not ports:
                time.sleep(1)
                continue
            console = ambyte_serial.AmbyteConsole(ports[0])
            if not console.wait_prompt(min(45, max(1, deadline-time.monotonic())), recover_service=True):
                raise wifi_settings.WifiSettingsError("The same Ambyte is still booting.")
            if console.identity().mac != mac:
                raise ExperimentSettingsError("The USB serial reports a different Ambyte MAC after reboot.")
        except (OSError, ambyte_serial.ConsoleError, wifi_settings.WifiSettingsError):
            if console is not None:
                console.close()
            time.sleep(1)
            continue
        except Exception:
            if console is not None:
                console.close()
            raise
        try:
            yield console
        finally:
            console.close()
        return
    raise ExperimentSettingsError("The same Ambyte did not return after reboot, or its USB driver did not report a MAC serial. "
                                  "Refresh ports and read the destination again; no provisioning was attempted.")


def workbook_source(client, experiment_id, firmware, *, log=lambda text: None):
    """Resolve only destination read endpoints and compile for the running image."""
    try:
        programming = client.resolve_programming(experiment_id, fw_version=firmware, log=log)
        if programming is None:
            raise ExperimentSettingsError("The experiment has no pinned Ambyte workbook programming cell.")
        return procedure.schedule_source(SimpleNamespace(programming=programming, schedule_script=None,
                                        release=SimpleNamespace(version=firmware)), log=log)
    except (OpenJIIError, procedure.ProcedureError) as exc:
        raise ExperimentSettingsError(f"No device settings were changed. {exc}") from None


def install_workbook(console, expected, source, journal, *, log=lambda text: None):
    """Shared journaled install, with loaded identity and preserved config proof.

    The caller holds the MAC lease. ServiceClient checks foreign operations and
    resumes only matching saved intent; an automatic check never archives an
    unresolved operation just to make progress.
    """
    descriptor = schedule_descriptor(source.blob, source.sha256, source.script_version,
                                     source.built_against_fw, source.workbook_version_id)
    entry = journal.load()
    if entry and entry['phase'] != 'complete' and entry['descriptor'] != descriptor:
        raise ExperimentSettingsError("Resolve the saved operation using Wi-Fi settings → Check saved result before installing a workbook.")
    current = firmware_update._snapshot(console)
    current = firmware_update._settled_schedule(console, current, log, before_flash=True)
    if not _same_device_config(current, expected):
        raise ExperimentSettingsError("Device identity, configuration or workbook changed before installation. Read the device again.")
    service = ServiceClient(console._ser, journal)
    service.upload_schedule(source.blob, source.sha256, source.script_version,
                            source.built_against_fw, source.workbook_version_id, log=log)
    if not console.wait_prompt(45):
        raise ExperimentSettingsError("The device did not return to its console after workbook installation.")
    current = firmware_update._snapshot(console)
    current = firmware_update._settled_schedule(console, current, log, before_flash=True)
    schedule = current.identity.schedule
    if (schedule.source != 'INSTALLED' or schedule.sha256 != source.sha256
            or schedule.workbook_version_id != source.workbook_version_id):
        raise ExperimentSettingsError("The destination workbook was not confirmed active after installation.")
    if not _same_device_config(current, expected, schedule=False):
        raise ExperimentSettingsError("The device configuration changed during workbook installation.")
    return current


def change_experiment(expected, client, experiment_id, *, allow_unknown_environment=False, log=lambda text: None):
    if expected.environment and expected.environment != client.env.key:
        raise ExperimentSettingsError("The device broker belongs to a different environment. Experiment maintenance preserves its broker and credentials.")
    if not expected.environment and not allow_unknown_environment:
        raise ExperimentSettingsError("The broker environment is unverified. Explicitly confirm keeping this broker before continuing.")
    new_root = topic_for_experiment(expected.device.settings['mqtt_topic_root'], experiment_id)
    if 'svc' not in expected.device.identity.capabilities:
        raise ExperimentSettingsError("Experiment maintenance requires service-capable firmware (v2.9.0+). Update firmware while keeping settings first.")
    # Resolve against RUNNING firmware. No onboard, register, issue or rotate
    # call belongs here, including as a fallback after a 403 or USB failure.
    source = workbook_source(client, experiment_id, expected.device.identity.fw, log=log)
    descriptor = schedule_descriptor(source.blob, source.sha256, source.script_version,
                                     source.built_against_fw, source.workbook_version_id)
    journal = OperationJournal(config.SERVICE_OPERATIONS_DIR, expected.mac)
    stage = 'No experiment maintenance writes confirmed'
    try:
        with journal.lease():
            entry = journal.load()
            if entry and entry['phase'] != 'complete' and entry['descriptor'] != descriptor:
                raise ExperimentSettingsError("Resolve the saved operation using Wi-Fi settings → Check saved result before changing experiments.")
            with wifi_settings._selected_console(expected.device.port) as console:
                _checked(console, expected)
                log(f"Installing destination workbook on {expected.mac}; keeping broker {expected.broker}.")
                current = install_workbook(console, expected.device, source, journal, log=log)
                stage = 'Destination workbook installed; topic writes may be partial'
                firmware_update._idle(console, current.identity)
                if not console.wait_prompt(45):
                    raise ExperimentSettingsError("The device did not leave service mode before topic update.")
                # _idle enters/exits service. Recheck the same descriptor after
                # that round trip before writing either persistent topic.
                if console.identity().mac != expected.mac:
                    raise ExperimentSettingsError("Connected device changed before topic update.")
                console.cfg_set('mqtt_topic_root', new_root)
                console.cfg_set('status_topic', config.default_status_topic(new_root))
                if (console.cfg_get('mqtt_topic_root') != new_root
                        or console.cfg_get('status_topic') != config.default_status_topic(new_root)):
                    raise ExperimentSettingsError("Persistent topic readback did not match the destination.")
                stage = 'Workbook and topics saved; reboot verification incomplete'
                console.reboot()
            with _after_reboot(expected.device.port, expected.mac) as console:
                after = firmware_update._snapshot(console)
                after = firmware_update._settled_schedule(console, after, log, before_flash=False)
                schedule = after.identity.schedule
                expected_after = replace(expected.device, settings=dict(expected.device.settings,
                    mqtt_topic_root=new_root, status_topic=config.default_status_topic(new_root)))
                changed, timezone_result = firmware_update._configuration_changes(expected_after, after)
                if (after.identity.mac != expected.mac or after.identity.thing_name != expected.device.identity.thing_name
                        or after.identity.fw != expected.device.identity.fw
                        or changed
                        or after.settings['mqtt_topic_root'] != new_root
                        or after.settings['status_topic'] != config.default_status_topic(new_root)
                        or schedule.source != 'INSTALLED' or schedule.sha256 != source.sha256
                        or schedule.workbook_version_id != source.workbook_version_id):
                    raise ExperimentSettingsError("The destination configuration/workbook was not verified after reboot.")
                return replace(_target(console, {client.env.key: expected.broker} if expected.environment else {}),
                               timezone_result=timezone_result)
    except (OSError, ambyte_serial.ConsoleError, ServiceClientError, wifi_settings.WifiSettingsError,
            firmware_update.FirmwareUpdateError, ExperimentSettingsError) as exc:
        raise ExperimentSettingsError(f"{stage}. {exc} Read the device destination and retry this action. No registration or certificate write was attempted.") from None
