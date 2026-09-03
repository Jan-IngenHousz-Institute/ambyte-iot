# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Streaming a Lua script down the console instead of the board downloading it.

The point is that onboarding stops needing the *device* to have network. The PC
already has it: it fetched the release listing and the manifest to get here.
"""

import base64
import hashlib

import pytest

from flash_gui import ambyte_serial, procedure, release_fetch
from flash_gui.ambyte_serial import (AmbyteConsole, ConsoleError,
                                     LuaReleaseStatus, UnsupportedConsoleCommand)
from flash_gui.procedure import ProcedureError
from flash_gui.release_fetch import LuaScriptRelease, ReleaseError

BLOB = b"-- ambyte schedule\n" + b"x" * 1200
SHA = hashlib.sha256(BLOB).hexdigest()


def _script(sha=SHA, size=len(BLOB)):
    return LuaScriptRelease(
        tag="lua-v1.2.0", asset_name="main.lua", script_name="main",
        script_version="1.2.0", built_against_fw="v1.8.1",
        asset_url="https://example.test/lua-v1.2.0/main.lua",
        sha256=sha, size_bytes=size, campaign_id="lua-v1.2.0")


class _PushConsole:
    """Mimics the firmware's stateless staging: size is the file's own size."""

    def __init__(self, begin_reply="lua begin: ready"):
        self.begin_reply = begin_reply
        self.staged = b""
        self.commands = []
        self.committed = None
        self.aborted = False

    def command(self, cmd, timeout=5.0):
        self.commands.append(cmd)
        if cmd == "lua begin":
            self.staged = b""
            return f"lua begin\n{self.begin_reply}\nambyte> "
        if cmd.startswith("lua put "):
            self.staged += base64.b64decode(cmd[len("lua put "):])
            return f"{cmd}\nlua put: {len(self.staged)} bytes\nambyte> "
        if cmd.startswith("lua commit "):
            self.committed = cmd.split()[2:]
            return f"{cmd}\nlua commit queued: id={self.committed[1]}\nambyte> "
        if cmd == "lua abort":
            self.aborted = True
            return "lua abort: discarded\nambyte> "
        raise AssertionError(f"unexpected command {cmd!r}")

    def close(self):
        pass


def _console(**kw):
    console = _PushConsole(**kw)
    # lua_push is an AmbyteConsole method; bind it to the fake transport.
    console.lua_push = AmbyteConsole.lua_push.__get__(console, AmbyteConsole)
    console._lua_abort_quietly = AmbyteConsole._lua_abort_quietly.__get__(
        console, AmbyteConsole)
    return console


# ── the wire protocol ────────────────────────────────────────────────────────
def test_push_streams_every_byte_and_commits():
    console = _console()
    console.lua_push(BLOB, SHA, "lua-v1.2.0", "1.2.0", "v1.8.1")
    assert console.staged == BLOB
    assert console.committed == [SHA, "lua-v1.2.0", "1.2.0", "v1.8.1"]
    assert not console.aborted


def test_chunks_fit_the_usb_jtag_rx_buffer():
    console = _console()
    console.lua_push(BLOB, SHA, "lua-v1.2.0", "1.2.0", "v1.8.1")
    puts = [c for c in console.commands if c.startswith("lua put ")]
    assert puts, "nothing was pushed"
    # NOT max_cmdline_length (512). ESP-IDF fixes the USB-Serial-JTAG driver's
    # rx_buffer_size at 256 and the REPL does not expose it, so a longer line is
    # swallowed and the command never answers. Measured on hardware: 224 works,
    # 264 hangs. Keep well clear, including the CRLF.
    assert max(len(c) for c in puts) + 2 <= 224


def test_old_firmware_is_reported_as_unsupported_not_broken():
    console = _console(
        begin_reply="Usage: lua <start|stop|status|release|install|exec>")
    with pytest.raises(UnsupportedConsoleCommand):
        console.lua_push(BLOB, SHA, "lua-v1.2.0", "1.2.0", "v1.8.1")


def test_a_dropped_chunk_is_caught_and_the_staging_file_discarded():
    console = _console()
    real = console.command

    def lying(cmd, timeout=5.0):
        reply = real(cmd, timeout)
        if cmd.startswith("lua put "):
            # Device silently kept fewer bytes than were sent.
            return reply.replace(f"lua put: {len(console.staged)} bytes",
                                 "lua put: 1 bytes")
        return reply

    console.command = lying
    with pytest.raises(ConsoleError, match="staged"):
        console.lua_push(BLOB, SHA, "lua-v1.2.0", "1.2.0", "v1.8.1")
    assert console.aborted, "a half-pushed file must not be left staged"
    assert console.committed is None


# ── the cached bytes ─────────────────────────────────────────────────────────
def test_script_bytes_verifies_the_cache_on_every_read(monkeypatch, tmp_path):
    monkeypatch.setattr(release_fetch, "SCRIPTS_CACHE_DIR", tmp_path)
    (tmp_path / f"{SHA}.lua").write_bytes(b"tampered")
    downloads = []

    def fake_download(url, dest, expect_size):
        downloads.append(url)
        dest.write_bytes(BLOB)

    monkeypatch.setattr(release_fetch, "_download", fake_download)
    assert release_fetch.script_bytes(_script(), log=lambda _m: None) == BLOB
    assert downloads, "a cache entry failing its digest must be refetched"


def test_script_bytes_reuses_a_good_cache_without_network(monkeypatch, tmp_path):
    monkeypatch.setattr(release_fetch, "SCRIPTS_CACHE_DIR", tmp_path)
    (tmp_path / f"{SHA}.lua").write_bytes(BLOB)
    monkeypatch.setattr(
        release_fetch, "_download",
        lambda *a: pytest.fail("must not download a cached script"))
    assert release_fetch.script_bytes(_script(), log=lambda _m: None) == BLOB


def test_script_bytes_can_download_silently_on_a_cold_cache(monkeypatch, tmp_path):
    """The provisioning caller historically passes log=None.

    A warm cache returned before logging and hid the bug; the first board on a
    clean PC instead tried to call None immediately before building littlefs.
    """
    monkeypatch.setattr(release_fetch, "SCRIPTS_CACHE_DIR", tmp_path)
    monkeypatch.setattr(
        release_fetch, "_download",
        lambda url, dest, expect_size: dest.write_bytes(BLOB))

    assert release_fetch.script_bytes(_script(), log=None) == BLOB


def test_script_bytes_rejects_a_download_that_does_not_match_the_manifest(
        monkeypatch, tmp_path):
    monkeypatch.setattr(release_fetch, "SCRIPTS_CACHE_DIR", tmp_path)
    monkeypatch.setattr(
        release_fetch, "_download",
        lambda url, dest, expect_size: dest.write_bytes(b"wrong bytes"))
    with pytest.raises(ReleaseError, match="does not match the manifest"):
        release_fetch.script_bytes(_script(), log=lambda _m: None)
    assert not list(tmp_path.iterdir()), "a bad download must not stay cached"


# ── procedure integration ────────────────────────────────────────────────────
class _ProcConsole(_PushConsole):
    def __init__(self, **kw):
        super().__init__(**kw)
        self.wifi_checked = False
        self.url_installed = None

    def lua_release(self, timeout=10.0):
        if self.committed is None and self.url_installed is None:
            return LuaReleaseStatus("b" * 64, "0.9.0", "v1.8.1", "v1.8.1",
                                    True, True)
        return LuaReleaseStatus(SHA, "1.2.0", "v1.8.1", "v1.8.1", True, True)

    def wifi_connected(self, timeout=10.0):
        self.wifi_checked = True
        return True

    def lua_install(self, *args):
        self.url_installed = args


def _run(monkeypatch, console, blob=BLOB):
    console.lua_push = AmbyteConsole.lua_push.__get__(console, AmbyteConsole)
    console._lua_abort_quietly = AmbyteConsole._lua_abort_quietly.__get__(
        console, AmbyteConsole)
    monkeypatch.setattr(ambyte_serial, "connect_after_boot",
                        lambda *a, **k: console)
    monkeypatch.setattr(procedure.time, "sleep", lambda _s: None)
    if blob is None:
        def boom(script, log=print):
            raise ReleaseError("offline and not cached")
        monkeypatch.setattr(release_fetch, "script_bytes", boom)
    else:
        monkeypatch.setattr(release_fetch, "script_bytes",
                            lambda script, log=print: blob)
    from types import SimpleNamespace
    return procedure.install_lua_script(
        SimpleNamespace(lua_script=_script(), wifi_ssid="bench-ap"),
        SimpleNamespace(port="COM7"), log=lambda _m: None)


def test_push_path_never_checks_wifi(monkeypatch):
    console = _ProcConsole()
    result = _run(monkeypatch, console)
    assert result.passed
    assert console.staged == BLOB
    # The whole point: a pushed install has no network dependency.
    assert not console.wifi_checked
    assert console.url_installed is None


def test_falls_back_to_the_url_installer_on_old_firmware(monkeypatch):
    console = _ProcConsole(
        begin_reply="Usage: lua <start|stop|status|release|install|exec>")
    result = _run(monkeypatch, console)
    assert result.passed
    assert console.url_installed is not None
    # The fallback does need the board online, so that check comes back.
    assert console.wifi_checked


def test_falls_back_when_the_bytes_are_not_available_locally(monkeypatch):
    console = _ProcConsole()
    result = _run(monkeypatch, console, blob=None)
    assert result.passed
    assert console.url_installed is not None
