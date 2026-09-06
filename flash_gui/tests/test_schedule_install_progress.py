# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""The Schedule install wait must narrate itself.

It polls silently for up to 6 minutes while the device downloads the script over
Wi-Fi. With no output, a healthy slow download was indistinguishable from a hung
flasher, and the operator cannot attach a serial monitor to check because the
GUI holds the port for the duration.
"""

import pytest

from flash_gui import ambyte_serial, procedure
from flash_gui.ambyte_serial import ConsoleError, ScheduleReleaseStatus
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

    def schedule_install(self, *args, **kwargs):
        return None

    def wifi_connected(self, timeout=10.0):
        return True

    def schedule_release(self, timeout=10.0):
        reply = self._replies.pop(0) if self._replies else self._stale()
        if isinstance(reply, Exception):
            raise reply
        return reply

    @staticmethod
    def _stale():
        return ScheduleReleaseStatus(sha256="b" * 64, script_version="0.9.0",
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
        script = procedure.ScheduleScriptRelease(
            tag="schedule-v1.2.0", asset_name="default.yaml", script_name="default",
            script_version="1.2.0", built_against_fw="v1.8.1",
            asset_url="https://example.test/default.yaml", sha256=script_sha,
            size_bytes=6731, campaign_id="schedule-v1.2.0")
        ctx = object.__new__(procedure.SessionContext)
        ctx.schedule_script = script
        ctx.wifi_ssid = "protoMUSIC-GATEWAY"
        run_state = object.__new__(procedure.DeviceRun)
        run_state.port = "COM7"
        return procedure.install_schedule_script(ctx, run_state, log=messages.append)

    return run, messages, clock


def test_progress_is_reported_while_the_device_downloads(wired):
    run, messages, _clock = wired
    # Never converges: the whole 360 s budget elapses.
    with pytest.raises(ProcedureError):
        run(_FakeConsole([]))

    progress = [m for m in messages if m.startswith("Still waiting for default.yaml")]
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
        run(_FakeConsole([ConsoleError("'schedule release' got no prompt back")] * 200))
    progress = [m for m in messages if m.startswith("Still waiting")]
    assert progress, "a console that never answers must still report progress"
    assert "no prompt back" in progress[0]


def test_success_still_returns_immediately_without_noise(wired):
    run, messages, _clock = wired
    good = ScheduleReleaseStatus(sha256=SHA, script_version="1.2.0",
                            built_against_fw="v1.8.1",
                            installed_on_fw="v1.8.1", verified=True,
                            running=True)
    # First call is the pre-install identity read, which already matches.
    item = run(_FakeConsole([good]))
    assert item.passed
    assert not [m for m in messages if m.startswith("Still waiting")]

# ── network precheck ─────────────────────────────────────────────────────────
class _WifiConsole(_FakeConsole):
    def __init__(self, states, replies=()):
        super().__init__(replies)
        self._states = list(states)
        self.installed = False

    def wifi_connected(self, timeout=10.0):
        state = self._states.pop(0) if self._states else self._states_last
        self._states_last = state
        if isinstance(state, Exception):
            raise state
        return state

    def schedule_install(self, *a, **k):
        self.installed = True


def test_no_ip_fails_fast_instead_of_after_the_full_install_deadline(wired):
    run, messages, clock = wired
    started = clock.now
    console = _WifiConsole([False] * 200)
    with pytest.raises(ProcedureError, match="never got an IP"):
        run(console)
    # Fails on the Wi-Fi budget, not the 360 s install budget.
    assert clock.now - started < procedure.SCHEDULE_INSTALL_DEADLINE_S
    assert clock.now - started >= procedure.SCHEDULE_WIFI_DEADLINE_S
    # And crucially, never queued a download that could not have worked.
    assert not console.installed


def test_late_association_still_proceeds(wired):
    run, messages, _clock = wired
    # Offline for the first few polls, as after a reboot, then up.
    console = _WifiConsole([False, False, True])
    with pytest.raises(ProcedureError):   # install itself never converges
        run(console)
    assert console.installed
    assert any("with an IP" in m for m in messages)


def test_firmware_that_does_not_report_wifi_is_not_blocked(wired):
    run, messages, _clock = wired
    console = _WifiConsole([None])
    with pytest.raises(ProcedureError):
        run(console)
    assert console.installed, "older firmware must not be treated as offline"


def test_flaky_console_read_is_not_treated_as_a_network_verdict(wired):
    run, messages, _clock = wired
    console = _WifiConsole([ConsoleError("no prompt back")])
    with pytest.raises(ProcedureError):
        run(console)
    assert console.installed
    assert any("Could not read Wi-Fi state" in m for m in messages)
