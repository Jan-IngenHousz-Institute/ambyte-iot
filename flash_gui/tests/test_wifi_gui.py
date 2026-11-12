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
    window.winfo_exists = lambda: True
    for name in ("read_btn", "close_btn", "ssid_entry", "password_entry", "update_btn", "recover_btn", "new_btn"):
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
    monkeypatch.setattr(wifi_settings, "update_wifi", update)
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
    monkeypatch.setattr(wifi_settings, "update_wifi", lambda *a, **kw: (_ for _ in ()).throw(AssertionError("must not write")))
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
    window._update()
    finish()
    assert window.new_btn.visible and window.new_btn.state == "normal"
    assert window.recover_btn.state == "normal"


def test_unexpected_worker_exception_cannot_display_password(monkeypatch):
    window, finish = dialog(monkeypatch)
    def broken(*args, **kwargs):
        raise RuntimeError("raw echoed synthetic-password")
    monkeypatch.setattr(wifi_settings, "update_wifi", broken)
    window._update()
    finish()
    assert "synthetic-password" not in window.message.value
    assert not window.working


def test_schedule_error_removes_previous_new_attempt_permission(monkeypatch):
    window, finish = dialog(monkeypatch)
    window.allow_new = True
    def blocked(*args, **kwargs):
        raise wifi_settings.WifiSettingsError("Resolve the saved schedule through the schedule controls.")
    monkeypatch.setattr(wifi_settings, "update_wifi", blocked)
    window._update()
    finish()
    assert not window.allow_new and not window.new_btn.visible


@pytest.mark.parametrize("confirm", [True, False])
def test_legacy_overwrite_requires_confirmation(monkeypatch, confirm):
    window, finish = dialog(monkeypatch)
    window.info = replace(INFO, service=False)
    confirmations, writes = [], []
    def ask(title, message, **kwargs):
        confirmations.append(message)
        return confirm
    def update(info, ssid, password, **kwargs):
        writes.append(info.mac)
        return wifi_settings.WifiResult(ssid, True)
    monkeypatch.setattr(wifi_gui.messagebox, "askyesno", ask)
    monkeypatch.setattr(wifi_settings, "update_wifi", update)
    window._update()
    finish()
    assert len(confirmations) == 1 and "will be overwritten" in confirmations[0]
    assert "synthetic-password" not in confirmations[0]
    assert writes == ([INFO.mac] if confirm else [])


def test_service_read_explains_reachable_hub_requirement(monkeypatch):
    window, finish = dialog(monkeypatch)
    monkeypatch.setattr(wifi_settings, "read_device", lambda port: INFO)
    window._read()
    finish()
    assert "hub in range" in window.network_note.value
    assert "only after a successful connection" in window.network_note.value


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
