# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""The async workbook-programming lookup's in-flight gate (review D, F1/F2).

Two failure modes these tests pin down:
  * a stale result from a previously selected experiment must not clear the
    current lookup's pending marker (onboarding would silently install the
    catalog default while the real lookup is still running);
  * a non-OpenJIIError in the worker (e.g. a malformed 200 body) must not
    wedge the gate forever.

App is exercised headless via object.__new__ plus fake Tk vars/labels — the
logic under test never touches a real display.
"""

import json
from types import SimpleNamespace

from flash_gui import gui
from flash_gui.openjii_client import OpenJIIError, WorkbookProgramming


class _FakeVar:
    def __init__(self):
        self.value = ""

    def set(self, value):
        self.value = value


class _FakeLabel:
    def __init__(self):
        self.options = {}

    def configure(self, **kw):
        self.options.update(kw)


def _app(current_id="exp-B", pending="exp-B"):
    app = object.__new__(gui.App)
    app.programming = None
    app.programming_experiment_id = None
    app.programming_override = False
    app._programming_pending_id = pending
    app.schedule_source_var = _FakeVar()
    app.workbook_warn_var = _FakeVar()
    app.schedule_row_label = _FakeLabel()
    app.log = lambda _msg: None
    app._post = lambda fn, *args: fn(*args)
    app._current_experiment = (
        lambda: SimpleNamespace(id=current_id) if current_id else None)
    return app


def _prog():
    return WorkbookProgramming(
        yaml_text="schema: jii.ambyte-schedule/v1-draft\n", workbook_id="wb",
        workbook_version_id="1c7b82b5-0000-1111-2222-333333333333",
        workbook_version_number=1, macros=())


# ── F1: stale results never clear the current lookup's gate ──────────────────
def test_stale_result_keeps_the_current_lookup_pending():
    app = _app(current_id="exp-B", pending="exp-B")
    gui.App._apply_programming(app, "exp-A", _prog())
    assert app._programming_pending_id == "exp-B"
    assert app.programming is None  # the stale result is dropped entirely


def test_stale_error_keeps_the_current_lookup_pending():
    app = _app(current_id="exp-B", pending="exp-B")
    gui.App._apply_programming_error(app, "exp-A", "boom")
    assert app._programming_pending_id == "exp-B"
    assert app.schedule_source_var.value == ""  # no stale "failed" label


def test_the_current_lookup_clears_its_own_marker():
    app = _app(current_id="exp-B", pending="exp-B")
    gui.App._apply_programming(app, "exp-B", _prog())
    assert app._programming_pending_id is None
    assert app.programming is not None
    assert "from workbook" in app.schedule_source_var.value

    app = _app(current_id="exp-B", pending="exp-B")
    gui.App._apply_programming_error(app, "exp-B", "nope")
    assert app._programming_pending_id is None
    assert "lookup failed" in app.schedule_source_var.value


# ── F2: any worker exception unblocks the gate ───────────────────────────────
class _InlineThread:
    def __init__(self, target, daemon=None):
        self._target = target

    def start(self):
        self._target()


def _resolve_with(monkeypatch, app, client):
    monkeypatch.setattr(gui.threading, "Thread", _InlineThread)
    app.client = client
    gui.App._resolve_programming_async(app, SimpleNamespace(id="exp-B"))


def test_unexpected_worker_exception_unblocks_onboarding(monkeypatch):
    app = _app(current_id="exp-B", pending=None)
    messages = []
    app.log = messages.append

    def broken(_experiment_id):
        raise json.JSONDecodeError("Expecting value", "doc", 0)

    _resolve_with(monkeypatch, app,
                  SimpleNamespace(resolve_programming=broken))
    assert app._programming_pending_id is None
    assert "lookup failed" in app.schedule_source_var.value
    # The exception type travels in the log detail, not the fixed label.
    assert any("JSONDecodeError" in m for m in messages)


def test_openjii_error_keeps_its_own_message(monkeypatch):
    app = _app(current_id="exp-B", pending=None)

    def refused(_experiment_id):
        raise OpenJIIError("the API key was rejected")

    _resolve_with(monkeypatch, app,
                  SimpleNamespace(resolve_programming=refused))
    assert app._programming_pending_id is None
    assert "lookup failed" in app.schedule_source_var.value
