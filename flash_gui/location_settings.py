# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only
"""Explicit experiment-location maintenance; never part of an automatic check.

Only audited legacy images may use sync loc without persisted identity readback.
The console prints four decimals, so their post-boot position proof has that
precision. PR3 supplies the configured tuple directly. All writes share the MAC
lease and unresolved/foreign operation guards, but never auto-resume a possible
location write: an acknowledgement can be lost after NVS persistence.
"""
from dataclasses import dataclass
import math
import re
from zoneinfo import ZoneInfo
from datetime import datetime, timezone

from . import config, firmware_update as fw, experiment_settings as experiments, wifi_settings
from .openjii_client import location_coordinates
from .service_client import OperationJournal
from .tz_geo import zone_for
from .tz_country import ZONE_COUNTRY


@dataclass(frozen=True)
class LocationOptions:
    locations: tuple = ()
    reason: str = ''


def current_options(target, client):
    if client is None:
        return LocationOptions(reason='Sign in to openJII to read experiment locations.')
    from .maintenance import experiment_from_topic
    experiment = experiment_from_topic(target.device.settings.get('mqtt_topic_root'))
    if experiment is None:
        return LocationOptions(reason='Unrecognized current experiment topic.')
    if target.environment and target.environment != client.env.key:
        return LocationOptions(reason='Select the API environment matching the device broker.')
    return destination_options(client, experiment)


def destination_options(client, experiment):
    try:
        locations = tuple(client.list_experiment_locations(experiment))
        return LocationOptions(locations, '' if locations else 'This experiment has no locations.')
    except Exception:
        return LocationOptions(reason='Experiment locations unavailable; check experiment read access.')


def change_default(identity, selected):
    location = identity.location
    if location is None or location.configured is None:
        return False, 'Configured location unavailable on this firmware; choose explicitly.'
    fallback = location.configured
    if not fallback.set:
        return True, 'This device has no configured location.'
    try:
        old = ZONE_COUNTRY.get(zone_for(fallback.lat, fallback.lon))
        new = ZONE_COUNTRY.get(zone_for(selected.latitude, selected.longitude))
    except (RuntimeError, ImportError):
        return False, 'Country lookup unavailable; choose explicitly.'
    return bool(old and new and old != new), ('Different country from the configured location.'
        if old and new and old != new else 'Keep the configured location unless you choose to replace it.')


def persistence_supported(identity):
    if 'timezone.auto' in identity.capabilities:
        return identity.location is not None and identity.timezone is not None
    # Pin audited releases. A version number alone cannot prove persistence for
    # an arbitrary old/custom CLI; add releases only after auditing their source.
    return (identity.fw or '').removeprefix('v') in {'2.9.0', '2.14.0', '2.16.0'}


def confirmation(identity, location):
    if not persistence_supported(identity):
        return None
    if 'timezone.auto' in identity.capabilities:
        pin = ' A changed location also releases this device’s manual timezone pin.' if identity.timezone.pinned else ''
        return (f'Set configured location to {location.label}? The Ambyte will determine its effective location and timezone.'
                + pin + ' Reboot and verify the same device?')
    zone = zone_for(location.latitude, location.longitude)
    if zone is None:
        return None
    return (f'Set configured location to {location.label} and fallback timezone to {zone}? '
            'This legacy firmware needs the explicitly confirmed fallback zone. Reboot and verify the same device?')


def _tuple_matches(coordinates, location, tolerance):
    return (coordinates is not None and coordinates.set
            and math.isclose(coordinates.lat, location.latitude, rel_tol=0, abs_tol=tolerance)
            and math.isclose(coordinates.lon, location.longitude, rel_tol=0, abs_tol=tolerance))


def _legacy_position(console, location):
    reply = console.command('sync loc', max_reply_bytes=2048)
    match = re.search(r'(?m)^loc: lat=(-?\d+\.\d{4}) lon=(-?\d+\.\d{4}) tz=[+-]?\d+ deployment=', reply)
    return bool(match and abs(float(match[1])-location.latitude) <= .0000501
                and abs(float(match[2])-location.longitude) <= .0000501)


def _legacy_runtime(console, zone):
    """An offset alone is insufficient: compare it at the device's UTC clock."""
    try:
        stamp = console.rtc_read()
        if stamp < 1577836800:  # No trustworthy modern UTC clock.
            return None
        expected = int(datetime.fromtimestamp(stamp, timezone.utc).astimezone(ZoneInfo(zone)).utcoffset().total_seconds())
        from .maintenance import _runtime_offset
        actual = _runtime_offset(console, type('Legacy', (), {'timezone': None})())
        return actual if actual == expected else None
    except Exception:
        # Missing host tzdata / RTC / status means saved, never verified.
        return None


def _same_site(first, second):
    a, b = first.identity, second.identity
    return (first.settings['timezone'] == second.settings['timezone']
            and (a.location.configured if a.location else None) == (b.location.configured if b.location else None)
            and (a.timezone.pinned if a.timezone else None) == (b.timezone.pinned if b.timezone else None))


def set_location(target, location, *, confirm, log=lambda text: None):
    """Result is operator-facing only: callers must not send it to persistent logs."""
    attempted = False
    try:
        location_coordinates(location.latitude, location.longitude)
        if not persistence_supported(target.device.identity):
            return "Location: this firmware can't store a location over USB; update first or use MQTT set_location."
        modern = 'timezone.auto' in target.device.identity.capabilities
        zone = None if modern else zone_for(location.latitude, location.longitude)
        prompt = confirmation(target.device.identity, location)
        if prompt is None:
            return 'Location: unchanged; location has no supported fallback timezone.'
        journal = OperationJournal(config.SERVICE_OPERATIONS_DIR, target.mac)
        with journal.lease():
            entry = journal.load()
            if entry and entry['phase'] != 'complete':
                return 'Location: unchanged; resolve the saved service operation first.'
            if not confirm(prompt):
                return 'Location: unchanged; operator declined.'
            with wifi_settings._selected_console(target.device.port) as console:
                fresh = fw._settled_schedule(console, fw._snapshot(console), log, before_flash=True)
                if not experiments._same_device_config(fresh, target.device) or not _same_site(fresh, target.device):
                    return 'Location: unchanged; device configuration changed. Read the device again.'
                fw._idle(console, fresh.identity)
                if not console.wait_prompt(45):
                    return 'Location: unchanged; device did not return to the console.'
                checked = fw._snapshot(console)
                if not experiments._same_device_config(fresh, checked) or not _same_site(fresh, checked):
                    return 'Location: unchanged; device changed before the write.'
                # Never pass command/exception text to logs: it contains private coordinates.
                attempted = True
                console.location_set(location.latitude, location.longitude)
                if modern:
                    if not _tuple_matches(console.identity().location.configured, location, .00000051):
                        raise ValueError('configured location readback mismatch')
                else:
                    console.cfg_set('timezone', zone)
                    if console.cfg_get('timezone') != zone:
                        raise ValueError('fallback readback mismatch')
                console.reboot()
            with experiments._after_reboot(target.device.port, target.mac) as console:
                after = fw._settled_schedule(console, fw._snapshot(console), log, before_flash=False)
                expected = dict(fresh.settings) if modern else dict(fresh.settings, timezone=zone)
                if (not experiments._same_device_config(fresh, after) or after.settings != expected):
                    raise ValueError('preserved state changed')
                if modern:
                    if not _tuple_matches(after.identity.location.configured, location, .00000051):
                        raise ValueError('configured location not retained')
                    active = after.identity.timezone
                    if active is None:
                        raise ValueError('effective timezone unavailable')
                    return (f'Location: saved {location.label}; configured readback verified after reboot. '
                            f'Effective timezone: {active.active or "unset"} (source={active.source}, '
                            f'location={after.identity.location.source}).')
                if not _legacy_position(console, location):
                    raise ValueError('post-boot configured position mismatch')
                offset = _legacy_runtime(console, zone)
                evidence = f'verified after reboot (UTC offset {offset:+d}s)' if offset is not None else 'saved, runtime unverified'
                return f'Location: saved {location.label}; fallback timezone {zone}; {evidence}.'
    except Exception:
        # SF1: all failures after transmission may have persisted, even if a USB
        # acknowledgement was lost. Never expose a command-bearing exception.
        return ('Location: change may be saved; USB verification incomplete. Read the device before retrying.' if attempted else
                'Location: unchanged; device unavailable, busy or unsupported. Read the device again.')
