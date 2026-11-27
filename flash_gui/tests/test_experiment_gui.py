# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only
from dataclasses import replace
from types import SimpleNamespace
import pytest

from flash_gui import gui, experiment_gui as g, experiment_settings as e
from flash_gui.tests.test_wifi_gui import Var, Widget
from flash_gui.tests.test_wifi_settings import usb  # noqa: F401
from flash_gui.tests.test_experiment_settings import target, NEW  # noqa: F401


def dialog(monkeypatch, target):
    w=object.__new__(g.ExperimentDialog)
    w.target,w.port,w.working=target.preview,'bench',False
    w.summary,w.message,w.cloud=Var(),Var(),Var()
    w.maintenance_text=Var()
    for name in ['read_btn','cloud_btn','change_btn','close_btn']:setattr(w,name,Widget())
    posted=[]
    w.app=SimpleNamespace(client=target.client,settings=SimpleNamespace(mqtt_endpoints={}),log=lambda text:None,
                          _current_experiment=lambda:SimpleNamespace(id=NEW,name='Destination example'),
                          _post=lambda fn,*args:posted.append(lambda:fn(*args)))
    w.winfo_exists=lambda:True
    class Thread:
        def __init__(self,*,target,daemon):self.target=target
        def start(self):self.target()
    monkeypatch.setattr(g.threading,'Thread',Thread)
    return w,lambda:[fn() for fn in posted]


def test_read_only_destination_window_opens_without_login_release_or_experiment(monkeypatch):
    calls=[]
    app=SimpleNamespace(busy=False,_selected_port=lambda:'bench',_set_busy=calls.append)
    monkeypatch.setattr(g,'ExperimentDialog',lambda owner,port:calls.append(port))
    gui.App._open_experiment_settings(app)
    assert app.busy and calls==[True,'bench']


def test_unknown_environment_requires_operator_confirmation_and_keeps_api_env_visible(monkeypatch,target):
    w,finish=dialog(monkeypatch,target)
    w.target=replace(w.target,environment=None)
    messages,calls=[],[]
    def confirm(title,text,**kw):messages.append(text);return True
    monkeypatch.setattr(g.messagebox,'askyesno',confirm)
    def change(*args,**kw):
        calls.append((kw,w.working,w.change_btn.state))
        return target.preview
    monkeypatch.setattr(e,'change_experiment',change)
    w._change();finish()
    assert 'API environment: dev' in messages[0] and 'UNKNOWN' in messages[0]
    assert calls[0][0]['allow_unknown_environment'] is True
    assert calls[0][1:] == (True,'disabled')
    assert 'verified after reboot' in w.message.value


def test_cancelled_confirmation_performs_no_work(monkeypatch,target):
    w,_=dialog(monkeypatch,target)
    monkeypatch.setattr(g.messagebox,'askyesno',lambda *a,**kw:False)
    monkeypatch.setattr(e,'change_experiment',lambda *a,**kw:pytest.fail('no consent'))
    w._change()
    assert not w.working


def test_known_environment_mismatch_stops_before_confirmation(monkeypatch,target):
    w,_=dialog(monkeypatch,target)
    w.target=replace(w.target,environment='prod')
    monkeypatch.setattr(g.messagebox,'askyesno',lambda *a,**kw:pytest.fail('must refuse'))
    w._change()
    assert 'differ' in w.message.value and not w.working


def test_failed_worker_revokes_stale_target_and_keeps_stage_message(monkeypatch,target):
    w,finish=dialog(monkeypatch,target)
    monkeypatch.setattr(g.messagebox,'askyesno',lambda *a,**kw:True)
    def fail(*a,**kw):raise e.ExperimentSettingsError('Workbook installed; topic write unconfirmed')
    monkeypatch.setattr(e,'change_experiment',fail)
    w._change();finish()
    assert w.target is None and w.change_btn.state=='disabled'
    assert 'topic write unconfirmed' in w.message.value


def test_device_display_does_not_claim_certificate_validity(monkeypatch,target):
    w,_=dialog(monkeypatch,target)
    w._show(replace(target.preview,environment=None))
    assert 'Unknown / unverified' in w.summary.value
    assert 'certificate remains unverified' in w.cloud.value


def test_primary_experiment_success_precedes_optional_checks(monkeypatch,target):
    w,_=dialog(monkeypatch,target)
    w.app._post=lambda fn,*a:fn(*a)
    monkeypatch.setattr(g.messagebox,'askyesno',lambda *a,**kw:True)
    changed=replace(target.preview,timezone_result='Timezone needs attention: old → new')
    monkeypatch.setattr(e,'change_experiment',lambda *a,**kw:changed)
    observed=[]
    def slow(*a,**kw):
        observed.append((w.working,w.message.value,w.maintenance_text.value))
        raise RuntimeError('optional backend failed')
    monkeypatch.setattr(g.maintenance,'run',slow)
    w._change()
    assert observed and observed[0][0]
    assert 'verified after reboot' in observed[0][1] and 'Timezone needs attention' in observed[0][1]
    assert 'Checking workbook' in observed[0][2]
    assert 'verified after reboot' in w.message.value and 'Timezone needs attention' in w.message.value
    assert not w.working


@pytest.mark.parametrize('optional_failure',[False,True])
def test_real_experiment_switch_warning_stays_visible_and_logged(monkeypatch,target,optional_failure):
    original=target.device.console
    def console(command):
        reply=original(command)
        if command=='reboot':target.values['timezone']='Etc/UTC'
        return reply
    target.device.console=console
    window,finish=dialog(monkeypatch,target)
    logs=[];window.app.log=logs.append
    monkeypatch.setattr(g.messagebox,'askyesno',lambda *a,**kw:True)
    if optional_failure:
        monkeypatch.setattr(g.maintenance,'run',lambda *a,**kw:(_ for _ in ()).throw(RuntimeError('optional lookup unavailable')))
    window._change();finish()
    warning='Timezone needs attention: Europe/Amsterdam → Etc/UTC'
    assert warning in window.target.timezone_result
    visible='\n'.join(v.value for v in (window.summary,window.message,window.maintenance_text))
    assert warning in visible
    assert any(warning in line for line in logs)


def _platform_dialog(monkeypatch, target, *, remove=True):
    w, finish = dialog(monkeypatch, target)
    w.remove_previous = Var(remove)
    w.app.experiments = [SimpleNamespace(id=target.preview.experiment_id, name='Old example')]
    calls = []
    monkeypatch.setattr(e, 'onboard_destination',
                        lambda *a, **kw: calls.append(('onboard', a[3])) or 'dev-1')
    monkeypatch.setattr(e, 'remove_previous_binding',
                        lambda client, device, previous, name: calls.append(('remove', previous, name)) or 'openJII: removed from Old example.')
    return w, finish, calls


def test_onboarding_runs_first_and_removal_after_the_verified_change(monkeypatch,target):
    w, finish, calls = _platform_dialog(monkeypatch, target)
    texts = []
    def confirm(title, text, **kw): texts.append(text); return True
    monkeypatch.setattr(g.messagebox, 'askyesno', confirm)
    monkeypatch.setattr(e, 'change_experiment', lambda *a, **kw: calls.append(('change',)) or target.preview)
    w._change(); finish()
    assert calls == [('onboard', NEW), ('change',), ('remove', target.preview.experiment_id, 'Old example')]
    assert 'First onboard it to Destination example on openJII' in texts[0]
    assert 'remove it from Old example (' in texts[0]
    assert 'openJII: onboarded to Destination example' in w.message.value
    assert 'openJII: removed from Old example.' in w.message.value


def test_a_refused_onboarding_never_touches_the_device(monkeypatch,target):
    w, finish, calls = _platform_dialog(monkeypatch, target)
    monkeypatch.setattr(g.messagebox, 'askyesno', lambda *a, **kw: True)
    def refuse(*a, **kw): raise e.ExperimentSettingsError('No device settings were changed. openJII refused (403)')
    monkeypatch.setattr(e, 'onboard_destination', refuse)
    monkeypatch.setattr(e, 'change_experiment', lambda *a, **kw: pytest.fail('device written after a refusal'))
    w._change(); finish()
    assert 'No device settings were changed' in w.message.value and w.target is None


def test_a_failed_device_change_reports_the_binding_and_skips_removal(monkeypatch,target):
    w, finish, calls = _platform_dialog(monkeypatch, target)
    monkeypatch.setattr(g.messagebox, 'askyesno', lambda *a, **kw: True)
    def fail(*a, **kw): raise e.ExperimentSettingsError('Workbook installed; topic write unconfirmed')
    monkeypatch.setattr(e, 'change_experiment', fail)
    w._change(); finish()
    assert calls == [('onboard', NEW)]
    assert 'openJII: onboarded to Destination example' in w.message.value
    assert 'topic write unconfirmed' in w.message.value


def test_unticked_removal_keeps_the_old_binding(monkeypatch,target):
    w, finish, calls = _platform_dialog(monkeypatch, target, remove=False)
    monkeypatch.setattr(g.messagebox, 'askyesno', lambda *a, **kw: True)
    monkeypatch.setattr(e, 'change_experiment', lambda *a, **kw: target.preview)
    w._change(); finish()
    assert [c[0] for c in calls] == ['onboard']


def test_an_empty_exception_is_still_a_failure(monkeypatch,target):
    w, finish, calls = _platform_dialog(monkeypatch, target)
    monkeypatch.setattr(g.messagebox, 'askyesno', lambda *a, **kw: True)
    def silent(*a, **kw): raise RuntimeError()
    monkeypatch.setattr(e, 'onboard_destination', silent)
    w._change(); finish()
    assert w.message.value == 'RuntimeError' and w.target is None


def test_a_different_board_on_refresh_keeps_the_verified_result(monkeypatch,target):
    w, _, calls = _platform_dialog(monkeypatch, target)
    w.app._post = lambda fn, *a: fn(*a)
    monkeypatch.setattr(g.messagebox, 'askyesno', lambda *a, **kw: True)
    monkeypatch.setattr(e, 'change_experiment', lambda *a, **kw: target.preview)
    monkeypatch.setattr(g.maintenance, 'run', lambda *a, **kw: g.maintenance.MaintenanceResult('Workbook: ok', 'Fallback timezone: ok'))
    other = replace(target.preview, device=replace(target.preview.device,
                    identity=replace(target.preview.device.identity, mac='02:00:00:00:00:99')))
    monkeypatch.setattr(e, 'read_target', lambda *a, **kw: other)
    w._change()
    assert w.target.mac == target.preview.mac
    assert 'openJII: onboarded to Destination example' in w.message.value


def test_a_change_that_cannot_happen_adds_no_binding(monkeypatch,target):
    w, finish, calls = _platform_dialog(monkeypatch, target)
    monkeypatch.setattr(g.messagebox, 'askyesno', lambda *a, **kw: True)
    def impossible(*a, **kw): raise e.ExperimentSettingsError('The stored topic is not a supported seven-segment ingest topic')
    monkeypatch.setattr(e, 'preflight', impossible)
    w._change(); finish()
    assert calls == [] and 'seven-segment' in w.message.value


def test_a_rerun_at_the_destination_says_why_nothing_is_removed(monkeypatch,target):
    w, finish, calls = _platform_dialog(monkeypatch, target)
    w.target = replace(w.target, experiment_id=NEW)
    texts = []
    def confirm(title, text, **kw): texts.append(text); return True
    monkeypatch.setattr(g.messagebox, 'askyesno', confirm)
    monkeypatch.setattr(e, 'change_experiment', lambda *a, **kw: target.preview)
    w._change(); finish()
    assert 'already sends to the destination' in texts[0]
    assert [c[0] for c in calls] == ['onboard']
