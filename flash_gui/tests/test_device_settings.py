# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Changing Wi-Fi, name and clock on an on-boarded board (no re-flash).

What these pin down, in order of how badly a regression would hurt:
  * the Wi-Fi password is sent but never returned, raised or logged, although
    the console echoes it;
  * SSIDs and passwords survive the console's argument parser byte for byte;
  * "stored but not connected" is a warning, not a failure, so field
    credentials can be set at a bench the field network does not reach;
  * a setting that needs a reboot is read back from the SAME board afterwards.

No hardware: the console is a fake, and the firmware's argument splitter is
ported below so the quoting is checked against the real parsing rules.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from flash_gui import device_settings, gui                      # noqa: E402
from flash_gui.ambyte_serial import (FAILURE_MARK, AmbyteConsole,  # noqa: E402
                                     BoardStatus, ConsoleError,
                                     UnsupportedConsoleCommand, WifiJoinResult,
                                     parse_status, quote_arg)
from flash_gui.device_settings import (BoardSession, Outcome,  # noqa: E402
                                       SettingsError, wifi_problem)

MAC = "E8:F6:0A:B1:1F:34"
PASSWORD = "hunter2-hunter2"


# ── the firmware's argument parser ───────────────────────────────────────────
def _split_argv(line: str) -> list[str]:
    """Port of ESP-IDF esp_console_split_argv (components/console/split_argv.c).

    Spaces separate, double quotes group, and a backslash escapes only
    backslash, double quote and space: any other escaped character is dropped.
    """
    space, arg, quoted = 0, 1, 2
    state, escaped = space, False
    args: list[str] = []
    current: list[str] = []
    for ch in line:
        if escaped:
            if ch in ('\\', '"', ' '):
                current.append(ch)
            escaped = False
        elif state == space:
            if ch == ' ':
                continue
            current = []
            if ch == '"':
                state = quoted
            elif ch == '\\':
                state, escaped = arg, True
            else:
                state = arg
                current.append(ch)
        elif ch == '\\':
            escaped = True
        elif (state == quoted and ch == '"') or (state == arg and ch == ' '):
            args.append("".join(current))
            state = space
        else:
            current.append(ch)
    if state != space:
        args.append("".join(current))
    return args


@pytest.mark.parametrize("value", [
    "plain", "two words", 'say "hi"', "back\\slash", "", " lead", "trail ",
    'mix \\" of both', "ends in a backslash\\", '"', "\\n is not a newline",
])
def test_quote_arg_round_trips_through_the_console_parser(value):
    for force in (False, True):
        line = f"cmd {quote_arg(value, force=force)} tail"
        assert _split_argv(line) == ["cmd", value, "tail"]


def test_quote_arg_leaves_simple_values_bare():
    assert quote_arg("Europe/Amsterdam") == "Europe/Amsterdam"
    # Exactly what cfg_set has always sent for a name with a space.
    assert quote_arg("AMBYTE 7") == '"AMBYTE 7"'
    assert quote_arg("") == '""'
    assert quote_arg("x", force=True) == '"x"'


# ── AmbyteConsole.wifi_join ──────────────────────────────────────────────────
def _console(reply_tail: str):
    """A console whose `command` echoes the line, then prints `reply_tail`."""
    console = object.__new__(AmbyteConsole)
    sent = []

    def command(cmd, timeout=5.0, shown=None):
        sent.append(SimpleNamespace(cmd=cmd, timeout=timeout, shown=shown))
        return f"ambyte> {cmd}\r\n{reply_tail}\r\nambyte> "

    console.command = command
    return console, sent


def test_wifi_join_sends_both_arguments_intact_and_reports_connected():
    ssid, password = "Lab Net", 'p"a\\ss w0rd'
    console, sent = _console('wifi_join "Lab Net": ESP_OK')

    result = console.wifi_join(ssid, password)

    assert result.connected and not result.retrying
    assert _split_argv(sent[0].cmd) == ["wifi_join", ssid, password]
    assert password not in sent[0].shown
    assert sent[0].shown == 'wifi_join "Lab Net" ********'


def test_wifi_join_open_network_sends_an_empty_password_argument():
    console, sent = _console('wifi_join "guest": ESP_OK')
    console.wifi_join("guest", "")
    assert _split_argv(sent[0].cmd) == ["wifi_join", "guest", ""]


@pytest.mark.parametrize("code, retrying", [
    ("WIFI_MANAGER_ERR_AUTH_REJECTED", True),
    ("ESP_FAIL", True),
    ("ESP_ERR_TIMEOUT", True),
    ("WIFI_MANAGER_ERR_NOT_RETRYING", False),
    ("WIFI_MANAGER_ERR_DRIVER_UNRESOLVED", False),
    ("ESP_ERR_WIFI_PASSWORD", False),
])
def test_wifi_join_reports_the_firmware_verdict(code, retrying):
    console, _ = _console(
        f'wifi_join "net": {code}\r\n'
        "  AP rejected the credentials (auth/handshake failure)\r\n"
        f"{FAILURE_MARK}")
    result = console.wifi_join("net", PASSWORD)
    assert result.code == code
    assert not result.connected
    assert result.retrying is retrying


def test_wifi_join_on_firmware_without_the_command_is_a_capability_signal():
    console, _ = _console("Unrecognized command")
    with pytest.raises(UnsupportedConsoleCommand):
        console.wifi_join("net", PASSWORD)


def test_wifi_join_without_a_result_line_withholds_the_echoed_password():
    console, _ = _console("Usage: wifi_join <ssid> <password>")
    with pytest.raises(ConsoleError) as err:
        console.wifi_join("net", PASSWORD)
    assert PASSWORD not in str(err.value)


def test_wifi_join_echo_is_never_read_as_the_verdict():
    # A password shaped like a result line: only the echo carries it.
    console, _ = _console("Usage: wifi_join <ssid> <password>")
    with pytest.raises(ConsoleError):
        console.wifi_join("net", 'x": ESP_OK')


class _SilentSerial:
    """A port that accepts writes and never answers."""

    def __init__(self):
        self.written = b""

    def reset_input_buffer(self):
        pass

    def write(self, data):
        self.written += data

    def read(self, _size):
        return b""


def test_wifi_join_timeout_names_the_command_without_the_password():
    console = object.__new__(AmbyteConsole)
    console.port = "COM9"
    console._ser = _SilentSerial()

    with pytest.raises(ConsoleError) as err:
        console.wifi_join("net", PASSWORD, timeout=0.05)

    assert PASSWORD.encode() in console._ser.written      # it was sent...
    assert PASSWORD not in str(err.value)                 # ...and not repeated
    assert "wifi_join" in str(err.value)


def test_parse_status_reads_mac_link_and_provisioning():
    status = parse_status(
        "CLI status:\r\n - RTC ready: yes\r\n"
        " - Wi-Fi: disconnected (provisioned: yes)\r\n"
        f" - MAC: {MAC.lower()}\r\n")
    assert status == BoardStatus(mac=MAC, wifi_connected=False,
                                 wifi_provisioned=True)


def test_parse_status_reports_none_for_lines_older_firmware_omits():
    assert parse_status("CLI status:\r\n - RTC ready: yes\r\n") == BoardStatus(
        mac=None, wifi_connected=None, wifi_provisioned=None)


# ── credential validation ────────────────────────────────────────────────────
@pytest.mark.parametrize("ssid, password", [
    ("net", "12345678"), ("net", ""), ("n" * 32, "p" * 63),
    ("Lab Net", 'p"a\\ss w0rd'),
])
def test_wifi_problem_accepts_sendable_credentials(ssid, password):
    assert wifi_problem(ssid, password) is None


@pytest.mark.parametrize("ssid, password, fragment", [
    ("", "12345678", "SSID first"),
    ("n" * 33, "12345678", "longer than 32"),
    ("net", "1234567", "8-63"),
    ("net", "p" * 64, "8-63"),
    ("café", "12345678", "printable ASCII"),
    ("net", "pässword1", "printable ASCII"),
    ("net", "tab\tinside", "printable ASCII"),
])
def test_wifi_problem_explains_what_cannot_be_sent(ssid, password, fragment):
    assert fragment in wifi_problem(ssid, password)


# ── BoardSession against a fake board ────────────────────────────────────────
class FakeBoard:
    """One board's state across console sessions and reboots."""

    def __init__(self):
        self.mac = MAC
        self.nvs = {"device_name": "AMBYTE_{MAC}",
                    "timezone": "Europe/Amsterdam"}
        self.provisioned = True
        self.link = True                # what `status` says about Wi-Fi now
        self.link_after_polls = None    # (n, state): flips after n more polls
        self.join_code = "ESP_OK"
        self.join_error = None
        self.rtc_offset = 37            # RTC minus host; None = unreadable
        self.accepts_writes = True
        self.rejected_keys = ()
        self.port = "COM7"
        self.port_after_reboot = "COM8"
        self.mac_after_reboot = None
        self.calls = []
        self.connects = []
        self.consoles = []
        self.connect_error = None

    def connect(self, port, deadline_s=0.0, log=None):
        if self.connect_error:
            raise self.connect_error
        self.connects.append((port, deadline_s))
        console = FakeConsole(self)
        self.consoles.append(console)
        return console

    def did(self, name):
        return [call for call in self.calls if call[0] == name]


class FakeConsole:
    def __init__(self, board: FakeBoard):
        self.board = board
        self.port = board.port
        self.closed = False

    def board_status(self, timeout=10.0):
        b = self.board
        return BoardStatus(mac=b.mac, wifi_connected=b.link,
                           wifi_provisioned=b.provisioned)

    def status_mac(self):
        return self.board.mac

    def wifi_connected(self, timeout=10.0):
        b = self.board
        b.calls.append(("wifi_poll",))
        if b.link_after_polls is not None:
            left, state = b.link_after_polls
            if left <= 1:
                b.link, b.link_after_polls = state, None
            else:
                b.link_after_polls = (left - 1, state)
        return b.link

    def cfg_get(self, key, timeout=5.0):
        return self.board.nvs.get(key)

    def cfg_set(self, key, value, timeout=5.0):
        b = self.board
        b.calls.append(("cfg_set", key, value))
        if key in b.rejected_keys:
            raise ConsoleError(f"cfg set {key} failed:\nESP_ERR_INVALID_ARG")
        if b.accepts_writes:
            b.nvs[key] = value

    def wifi_join(self, ssid, password):
        b = self.board
        b.calls.append(("wifi_join", ssid, password))
        if b.join_error:
            raise b.join_error
        b.link = b.join_code == "ESP_OK"
        return WifiJoinResult(code=b.join_code)

    def rtc_read(self):
        if self.board.rtc_offset is None:
            raise ConsoleError("rtc read failed:\nRTC read error")
        return int(time.time()) + self.board.rtc_offset

    def rtc_set(self, epoch):
        b = self.board
        b.calls.append(("rtc_set", epoch))
        if b.accepts_writes:
            b.rtc_offset = epoch - int(time.time())
        return epoch

    def reboot(self):
        b = self.board
        b.calls.append(("reboot",))
        b.port = b.port_after_reboot
        if b.mac_after_reboot:
            b.mac = b.mac_after_reboot

    def close(self):
        self.closed = True


@pytest.fixture
def board(monkeypatch):
    fake = FakeBoard()
    monkeypatch.setattr(device_settings.ambyte_serial, "connect_after_boot",
                        fake.connect)
    monkeypatch.setattr(device_settings.time, "sleep", lambda _s: None)
    # One status poll, no real waiting, unless a test wants the settle window.
    monkeypatch.setattr(device_settings, "WIFI_SETTLE_S", 0.0)
    return fake


@pytest.fixture
def logs():
    return []


def _open(logs) -> BoardSession:
    return BoardSession("COM7", log=logs.append)


def test_session_identifies_the_board_with_its_expanded_name(board, logs):
    with _open(logs) as session:
        ident = session.identity
    assert ident.mac == MAC
    assert ident.name == f"AMBYTE_{MAC}"
    assert ident.timezone == "Europe/Amsterdam"
    assert MAC in ident.describe()
    assert board.consoles[0].closed


def test_session_reports_an_unreachable_console(board, logs):
    board.connect_error = ConsoleError("no ambyte console answered within 90s")
    with pytest.raises(SettingsError, match="no ambyte console"):
        _open(logs)


# Wi-Fi ----------------------------------------------------------------------
def test_set_wifi_joined(board, logs):
    with _open(logs) as session:
        outcome = session.set_wifi("Lab Net", PASSWORD)
    assert outcome == Outcome("Wi-Fi: joined 'Lab Net'",
                              "The board is connected and has an IP.")
    assert board.did("wifi_join") == [("wifi_join", "Lab Net", PASSWORD)]
    assert not session.rebooted


def test_set_wifi_out_of_range_is_stored_and_reported_as_a_warning(board, logs):
    board.join_code = "ESP_FAIL"
    with _open(logs) as session:
        outcome = session.set_wifi("Field Net", PASSWORD)
    assert outcome.warning
    assert outcome.summary == "Wi-Fi: saved, not connected yet"
    assert "keeps retrying" in outcome.detail


def test_set_wifi_rejected_password_is_a_distinct_warning(board, logs):
    board.join_code = "WIFI_MANAGER_ERR_AUTH_REJECTED"
    with _open(logs) as session:
        outcome = session.set_wifi("Lab Net", PASSWORD)
    assert outcome.warning
    assert outcome.summary == "Wi-Fi: saved, password rejected"


def test_set_wifi_slow_dhcp_counts_as_joined(board, logs, monkeypatch):
    monkeypatch.setattr(device_settings, "WIFI_SETTLE_S", 60.0)
    board.join_code = "ESP_ERR_TIMEOUT"
    board.link_after_polls = (2, True)
    with _open(logs) as session:
        outcome = session.set_wifi("Lab Net", PASSWORD)
    assert not outcome.warning
    assert outcome.summary == "Wi-Fi: joined 'Lab Net'"
    assert len(board.did("wifi_poll")) == 2


@pytest.mark.parametrize("code, fragment", [
    ("WIFI_MANAGER_ERR_NOT_RETRYING", "nothing is retrying"),
    ("WIFI_MANAGER_ERR_DRIVER_UNRESOLVED", "Reboot the board"),
    ("ESP_ERR_WIFI_PASSWORD", "ESP_ERR_WIFI_PASSWORD"),
])
def test_set_wifi_failures_say_what_to_do(board, logs, code, fragment):
    board.join_code = code
    with _open(logs) as session:
        with pytest.raises(SettingsError, match=fragment):
            session.set_wifi("Lab Net", PASSWORD)


def test_set_wifi_refuses_a_board_that_was_never_onboarded(board, logs):
    board.provisioned = False
    with _open(logs) as session:
        with pytest.raises(SettingsError, match="never been on-boarded"):
            session.set_wifi("Lab Net", PASSWORD)
    assert not board.did("wifi_join")


def test_set_wifi_proceeds_when_old_firmware_omits_the_flag(board, logs):
    board.provisioned = None
    with _open(logs) as session:
        assert not session.set_wifi("Lab Net", PASSWORD).warning


def test_set_wifi_on_firmware_without_wifi_join_points_to_onboarding(board,
                                                                     logs):
    board.join_error = UnsupportedConsoleCommand("no `wifi_join`")
    with _open(logs) as session:
        with pytest.raises(SettingsError, match="On-board device"):
            session.set_wifi("Lab Net", PASSWORD)


def test_set_wifi_validates_before_touching_the_board(board, logs):
    with _open(logs) as session:
        with pytest.raises(SettingsError, match="8-63"):
            session.set_wifi("Lab Net", "short")
    assert not board.did("wifi_join")


@pytest.mark.parametrize("code", [
    "ESP_OK", "ESP_FAIL", "WIFI_MANAGER_ERR_AUTH_REJECTED",
    "WIFI_MANAGER_ERR_NOT_RETRYING",
])
def test_set_wifi_never_logs_or_reports_the_password(board, logs, code):
    board.join_code = code
    said = []
    with _open(logs) as session:
        try:
            outcome = session.set_wifi("Lab Net", PASSWORD)
            said += [outcome.summary, outcome.detail]
        except SettingsError as exc:
            said.append(str(exc))
    assert PASSWORD not in "\n".join(logs + said)


# name -----------------------------------------------------------------------
def test_rename_writes_reboots_and_reads_back_from_the_same_board(board, logs):
    with _open(logs) as session:
        outcome = session.rename("  Greenhouse 3  ")

    assert outcome.summary == "Name: 'Greenhouse 3'"
    assert f"AMBYTE_{MAC}" in outcome.detail
    assert board.calls == [("cfg_set", "device_name", "Greenhouse 3"),
                           ("reboot",)]
    assert len(board.connects) == 2
    assert board.consoles[0].closed and board.consoles[1].closed
    # The S3 came back on another port name; the session followed it.
    assert session.rebooted and session.port == "COM8"


def test_rename_to_the_current_name_changes_nothing(board, logs):
    with _open(logs) as session:
        outcome = session.rename(f"AMBYTE_{MAC}")
    assert outcome.summary == f"Name: already 'AMBYTE_{MAC}'"
    assert board.calls == [] and not session.rebooted


def test_rename_rejects_a_name_the_console_cannot_carry(board, logs):
    with _open(logs) as session:
        with pytest.raises(SettingsError, match="invalid device name"):
            session.rename('bad"name')
    assert board.calls == []


def test_rename_fails_when_the_board_reports_another_name_afterwards(board,
                                                                     logs):
    board.accepts_writes = False
    with _open(logs) as session:
        with pytest.raises(SettingsError, match="expected 'Greenhouse 3'"):
            session.rename("Greenhouse 3")


def test_rename_refuses_to_verify_against_a_different_board(board, logs):
    board.mac_after_reboot = "AA:BB:CC:DD:EE:FF"
    with _open(logs) as session:
        with pytest.raises(SettingsError, match="different board"):
            session.rename("Greenhouse 3")


def test_rename_reports_a_board_that_does_not_come_back(board, logs):
    with _open(logs) as session:
        board.connect_error = ConsoleError("no ambyte console answered")
        with pytest.raises(SettingsError, match="did not come back"):
            session.rename("Greenhouse 3")


# clock ----------------------------------------------------------------------
def test_set_clock_uses_this_pcs_utc_and_reports_the_drift(board, logs):
    before = int(time.time())
    with _open(logs) as session:
        outcome = session.set_clock()
    after = int(time.time())

    (_, epoch), = board.did("rtc_set")
    assert before <= epoch <= after
    assert outcome.summary == "Clock: set, +0 s vs this PC"
    assert outcome.detail == "Was +37 s against this PC."
    assert not session.rebooted and not board.did("cfg_set")


def test_set_clock_brings_a_factory_fresh_rtc_online(board, logs):
    board.rtc_offset = None             # every read fails until it is set
    with _open(logs) as session:
        outcome = session.set_clock()
    assert outcome.summary == "Clock: set, +0 s vs this PC"
    assert outcome.detail == "Was unreadable against this PC."


def test_set_clock_with_a_new_timezone_reboots_then_sets_the_time(board, logs):
    with _open(logs) as session:
        outcome = session.set_clock(timezone="America/La_Paz")

    assert [call[0] for call in board.calls] == ["cfg_set", "reboot", "rtc_set"]
    assert board.nvs["timezone"] == "America/La_Paz"
    assert "'Europe/Amsterdam' changed to 'America/La_Paz'" in outcome.detail
    assert session.rebooted


def test_set_clock_with_the_same_timezone_does_not_reboot(board, logs):
    with _open(logs) as session:
        outcome = session.set_clock(timezone="Europe/Amsterdam")
    assert [call[0] for call in board.calls] == ["rtc_set"]
    assert "Timezone already Europe/Amsterdam." in outcome.detail
    assert not session.rebooted


def test_set_clock_refuses_a_zone_the_firmware_table_lacks(board, logs):
    with _open(logs) as session:
        with pytest.raises(SettingsError, match="zone table"):
            session.set_clock(timezone="Mars/Olympus_Mons")
    assert board.calls == []


def test_set_clock_leaves_the_time_alone_when_the_board_rejects_the_zone(
        board, logs):
    board.rejected_keys = ("timezone",)
    with _open(logs) as session:
        with pytest.raises(SettingsError, match="rejected the timezone"):
            session.set_clock(timezone="America/La_Paz")
    assert not board.did("rtc_set") and not board.did("reboot")


def test_set_clock_fails_when_the_read_back_is_off(board, logs):
    board.rtc_offset = 5000
    board.accepts_writes = False
    with _open(logs) as session:
        with pytest.raises(SettingsError, match=r"\+5000 s"):
            session.set_clock()


# ── GUI glue (headless: App via object.__new__, as in test_workbook_gui) ─────
class _Var:
    def __init__(self, value=""):
        self.value = value

    def get(self):
        return self.value

    def set(self, value):
        self.value = value


class _InlineThread:
    """Runs the worker on start(), so a test sees the whole action."""

    def __init__(self, target, daemon=None, args=()):
        self._target, self._args = target, args

    def start(self):
        self._target(*self._args)


class _Session:
    def __init__(self, rebooted=False):
        self.port = "COM8" if rebooted else "COM7"
        self.rebooted = rebooted
        self.closed = False
        self.identity = SimpleNamespace(
            mac=MAC, name="Greenhouse 3", describe=lambda: "Greenhouse 3")

    def close(self):
        self.closed = True


@pytest.fixture
def app(monkeypatch):
    app = object.__new__(gui.App)
    app.busy = False
    app.result_var = _Var()
    app.busy_states = []
    app.refreshed = []
    app.logged = []
    app.dialogs = []
    app._set_busy = app.busy_states.append
    app._refresh_ports = app.refreshed.append
    app._selected_port = lambda: "COM7"
    app._post = lambda fn, *args: fn(*args)
    app.log = app.logged.append
    monkeypatch.setattr(gui.threading, "Thread", _InlineThread)
    for kind in ("showwarning", "showerror"):
        monkeypatch.setattr(
            gui.messagebox, kind,
            lambda title, text, kind=kind, **_kw: app.dialogs.append(
                (kind, title, text)))
    return app


def _use_session(monkeypatch, session):
    monkeypatch.setattr(gui.device_settings, "BoardSession",
                        lambda port, log=None: session)


def test_run_setting_shows_the_outcome_and_releases_the_port(app, monkeypatch):
    session = _Session()
    _use_session(monkeypatch, session)

    gui.App._run_setting(app, "Clock", lambda board: Outcome(
        "Clock: set, +0 s vs this PC", "Was +37 s against this PC."))

    assert app.result_var.value == "Clock: set, +0 s vs this PC"
    assert app.busy_states == [True, False]
    assert session.closed
    assert app.dialogs == [] and app.refreshed == []
    assert any("DONE" in line for line in app.logged)


def test_run_setting_raises_a_warning_outcome_to_the_operator(app, monkeypatch):
    _use_session(monkeypatch, _Session())
    gui.App._run_setting(app, "Wi-Fi", lambda board: Outcome(
        "Wi-Fi: saved, not connected yet", "It keeps retrying.", warning=True))
    assert app.result_var.value == "⚠ Wi-Fi: saved, not connected yet"
    assert app.dialogs == [("showwarning", "Wi-Fi", "It keeps retrying.")]


def test_run_setting_follows_a_board_that_rebooted(app, monkeypatch):
    _use_session(monkeypatch, _Session(rebooted=True))
    gui.App._run_setting(app, "Rename",
                         lambda board: Outcome("Name: 'Greenhouse 3'"))
    assert app.refreshed == ["COM8"]


def test_run_setting_reports_a_failure_and_still_releases_the_port(
        app, monkeypatch):
    session = _Session()
    _use_session(monkeypatch, session)

    def action(board):
        raise SettingsError("the board did not accept the Wi-Fi settings")

    gui.App._run_setting(app, "Wi-Fi", action)

    assert app.result_var.value == "Wi-Fi: FAILED"
    assert app.dialogs == [
        ("showerror", "Wi-Fi", "the board did not accept the Wi-Fi settings")]
    assert session.closed and app.busy_states == [True, False]


def test_run_setting_cancelled_by_the_operator_changes_nothing(app,
                                                               monkeypatch):
    session = _Session()
    _use_session(monkeypatch, session)
    gui.App._run_setting(app, "Rename", lambda board: None)
    assert app.result_var.value == "" and app.dialogs == []
    assert session.closed
    assert any("cancelled" in line for line in app.logged)


def test_run_setting_needs_a_port_and_an_idle_gui(app, monkeypatch):
    opened = []
    monkeypatch.setattr(gui.device_settings, "BoardSession",
                        lambda port, log=None: opened.append(port))
    app._selected_port = lambda: None
    gui.App._run_setting(app, "Clock", lambda board: None)
    app._selected_port = lambda: "COM7"
    app.busy = True
    gui.App._run_setting(app, "Clock", lambda board: None)
    assert opened == [] and app.busy_states == []
    assert [kind for kind, _title, _text in app.dialogs] == ["showwarning"] * 2


def test_send_wifi_validates_before_opening_the_port(app):
    app.ssid_var = _Var("Lab Net")
    app.wifi_pass_var = _Var("short")
    app._run_setting = lambda *_a: pytest.fail("must not reach the board")
    gui.App._send_wifi(app)
    assert app.dialogs[0][0] == "showwarning" and "8-63" in app.dialogs[0][2]


def test_ask_device_name_repeats_until_the_name_is_valid(app, monkeypatch):
    answers = iter(['bad"name', "Greenhouse 3"])
    prompts = []

    def askstring(_title, prompt, **_kw):
        prompts.append(prompt)
        return next(answers)

    monkeypatch.setattr(gui.simpledialog, "askstring", askstring)
    app._ask_on_main = lambda fn: fn()

    assert gui.App._ask_device_name(app, "Device name:", "old") == "Greenhouse 3"
    assert prompts[0] == "Device name:" and "is invalid" in prompts[1]


def test_ask_device_name_returns_none_when_cancelled(app, monkeypatch):
    monkeypatch.setattr(gui.simpledialog, "askstring", lambda *_a, **_k: None)
    app._ask_on_main = lambda fn: fn()
    assert gui.App._ask_device_name(app, "Device name:", "old") is None


def test_refresh_ports_keeps_the_preferred_port_selected(app, monkeypatch):
    selected = []
    app.port_box = SimpleNamespace(configure=lambda **_kw: None,
                                   current=selected.append)
    app.port_var = _Var()
    monkeypatch.setattr(gui.esptool_ops, "list_serial_ports", lambda: [
        {"device": "COM3", "description": "USB JTAG", "is_ambyte_jtag": True},
        {"device": "COM8", "description": "USB JTAG", "is_ambyte_jtag": True},
    ])
    gui.App._refresh_ports(app, "COM8")
    gui.App._refresh_ports(app, "COM99")      # gone: fall back to the first
    gui.App._refresh_ports(app)
    assert selected == [1, 0, 0]
