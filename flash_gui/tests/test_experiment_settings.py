# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only
"""Direct topic/workbook maintenance using the real console and service client."""
from dataclasses import replace
from types import SimpleNamespace
import pytest

from flash_gui import ambyte_serial, config, experiment_settings as e
from flash_gui.openjii_client import OpenJIIError
from flash_gui.tests.test_wifi_settings import usb  # noqa: F401
from flash_gui.tests.test_workbook_procedure import _programming
from flash_gui.tests.test_service_gui import MAC

OLD = '11111111-2222-4333-8444-555555555555'
NEW = 'aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee'
ROOT = f'experiment/data_ingest/v1/{OLD}/multispeq/v1.0/AMBYTE_{{MAC}}'


@pytest.fixture
def target(usb, monkeypatch):
    device, handles, directory = usb
    values = dict(mqtt_uri='mqtts://example.test:8883', mqtt_client_id='AMBYTE_{MAC}',
                  mqtt_topic_root=ROOT, status_topic=ROOT+'/status',
                  command_topic='device/scripts/v1/Ambyte/2/AMBYTE_{MAC}',
                  device_name='Field_{MAC}', timezone='Europe/Amsterdam')
    original_answer = device.answer
    def answer(body):
        value = original_answer(body)
        if body.startswith('svc.result ') and value.get('kind') == 'schedule' and value.get('state') == 'succeeded':
            device.active_sha, device.active_version, device.active_wb = value['sha256'], value['version'], value['workbook']
        return value
    device.answer = answer
    original = device.console
    def console(command):
        if command.startswith('cfg get '):
            device.plain.append(command)
            key = command.split()[2]
            return f'{key} = {values[key]}\r\nambyte> '.encode()
        if command.startswith('cfg set '):
            device.plain.append(command)
            _, _, key, value = command.split(' ', 3)
            if key == getattr(device, 'fail_key', None):
                return b'cfg set: ESP_FAIL\r\nCommand returned non-zero error code\r\nambyte> '
            values[key] = value
            return f'cfg set {key} = {value}  (reboot to apply)\r\nambyte> '.encode()
        if command == 'reboot':
            device.plain.append(command)
            return b'ambyte> '
        return original(command)
    device.console = console
    monkeypatch.setattr(ambyte_serial.list_ports, 'comports', lambda: [SimpleNamespace(device='bench', serial_number=MAC)])
    calls = []
    def resolve(experiment_id, fw_version, log):
        calls.append((experiment_id, fw_version))
        return _programming()
    client = SimpleNamespace(env=config.ENVIRONMENTS['dev'], resolve_programming=resolve)
    preview = e.read_target('bench', {'dev':'example.test'})
    return SimpleNamespace(device=device, handles=handles, directory=directory, values=values,
                           client=client, calls=calls, preview=preview)


def test_direct_change_preserves_broker_credentials_and_raw_topic_tail(target):
    original = dict(target.values)
    result = e.change_experiment(target.preview, target.client, NEW)
    assert result.experiment_id == NEW and result.broker == 'example.test'
    assert target.calls == [(NEW, '2.6.2')]
    expected = ROOT.replace(OLD, NEW)
    assert target.values['mqtt_topic_root'] == expected
    assert target.values['status_topic'] == expected + '/status'
    assert all(target.values[k] == v for k,v in original.items() if k not in {'mqtt_topic_root','status_topic'})
    mutations = [c for c in target.device.plain if c.startswith('cfg set ')]
    assert mutations == [f'cfg set mqtt_topic_root {expected}', f'cfg set status_topic {expected}/status']
    assert target.device.commits == 1 and target.device.joins == 0
    assert all(not h.is_open for h in target.handles)


def test_destination_access_error_does_not_open_usb_or_call_onboarding(target):
    def denied(*args, **kw):raise OpenJIIError('403 destination experiment inaccessible')
    target.client.resolve_programming = denied
    count = len(target.handles)
    with pytest.raises(e.ExperimentSettingsError, match='No device settings were changed.*403'):
        e.change_experiment(target.preview, target.client, NEW)
    assert len(target.handles) == count and not target.device.commits


def test_unknown_environment_requires_explicit_confirmation(target):
    preview = replace(target.preview, environment=None)
    with pytest.raises(e.ExperimentSettingsError, match='unverified'):
        e.change_experiment(preview, target.client, NEW)
    assert not target.calls
    assert e.change_experiment(preview, target.client, NEW, allow_unknown_environment=True).environment is None


def test_known_environment_mismatch_never_resolves_or_mutates(target):
    target.client.env = config.ENVIRONMENTS['prod']
    with pytest.raises(e.ExperimentSettingsError, match='different environment'):
        e.change_experiment(target.preview, target.client, NEW)
    assert not target.calls and not target.device.commits


@pytest.mark.parametrize('topic', ['experiment/data_ingest/v1/legacy/multispeq/v1.0/a', ROOT+'/1234', ROOT.replace('AMBYTE_{MAC}','x'*240)])
def test_unsupported_or_oversize_topic_refused(topic):
    with pytest.raises(e.ExperimentSettingsError):e.topic_for_experiment(topic, NEW)


def test_legacy_status_migration_destination_is_refused():
    with pytest.raises(e.ExperimentSettingsError, match='migration'):
        e.topic_for_experiment(ROOT, 'a3b865d1-0000-4000-8000-000000000000')


def test_swapped_board_refused_before_workbook_or_topic_write(target):
    target.device.mac = '00:11:22:33:44:55'
    with pytest.raises(e.ExperimentSettingsError, match='changed'):
        e.change_experiment(target.preview, target.client, NEW)
    assert not target.device.commits and not any(c.startswith('cfg set ') for c in target.device.plain)


def test_failed_workbook_never_changes_topics_or_reboots(target):
    target.device.outcome, target.device.error_reason = 'failed','compile_failed'
    with pytest.raises(e.ExperimentSettingsError):e.change_experiment(target.preview, target.client, NEW)
    assert not any(c.startswith('cfg set ') or c == 'reboot' for c in target.device.plain)


def test_partial_topic_write_is_truthfully_reported_and_rerunnable(target):
    target.device.fail_key = 'status_topic'
    with pytest.raises(e.ExperimentSettingsError, match='topic writes may be partial'):
        e.change_experiment(target.preview, target.client, NEW)
    assert target.values['mqtt_topic_root'] == ROOT.replace(OLD, NEW)
    assert target.values['status_topic'] == ROOT+'/status'
    assert 'reboot' not in target.device.plain
    del target.device.fail_key
    preview = e.read_target('bench', {'dev':'example.test'})
    assert e.change_experiment(preview,target.client,NEW).experiment_id == NEW


def test_cloud_status_is_optional_registry_evidence_not_certificate_validation(target):
    target.client.find_device = lambda mac: {'thingName':target.preview.device.identity.thing_name,
                                             'status':'active','connectivity':{'connected':True}}
    text = e.cloud_status(target.preview,target.client)
    assert 'dev registry: active, connection connected' in text and 'does not compare' in text


def test_reboot_enumeration_never_opens_unrelated_board(target, monkeypatch):
    monkeypatch.setattr(ambyte_serial.list_ports, 'comports', lambda: [
        SimpleNamespace(device='wrong', serial_number='00:11:22:33:44:55'),
        SimpleNamespace(device='bench', serial_number=MAC)])
    assert e.change_experiment(target.preview,target.client,NEW).mac == MAC
    assert all(h.port=='bench' for h in target.handles)


def test_reads_and_rechecks_wait_for_loaded_schedule_after_usb_reset(target, monkeypatch):
    original_factory = ambyte_serial.serial.Serial
    original_console = target.device.console
    reads = []
    def factory():
        target.device.loaded_source = 'NONE'
        target.device.startup_reads = 0
        return original_factory()
    def console(command):
        if command == 'identity':
            target.device.startup_reads += 1
            reads.append(1)
            if target.device.startup_reads >= 3:target.device.loaded_source = 'INSTALLED'
        return original_console(command)
    monkeypatch.setattr(ambyte_serial.serial,'Serial',factory)
    target.device.console = console
    preview = e.read_target('bench',{'dev':'example.test'})
    assert preview.device.identity.schedule.source=='INSTALLED'
    assert e.change_experiment(preview,target.client,NEW).experiment_id==NEW
    assert len(reads)>6


def test_post_reboot_transient_open_failure_is_retried_on_matching_mac_only(target, monkeypatch):
    factory = ambyte_serial.serial.Serial
    failures = []
    def reconnect():
        if 'reboot' in target.device.plain and not failures:
            failures.append(1)
            raise OSError('old enumeration disappeared')
        return factory()
    monkeypatch.setattr(ambyte_serial.serial,'Serial',reconnect)
    assert e.change_experiment(target.preview,target.client,NEW).experiment_id==NEW
    assert failures==[1]


def test_wrong_workbook_after_success_result_blocks_topic_writes(target):
    answer = target.device.answer
    def wrong(body):
        result = answer(body)
        if body.startswith('svc.result ') and result.get('kind')=='schedule' and result.get('state')=='succeeded':
            target.device.active_wb=None
        return result
    target.device.answer=wrong
    with pytest.raises(e.ExperimentSettingsError,match='workbook was not confirmed active'):
        e.change_experiment(target.preview,target.client,NEW)
    assert not any(c.startswith('cfg set ') for c in target.device.plain)


def test_experiment_change_warns_about_unexplained_timezone_drift(target):
    original=target.device.console
    def console(cmd):
        result=original(cmd)
        if cmd=='reboot':target.values['timezone']='Etc/UTC'
        return result
    target.device.console=console
    result=e.change_experiment(target.preview,target.client,NEW)
    assert 'Timezone needs attention: Europe/Amsterdam → Etc/UTC' in result.timezone_result
