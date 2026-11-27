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
    # A registry where the signed-in account manages this board (HAR-345).
    bound = {OLD}
    client.find_device = lambda mac: {'id': 'dev-1', 'thingName': preview.device.identity.thing_name}
    client.bind_device = lambda device_id, experiment_id: bound.add(experiment_id)
    client.device_experiment_ids = lambda device_id: set(bound)
    client.remove_experiment_device = lambda experiment_id, device_id: bound.discard(experiment_id) is None
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


# ── HAR-345: openJII bindings around a verified experiment change ──────────

THING = 'AMBYTE_02000000000001'


class Platform:
    """A registry that records binding writes and answers the read-back."""
    def __init__(self, *, visible=True, thing=THING, refuse=None, lands=True, remove_status=True, unreadable=False):
        self.visible, self.thing, self.refuse, self.lands = visible, thing, refuse, lands
        self.remove_status, self.unreadable = remove_status, unreadable
        self.bound, self.bind_calls, self.removed = {OLD}, [], []

    def find_device(self, mac):
        return {'id': 'dev-1', 'thingName': self.thing, 'serialNumber': mac} if self.visible else None

    def bind_device(self, device_id, experiment_id):
        self.bind_calls.append((device_id, experiment_id))
        if self.lands:
            self.bound.add(experiment_id)
        if self.refuse:
            raise self.refuse

    def device_experiment_ids(self, device_id):
        if self.unreadable:
            raise OpenJIIError('reading device experiments failed (500)')
        return set(self.bound)

    def remove_experiment_device(self, experiment_id, device_id):
        if isinstance(self.remove_status, Exception):
            raise self.remove_status
        self.removed.append((experiment_id, device_id))
        return self.remove_status


def test_onboarding_binds_the_destination_and_returns_the_device():
    platform = Platform()
    assert e.onboard_destination(platform, MAC, THING, NEW, 'Destination') == 'dev-1'
    assert platform.bind_calls == [('dev-1', NEW)] and NEW in platform.bound


def test_a_refused_onboarding_stops_before_any_device_write():
    platform = Platform(refuse=OpenJIIError('onboarding device failed (403): no access', 403), lands=False)
    with pytest.raises(e.ExperimentSettingsError, match='No device settings were changed.*refused.*403'):
        e.onboard_destination(platform, MAC, THING, NEW, 'Destination')


def test_a_definite_refusal_stops_even_when_an_older_binding_exists():
    platform = Platform(refuse=OpenJIIError('onboarding device failed (403): no access', 403), lands=False)
    platform.bound.add(NEW)  # bound earlier by someone with the rights
    with pytest.raises(e.ExperimentSettingsError, match='refused'):
        e.onboard_destination(platform, MAC, THING, NEW)


@pytest.mark.parametrize('error', [OpenJIIError('onboarding response could not be read'),
                                   OpenJIIError('onboarding device failed (502): bad gateway', 502),
                                   TimeoutError('timed out')])
def test_an_ambiguous_write_that_landed_counts(error):
    assert e.onboard_destination(Platform(refuse=error, lands=True), MAC, THING, NEW) == 'dev-1'


def test_an_accepted_write_missing_on_read_back_stops_without_calling_it_a_refusal():
    platform = Platform(lands=False)
    with pytest.raises(e.ExperimentSettingsError, match='accepted the request but does not list') as error:
        e.onboard_destination(platform, MAC, THING, NEW)
    assert 'None' not in str(error.value)


def test_an_unconfirmed_binding_stops_the_change():
    platform = Platform(unreadable=True)
    with pytest.raises(e.ExperimentSettingsError, match='could not confirm'):
        e.onboard_destination(platform, MAC, THING, NEW)


@pytest.mark.parametrize('platform,reason', [
    (Platform(visible=False), 'cannot see'),
    (Platform(thing='AMBYTE_OTHER'), 'different client name'),
])
def test_onboarding_stops_for_a_device_it_cannot_match(platform, reason):
    with pytest.raises(e.ExperimentSettingsError, match=reason):
        e.onboard_destination(platform, MAC, THING, NEW)
    assert platform.bind_calls == []


def test_onboarding_never_lets_an_unexpected_failure_through():
    class Broken(Platform):
        def find_device(self, mac):
            raise ValueError('unexpected payload')
    with pytest.raises(e.ExperimentSettingsError, match='could not be read'):
        e.onboard_destination(Broken(), MAC, THING, NEW)


@pytest.mark.parametrize('outcome,expected', [
    (True, 'removed from Old example'),
    (False, 'no binding to Old example'),
    (OpenJIIError('removing the device from the experiment failed (403): no', 403), 'removal from Old example (' ),
])
def test_removal_reports_its_outcome_and_never_raises(outcome, expected):
    text = e.remove_previous_binding(Platform(remove_status=outcome), 'dev-1', OLD, 'Old example')
    assert expected in text and OLD in text


@pytest.mark.parametrize('still_bound,expected', [(False, 'removed from'), (True, 'did not take effect')])
def test_an_ambiguous_removal_is_settled_by_the_read_back(still_bound, expected):
    platform = Platform(remove_status=TimeoutError('timed out'))
    if not still_bound:
        platform.bound.discard(OLD)  # the DELETE committed before the timeout
    assert expected in e.remove_previous_binding(platform, 'dev-1', OLD, 'Old example')


def test_client_binding_and_read_back_map_statuses():
    from flash_gui.openjii_client import OpenJIIClient
    client = object.__new__(OpenJIIClient)
    seen = []
    def reply(status, payload=None, raise_=None):
        def request(method, path, body=None, timeout=60):
            seen.append((method, path, body))
            if raise_:
                raise raise_
            return status, payload
        return request
    client._request = reply(200, {'thingName': 'x'})  # config shape is irrelevant here
    client.bind_device('dev-1', NEW)
    assert seen[-1] == ('POST', '/api/v1/devices/dev-1/onboard', {'experimentIds': [NEW], 'includeWorkbook': False})
    client._request = reply(403, {'message': 'forbidden'})
    with pytest.raises(OpenJIIError, match='403') as refused:
        client.bind_device('dev-1', NEW)
    assert refused.value.status == 403
    client._request = reply(None, raise_=ValueError('bad json'))
    with pytest.raises(OpenJIIError, match='could not be read') as unread:
        client.bind_device('dev-1', NEW)
    assert unread.value.status is None
    client._request = reply(200, [{'id': OLD}, {'id': NEW}, 'junk'])
    assert client.device_experiment_ids('dev-1') == {OLD, NEW}
    client._request = reply(204)
    assert client.remove_experiment_device(OLD, 'dev-1') is True
    assert seen[-1][:2] == ('DELETE', f'/api/v1/experiments/{OLD}/devices/dev-1')
    client._request = reply(404, {'message': 'not attached'})
    assert client.remove_experiment_device(OLD, 'dev-1') is False
