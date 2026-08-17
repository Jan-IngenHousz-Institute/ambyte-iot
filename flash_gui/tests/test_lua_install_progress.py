# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""The Lua install wait must narrate itself.

It polls silently for up to 6 minutes while the device downloads the script over
Wi-Fi. With no output, a healthy slow download was indistinguishable from a hung
flasher, and the operator cannot attach a serial monitor to check because the
GUI holds the port for the duration.
"""

import pytest

from flash_gui import ambyte_serial, procedure
from flash_gui.ambyte_serial import ConsoleError, LuaReleaseStatus
from flash_gui.procedure import ProcedureError

SHA = "a" * 64


class _FakeClock:
    """Advances only when the code under test sleeps, so tests stay instant."""

    def __init__(self):
        self.now = 1_000_000.0

    def time(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class _FakeConsole:
    def __init__(self, replies):
        self._replies = list(replies)
        self.closed = False

    def lua_install(self, *args, **kwargs):
        return None

    def lua_release(self, timeout=10.0):
        reply = self._replies.pop(0) if self._replies else self._stale()
        if isinstance(reply, Exception):
            raise reply
        return reply

    @staticmethod
    def _stale():
        return LuaReleaseStatus(sha256="b" * 64, script_version="0.9.0",
                                built_against_fw="v1.8.1",
                                installed_on_fw="v1.8.1", verified=True,
                                running=True)

    def close(self):
        self.closed = True


@pytest.fixture
def wired(monkeypatch):
    clock = _FakeClock()
    monkeypatch.setattr(procedure.time, "time", clock.time)
    monkeypatch.setattr(procedure.time, "sleep", clock.sleep)
    messages = []

    def run(console, script_sha=SHA):
        monkeypatch.setattr(ambyte_serial, "connect_after_boot",
                            lambda *a, **k: console)
        script = procedure.LuaScriptRelease(
            tag="lua-v1.2.0", asset_name="main.lua", script_name="main",
            script_version="1.2.0", built_against_fw="v1.8.1",
            asset_url="https://example.test/main.lua", sha256=script_sha,
            size_bytes=6731, campaign_id="lua-v1.2.0")
        ctx = object.__new__(procedure.SessionContext)
        ctx.lua_script = script
        run_state = object.__new__(procedure.DeviceRun)
        run_state.port = "COM7"
        return procedure.install_lua_script(ctx, run_state, log=messages.append)

    return run, messages, clock


def test_progress_is_reported_while_the_device_downloads(wired):
    run, messages, _clock = wired
    # Never converges: the whole 360 s budget elapses.
    with pytest.raises(ProcedureError):
        run(_FakeConsole([]))

    progress = [m for m in messages if m.startswith("Still waiting for main.lua")]
    # Roughly one line per 15 s across 360 s, not a single silent block.
    assert len(progress) >= 15
    # Each line carries what the device reports and how long is left.
    assert "device still on 0.9.0/bbbbbbbbbbbb" in progress[0]
    assert "want 1.2.0/aaaaaaaaaaaa" in progress[0]
    assert "s left)" in progress[0]


def test_progress_lines_are_spaced_not_emitted_every_poll(wired):
    run, messages, _clock = wired
    with pytest.raises(ProcedureError):
        run(_FakeConsole([]))
    progress = [m for m in messages if m.startswith("Still waiting")]
    # The poll runs every ~2 s; without spacing this would be ~170 lines.
    assert len(progress) < 40


def test_console_hiccups_are_surfaced_rather_than_swallowed(wired):
    run, messages, _clock = wired
    with pytest.raises(ProcedureError):
        run(_FakeConsole([ConsoleError("'lua release' got no prompt back")] * 200))
    progress = [m for m in messages if m.startswith("Still waiting")]
    assert progress, "a console that never answers must still report progress"
    assert "no prompt back" in progress[0]


def test_success_still_returns_immediately_without_noise(wired):
    run, messages, _clock = wired
    good = LuaReleaseStatus(sha256=SHA, script_version="1.2.0",
                            built_against_fw="v1.8.1",
                            installed_on_fw="v1.8.1", verified=True,
                            running=True)
    # First call is the pre-install identity read, which already matches.
    item = run(_FakeConsole([good]))
    assert item.passed
    assert not [m for m in messages if m.startswith("Still waiting")]
