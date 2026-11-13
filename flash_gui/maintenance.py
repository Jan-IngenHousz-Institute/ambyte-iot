# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only
"""Best-effort maintenance following a successful local device action.

A workbook/cloud failure belongs to its own result, never to the Wi-Fi save or
firmware verification which already succeeded. No registry or provisioning API
is called here. Raw device topics select the experiment, not the GUI selection.
"""
from dataclasses import dataclass

from . import config, experiment_settings, firmware_update, wifi_settings
from .service_client import OperationJournal


@dataclass(frozen=True)
class MaintenanceResult:
    workbook: str
    timezone: str = 'Timezone: not checked'

    @property
    def text(self):
        return f'{self.workbook}\n{self.timezone}'


def experiment_from_topic(raw):
    match = experiment_settings._TOPIC.fullmatch(raw or '')
    if match is None:
        return None
    try:
        return experiment_settings._uuid(match[1])
    except experiment_settings.ExperimentSettingsError:
        return None


def check_workbook(console, device, client, journal, *, log=lambda text: None):
    """Called with a settled snapshot and the MAC lease held."""
    if client is None:
        return 'Workbook: not checked: not signed in to openJII'
    experiment = experiment_from_topic(device.settings['mqtt_topic_root'])
    if experiment is None:
        return 'Workbook: not checked: unrecognized stored ingest topic'
    if 'svc' not in device.identity.capabilities:
        return 'Workbook: not checked: firmware lacks journaled serial schedule push; update firmware first'
    try:
        source = experiment_settings.workbook_source(client, experiment, device.identity.fw, log=log)
        schedule = device.identity.schedule
        if (schedule.source == 'INSTALLED' and schedule.workbook_version_id == source.workbook_version_id
                and schedule.sha256 == source.sha256):
            return 'Workbook: up to date'
        experiment_settings.install_workbook(console, device, source, journal, log=log)
        return f'Workbook: installed {source.label.removeprefix("workbook ")}'
    except Exception as exc:
        # Read/install exceptions contain no Wi-Fi command, cert or key. Unknown
        # exceptions (including malformed cloud replies) get a generic label.
        from .ambyte_serial import ConsoleError
        from .service_client import ServiceClientError
        known = (experiment_settings.ExperimentSettingsError, firmware_update.FirmwareUpdateError,
                 ServiceClientError, ConsoleError, OSError)
        reason = str(exc) if isinstance(exc, known) else 'cloud lookup or workbook compilation failed'
        return f'Workbook: not checked: {reason}'


def check_session(console, mac, client=None, *, confirm_timezone=None, log=lambda text: None):
    try:
        journal = OperationJournal(config.SERVICE_OPERATIONS_DIR, mac)
        with journal.lease():
            device = firmware_update._snapshot(console)
            device = firmware_update._settled_schedule(console, device, log, before_flash=True)
            if device.identity.mac != mac:
                return MaintenanceResult('Workbook: not checked: connected Ambyte changed',
                                         'Timezone: not checked: connected Ambyte changed')
            workbook = check_workbook(console, device, client, journal, log=log)
            timezone = check_timezone(console, device, journal, confirm_timezone, log=log)
            result = MaintenanceResult(workbook, timezone)
    except Exception:
        result = MaintenanceResult('Workbook: not checked: device unavailable, unsupported or busy; read device again',
                                   'Timezone: not checked: device unavailable, unsupported or busy')
    log(result.text)
    return result


def run(port, mac, client=None, *, confirm_timezone=None, log=lambda text: None):
    """Recheck the same MAC; additional maintenance never changes base success."""
    try:
        with wifi_settings._selected_console(port) as console:
            return check_session(console, mac, client, confirm_timezone=confirm_timezone, log=log)
    except Exception:
        return MaintenanceResult('Workbook: not checked: device unavailable or unsupported; read device again',
                                 'Timezone: not checked: device unavailable or unsupported')


def check_timezone(console, device, journal, confirm, *, log=lambda text: None):
    # Filled by the persisted-location contract; older firmware fails closed.
    return f'Timezone: {device.settings.get("timezone") or "unset"}; location unavailable (firmware too old)'
