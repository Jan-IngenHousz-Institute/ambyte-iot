# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only
"""Existing-device experiment maintenance; never register or issue credentials.

The destination workbook is authorized by its read endpoints. Device registry
visibility/bindings are not an authorization gate for the wildcard ingest
policy. Broker/certificate ownership is preserved, not guessed from API login.
Queued records deliberately follow the current topic/provenance at publication.
"""
from contextlib import contextmanager
from dataclasses import dataclass
from types import SimpleNamespace
from urllib.parse import urlsplit
from uuid import UUID
import re
import time

from . import ambyte_serial, config, firmware_update, procedure, wifi_settings
from .service_client import OperationJournal, ServiceClient, ServiceClientError, schedule_descriptor


class ExperimentSettingsError(RuntimeError):
    pass


@dataclass(frozen=True)
class DeviceTarget:
    device: firmware_update.ConfiguredDevice
    broker: str
    experiment_id: str | None
    environment: str | None

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
    broker = urlsplit(device.settings['mqtt_uri']).hostname.lower()
    matches = [env for env, host in endpoints.items() if env in config.ENVIRONMENTS and host == broker]
    match = _TOPIC.fullmatch(device.settings['mqtt_topic_root'] or '')
    experiment = match[1] if match else None
    return DeviceTarget(device, broker, experiment, matches[0] if len(matches) == 1 else None)


def read_target(port, endpoints=None):
    try:
        with wifi_settings._selected_console(port) as console:
            return _target(console, endpoints or {})
    except (ambyte_serial.ConsoleError, firmware_update.FirmwareUpdateError, wifi_settings.WifiSettingsError) as exc:
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


def _checked(console, expected):
    current = _target(console, {})
    if current.device != expected.device:
        # Port stays bound but reads may be separated by an API lookup. All
        # configuration and loaded-program fields must still match the preview.
        raise ExperimentSettingsError("Device identity, configuration or workbook changed. Read the device destination again.")
    return current


@contextmanager
def _after_reboot(previous_port, mac):
    # Enumerating ports is read-only. Open only the same USB serial/MAC, never
    # connect_after_boot's scan that can reset unrelated Ambytes.
    deadline = time.monotonic() + 180
    while time.monotonic() < deadline:
        ports = [p.device for p in ambyte_serial.list_ports.comports()
                 if (p.serial_number or '').replace(':', '').upper() == mac.replace(':', '').upper()]
        if len(ports) > 1:
            raise ExperimentSettingsError("More than one USB port reports the expected serial. Select the device again.")
        if ports:
            with wifi_settings._selected_console(ports[0]) as console:
                yield console
                return
        time.sleep(1)
    raise ExperimentSettingsError("The same Ambyte did not return after reboot. Read its destination and retry; no provisioning was attempted.")


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
    programming = client.resolve_programming(experiment_id, fw_version=expected.device.identity.fw, log=log)
    if programming is None:
        raise ExperimentSettingsError("The selected experiment has no pinned Ambyte workbook programming cell.")
    source = procedure.schedule_source(SimpleNamespace(programming=programming, schedule_script=None,
                                      release=SimpleNamespace(version=expected.device.identity.fw)), log=log)
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
                service = ServiceClient(console._ser, journal)
                service.upload_schedule(source.blob, source.sha256, source.script_version,
                                        source.built_against_fw, source.workbook_version_id, log=log)
                stage = 'Destination workbook installed; topic writes may be partial'
                if not console.wait_prompt(45):
                    raise ExperimentSettingsError("The device did not return to its console after workbook installation.")
                current = firmware_update._snapshot(console)
                if (current.identity.mac != expected.mac or current.identity.thing_name != expected.device.identity.thing_name
                        or current.settings != expected.device.settings):
                    raise ExperimentSettingsError("The device configuration changed before topic update.")
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
                preserved = ('mqtt_uri', 'mqtt_client_id', 'command_topic', 'device_name', 'timezone')
                if (after.identity.mac != expected.mac or after.identity.thing_name != expected.device.identity.thing_name
                        or after.identity.fw != expected.device.identity.fw
                        or any(after.settings[key] != expected.device.settings[key] for key in preserved)
                        or after.settings['mqtt_topic_root'] != new_root
                        or after.settings['status_topic'] != config.default_status_topic(new_root)
                        or schedule.source != 'INSTALLED' or schedule.sha256 != source.sha256
                        or schedule.workbook_version_id != source.workbook_version_id):
                    raise ExperimentSettingsError("The destination configuration/workbook was not verified after reboot.")
                return _target(console, {client.env.key: expected.broker} if expected.environment else {})
    except (ambyte_serial.ConsoleError, ServiceClientError, wifi_settings.WifiSettingsError,
            firmware_update.FirmwareUpdateError, ExperimentSettingsError) as exc:
        raise ExperimentSettingsError(f"{stage}. {exc} Read the device destination and retry this action. No registration or certificate write was attempted.") from None
