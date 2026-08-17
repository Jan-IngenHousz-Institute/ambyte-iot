# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Talk to the ambyte firmware's USB-Serial-JTAG console (pyserial).

Console facts this module is built on (components/CLI/CLI.c):
  * prompt is exactly "ambyte> "; the REPL comes up ~20-35 s after reset
  * linenoise echoes input and emits ANSI refresh/hint sequences — responses
    are found by searching the whole (ANSI-stripped) reply, never by anchoring
    on the echo line
  * a handler failure prints "Command returned non-zero error code" — success
    is the absence of that line (plus per-command success patterns)
  * `cfg get` returns the RAW NVS string: a fleet-default board legitimately
    answers `device_name = AMBYTE_{MAC}` — the firmware expands the {MAC}
    token at boot, in RAM only
  * `rtc set <epoch>` (UTC seconds) applies immediately; `rtc` reads UTC
  * opening the port must not assert DTR/RTS: esptool drives those lines to
    reset/bootloader-trip the chip, so leaving them deasserted means merely
    opening the port never disturbs the running app
"""

from __future__ import annotations

import re
import time
from dataclasses import dataclass, field

import serial
from serial.tools import list_ports

from .config import USB_JTAG_VID

PROMPT = "ambyte> "
_ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
_MAC_RE = re.compile(r"([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})")
_STATUS_MAC_RE = re.compile(r"-\s*MAC:\s*([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})")
_RTC_RE = re.compile(r"RTC:\s*(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\s*\((\d+)\)")
_LUA_RELEASE_RE = re.compile(
    r"lua release:\s+sha256=([0-9A-Fa-f]{64})\s+version=(\S+)\s+"
    r"built_against_fw=(\S+)\s+installed_on_fw=(\S+)\s+"
    r"verified=(true|false)\s+running=(true|false)")
_CFG_GET_RE_TMPL = r"{key}\s*=\s*(.*)"
FAILURE_MARK = "Command returned non-zero error code"

# The firmware expands this token at boot; over `cfg get` it comes back raw.
MAC_TOKEN = "{MAC}"


class ConsoleError(RuntimeError):
    """Console unreachable or a command failed."""


def expand_mac_token(value: str, mac: str) -> str:
    """Render a raw NVS value the way the firmware does at boot."""
    return value.replace(MAC_TOKEN, mac.upper())


def esp_jtag_ports(exclude: str | None = None) -> list[str]:
    """Espressif USB-Serial-JTAG ports currently enumerated (VID 0x303A)."""
    out = []
    for p in list_ports.comports():
        # vid-only match on purpose: post-reset ghosts sometimes drop the pid.
        if p.vid == USB_JTAG_VID and p.device != exclude:
            out.append(p.device)
    return sorted(out)


@dataclass
class ProbeResult:
    """What the pre-flash serial check learned about a board."""

    is_ambyte: bool = False
    mac: str | None = None
    device_name: str | None = None      # raw NVS value ({MAC} un-expanded)
    raw: str = field(default="", repr=False)


@dataclass(frozen=True)
class LuaReleaseStatus:
    sha256: str
    script_version: str
    built_against_fw: str
    installed_on_fw: str
    verified: bool
    running: bool


class AmbyteConsole:
    """One open console session. Use as a context manager."""

    def __init__(self, port: str, timeout: float = 1.0):
        self.port = port
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = 115200        # irrelevant on USB CDC, pyserial needs one
        ser.timeout = 0.2
        ser.dtr = False
        ser.rts = False
        ser.open()
        self._ser = ser
        self._timeout = timeout

    def close(self) -> None:
        try:
            self._ser.close()
        except Exception:
            pass

    def __enter__(self) -> "AmbyteConsole":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    # ── low level ────────────────────────────────────────────────────────
    def _read_available(self) -> str:
        try:
            chunk = self._ser.read(4096)
        except (OSError, serial.SerialException) as exc:
            raise ConsoleError(f"port {self.port} went away: {exc}") from exc
        return chunk.decode("utf-8", "replace") if chunk else ""

    def wait_prompt(self, timeout: float | None = None) -> bool:
        """Nudge with empty lines until the `ambyte> ` prompt shows up.

        An empty line makes linenoise re-print the prompt (we may have
        connected long after it was first shown).
        """
        deadline = time.time() + (timeout or self._timeout)
        buf = ""
        self._ser.reset_input_buffer()
        self._write_line("")
        last_nudge = time.time()
        while time.time() < deadline:
            got = self._read_available()
            if got:
                buf += got
                if PROMPT in _ANSI_RE.sub("", buf):
                    return True
            elif time.time() - last_nudge > 1.0:
                self._write_line("")
                last_nudge = time.time()
        return False

    def _write_line(self, line: str) -> None:
        try:
            self._ser.write((line + "\r\n").encode("utf-8"))
        except (OSError, serial.SerialException) as exc:
            raise ConsoleError(f"port {self.port} went away: {exc}") from exc

    def command(self, cmd: str, timeout: float = 5.0) -> str:
        """Send one command, return the ANSI-stripped reply text.

        The reply is everything up to the re-printed prompt. Raises on the
        REPL's non-zero-exit marker; per-command parsing is the caller's job.
        """
        self._ser.reset_input_buffer()
        self._write_line(cmd)
        deadline = time.time() + timeout
        buf = ""
        while time.time() < deadline:
            buf += self._read_available()
            plain = _ANSI_RE.sub("", buf)
            # The echo arrives first; the prompt we want is the one printed
            # AFTER the command output, i.e. following at least one newline.
            tail = plain.rfind(PROMPT)
            if tail > 0 and "\n" in plain[:tail]:
                return plain
        raise ConsoleError(f"'{cmd}' got no prompt back within {timeout:.0f}s")

    # ── commands ─────────────────────────────────────────────────────────
    def status(self, timeout: float = 5.0) -> str:
        return self.command("status", timeout)

    def status_mac(self) -> str | None:
        m = _STATUS_MAC_RE.search(self.status())
        return m.group(1).upper() if m else None

    def cfg_get(self, key: str, timeout: float = 5.0) -> str | None:
        """Raw NVS value, or None when unset/unreadable."""
        reply = self.command(f"cfg get {key}", timeout)
        if FAILURE_MARK in reply:
            raise ConsoleError(f"cfg get {key} failed:\n{reply[-300:]}")
        for line in reply.splitlines():
            line = line.strip()
            m = re.match(_CFG_GET_RE_TMPL.format(key=re.escape(key)), line)
            # Skip the echo line ("cfg get <key>") — it carries no '='.
            if m and not line.startswith("cfg get"):
                value = m.group(1).strip()
                return None if value.startswith("(unset") else value
        return None

    def cfg_set(self, key: str, value: str, timeout: float = 5.0) -> None:
        arg = f'"{value}"' if " " in value else value
        reply = self.command(f"cfg set {key} {arg}", timeout)
        # Success is exactly: `cfg set <key> = <value>  (reboot to apply)`
        if "(reboot to apply)" not in reply or FAILURE_MARK in reply:
            raise ConsoleError(f"cfg set {key} failed:\n{reply[-300:]}")

    def rtc_read(self) -> int:
        """Current RTC as UTC epoch seconds."""
        reply = self.command("rtc")
        if FAILURE_MARK in reply:
            raise ConsoleError(f"rtc read failed:\n{reply[-300:]}")
        m = _RTC_RE.search(reply)
        if not m:
            raise ConsoleError(f"rtc reply not understood:\n{reply[-300:]}")
        return int(m.group(2))

    def rtc_set(self, epoch: int) -> int:
        """Set the RTC (UTC epoch). Applies immediately. Returns the echoed
        epoch from the `RTC set; RTC: ... (<epoch>)` confirmation."""
        reply = self.command(f"rtc set {int(epoch)}")
        if "RTC set" not in reply or FAILURE_MARK in reply:
            raise ConsoleError(f"rtc set failed:\n{reply[-300:]}")
        m = _RTC_RE.search(reply)
        if not m:
            raise ConsoleError(f"rtc set confirmation not understood:\n{reply[-300:]}")
        return int(m.group(2))

    def lua_install(self, url: str, sha256: str, campaign_id: str,
                    script_version: str, built_against_fw: str) -> None:
        """Queue the firmware's safe URL installer for one released script."""
        args = (url, sha256, campaign_id, script_version, built_against_fw)
        if any(not value or any(ch.isspace() for ch in value) for value in args):
            raise ConsoleError("Lua release fields must be non-empty and whitespace-free")
        cmd = "lua install " + " ".join(args)
        reply = self.command(cmd, timeout=10.0)
        if "lua install queued:" not in reply or FAILURE_MARK in reply:
            raise ConsoleError(f"Lua install request failed:\n{reply[-500:]}")

    def lua_release(self, timeout: float = 10.0) -> LuaReleaseStatus:
        """Read the active /sdcard/main.lua identity and runner state."""
        reply = self.command("lua release", timeout=timeout)
        if FAILURE_MARK in reply:
            raise ConsoleError(f"Lua release status failed:\n{reply[-500:]}")
        match = _LUA_RELEASE_RE.search(reply)
        if match is None:
            raise ConsoleError(f"Lua release status not understood:\n{reply[-500:]}")

        def value(raw: str) -> str:
            return "" if raw == "-" else raw

        return LuaReleaseStatus(
            sha256=match.group(1).lower(),
            script_version=value(match.group(2)),
            built_against_fw=value(match.group(3)),
            installed_on_fw=value(match.group(4)),
            verified=match.group(5) == "true",
            running=match.group(6) == "true",
        )

    def reboot(self) -> None:
        """Fire `reboot`; the port will drop and re-enumerate."""
        try:
            self._write_line("reboot")
            time.sleep(0.5)
        except ConsoleError:
            pass   # port dropping mid-write IS the reboot happening


# ── high-level helpers ──────────────────────────────────────────────────────
def probe_device(port: str, timeout: float = 2.0, retries: int = 1) -> ProbeResult:
    """Pre-flash check: is a live ambyte console on this port, and what name
    does it carry? Short timeout by design — an unflashed/foreign board simply
    times out and the caller falls back to esptool for the MAC."""
    result = ProbeResult()
    for _ in range(1 + retries):
        try:
            with AmbyteConsole(port) as con:
                if not con.wait_prompt(timeout):
                    continue
                result.is_ambyte = True
                try:
                    status = con.status()
                    result.raw = status
                    m = _STATUS_MAC_RE.search(status)
                    if m:
                        result.mac = m.group(1).upper()
                except ConsoleError:
                    pass
                try:
                    result.device_name = con.cfg_get("device_name")
                except ConsoleError:
                    pass
                return result
        except (ConsoleError, OSError, serial.SerialException):
            continue
    return result


def connect_after_boot(preferred_port: str | None, deadline_s: float = 180.0,
                       log=None) -> AmbyteConsole:
    """Console session on a freshly rebooted board.

    The ESP32-S3 native USB-Serial-JTAG re-enumerates to a NEW port name on
    every reset (and leaves ghost ports behind), and the CLI task only starts
    ~20-35 s into boot — so rescan every live JTAG port until one answers with
    the prompt, retrying across re-enumerations until the deadline.
    """
    deadline = time.time() + deadline_s
    said_wait = False
    con: AmbyteConsole | None = None
    active_port: str | None = None
    while time.time() < deadline:
        if con is None:
            cands = ([preferred_port] if preferred_port else []) + \
                [p for p in esp_jtag_ports() if p != preferred_port]
            for cand in cands:
                try:
                    con = AmbyteConsole(cand)
                    active_port = cand
                    break
                except (OSError, serial.SerialException):
                    continue           # ghost / not ready / busy

        if con is not None:
            try:
                # Keep this handle open across polling slices. Opening the
                # ESP32-S3 USB console can reset the board; repeatedly closing
                # and reopening it every three seconds traps startup in a boot
                # loop before the CLI has time to appear.
                remaining = max(0.1, deadline - time.time())
                if con.wait_prompt(timeout=min(3.0, remaining)):
                    if log:
                        log(f"Console up on {active_port}.")
                    return con
            except ConsoleError:
                con.close()
                con = None
                active_port = None
                continue

            # A real USB disconnect makes the open descriptor unusable and is
            # normally raised above. This explicit presence check also handles
            # platforms that leave a quiet ghost descriptor behind.
            if active_port not in esp_jtag_ports():
                con.close()
                con = None
                active_port = None

        if log and not said_wait:
            log("Waiting for the board's console (it starts ~20-35 s after "
                "boot; the USB port may re-enumerate)...")
            said_wait = True
        if con is None:
            time.sleep(1.5)
    if con is not None:
        con.close()
    raise ConsoleError(
        f"no ambyte console answered within {deadline_s:.0f}s — the board may "
        "still be booting, or the USB port re-enumerated to a different name.")
