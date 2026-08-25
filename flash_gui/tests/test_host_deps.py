# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""A missing runtime package must fail BEFORE anything touches a board.

2026-08-25: "No module named 'littlefs'" surfaced at step 3, after openJII had
already rotated the board's certificate, three times in one day because the GUI
was launched from three different interpreters. The check now runs at GUI start
and again in prepare_provisioning ahead of provision_device().
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from flash_gui import host_deps, procedure                      # noqa: E402
from flash_gui.procedure import ProcedureError                  # noqa: E402


def _hide(monkeypatch, *names):
    real = host_deps.importlib.import_module

    def fake_import(name, *args, **kwargs):
        if name.split(".")[0] in names:
            raise ImportError(f"No module named '{name}'")
        return real(name, *args, **kwargs)
    monkeypatch.setattr(host_deps.importlib, "import_module", fake_import)


def test_missing_package_is_reported_by_pip_name_and_interpreter(monkeypatch):
    _hide(monkeypatch, "littlefs")
    missing = host_deps.missing_host_dependencies()
    assert missing == ["littlefs-python"]          # pip name, not module name
    text = host_deps.describe_missing(missing)
    assert sys.executable in text
    assert "pip install -r flash_gui/requirements.txt" in text
    with pytest.raises(host_deps.HostDependencyError):
        host_deps.require_host_dependencies()


def test_all_present_means_nothing_missing():
    assert host_deps.missing_host_dependencies() == []


def test_prepare_provisioning_refuses_before_rotating_credentials(monkeypatch):
    """The gate sits ahead of provision_device(): openJII must not be called."""
    monkeypatch.setattr(host_deps, "missing_host_dependencies",
                        lambda: ["littlefs-python"])
    calls = []

    class Client:
        def provision_device(self, *a, **k):
            calls.append(a)
            raise AssertionError("must not be reached")

    class Env:
        key = "dev"
        mqtt_uri = "mqtts://example"

    ctx = procedure.SessionContext(
        env=Env(), client=Client(), release=None, lua_script=None,
        topic_root_template="t/{thingName}", timezone="Europe/Amsterdam",
        wifi_ssid="s", wifi_password="p")
    run = procedure.DeviceRun(
        port="COM7", name="Bench 1",
        preflight=procedure.PreflightInfo(port="COM7", mac="AA:BB:CC:DD:EE:FF",
                                          had_console=True, stored_name=None))
    monkeypatch.setattr(procedure.timezones, "firmware_supports", lambda tz: True)

    with pytest.raises(ProcedureError) as exc:
        procedure.prepare_provisioning(ctx, run, log=lambda _m: None)

    assert exc.value.step == "credentials"
    assert "littlefs-python" in str(exc.value)
    assert calls == []
