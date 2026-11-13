# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only
"""Best-effort maintenance following a successful local device action.

A workbook/cloud failure belongs to its own result, never to the Wi-Fi save or
firmware verification which already succeeded. No registry or provisioning API
is called here. Raw device topics select the experiment, not the GUI selection.
"""
from dataclasses import dataclass
import re

from . import config, experiment_settings, firmware_update, wifi_settings
from .service_client import OperationJournal


@dataclass(frozen=True)
class MaintenanceResult:
    workbook: str
    timezone: str = 'Timezone: not checked'
    port: str | None = None

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
            timezone, port = check_timezone(console, device, journal, confirm_timezone, log=log)
            result = MaintenanceResult(workbook, timezone, port)
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


@dataclass(frozen=True)
class TimezoneMismatch:
    mac: str
    stored: str | None
    latitude: float
    longitude: float
    resolved: str


def _runtime_offset(console, identity):
    # The firmware uses this offset for scheduling. Requiring the public status
    # evidence catches a persisted-but-unapplied correction without host tzdata
    # (which is absent on some packaged Windows installations).
    status = console.command('status', max_reply_bytes=8192)
    match = re.search(r'RTC local: [^\r\n]+ \(UTC([+-])(\d{2}):(\d{2})\)', status)
    if match is None:
        raise ValueError('runtime RTC local offset unavailable')
    hours, minutes = int(match[2]), int(match[3])
    if hours > 23 or minutes > 59:
        raise ValueError('invalid runtime RTC offset')
    offset = (hours * 3600 + minutes * 60) * (-1 if match[1] == '-' else 1)
    if identity.timezone is not None and offset != identity.timezone.utc_offset_seconds:
        raise ValueError('RTC local offset disagrees with active timezone identity')
    return offset


def check_timezone(console, device, journal, confirm, *, log=lambda text: None):
    port = console.port
    stored = device.settings.get('timezone')
    prefix = f'Fallback timezone: {stored or "unset"}'
    written = False
    try:
        from .tz_geo import zone_for, is_unset_location
        # Refresh after a possible workbook installation, then use only the
        # additive persisted-location contract. sync loc is runtime fallback.
        current = firmware_update._snapshot(console)
        current = firmware_update._settled_schedule(console, current, log, before_flash=True)
        if current.identity.mac != device.identity.mac:
            return 'Timezone: not checked: connected Ambyte changed', port
        stored = current.settings.get('timezone')
        prefix = f'Fallback timezone: {stored or "unset"}'
        location = current.identity.location
        zone = current.identity.timezone
        if location is None:
            detail = current.identity.location_error or 'firmware too old'
            return prefix + f'; effective location unavailable ({detail})', port
        fallback = location.configured
        if fallback is not None and fallback.set:
            prefix += f'; fallback coordinates: {fallback.lat}, {fallback.lon}'
        else:
            prefix += '; no fallback coordinates'
        accuracy = f'{location.accuracy_m:g} m' if location.accuracy_m is not None else 'unknown'
        effective = f'{location.lat}, {location.lon}' if location.set else 'unavailable'
        prefix += f'\nEffective location: {effective} (source={location.source}, accuracy={accuracy})'
        prefix += f'\nEffective timezone: {zone.active or "unset"} (source={zone.source})' if zone else '\nEffective timezone: unavailable'
        if location.source == 'live':
            return prefix + '; live location is managed by the Ambyte', port
        if current.identity.timezone_error or (zone is not None and zone.source == 'unknown'):
            return prefix + '; fallback not checked: timezone source unavailable', port
        if fallback is None or not fallback.set or is_unset_location(fallback.lat, fallback.lon):
            return prefix + '; no deployment coordinates', port
        resolved = zone_for(fallback.lat, fallback.lon)
        if resolved is None:
            return prefix + '; fallback location timezone unresolved', port
        if (zone is not None and zone.source == 'auto' and zone.resolved is not None
                and zone.configured == zone.active == zone.resolved == stored and resolved != stored):
            return prefix + f'; warning: GUI fallback zone {resolved}, firmware zone {stored} (firmware is authoritative)', port
        if resolved == stored:
            return prefix + '; matches fallback location', port
        mismatch = TimezoneMismatch(current.identity.mac, stored, fallback.lat, fallback.lon, resolved)
        if confirm is None:
            return prefix + f'; mismatch with {resolved} (confirmation required, unchanged)', port
        if not confirm(mismatch):
            return prefix + f'; kept (operator declined {resolved})', port
        entry = journal.load()
        if entry and entry['phase'] != 'complete':
            raise ValueError('resolve the saved service operation before changing timezone')
        firmware_update._idle(console, current.identity)
        if not console.wait_prompt(45):
            raise ValueError('device did not leave service mode')
        fresh = firmware_update._snapshot(console)
        fresh = firmware_update._settled_schedule(console, fresh, log, before_flash=True)
        if fresh != current:
            raise ValueError('device configuration or location changed while awaiting confirmation')
        automatic = fresh.identity.timezone is not None
        # New firmware owns resolution. Never send an explicit zone followed by
        # auto: differing dataset versions must be reported, not fought over.
        console.cfg_set('timezone', 'auto' if automatic else resolved)
        written = True
        if automatic:
            if console.cfg_get('timezone_source') != 'auto':
                raise ValueError('automatic timezone source readback did not match')
        elif console.cfg_get('timezone') != resolved:
            raise ValueError('stored timezone readback did not match')
        console.reboot()
        console.close()
        with experiment_settings._after_reboot(port, fresh.identity.mac) as after_console:
            port = after_console.port
            after = firmware_update._snapshot(after_console)
            after = firmware_update._settled_schedule(after_console, after, log, before_flash=False)
            zone = after.identity.timezone
            if automatic and (zone is None or zone.source != 'auto' or zone.active != resolved):
                active = zone.active if zone else 'unavailable'
                raise ValueError(f'GUI expected {resolved}, firmware active zone is {active}; no retry was attempted')
            expected_settings = dict(fresh.settings, timezone=resolved)
            if (after.identity.mac != fresh.identity.mac or after.identity.thing_name != fresh.identity.thing_name
                    or after.identity.fw != fresh.identity.fw or after.identity.schedule != fresh.identity.schedule
                    or after.identity.location != fresh.identity.location or after.settings != expected_settings):
                raise ValueError('post-reboot timezone or preserved configuration/workbook did not match')
            offset = _runtime_offset(after_console, after.identity)
        return f'Timezone: updated to {resolved}; verified after reboot (UTC offset {offset:+d}s)', port
    except Exception as error:
        # No cloud/serial exception from an optional check may replace a
        # successful Wi-Fi/firmware result. Preserve the write stage truthfully.
        reason = str(error) if isinstance(error, ValueError) else 'USB verification failed'
        stage = 'change may be saved, verification incomplete' if written else 'unchanged'
        return prefix + f'; {stage}: {reason}', port
