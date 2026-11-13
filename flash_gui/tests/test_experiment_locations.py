# SPDX-License-Identifier: GPL-3.0-only
"""Location API validation, explicit choice and guarded real-console writes."""
import json
from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace
import pytest

from flash_gui import config, openjii_client as api, location_settings as loc, experiment_settings as exp, ambyte_serial
from flash_gui.tests.test_wifi_settings import usb  # noqa: F401
from flash_gui.tests.test_experiment_settings import target, OLD  # noqa: F401

AMSTERDAM=api.ExperimentLocation('site-a','Synthetic Amsterdam',52.3730,4.8924)
ULURU=api.ExperimentLocation('site-b','Synthetic Uluru',-25.3444,131.0369)


def client_for(payload,status=200):
    client=api.OpenJIIClient(config.ENVIRONMENTS['dev'],'synthetic-test-key')
    calls=[]
    def request(method,path):calls.append((method,path));return status,payload
    client._request=request
    return client,calls


def entry(**changes):
    return dict(id='site-a',name='Synthetic Amsterdam',latitude=52.3730,longitude=4.8924,**changes)


@pytest.mark.parametrize('items',[[],[entry()],[entry(),dict(entry(),id='site-b',name='Second')]])
def test_location_api_valid_empty_single_and_several(items):
    client,calls=client_for(items)
    locations=client.list_experiment_locations(OLD)
    assert len(locations)==len(items)
    assert calls==[('GET',f'/api/v1/experiments/{OLD}/locations')]
    if locations:assert locations[0]==AMSTERDAM


@pytest.mark.parametrize('lat,lon',[(True,5),(52,False),(None,5),('52',5),(float('nan'),5),
    (52,float('inf')),(91,5),(-91,5),(52,181),(52,-181),(0,0),pytest.param(10**10000,5,id='oversized-number')])
def test_location_api_rejects_invalid_coordinates_without_echoing_them(lat,lon):
    client,_=client_for([dict(entry(),latitude=lat,longitude=lon)])
    with pytest.raises(api.OpenJIIError,match='invalid or unset coordinates'):client.list_experiment_locations(OLD)


@pytest.mark.parametrize('payload,status',[(None,200),({},200),([None],200),([dict(entry(),name='')],200),
    ([dict(entry(),id='')],200),([entry(),entry()],200),({'latitude':12.3456},403)])
def test_location_api_refuses_malformed_or_inaccessible_lists(payload,status):
    client,_=client_for(payload,status)
    with pytest.raises(api.OpenJIIError) as error:client.list_experiment_locations(OLD)
    assert '12.3456' not in str(error.value)


@pytest.fixture
def location_bench(target):
    """Production AmbyteConsole over the USB byte transport and captured PR3 shape."""
    original=target.device.console
    state=SimpleNamespace(modern=True,version='2.16.0',stored=None,offset=3600,bad_readback=False,
                          lost=None,after_boot=False,post_boot_position=None,pinned=False)
    group=json.loads((Path(__file__).parent/'fixtures/timezone-contract/physical-live.json').read_text())
    def console(cmd):
        if cmd.startswith('sync loc '):
            target.device.plain.append(cmd)
            state.stored=tuple(map(float,cmd.split()[2:4]))
            state.pinned=False
            if state.lost=='location':return b''
            return f'loc: lat={state.stored[0]:.4f} lon={state.stored[1]:.4f} tz=+1 deployment=-\r\nambyte> '.encode()
        if cmd=='sync loc':
            target.device.plain.append(cmd)
            lat,lon=state.post_boot_position or state.stored or (52.173,5.819)
            return f'loc: lat={lat:.4f} lon={lon:.4f} tz=+1 deployment=-\r\nambyte> '.encode()
        if cmd=='rtc':return b'RTC: 2026-11-13 09:00:00 (1794560400)\r\nambyte> '
        if cmd=='status':
            sign='-' if state.offset<0 else '+';off=abs(state.offset)
            return f' - RTC local: 2026-11-13 10:00:00 (UTC{sign}{off//3600:02d}:{off%3600//60:02d})\r\nambyte> '.encode()
        reply=original(cmd)
        if cmd.startswith('cfg set timezone ') and state.lost=='timezone':return b''
        if cmd=='reboot':state.after_boot=True
        if cmd=='identity':
            lines=reply.decode().splitlines()
            for i,line in enumerate(lines):
                if not line.startswith('{'):continue
                obj=json.loads(line);obj['fw']=state.version
                if state.modern:
                    obj.update(json.loads(json.dumps(group)));obj['capabilities'].append('timezone.auto')
                    if state.stored:
                        lat,lon=state.stored
                        if state.bad_readback:lat+=1
                        obj['location'].update(set=True,source='configured',lat=lat,lon=lon,accuracy_m=None,
                            configured=dict(set=True,lat=lat,lon=lon))
                    obj['timezone']['configured']=target.values['timezone']
                    if state.pinned:
                        obj['timezone'].update(pinned=True,source='manual',active=target.values['timezone'])
                lines[i]=json.dumps(obj)
            return '\r\n'.join(lines).encode()
        return reply
    target.device.console=console
    def preview():return exp.read_target('bench',{'dev':'example.test'})
    target.location_state=state;target.read=preview
    return target


def writes(bench):
    return [cmd for cmd in bench.device.plain if cmd.startswith(('sync loc ','cfg set ')) or cmd=='reboot']


def test_pr3_persists_reads_configured_and_never_writes_timezone(location_bench):
    b=location_bench;before=dict(b.values);prompts=[];logs=[]
    result=loc.set_location(b.read(),AMSTERDAM,confirm=lambda text:prompts.append(text) or True,log=logs.append)
    assert 'configured readback verified after reboot' in result and 'Europe/Amsterdam' in result
    assert writes(b)==['sync loc 52.37300000 4.89240000','reboot']
    assert b.values==before and b.location_state.stored==(52.373,4.8924)
    assert len(prompts)==1 and 'Synthetic Amsterdam' in prompts[0]
    assert not any('52.373' in line or '4.8924' in line for line in logs)
    assert all(not handle.is_open for handle in b.handles)


@pytest.mark.parametrize('version',['2.9.0','2.14.0','2.16.0'])
def test_audited_legacy_persistence_timezone_reboot_and_expected_offset(location_bench,version):
    b=location_bench;b.location_state.modern=False;b.location_state.version=version
    b.values['timezone']='Europe/Berlin';prompts=[]
    result=loc.set_location(b.read(),AMSTERDAM,confirm=lambda text:prompts.append(text) or True)
    assert 'verified after reboot (UTC offset +3600s)' in result
    assert writes(b)==['sync loc 52.37300000 4.89240000','cfg set timezone Europe/Amsterdam','reboot']
    assert 'fallback timezone to Europe/Amsterdam' in prompts[0]
    assert 'sync loc' in b.device.plain[b.device.plain.index('reboot')+1:]


@pytest.mark.parametrize('offset',[0,34200])
def test_legacy_wrong_offset_is_saved_but_runtime_unverified(location_bench,offset):
    b=location_bench;b.location_state.modern=False;b.location_state.offset=offset
    result=loc.set_location(b.read(),AMSTERDAM,confirm=lambda _:True)
    assert 'saved, runtime unverified' in result and 'verified after reboot' not in result


def test_legacy_without_audited_persistence_refuses_before_confirmation(location_bench):
    b=location_bench;b.location_state.modern=False;b.location_state.version='2.8.0'
    result=loc.set_location(b.read(),AMSTERDAM,confirm=lambda _:pytest.fail('unsupported'))
    assert "can't store a location over USB" in result and not writes(b)


@pytest.mark.parametrize('lost',['location','timezone'])
def test_lost_reply_after_persistence_is_uncertain_and_does_not_echo_private_command(location_bench,lost):
    b=location_bench;b.location_state.modern=False;b.location_state.lost=lost
    result=loc.set_location(b.read(),AMSTERDAM,confirm=lambda _:True)
    assert 'change may be saved' in result and 'unchanged' not in result
    assert '52.373' not in result and 'reboot' not in writes(b)


def test_operator_decline_does_not_open_or_write(location_bench):
    b=location_bench;preview=b.read();opens=len(b.handles)
    result=loc.set_location(preview,AMSTERDAM,confirm=lambda _:False)
    assert 'operator declined' in result and len(b.handles)==opens and not writes(b)


def test_pr3_bad_configured_readback_is_uncertain(location_bench):
    b=location_bench;b.location_state.bad_readback=True
    result=loc.set_location(b.read(),AMSTERDAM,confirm=lambda _:True)
    assert 'change may be saved' in result and 'reboot' not in writes(b)


def test_legacy_postboot_position_must_match_print_precision(location_bench):
    b=location_bench;b.location_state.modern=False;b.location_state.post_boot_position=(52.374,4.8924)
    result=loc.set_location(b.read(),AMSTERDAM,confirm=lambda _:True)
    assert 'verification incomplete' in result


def test_device_change_during_confirmation_is_refused(location_bench):
    b=location_bench;preview=b.read()
    def confirm(_):b.values['device_name']='changed';return True
    result=loc.set_location(preview,AMSTERDAM,confirm=confirm)
    assert 'unchanged; device configuration changed' in result and not writes(b)


def test_unresolved_journal_precedes_confirmation(location_bench):
    from flash_gui.tests.test_wifi_settings import _pending_schedule
    b=location_bench;preview=b.read()
    _pending_schedule((b.device,b.handles,b.directory),'staging')
    result=loc.set_location(preview,AMSTERDAM,confirm=lambda _:pytest.fail('journal first'))
    assert 'resolve the saved service operation' in result and not writes(b)


def test_foreign_operation_refuses_before_any_location_write(location_bench):
    from flash_gui.tests.test_wifi_settings import _pending_schedule
    b=location_bench;preview=b.read()
    _,journal,_=_pending_schedule((b.device,b.handles,b.directory),'staging')
    journal.path.unlink() # A foreign device operation with no local saved intent.
    result=loc.set_location(preview,AMSTERDAM,confirm=lambda _:True)
    assert 'unchanged' in result and not writes(b)


@pytest.mark.parametrize('kind,selected,expected',[('none',AMSTERDAM,True),('same',AMSTERDAM,False),
    ('different',ULURU,True),('legacy',AMSTERDAM,False)])
def test_change_default_uses_configured_country_only(location_bench,kind,selected,expected):
    b=location_bench
    if kind=='legacy':b.location_state.modern=False
    if kind in {'same','different'}:b.location_state.stored=(52.0,5.0)
    identity=b.read().device.identity
    enabled,reason=loc.change_default(identity,selected)
    assert enabled is expected
    if kind=='legacy':assert 'unavailable on this firmware; choose explicitly' in reason


@pytest.mark.parametrize('mode',['signed-out','bad-topic','empty','invisible','several'])
def test_current_action_options_enable_only_readable_current_experiment(location_bench,mode):
    b=location_bench;target=b.read();calls=[]
    def listing(experiment):
        calls.append(experiment)
        if mode=='invisible':raise api.OpenJIIError('403')
        return [] if mode=='empty' else [AMSTERDAM,ULURU]
    client=SimpleNamespace(env=config.ENVIRONMENTS['dev'],list_experiment_locations=listing)
    if mode=='bad-topic':target=replace(target,device=replace(target.device,settings=dict(target.device.settings,mqtt_topic_root='legacy')))
    options=loc.current_options(target,None if mode=='signed-out' else client)
    assert bool(options.locations)==(mode=='several')
    assert bool(options.reason)==(mode!='several')
    assert calls==([] if mode in {'signed-out','bad-topic'} else [OLD])


def picker():
    from flash_gui.location_gui import LocationPicker
    from flash_gui.tests.test_wifi_gui import Var,Widget
    p=object.__new__(LocationPicker)
    p.value,p.reason,p.box=Var(),Var(),Widget()
    p.options=()
    return p


@pytest.mark.parametrize('locations',[(),(AMSTERDAM,),(AMSTERDAM,ULURU)])
def test_picker_single_prefill_and_multiple_require_explicit_choice(locations):
    p=picker();p.set_options(loc.LocationOptions(locations,'No locations' if not locations else ''))
    assert p.selected()==(AMSTERDAM if len(locations)==1 else None)
    assert p.box.state==('readonly' if locations else 'disabled')
    if len(locations)>1:
        p.value.set(f'{ULURU.label} [{ULURU.id}]')
        assert p.selected()==ULURU


def test_setup_selection_prefills_coordinates_then_allows_manual_override():
    from flash_gui import gui
    from flash_gui.tests.test_wifi_gui import Var
    p=picker();p.set_options(loc.LocationOptions((AMSTERDAM,)))
    app=SimpleNamespace(setup_location_picker=p,setup_lat=Var(),setup_lon=Var(),setup_tz=Var('Europe/Berlin'))
    gui.App._use_setup_location(app)
    assert (app.setup_lat.value,app.setup_lon.value,app.setup_tz.value)==('52.373','4.8924','')
    app.setup_lat.set('-25.3444');app.setup_lon.set('131.0369');app.setup_tz.set('Europe/Berlin')
    assert gui.timezones.setup_zone(app.setup_lat.value,app.setup_lon.value,app.setup_tz.value)[0]=='Europe/Berlin'


def test_setup_experiment_change_clears_only_unchanged_auto_prefill():
    from flash_gui import gui
    from flash_gui.tests.test_wifi_gui import Var
    app=SimpleNamespace(setup_location_picker=picker(),setup_lat=Var('52.373'),setup_lon=Var('4.8924'),
        _setup_location_prefill=('52.373','4.8924'))
    gui.App._reset_setup_location(app)
    assert app.setup_lat.value==app.setup_lon.value==''
    app.setup_lat.set('51');app.setup_lon.set('4');app._setup_location_prefill=('52.373','4.8924')
    gui.App._reset_setup_location(app)
    assert (app.setup_lat.value,app.setup_lon.value)==('51','4')


def test_stale_setup_location_response_cannot_override_the_current_selection():
    from flash_gui import gui
    client=object();calls=[]
    app=SimpleNamespace(client=client,_current_experiment=lambda:SimpleNamespace(id='current'),
        setup_location_picker=SimpleNamespace(set_options=calls.append),_use_setup_location=lambda:calls.append('used'))
    gui.App._apply_setup_locations(app,client,'old',loc.LocationOptions((AMSTERDAM,)))
    gui.App._apply_setup_locations(app,object(),'current',loc.LocationOptions((AMSTERDAM,)))
    assert not calls
    gui.App._apply_setup_locations(app,client,'current',loc.LocationOptions((AMSTERDAM,)))
    assert calls[-1]=='used'


def test_current_location_control_disabled_with_reason_and_enabled_after_explicit_choice(monkeypatch,target):
    from flash_gui.tests.test_experiment_gui import dialog
    from flash_gui.tests.test_wifi_gui import Var,Widget
    w,_=dialog(monkeypatch,target)
    w.current_location=picker();w.destination_location=picker()
    w.location_btn,w.also_btn=Widget(),Widget()
    w.current_location.set_options(loc.LocationOptions(reason='This experiment has no locations.'))
    w._controls()
    assert w.location_btn.state=='disabled' and 'no locations' in w.current_location.reason.value
    w.current_location.set_options(loc.LocationOptions((AMSTERDAM,ULURU)))
    w._controls();assert w.location_btn.state=='disabled'
    w.current_location.value.set(f'{AMSTERDAM.label} [{AMSTERDAM.id}]')
    w._controls();assert w.location_btn.state=='normal'


def test_experiment_location_failure_stays_separate_and_after_primary_success(monkeypatch,target):
    from flash_gui.tests.test_experiment_gui import dialog
    from flash_gui.tests.test_wifi_gui import Var
    from flash_gui import experiment_gui
    w,finish=dialog(monkeypatch,target)
    w.destination_location=SimpleNamespace(selected=lambda:AMSTERDAM)
    w.also_location=Var(True)
    monkeypatch.setattr(exp,'change_experiment',lambda *a,**kw:target.preview)
    monkeypatch.setattr(experiment_gui.messagebox,'askyesno',lambda *a,**kw:True)
    # Run posts synchronously to inspect phase ordering outside caught callbacks.
    w.app._post=lambda fn,*a:fn(*a)
    observed=[]
    def set_location(value,chosen,**kw):
        observed.append((w.message.value,chosen))
        return 'Location: change may be saved; USB verification incomplete.'
    monkeypatch.setattr(loc,'set_location',set_location)
    w._change()
    assert 'verified after reboot' in observed[0][0] and observed[0][1]==AMSTERDAM
    assert 'verified after reboot' in w.message.value and 'Location: change may be saved' in w.message.value


def test_automatic_summary_logs_redact_coordinates_but_operator_result_keeps_them(location_bench):
    from flash_gui import maintenance
    b=location_bench;b.location_state.stored=(52.373,4.8924);logs=[]
    with ambyte_serial.AmbyteConsole('bench') as console:
        result=maintenance.check_session(console,b.preview.mac,log=logs.append)
    assert '52.373' in result.timezone and '4.8924' in result.timezone
    assert all('52.373' not in line and '4.8924' not in line for line in logs)


def test_country_map_failure_keeps_the_location_change_opt_in(location_bench,monkeypatch):
    b=location_bench;b.location_state.stored=(52.0,5.0)
    monkeypatch.setattr(loc,'zone_for',lambda *a:(_ for _ in ()).throw(RuntimeError('map unavailable')))
    default,reason=loc.change_default(b.read().device.identity,ULURU)
    assert not default and 'lookup unavailable; choose explicitly' in reason


def test_explicit_location_confirmation_names_the_release_of_a_manual_pin(location_bench):
    b=location_bench;b.location_state.pinned=True;b.values['timezone']='Europe/Berlin';prompts=[]
    result=loc.set_location(b.read(),AMSTERDAM,confirm=lambda text:prompts.append(text) or True)
    assert 'releases this device’s manual timezone pin' in prompts[0]
    assert 'configured readback verified after reboot' in result
    assert b.values['timezone']=='Europe/Berlin' and not b.location_state.pinned
    assert writes(b)==['sync loc 52.37300000 4.89240000','reboot']
