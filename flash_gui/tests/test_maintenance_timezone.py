# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only
"""Persisted-location discovery and consent using real console/service bytes."""
import json
from dataclasses import replace
from types import SimpleNamespace
import pytest

from flash_gui import maintenance as m, ambyte_serial, config, gui
from flash_gui.tests.test_experiment_settings import target  # noqa: F401
from flash_gui.tests.test_wifi_settings import usb  # noqa: F401


@pytest.fixture
def site(target):
    target.values['timezone'] = 'Europe/Berlin'
    target.values['timezone_source'] = 'provisioned'
    target.device.site = dict(set=True, lat=-25.3444, lon=131.0369)
    target.device.resolved = 'Australia/Darwin'
    target.device.active_zone = 'Europe/Berlin'
    target.device.offset = 3600
    original = target.device.console
    def console(command):
        if command == 'cfg set timezone auto':
            target.device.plain.append(command)
            target.values['timezone_source'] = 'auto'
            return b'cfg set timezone = auto  (reboot to apply)\r\nambyte> '
        if command == 'reboot':
            if target.values['timezone_source'] == 'auto':
                target.values['timezone'] = target.device.resolved
            target.device.active_zone = target.values['timezone']
            target.device.offset = 34200
        if command == 'status':
            target.device.plain.append(command)
            return b' - RTC (UTC): 2026-11-13 09:00:00 (1794560400)\r\n - RTC local: 2026-11-13 18:30:00 (UTC+09:30)\r\nambyte> '
        result = original(command)
        if command == 'identity':
            lines = result.decode().splitlines()
            for i,line in enumerate(lines):
                if line.startswith('{'):
                    obj = json.loads(line)
                    fallback = target.device.site
                    obj['location'] = dict(fallback, source='configured' if fallback['set'] else 'none', accuracy_m=None,
                        configured=fallback, live=dict(set=False,lat=None,lon=None,accuracy_m=None,age_s=None))
                    if hasattr(target.device, 'live_site'):
                        obj['location'].update(target.device.live_site)
                    if not getattr(target.device, 'legacy_zone', False):
                        obj['timezone'] = dict(configured=target.values['timezone'], active=target.device.active_zone,
                            source=target.values['timezone_source'], resolved=target.device.resolved,
                            utc_offset_seconds=target.device.offset)
                    lines[i] = json.dumps(obj)
            return ('\r\n'.join(lines)).encode()
        return result
    target.device.console = console
    return target


def mutations(site):
    return [x for x in site.device.plain if x.startswith('cfg set ') or x == 'reboot']


def test_yes_uses_firmware_auto_reboots_and_verifies_active_zone_and_offset(site):
    prompts = []
    result = m.run('bench', site.preview.mac, confirm_timezone=lambda p: prompts.append(p) or True)
    assert prompts == [m.TimezoneMismatch(site.preview.mac, 'Europe/Berlin', -25.3444, 131.0369, 'Australia/Darwin')]
    assert 'updated to Australia/Darwin' in result.timezone and '+34200s' in result.timezone
    assert mutations(site) == ['cfg set timezone auto', 'reboot']
    assert site.values['timezone'] == 'Australia/Darwin' and site.values['timezone_source'] == 'auto'
    assert not site.device.commits


def test_no_preserves_zone_and_source_without_reboot(site):
    result = m.run('bench', site.preview.mac, confirm_timezone=lambda p: False)
    assert 'operator declined Australia/Darwin' in result.timezone
    assert site.values['timezone'] == 'Europe/Berlin' and site.values['timezone_source'] == 'provisioned'
    assert not mutations(site)


@pytest.mark.parametrize('location', [dict(set=False,lat=None,lon=None), dict(set=True,lat=0,lon=0)])
def test_no_coordinates_never_prompts_or_writes(site, location):
    site.device.site = location
    result = m.run('bench', site.preview.mac, confirm_timezone=lambda p: pytest.fail('must not ask'))
    assert ('no deployment coordinates' in result.timezone or 'malformed location metadata' in result.timezone) and not mutations(site)


def test_ocean_no_match_never_prompts_or_writes(site):
    site.device.site = dict(set=True,lat=0,lon=-140)
    result = m.run('bench', site.preview.mac, confirm_timezone=lambda p: pytest.fail('must not ask'))
    assert 'unresolved' in result.timezone and not mutations(site)


def test_matching_zone_reports_coordinates_without_prompt(site):
    site.values['timezone'] = 'Australia/Darwin'
    result = m.run('bench', site.preview.mac, confirm_timezone=lambda p: pytest.fail('must not ask'))
    assert 'matches fallback location' in result.timezone and '-25.3444, 131.0369' in result.timezone
    assert not mutations(site)


def test_no_confirmation_callback_does_not_write(site):
    result = m.run('bench', site.preview.mac)
    assert 'confirmation required, unchanged' in result.timezone and not mutations(site)


def test_old_identity_fails_closed_without_sync_loc_fallback(target):
    result = m.run('bench', target.preview.mac, confirm_timezone=lambda p: pytest.fail('must not ask'))
    assert 'effective location unavailable (firmware too old)' in result.timezone
    assert 'sync loc' not in target.device.plain


def test_dataset_disagreement_reports_both_zones_and_does_not_retry(site):
    site.device.resolved = 'Australia/Adelaide'
    result = m.run('bench', site.preview.mac, confirm_timezone=lambda p: True)
    assert 'GUI expected Australia/Darwin, firmware active zone is Australia/Adelaide' in result.timezone
    assert 'verification incomplete' in result.timezone
    assert mutations(site) == ['cfg set timezone auto', 'reboot']


def test_device_changed_during_prompt_blocks_write(site):
    def swapped(p):
        site.device.mac = '00:11:22:33:44:55'
        return True
    result = m.run('bench', site.preview.mac, confirm_timezone=swapped)
    assert 'changed while awaiting confirmation' in result.timezone and not mutations(site)


def test_source_readback_failure_reports_possible_save_without_reboot(site):
    original = site.device.console
    def console(command):
        if command == 'cfg get timezone_source':
            return b'timezone_source = manual\r\nambyte> '
        return original(command)
    site.device.console = console
    result = m.run('bench', site.preview.mac, confirm_timezone=lambda p: True)
    assert 'change may be saved' in result.timezone and 'source readback' in result.timezone
    assert mutations(site) == ['cfg set timezone auto']


def test_runtime_status_must_agree_with_identity_offset(site):
    original = site.device.console
    def console(command):
        value = original(command)
        return value.replace(b'UTC+09:30', b'UTC+01:00') if command == 'status' else value
    site.device.console = console
    result = m.run('bench', site.preview.mac, confirm_timezone=lambda p: True)
    assert 'RTC local offset disagrees' in result.timezone


def test_older_location_contract_without_auto_uses_explicit_zone_and_readback(site):
    site.device.legacy_zone = True
    result = m.run('bench', site.preview.mac, confirm_timezone=lambda p: True)
    assert 'saved Australia/Darwin; runtime unverified' in result.timezone
    assert 'verified after reboot' not in result.timezone
    assert mutations(site) == ['cfg set timezone Australia/Darwin', 'reboot']


def test_gui_confirmation_is_marshaled_to_tk_and_shows_both_zones(monkeypatch):
    calls=[]
    app=SimpleNamespace(_ask_on_main=lambda fn: calls.append('main') or fn(), winfo_toplevel=lambda:'root')
    monkeypatch.setattr(gui.messagebox,'askyesno',lambda title,text,**kw:calls.append((title,text,kw)) or False)
    mismatch=m.TimezoneMismatch('AA:BB:CC:DD:EE:FF','Europe/Berlin',-25.3444,131.0369,'Australia/Darwin')
    assert gui.App._confirm_timezone(app,mismatch) is False
    assert calls[0]=='main' and 'Europe/Berlin' in calls[1][1] and 'Australia/Darwin' in calls[1][1]


def test_firmware_auto_zone_is_authoritative_on_later_read(site):
    site.values.update(timezone='Australia/Adelaide', timezone_source='auto')
    site.device.active_zone=site.device.resolved='Australia/Adelaide'
    result=m.run('bench',site.preview.mac,confirm_timezone=lambda p:pytest.fail('do not fight firmware resolver'))
    assert 'GUI fallback zone Australia/Darwin, firmware zone Australia/Adelaide' in result.timezone
    assert not mutations(site)


def test_timezone_confirmation_does_not_replace_an_unresolved_service_operation(site):
    from flash_gui.tests.test_wifi_settings import _pending_schedule
    info,journal,entry=_pending_schedule((site.device,site.handles,site.directory),'staging')
    result=m.run('bench',info.mac,confirm_timezone=lambda p:True)
    assert 'resolve the saved service operation' in result.timezone and not mutations(site)
    assert journal.load()==entry


def test_live_location_is_displayed_separately_and_never_prompts_to_override(site):
    site.device.live_site=dict(set=True,lat=52.0,lon=5.0,source='live',accuracy_m=1000,
        live=dict(set=True,lat=52.0,lon=5.0,accuracy_m=1000,age_s=30))
    site.device.active_zone=site.device.resolved='Europe/Amsterdam'
    result=m.run('bench',site.preview.mac,confirm_timezone=lambda p:pytest.fail('live location must remain informational'))
    assert 'Fallback timezone: Europe/Berlin; fallback coordinates: -25.3444, 131.0369' in result.timezone
    assert 'Effective location: 52.0, 5.0 (source=live, accuracy=1000 m)' in result.timezone
    assert 'Effective timezone: Europe/Amsterdam' in result.timezone and not mutations(site)


def test_live_estimate_age_changes_do_not_block_workbook_install(site):
    live=dict(set=True,lat=52.0,lon=5.0,accuracy_m=1000,age_s=0)
    site.device.live_site=dict(set=True,lat=52.0,lon=5.0,source='live',accuracy_m=1000,live=live)
    original=site.device.console
    def console(command):
        if command=='identity':live['age_s']+=1
        return original(command)
    site.device.console=console
    result=m.run('bench',site.preview.mac,site.client,confirm_timezone=lambda p:pytest.fail('no live override'))
    assert result.workbook=='Workbook: installed v3' and site.device.commits==1
    assert 'source=live' in result.timezone


@pytest.mark.parametrize('legacy', [False, True])
def test_lost_setter_reply_reports_uncertain_mutation_after_real_persistence(site, legacy):
    site.device.legacy_zone=legacy
    original=site.device.console
    def console(command):
        value=original(command)
        if command.startswith('cfg set timezone '):
            return b''  # Setter ran, but its USB response never reaches the host.
        return value
    site.device.console=console
    result=m.run('bench',site.preview.mac,confirm_timezone=lambda p:True)
    if legacy:assert site.values['timezone']=='Australia/Darwin'
    else:assert site.values['timezone_source']=='auto'
    assert 'change may be saved, verification incomplete' in result.timezone
    assert 'unchanged:' not in result.timezone and 'reboot' not in mutations(site)
    assert result.workbook=='Workbook: not checked: not signed in to openJII'


def test_legacy_wrong_runtime_offset_is_saved_but_never_verified(site):
    site.device.legacy_zone=True
    original=site.device.console
    def console(command):
        value=original(command)
        return value.replace(b'UTC+09:30',b'UTC+01:00') if command=='status' else value
    site.device.console=console
    result=m.run('bench',site.preview.mac,confirm_timezone=lambda p:True)
    assert site.values['timezone']=='Australia/Darwin'
    assert 'saved Australia/Darwin; runtime unverified' in result.timezone
    assert 'observed UTC offset +3600s' in result.timezone
    assert 'verified after reboot' not in result.timezone
