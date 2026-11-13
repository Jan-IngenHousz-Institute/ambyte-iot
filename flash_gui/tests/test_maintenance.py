# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only
"""Automatic checks exercise the production console, compiler, journal and service."""
from dataclasses import replace
import pytest

from flash_gui import maintenance as m, wifi_settings
from flash_gui.tests.test_experiment_settings import target, OLD, ROOT  # noqa: F401
from flash_gui.tests.test_wifi_settings import usb  # noqa: F401
from flash_gui.openjii_client import OpenJIIError


def test_signed_out_does_not_resolve_or_install(target):
    result = m.run('bench', target.preview.mac)
    assert 'not signed in' in result.workbook
    assert not target.calls and not target.device.commits


@pytest.mark.parametrize('root', [ROOT+'/extra', ROOT.replace(OLD, 'legacy'), ROOT.upper(), ROOT.replace(OLD, 'x'*36), ''])
def test_unrecognized_topics_fail_closed_without_cloud_or_writes(target, root):
    target.values['mqtt_topic_root'] = root
    assert 'not checked' in m.run('bench', target.preview.mac, target.client).workbook
    assert not target.calls and not target.device.commits


def test_invisible_experiment_does_not_install_or_query_registry(target):
    def denied(*a, **kw): raise OpenJIIError('403 experiment inaccessible')
    target.client.resolve_programming = denied
    result = m.run('bench', target.preview.mac, target.client)
    assert '403' in result.workbook and not target.device.commits


def test_no_pinned_workbook_is_reported(target):
    target.client.resolve_programming = lambda *a, **kw: None
    assert 'no pinned' in m.run('bench', target.preview.mac, target.client).workbook
    assert not target.device.commits


@pytest.mark.parametrize('source,workbook,sha', [('EMBEDDED_DEFAULT', None, None), ('INSTALLED', None, 'a'*64),
                                               ('INSTALLED', '11111111-2222-4333-8444-555555555555', 'b'*64)])
def test_missing_and_outdated_workbooks_are_installed_without_topic_writes(target, source, workbook, sha):
    target.device.loaded_source = source
    target.device.active_wb = workbook
    target.device.active_sha = sha
    answer = target.device.answer
    def installed(body):
        value = answer(body)
        if body.startswith('svc.result ') and value.get('state') == 'succeeded':
            target.device.loaded_source = 'INSTALLED'
        return value
    target.device.answer = installed
    before = dict(target.values)
    result = m.run('bench', target.preview.mac, target.client)
    assert result.workbook == 'Workbook: installed v3'
    assert target.device.commits == 1 and target.values == before
    assert target.calls == [(OLD, '2.6.2')]
    assert not any(c.startswith(('cfg set ', 'wifi_join ')) for c in target.device.plain)
    assert m.run('bench', target.preview.mac, target.client).workbook == 'Workbook: up to date'
    assert target.device.commits == 1


def test_matching_workbook_id_with_different_sha_is_replaced(target):
    source = m.experiment_settings.workbook_source(target.client, OLD, '2.6.2')
    target.device.active_wb, target.device.active_sha = source.workbook_version_id, 'b'*64
    result = m.run('bench', target.preview.mac, target.client)
    assert 'installed' in result.workbook and target.device.commits == 1


def test_legacy_capabilities_skip_serial_push(target, monkeypatch):
    snapshot = replace(target.preview.device, identity=replace(target.preview.device.identity, capabilities=frozenset({'identity'})))
    monkeypatch.setattr(m.firmware_update, '_snapshot', lambda c: snapshot)
    assert 'lacks journaled serial' in m.run('bench', target.preview.mac, target.client).workbook
    assert not target.calls and not target.device.commits


def test_install_failure_does_not_erase_successful_wifi_save(target):
    info = wifi_settings.read_device('bench')
    saved = wifi_settings.save_wifi(info, 'Field hub 1', 'test-password')
    target.device.outcome, target.device.error_reason = 'failed', 'compile_failed'
    extra = m.run('bench', info.mac, target.client)
    assert saved.connected and saved.ssid == 'Field hub 1'
    assert 'not checked' in extra.workbook and 'compile_failed' in extra.workbook
    assert not any(c.startswith('cfg set ') for c in target.device.plain)


def test_swapped_board_never_resolves_or_installs(target):
    result = m.run('bench', '00:11:22:33:44:55', target.client)
    assert 'changed' in result.workbook and not target.calls and not target.device.commits


def test_unknown_cloud_error_is_separate_and_sanitized(target):
    target.client.resolve_programming = lambda *a, **kw: (_ for _ in ()).throw(RuntimeError('private transport detail'))
    result = m.run('bench', target.preview.mac, target.client)
    assert 'cloud lookup' in result.workbook and 'private' not in result.workbook


@pytest.mark.parametrize('foreign', [False, True])
def test_automatic_check_preserves_saved_and_foreign_operations(target, foreign):
    from flash_gui.tests.test_wifi_settings import _pending_schedule
    info,journal,entry=_pending_schedule((target.device,target.handles,target.directory),'staging')
    if foreign:journal.path.unlink()
    before=dict(target.device.records[entry['op']])
    result=m.run('bench',info.mac,target.client)
    assert 'not checked' in result.workbook and not target.device.commits
    assert target.device.records[entry['op']]==before
    assert not journal.path.with_suffix('.previous.json').exists()
    if not foreign:assert journal.load()==entry
