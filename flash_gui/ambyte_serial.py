# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Talk to the ambyte firmware's USB-Serial-JTAG console (pyserial).

Console facts this module is built on (components/CLI/CLI.c):
  * prompt is exactly "ambyte> "; the REPL comes up ~20-35 s after reset
  * linenoise echoes input and emits ANSI refresh/hint sequences, so responses
    are found by searching the whole (ANSI-stripped) reply, never by anchoring
    on the echo line
  * a handler failure prints "Command returned non-zero error code"; success
    is the absence of that line (plus per-command success patterns)
  * `cfg get` returns the RAW NVS string: a fleet-default board legitimately
    answers `device_name = AMBYTE_{MAC}`, and the firmware expands the {MAC}
    token at boot, in RAM only
  * `rtc set <epoch>` (UTC seconds) applies immediately; `rtc` reads UTC
  * opening the port must not assert DTR/RTS: esptool drives those lines to
    reset/bootloader-trip the chip, so leaving them deasserted means merely
    opening the port never disturbs the running app
"""

from __future__ import annotations

import base64
import re
import time
from dataclasses import dataclass, field

import serial
from serial.tools import list_ports

from . import esptool_ops
from .config import USB_JTAG_VID

PROMPT = "ambyte> "
# The ROM prints "rst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)" on the
# USB-Serial-JTAG console at every reset (CONFIG_ESP_ROM_CONSOLE_OUTPUT_SECONDARY),
# so counting these while waiting for the CLI turns "no prompt" into "the board
# reset N times" — a boot loop — with the last reset reason attached.
_RESET_BANNER_RE = re.compile(r"rst:0x[0-9a-fA-F]+\s*\(([A-Z_0-9]+)\)")
_PANIC_RE = re.compile(
    r"(Guru Meditation Error[^\r\n]*|Brownout detector was triggered|"
    r"abort\(\) was called[^\r\n]*|assert failed[^\r\n]*)")
# Keep this much of the most recent console output for the failure report.
RX_TAIL_BYTES = 8192
_ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
_MAC_RE = re.compile(r"([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})")
_STATUS_MAC_RE = re.compile(r"-\s*MAC:\s*([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})")
_RTC_RE = re.compile(r"RTC:\s*(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\s*\((\d+)\)")
_LUA_RELEASE_RE = re.compile(
    r"lua release:\s+sha256=([0-9A-Fa-f]{64})\s+version=(\S+)\s+"
    r"built_against_fw=(\S+)\s+installed_on_fw=(\S+)\s+"
    r"verified=(true|false)\s+running=(true|false)")
_CFG_GET_RE_TMPL = r"{key}\s*=\s*(.*)"
# `status` prints " - Wi-Fi: connected (provisioned: yes)". The firmware sets
# that connected bit on IP_EVENT_STA_GOT_IP, not on association, so "connected"
# already means the board holds a DHCP lease.
_WIFI_RE = re.compile(r"Wi-?Fi:\s*(connected|disconnected)", re.IGNORECASE)
_SD_RE = re.compile(r"SD card:\s*(mounted|absent)", re.IGNORECASE)
_LUA_PUT_RE = re.compile(r"lua put:\s*(\d+)\s*bytes")
# NOT bounded by max_cmdline_length (512). The real limit is the USB-Serial-JTAG
# driver's rx_buffer_size, which ESP-IDF fixes at 256 bytes in
# USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT and which the REPL does not let us size
# (esp_console_dev_usb_serial_jtag_config_t is an empty struct). Overrun it and
# the line never reaches the parser, so the command simply never answers.
# Measured on hardware: a 224-character line works, 264 hangs. 144 raw bytes
# encodes to 192, so the line is 200 characters, comfortably clear of 256.
LUA_PUT_CHUNK_BYTES = 144
FAILURE_MARK = "Command returned non-zero error code"

# The firmware expands this token at boot; over `cfg get` it comes back raw.
MAC_TOKEN = "{MAC}"


class ConsoleError(RuntimeError):
    """Console unreachable or a command failed."""


class UnsupportedConsoleCommand(ConsoleError):
    """The firmware does not implement this command.

    Distinct from a failure so callers can fall back rather than give up: a board
    flashed with a release older than the serial-push commands is fine, it just
    has to install its script the old way.
    """


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
        # Observed 2026-08-25: a board whose app is up but not servicing the
        # USB-JTAG console (crashed / not yet at console init) stays enumerated
        # and never drains USB OUT, so a write with no timeout blocks FOREVER
        # and the whole GUI hangs on the first prompt nudge. Bounded, so it
        # surfaces as a ConsoleError the wait loop can act on instead.
        ser.write_timeout = 2.0
        ser.dtr = False
        ser.rts = False
        ser.open()
        self._ser = ser
        self._timeout = timeout
        # Everything the board printed while we held the port, bounded. The
        # firmware and the ROM share this console, so an empty tail after a
        # long wait means "nothing is running", not "the CLI is slow".
        self.rx_total = 0
        self._rx_tail = ""

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
        if not chunk:
            return ""
        text = chunk.decode("utf-8", "replace")
        self.rx_total = getattr(self, "rx_total", 0) + len(chunk)
        self._rx_tail = (getattr(self, "_rx_tail", "") + text)[-RX_TAIL_BYTES:]
        return text

    def rx_tail(self, lines: int = 6) -> str:
        """The last few non-empty, ANSI-stripped lines the board printed."""
        clean = _ANSI_RE.sub("", getattr(self, "_rx_tail", ""))
        kept = [ln.strip() for ln in clean.replace("\r", "\n").split("\n")
                if ln.strip()]
        return "\n".join(kept[-lines:])

    def reset_count(self) -> int:
        return len(_RESET_BANNER_RE.findall(getattr(self, "_rx_tail", "")))

    def last_reset_reason(self) -> str | None:
        hits = _RESET_BANNER_RE.findall(getattr(self, "_rx_tail", ""))
        return hits[-1] if hits else None

    def last_panic(self) -> str | None:
        hits = _PANIC_RE.findall(_ANSI_RE.sub("", getattr(self, "_rx_tail", "")))
        return hits[-1].strip() if hits else None

    # NB: there is deliberately no "pulse RTS to reset" helper here. Verified
    # on hardware 2026-08-25: on this Windows usbser stack an RTS reset boots
    # the S3 into the ROM downloader (emulated boot strap reads DTR as
    # asserted). Reboot via esptool's watchdog reset instead — see
    # esptool_ops.reboot_into_app and rescue_download_mode().

    def rescue_download_mode(self) -> bool:
        """Boot a ROM-downloader-parked chip into the app, asking over THIS
        handle. True when the ROM was listening and has been rebooted — the
        USB device re-enumerates, so this handle is dead afterwards and the
        caller must reopen; False when nothing answered (app presumably
        running). See esptool_ops.rescue_from_download_mode.
        """
        return esptool_ops.rescue_from_download_mode(self._ser)

    def wait_prompt(self, timeout: float | None = None) -> bool:
        """Nudge with empty lines until the `ambyte> ` prompt shows up.

        An empty line makes linenoise re-print the prompt (we may have
        connected long after it was first shown).
        """
        deadline = time.time() + (timeout or self._timeout)
        buf = ""
        try:
            self._ser.reset_input_buffer()
        except (OSError, serial.SerialException) as exc:
            # PortNotOpenError included: a handle closed under us (USB drop,
            # or a helper that closed it) must become a ConsoleError the wait
            # loop can recover from, never an "UNEXPECTED ERROR" dialog.
            raise ConsoleError(f"port {self.port} went away: {exc}") from exc
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

    def wifi_connected(self, timeout: float = 10.0) -> bool | None:
        """Whether the board has an IP, or None if it does not say.

        None means "do not block on this": an older firmware whose `status`
        omits the line must not be treated as offline.
        """
        m = _WIFI_RE.search(self.status(timeout))
        return None if m is None else m.group(1).lower() == "connected"

    def sd_mounted(self, timeout: float = 10.0) -> bool | None:
        """Whether the archive SD card is mounted, or None if it does not say.

        Since the event store and main.lua live on internal flash, the card is
        optional — this is for operator information, never a gate. None means
        the firmware predates the status line.
        """
        m = _SD_RE.search(self.status(timeout))
        return None if m is None else m.group(1).lower() == "mounted"

    def cfg_get(self, key: str, timeout: float = 5.0) -> str | None:
        """Raw NVS value, or None when unset/unreadable."""
        reply = self.command(f"cfg get {key}", timeout)
        if FAILURE_MARK in reply:
            raise ConsoleError(f"cfg get {key} failed:\n{reply[-300:]}")
        for line in reply.splitlines():
            line = line.strip()
            m = re.match(_CFG_GET_RE_TMPL.format(key=re.escape(key)), line)
            # Skip the echo line ("cfg get <key>"), it carries no '='.
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

    def lua_push(self, blob: bytes, sha256: str, campaign_id: str,
                 script_version: str, built_against_fw: str,
                 log=None) -> None:
        """Stream a script down this console and install it, no device network.

        Raises UnsupportedConsoleCommand when the firmware predates the push
        commands, so the caller can fall back to the URL installer.
        """
        args = (sha256, campaign_id, script_version, built_against_fw)
        if any(not value or any(ch.isspace() for ch in value) for value in args):
            raise ConsoleError("Lua release fields must be non-empty and whitespace-free")

        reply = self.command("lua begin", timeout=10.0)
        if "lua begin: ready" not in reply:
            # Older firmware answers the `lua <start|stop|...>` usage line and a
            # non-zero exit; that is a capability signal, not a failure.
            if "Usage: lua" in reply or FAILURE_MARK in reply:
                raise UnsupportedConsoleCommand(
                    "this firmware has no `lua begin`/`lua put`")
            raise ConsoleError(f"lua begin failed:\n{reply[-500:]}")

        sent = 0
        for offset in range(0, len(blob), LUA_PUT_CHUNK_BYTES):
            chunk = blob[offset:offset + LUA_PUT_CHUNK_BYTES]
            encoded = base64.b64encode(chunk).decode("ascii")
            reply = self.command(f"lua put {encoded}", timeout=15.0)
            match = _LUA_PUT_RE.search(reply)
            if match is None or FAILURE_MARK in reply:
                self._lua_abort_quietly()
                raise ConsoleError(f"lua put failed at byte {sent}:\n{reply[-500:]}")
            sent += len(chunk)
            # The device reports the staged file's own size, so a silently
            # dropped chunk is caught here rather than by the digest at commit.
            if int(match.group(1)) != sent:
                self._lua_abort_quietly()
                raise ConsoleError(
                    f"device staged {match.group(1)} bytes after {sent} were sent")
            if log is not None:
                log(f"Pushed {sent}/{len(blob)} bytes of {script_version}.")

        reply = self.command("lua commit " + " ".join(args), timeout=15.0)
        if "lua commit queued:" not in reply or FAILURE_MARK in reply:
            self._lua_abort_quietly()
            raise ConsoleError(f"lua commit failed:\n{reply[-500:]}")

    def _lua_abort_quietly(self) -> None:
        """Drop a half-pushed staging file; never mask the original error."""
        try:
            self.command("lua abort", timeout=10.0)
        except ConsoleError:
            pass

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
    does it carry? Short timeout by design: an unflashed/foreign board simply
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


# Wait-loop tunables (module-level so tests can shrink them).
# esptool has just closed the port after its watchdog reboot, which drops and
# re-enumerates the USB device; give Windows a moment before we start
# scanning. The loop copes with a port that is not back yet, so this only
# spares a few noisy "cannot open" lines.
CONSOLE_SETTLE_S = 1.0
# One "still waiting" line per this interval, so a 3-minute silence in the GUI
# log is never mistaken for a hung tool.
CONSOLE_PROGRESS_S = 30.0
# Both the ROM and the firmware print on this console at boot, so a board that
# has produced zero bytes for this long after the settle window is not slow: it
# is parked in the ROM downloader, unpowered, or the port belongs to someone
# else. Check for the first case once and reset out of it.
CONSOLE_SILENCE_RESET_S = 45.0
# Reset banners seen before the CLI appears. One is the normal boot; a
# firmware may legitimately reboot once more (first-boot NVS/partition fixups).
# This many means a boot loop, and waiting the full deadline is pointless.
BOOT_LOOP_RESETS = 4


def connect_after_boot(preferred_port: str | None, deadline_s: float = 180.0,
                       log=None, settle_s: float | None = None) -> AmbyteConsole:
    """Console session on a freshly rebooted board.

    The ESP32-S3 native USB-Serial-JTAG re-enumerates to a NEW port name on
    every reset (and leaves ghost ports behind), and the CLI task only starts
    ~20-35 s into boot, so rescan every live JTAG port until one answers with
    the prompt, retrying across re-enumerations until the deadline.

    Everything the board prints meanwhile is captured, so the wait is
    self-diagnosing: total silence, a boot loop (repeated ROM reset banners,
    with the last reset reason / panic), and a busy port each get their own
    verdict instead of one generic timeout.

    Hardware-verified 2026-08-25 (fw 1.10.1, Windows 11, two PCs): esptool's
    RTS "hard-reset" boots this board into the ROM downloader every time
    (emulated boot strap, see esptool_ops.reboot_into_app), which is why the
    GUI used to wait 180 s on a silent port after every flash. The flasher now
    reboots via the RTC watchdog instead, so normally the app is simply
    booting when we get here. As a backstop, the FIRST thing done on a newly
    opened handle is to ask the ROM through it (`no-reset` sync); if it
    answers, the board is rebooted the same way — the handle then dies with
    the USB re-enumeration and the loop reopens. The silence check below
    repeats that once more.
    """
    if settle_s is None:
        settle_s = CONSOLE_SETTLE_S
    start = time.time()
    deadline = start + deadline_s
    said_wait = False
    con: AmbyteConsole | None = None
    active_port: str | None = None
    ports_seen: set[str] = set()
    open_failures: dict[str, str] = {}    # port -> last error text, logged once
    silence_checked = False
    next_progress = start + CONSOLE_PROGRESS_S
    # Console output folded in from handles that were closed along the way.
    rx_total = 0
    rx_tail = ""
    resets = 0
    last_reason: str | None = None
    panic: str | None = None

    def fold(c: AmbyteConsole) -> None:
        nonlocal rx_total, rx_tail, resets, last_reason, panic
        rx_total += c.rx_total
        if c.rx_total:
            rx_tail = c.rx_tail(8)
        resets += c.reset_count()
        last_reason = c.last_reset_reason() or last_reason
        panic = c.last_panic() or panic

    def drop() -> None:
        nonlocal con, active_port
        if con is not None:
            fold(con)
            con.close()
        con = None
        active_port = None

    # Check/rescue download mode on the first handle we get (and again if the
    # board then stays silent), never on every reopen: a flapping port would
    # otherwise turn into a reset storm.
    rescue_pending = True

    if settle_s > 0:
        time.sleep(min(settle_s, deadline_s))

    while time.time() < deadline:
        if con is None:
            cands = ([preferred_port] if preferred_port else []) + \
                [p for p in esp_jtag_ports() if p != preferred_port]
            ports_seen.update(cands)
            for cand in cands:
                try:
                    con = AmbyteConsole(cand)
                    active_port = cand
                    if rescue_pending:
                        rescue_pending = False
                        if con.rescue_download_mode():
                            if log:
                                log(f"Board on {cand} was sitting in the ROM "
                                    "downloader; rebooted it via watchdog. The "
                                    "port re-enumerates; CLI in ~20-35 s...")
                        elif log:
                            log(f"Board on {cand} is not in the ROM downloader; "
                                "waiting for its console.")
                    break
                except (OSError, serial.SerialException) as exc:
                    # ghost / not ready / busy — but say so once per port,
                    # because "busy" is a different problem from "slow board".
                    msg = (str(exc).splitlines() or ["?"])[0][:120]
                    if log and open_failures.get(cand) != msg:
                        log(f"Cannot open {cand} ({msg}); if this persists, "
                            "another program is holding the port.")
                    open_failures[cand] = msg
                    continue

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
                drop()
                continue

            # A real USB disconnect makes the open descriptor unusable and is
            # normally raised above. This explicit presence check also handles
            # platforms that leave a quiet ghost descriptor behind.
            if active_port not in esp_jtag_ports():
                drop()

        if log and not said_wait:
            log("Waiting for the board's console (it starts ~20-35 s after "
                "boot; the USB port may re-enumerate)...")
            said_wait = True

        now = time.time()
        total_rx = rx_total + (con.rx_total if con else 0)
        total_resets = resets + (con.reset_count() if con else 0)

        if total_resets >= BOOT_LOOP_RESETS:
            drop()
            detail = f"last reset reason: {last_reason or 'unknown'}"
            if panic:
                detail += f"; last panic: {panic}"
            raise ConsoleError(
                f"the board reset {resets} times while waiting for its console "
                f"— it is boot-looping, not slow ({detail}). Check power/battery "
                "and the SD card, then use Retry provisioning (no re-flash).\n"
                f"Last console output:\n{rx_tail}")

        if (not silence_checked and total_rx == 0
                and now - start >= settle_s + CONSOLE_SILENCE_RESET_S):
            silence_checked = True
            if log:
                log(f"Nothing printed for {CONSOLE_SILENCE_RESET_S:.0f}s. Checking "
                    "again whether the board is parked in the ROM bootloader "
                    "(download mode)...")
            if con is not None:
                # Same handle, never a reopen (reopening is what parks it).
                if con.rescue_download_mode():
                    if log:
                        log("It was: rebooted it via watchdog through "
                            f"{active_port}; the port re-enumerates, reopening.")
                    resets = 0
                elif log:
                    log("The ROM does not answer either; the board is not "
                        "in download mode.")
            else:
                # No handle to talk through: let the next open run the check.
                rescue_pending = True
            continue

        if log and now >= next_progress:
            next_progress = now + CONSOLE_PROGRESS_S
            live = ", ".join(esp_jtag_ports()) or "none"
            holding = f"holding {active_port}" if con else "no port open"
            if total_rx == 0:
                state = "no output at all"
            else:
                state = (f"{total_rx} bytes of boot output, {total_resets} reset "
                         "banner(s), CLI not up yet")
            log(f"Still waiting ({now - start:.0f}s): Espressif ports: {live}; "
                f"{holding}; {state}.")

        if con is None:
            time.sleep(1.5)

    drop()
    if rx_total == 0:
        why = ("the board printed NOTHING on its console, so it is not merely "
               "slow: it is parked in the ROM bootloader, not booting, or the "
               "port is held by another program. Unplug and replug the USB "
               "cable, close any serial monitor, click Refresh, then use "
               "Retry provisioning (no re-flash).")
    else:
        seen = f"{rx_total} bytes of boot output, {resets} reset banner(s)"
        if last_reason:
            seen += f", last reset reason {last_reason}"
        if panic:
            seen += f", last panic: {panic}"
        why = (f"the board printed {seen}, but its CLI never came up. It may "
               "still be recovering the SD card: wait a minute and use Retry "
               "provisioning (no re-flash), or remove the SD card and retry.")
    ports = ", ".join(sorted(ports_seen)) or "none"
    msg = (f"no ambyte console answered within {deadline_s:.0f}s; {why}\n"
           f"Espressif ports seen: {ports}.")
    if open_failures:
        msg += "\nPorts that would not open: " + "; ".join(
            f"{p} ({e})" for p, e in sorted(open_failures.items()))
    if rx_tail:
        msg += f"\nLast console output:\n{rx_tail}"
    raise ConsoleError(msg)
