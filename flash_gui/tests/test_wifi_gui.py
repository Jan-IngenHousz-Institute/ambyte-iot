# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only
"""Headless checks for local access, worker boundaries and stale device gates."""
from types import SimpleNamespace
from dataclasses import replace

import pytest

from flash_gui import gui, wifi_gui, wifi_settings


INFO = wifi_settings.DeviceInfo("bench", "AA:BB:CC:DD:EE:FF", "Field board", "2.6.2", True, False)


class Var:
    def __init__(self, value=""):
        self.value = value

    def get(self):
        return self.value

    def set(self, value):
        self.value = value


class Widget:
    def __init__(self):
        self.state = "normal"
        self.visible = True

    def configure(self, **options):
        if "state" in options:
            self.state = options["state"]

    def grid(self):
        self.visible = True

    def grid_remove(self):
        self.visible = False


def dialog(monkeypatch):
    window = object.__new__(wifi_gui.WifiDialog)
    window.info, window.port, window.working, window.allow_new = INFO, "bench", False, False
    window.saved_schedule = None
    window.ssid, window.password = Var("Field hub 1"), Var("synthetic-password")
    window.device_text, window.message, window.network_note = Var(), Var(), Var()
    window.maintenance_text = Var()
    monkeypatch.setattr(wifi_gui.maintenance, "run", lambda *a, **kw: wifi_gui.maintenance.MaintenanceResult("Workbook: up to date"))
    window.winfo_exists = lambda: True
    for name in ("read_btn", "close_btn", "ssid_entry", "password_entry", "update_btn", "test_btn", "recover_btn", "new_btn"):
        setattr(window, name, Widget())
    posted = []
    window.app = SimpleNamespace(_post=lambda fn, *args: posted.append(lambda: fn(*args)))
    class Thread:
        def __init__(self, *, target, daemon):
            self.target = target

        def start(self):
            self.target()
    monkeypatch.setattr(wifi_gui.threading, "Thread", Thread)
    def finish():
        while posted:
            posted.pop(0)()
    return window, finish


def test_main_action_is_available_without_login_release_or_experiment(monkeypatch):
    calls = []
    app = SimpleNamespace(busy=False, _selected_port=lambda: "bench", _set_busy=calls.append)
    monkeypatch.setattr(wifi_gui, "WifiDialog", lambda owner, port: calls.append((owner, port)))
    gui.App._open_wifi_settings(app)
    assert app.busy and calls == [True, (app, "bench")]


def test_update_uses_worker_and_posts_result_to_main_thread(monkeypatch):
    window, finish = dialog(monkeypatch)
    calls = []
    def update(info, ssid, password, **kwargs):
        assert window.working and window.update_btn.state == "disabled"
        calls.append((info, ssid, password))
        return wifi_settings.WifiResult(ssid, True)
    monkeypatch.setattr(wifi_settings, "save_wifi", update)
    window._update()
    assert calls == [(INFO, "Field hub 1", "synthetic-password")]
    assert window.working and "saved and connection confirmed" not in window.message.value
    finish()
    assert not window.working and window.update_btn.state == "normal"
    assert "saved and connection confirmed" in window.message.value
    assert "synthetic-password" not in window.message.value


def test_failed_refresh_revokes_stale_write_permission(monkeypatch):
    window, finish = dialog(monkeypatch)
    def broken(port):
        raise wifi_settings.WifiSettingsError("USB unavailable")
    monkeypatch.setattr(wifi_settings, "read_device", broken)
    window._read()
    assert window.info is None
    finish()
    assert window.info is None and window.update_btn.state == "disabled"
    assert window.message.value == "USB unavailable"


def test_validation_keeps_invalid_credentials_off_worker(monkeypatch):
    window, _ = dialog(monkeypatch)
    window.password.set("short")
    monkeypatch.setattr(wifi_settings, "save_wifi", lambda *a, **kw: (_ for _ in ()).throw(AssertionError("must not write")))
    window._update()
    assert not window.working and "8–63" in window.message.value


def test_recovery_does_not_read_or_resend_password(monkeypatch):
    window, finish = dialog(monkeypatch)
    window.password.get = lambda: (_ for _ in ()).throw(AssertionError("must not read password"))
    monkeypatch.setattr(wifi_settings, "check_saved_result", lambda info: wifi_settings.WifiResult("Previous hub", True))
    window._recover()
    finish()
    assert "Previous hub was saved" in window.message.value


def test_uncertain_result_exposes_explicit_new_attempt(monkeypatch):
    window, finish = dialog(monkeypatch)
    def uncertain(*args, **kwargs):
        raise wifi_settings.WifiSettingsError("outcome unknown", uncertain=True)
    monkeypatch.setattr(wifi_settings, "update_wifi", uncertain)
    window._update(test_connection=True)
    finish()
    assert window.new_btn.visible and window.new_btn.state == "normal"
    assert window.recover_btn.state == "normal"


def test_unexpected_worker_exception_cannot_display_password(monkeypatch):
    window, finish = dialog(monkeypatch)
    def broken(*args, **kwargs):
        raise RuntimeError("raw echoed synthetic-password")
    monkeypatch.setattr(wifi_settings, "save_wifi", broken)
    window._update()
    finish()
    assert "synthetic-password" not in window.message.value
    assert not window.working


def test_schedule_error_removes_previous_new_attempt_permission(monkeypatch):
    window, finish = dialog(monkeypatch)
    window.allow_new = True
    def blocked(*args, **kwargs):
        raise wifi_settings.WifiSettingsError("Resolve the saved schedule through the schedule controls.")
    monkeypatch.setattr(wifi_settings, "save_wifi", blocked)
    window._update()
    finish()
    assert not window.allow_new and not window.new_btn.visible


@pytest.mark.parametrize("service", [True, False])
def test_default_save_does_not_require_the_hub_or_a_second_confirmation(monkeypatch, service):
    window, finish = dialog(monkeypatch)
    window.info = replace(INFO, service=service)
    writes = []
    monkeypatch.setattr(wifi_gui.messagebox, "askyesno", lambda *a, **kw: pytest.fail("Save Wi-Fi already authorizes replacement"))
    monkeypatch.setattr(wifi_settings, "update_wifi", lambda *a, **kw: pytest.fail("default must not trial"))
    def save(info, ssid, password):
        writes.append(info.mac)
        return wifi_settings.WifiResult(ssid, False)
    monkeypatch.setattr(wifi_settings, "save_wifi", save)
    window._update()
    finish()
    assert writes == [INFO.mac]
    assert "Wi-Fi saved" in window.message.value and "Connection not confirmed" in window.message.value
    assert not window.new_btn.visible


def test_optional_test_uses_the_service_trial(monkeypatch):
    window, finish = dialog(monkeypatch)
    calls = []
    monkeypatch.setattr(wifi_settings, "save_wifi", lambda *a: pytest.fail("trial must not overwrite offline"))
    def trial(*args, **kwargs):
        calls.append(kwargs)
        return wifi_settings.WifiResult("Field hub 1", True)
    monkeypatch.setattr(wifi_settings, "update_wifi", trial)
    window._update(test_connection=True)
    finish()
    assert calls == [{"new_request": False}]
    assert "connection confirmed" in window.message.value


def test_unknown_console_save_does_not_offer_service_resend(monkeypatch):
    window, finish = dialog(monkeypatch)
    def lost(*args):
        raise wifi_settings.WifiSettingsError("outcome unknown", uncertain=True)
    monkeypatch.setattr(wifi_settings, "save_wifi", lost)
    window._update()
    finish()
    assert not window.allow_new and not window.new_btn.visible


def test_explicit_new_trial_does_not_switch_to_persistent_save(monkeypatch):
    window, _ = dialog(monkeypatch)
    window.allow_new = True
    calls = []
    monkeypatch.setattr(wifi_gui.messagebox, "askyesno", lambda *a, **kw: True)
    window._update = lambda **kw: calls.append(kw)
    window._new_attempt()
    assert calls == [{"new_request": True, "test_connection": True}]


@pytest.mark.parametrize("service", [True, False])
def test_read_explains_offline_save_and_optional_connection_test(monkeypatch, service):
    window, finish = dialog(monkeypatch)
    monkeypatch.setattr(wifi_settings, "read_device", lambda port: replace(INFO, service=service))
    window._read()
    finish()
    assert "even if the hub is out of range" in window.network_note.value
    assert window.test_btn.state == ("normal" if service else "disabled")


def test_failed_constructor_destroys_partial_window(monkeypatch):
    destroyed = []
    monkeypatch.setattr(wifi_gui.tk.Toplevel, "__init__", lambda *args, **kwargs: None)
    def fail(*args):
        raise wifi_gui.tk.TclError("modal grab failed")
    monkeypatch.setattr(wifi_gui.WifiDialog, "_build", fail)
    monkeypatch.setattr(wifi_gui.WifiDialog, "destroy", lambda self: destroyed.append(self))
    with pytest.raises(wifi_gui.tk.TclError, match="modal grab failed"):
        wifi_gui.WifiDialog(object(), "bench")
    assert len(destroyed) == 1


def test_failed_dialog_creation_releases_app_busy_gate(monkeypatch):
    calls = []
    app = SimpleNamespace(busy=False, _selected_port=lambda: "bench", _set_busy=calls.append)
    def fail(*args):
        raise wifi_gui.tk.TclError("modal grab failed")
    monkeypatch.setattr(wifi_gui, "WifiDialog", fail)
    with pytest.raises(wifi_gui.tk.TclError):
        gui.App._open_wifi_settings(app)
    assert calls == [True, False]


def test_automatic_check_follows_save_and_its_error_keeps_saved_result(monkeypatch):
    window,finish=dialog(monkeypatch)
    window.app.client=object()
    order=[]
    def save(*a,**kw):
        order.append('saved')
        return wifi_settings.WifiResult('Field hub 1',False)
    def maintain(port,mac,client,**kw):
        assert order==['saved'] and client is window.app.client
        order.append('checked')
        return wifi_gui.maintenance.MaintenanceResult('Workbook: not checked: cloud unavailable')
    monkeypatch.setattr(wifi_settings,'save_wifi',save)
    monkeypatch.setattr(wifi_gui.maintenance,'run',maintain)
    window._update();finish()
    assert order==['saved','checked']
    assert 'Wi-Fi saved' in window.message.value and 'cloud unavailable' in window.maintenance_text.value


def test_read_automatically_checks_device_mac_without_selected_experiment(monkeypatch):
    window,finish=dialog(monkeypatch)
    calls=[]
    monkeypatch.setattr(wifi_settings,'read_device',lambda port:INFO)
    monkeypatch.setattr(wifi_gui.maintenance,'run',lambda port,mac,client,**kw:
        calls.append((port,mac,client)) or wifi_gui.maintenance.MaintenanceResult('Workbook: up to date'))
    window._read();finish()
    assert calls==[('bench',INFO.mac,None)] and window.maintenance_text.value.startswith('Workbook: up to date')


def test_primary_save_is_visible_while_optional_maintenance_is_still_running(monkeypatch):
    window,_=dialog(monkeypatch)
    window.app._post=lambda fn,*a:fn(*a)
    monkeypatch.setattr(wifi_settings,'save_wifi',lambda *a:wifi_settings.WifiResult('Field hub 1',True))
    def slow(*a,**kw):
        assert window.working and 'Wi-Fi saved and connection confirmed' in window.message.value
        assert 'Checking workbook' in window.maintenance_text.value
        assert window.update_btn.state=='disabled'
        raise RuntimeError('optional backend failed')
    monkeypatch.setattr(wifi_gui.maintenance,'run',slow)
    window._update()
    assert 'Wi-Fi saved and connection confirmed' in window.message.value
    assert 'optional maintenance unavailable' in window.maintenance_text.value
    assert not window.working
