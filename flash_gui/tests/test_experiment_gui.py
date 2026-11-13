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
        assert w.working and w.change_btn.state=='disabled'
        calls.append(kw)
        return target.preview
    monkeypatch.setattr(e,'change_experiment',change)
    w._change();finish()
    assert 'API environment: dev' in messages[0] and 'UNKNOWN' in messages[0]
    assert calls[0]['allow_unknown_environment'] is True
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
    def slow(*a,**kw):
        assert w.working and 'verified after reboot' in w.message.value
        assert 'Timezone needs attention' in w.message.value and 'Checking workbook' in w.maintenance_text.value
        raise RuntimeError('optional backend failed')
    monkeypatch.setattr(g.maintenance,'run',slow)
    w._change()
    assert 'verified after reboot' in w.message.value and not w.working
