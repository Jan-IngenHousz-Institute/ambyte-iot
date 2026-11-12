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
  * `wifi_join <ssid> <password>` stores the credentials in the Wi-Fi driver's
    own NVS and blocks up to ~10 s for an IP. It prints exactly one result
    line, `wifi_join "<ssid>": <ESP_OK | error name>`. The console echoes the
    typed line, password included, so nothing from that reply except the
    result line may ever reach a log or an exception
  * arguments are split by esp_console_split_argv: spaces separate, double
    quotes group, and a backslash escapes ONLY backslash, double quote and
    space (any other escaped character is dropped)
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

from .config import USB_JTAG_VID

PROMPT = "ambyte> "
_ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
_MAC_RE = re.compile(r"([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})")
_STATUS_MAC_RE = re.compile(r"-\s*MAC:\s*([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})")
_RTC_RE = re.compile(r"RTC:\s*(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\s*\((\d+)\)")
_SCHEDULE_RELEASE_RE = re.compile(
    r"schedule release:\s+sha256=([0-9A-Fa-f]{64})\s+version=(\S+)\s+"
    r"built_against_fw=(\S+)\s+installed_on_fw=(\S+)\s+"
    r"verified=(true|false)\s+running=(true|false)")
_CFG_GET_RE_TMPL = r"{key}\s*=\s*(.*)"
# `status` prints " - Wi-Fi: connected (provisioned: yes)". The firmware sets
# that connected bit on IP_EVENT_STA_GOT_IP, not on association, so "connected"
# already means the board holds a DHCP lease.
_WIFI_RE = re.compile(r"Wi-?Fi:\s*(connected|disconnected)", re.IGNORECASE)
_WIFI_PROVISIONED_RE = re.compile(r"provisioned:\s*(yes|no)", re.IGNORECASE)
# The handler's one result line: `wifi_join "<ssid>": <name>`. The name is
# anchored to the end of the line and limited to what esp_err_to_name can
# print, so the echoed command (which always ends in the closing quote of the
# quoted password) can never be mistaken for it.
_WIFI_JOIN_RE = re.compile(
    r'wifi_join ".*": ([A-Z][A-Z0-9_ ]*?)[ \t\r]*$', re.MULTILINE)
# Worst case inside the firmware: two bounded driver retries (~2 s each), a
# 2 s drain of the superseded attempt, then the 10 s wait for an IP.
WIFI_JOIN_TIMEOUT_S = 30.0
WIFI_JOIN_OK = "ESP_OK"
WIFI_JOIN_AUTH_REJECTED = "WIFI_MANAGER_ERR_AUTH_REJECTED"
WIFI_JOIN_NOT_RETRYING = "WIFI_MANAGER_ERR_NOT_RETRYING"
WIFI_JOIN_DRIVER_UNRESOLVED = "WIFI_MANAGER_ERR_DRIVER_UNRESOLVED"
# After these the credentials are stored and the firmware keeps retrying them
# on its own backoff (components/wifi_manager/include/wifi_manager.h).
WIFI_JOIN_RETRYING = (WIFI_JOIN_AUTH_REJECTED, "ESP_FAIL", "ESP_ERR_TIMEOUT")
_SD_RE = re.compile(r"SD card:\s*(mounted|absent)", re.IGNORECASE)
_SCHEDULE_PUT_RE = re.compile(r"schedule put:\s*(\d+)\s*bytes")
# `schedule status` prints the program header as
# "  id=<id> version=<v> workbook=<wb> name=<name>" (CLI.c); workbook is "-"
# when the schedule carries no workbookVersionId. Firmware predating the
# workbook provenance prints no such line at all.
_SCHEDULE_HDR_RE = re.compile(r"(?m)^\s*id=\S+ version=\S+ workbook=(\S+)")
# NOT bounded by max_cmdline_length (512). The real limit is the USB-Serial-JTAG
# driver's rx_buffer_size, which ESP-IDF fixes at 256 bytes in
# USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT and which the REPL does not let us size
# (esp_console_dev_usb_serial_jtag_config_t is an empty struct). Overrun it and
# the line never reaches the parser, so the command simply never answers.
# Measured on hardware: a 224-character line works, 264 hangs. 144 raw bytes
# encodes to 192, so the line is 200 characters, comfortably clear of 256.
SCHEDULE_PUT_CHUNK_BYTES = 144
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


def quote_arg(value: str, force: bool = False) -> str:
    """One console argument, as esp_console_split_argv will read it back.

    A value with no space, quote or backslash goes bare. Anything else is
    wrapped in double quotes with backslash and double quote escaped: the
    parser drops a backslash that precedes any other character, so a literal
    backslash must always be doubled. An empty value only survives as `""`.
    `force` quotes unconditionally.
    """
    if not force and value and not any(ch in value for ch in ' "\\'):
        return value
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


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
class BoardStatus:
    """What one `status` reply says about the board. None = not reported
    (older firmware), which callers must not read as a negative."""

    mac: str | None
    wifi_connected: bool | None
    wifi_provisioned: bool | None


def parse_status(text: str) -> BoardStatus:
    mac = _STATUS_MAC_RE.search(text)
    wifi = _WIFI_RE.search(text)
    provisioned = _WIFI_PROVISIONED_RE.search(text)
    return BoardStatus(
        mac=mac.group(1).upper() if mac else None,
        wifi_connected=(None if wifi is None
                        else wifi.group(1).lower() == "connected"),
        wifi_provisioned=(None if provisioned is None
                          else provisioned.group(1).lower() == "yes"),
    )


@dataclass(frozen=True)
class WifiJoinResult:
    """The firmware's verdict on one `wifi_join`, by its printed name."""

    code: str

    @property
    def connected(self) -> bool:
        """Associated and holding a DHCP lease."""
        return self.code == WIFI_JOIN_OK

    @property
    def retrying(self) -> bool:
        """Not connected, but the credentials are stored and the firmware
        keeps trying them."""
        return self.code in WIFI_JOIN_RETRYING


@dataclass(frozen=True)
class ScheduleReleaseStatus:
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

    def listen(self, seconds: float) -> tuple[bool, int]:
        """Passively classify the board: (prompt_seen, bytes_seen).

        Sends one empty-line nudge up front (so an idle-but-alive REPL shows
        its prompt) then only reads. A BOOTING board streams ESP_LOG traffic
        long before the prompt exists; a chip parked in the S3's ROM download
        mode is bit-for-bit silent. That asymmetry lets a caller conclude
        "parked" after ~10 s of true silence instead of waiting out a full
        console deadline (see factory_test.connect_console)."""
        self._ser.reset_input_buffer()
        self._write_line("")
        deadline = time.time() + seconds
        buf = ""
        seen = 0
        while time.time() < deadline:
            got = self._read_available()
            if got:
                seen += len(got)
                buf += got
                if PROMPT in _ANSI_RE.sub("", buf):
                    return True, seen
        return False, seen

    def command(self, cmd: str, timeout: float = 5.0,
                shown: str | None = None) -> str:
        """Send one command, return the ANSI-stripped reply text.

        The reply is everything up to the re-printed prompt. Raises on the
        REPL's non-zero-exit marker; per-command parsing is the caller's job.
        `shown` replaces the command in the timeout error, for a command line
        that carries a secret.
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
        raise ConsoleError(
            f"'{shown or cmd}' got no prompt back within {timeout:.0f}s")

    # ── commands ─────────────────────────────────────────────────────────
    def status(self, timeout: float = 5.0) -> str:
        return self.command("status", timeout)

    def status_mac(self) -> str | None:
        m = _STATUS_MAC_RE.search(self.status())
        return m.group(1).upper() if m else None

    def board_status(self, timeout: float = 10.0) -> BoardStatus:
        """MAC, Wi-Fi link and provisioning flag from one `status` reply."""
        return parse_status(self.status(timeout))

    def wifi_connected(self, timeout: float = 10.0) -> bool | None:
        """Whether the board has an IP, or None if it does not say.

        None means "do not block on this": an older firmware whose `status`
        omits the line must not be treated as offline.
        """
        m = _WIFI_RE.search(self.status(timeout))
        return None if m is None else m.group(1).lower() == "connected"

    def sd_mounted(self, timeout: float = 10.0) -> bool | None:
        """Whether the archive SD card is mounted, or None if it does not say.

        Since the event store and schedule.yaml live on internal flash, the card is
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
        reply = self.command(f"cfg set {key} {quote_arg(value)}", timeout)
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

    def wifi_join(self, ssid: str, password: str,
                  timeout: float = WIFI_JOIN_TIMEOUT_S) -> WifiJoinResult:
        """Store new Wi-Fi credentials and try them now.

        Returns the firmware's verdict; a board that stored the credentials
        but could not reach the access point is a result, not an exception.
        Raises UnsupportedConsoleCommand when the firmware predates
        `wifi_join`. The reply echoes the password, so no part of it other
        than the parsed result name is ever returned or raised.
        """
        line = f"wifi_join {quote_arg(ssid, force=True)} " \
               f"{quote_arg(password, force=True)}"
        reply = self.command(
            line, timeout,
            shown=f"wifi_join {quote_arg(ssid, force=True)} ********")
        results = _WIFI_JOIN_RE.findall(reply)
        if results:
            # The echo precedes the handler's output, so the last match is
            # the verdict.
            return WifiJoinResult(code=results[-1])
        if "Unrecognized command" in reply:
            raise UnsupportedConsoleCommand(
                "this firmware has no `wifi_join` command")
        raise ConsoleError(
            "wifi_join printed no result line (the reply is withheld: the "
            "console echoes the password)")

    def schedule_install(self, url: str, sha256: str, campaign_id: str,
                    script_version: str, built_against_fw: str) -> None:
        """Queue the firmware's safe URL installer for one released script."""
        args = (url, sha256, campaign_id, script_version, built_against_fw)
        if any(not value or any(ch.isspace() for ch in value) for value in args):
            raise ConsoleError("Schedule release fields must be non-empty and whitespace-free")
        cmd = "schedule install " + " ".join(args)
        reply = self.command(cmd, timeout=10.0)
        if "schedule install queued:" not in reply or FAILURE_MARK in reply:
            raise ConsoleError(f"Schedule install request failed:\n{reply[-500:]}")

    def schedule_push(self, blob: bytes, sha256: str, campaign_id: str,
                 script_version: str, built_against_fw: str,
                 log=None) -> None:
        """Stream a script down this console and install it, no device network.

        Raises UnsupportedConsoleCommand when the firmware predates the push
        commands, so the caller can fall back to the URL installer.
        """
        args = (sha256, campaign_id, script_version, built_against_fw)
        if any(not value or any(ch.isspace() for ch in value) for value in args):
            raise ConsoleError("Schedule release fields must be non-empty and whitespace-free")

        reply = self.command("schedule begin", timeout=10.0)
        if "schedule begin: ready" not in reply:
            # Older firmware answers the `schedule <start|stop|...>` usage line and a
            # non-zero exit; that is a capability signal, not a failure.
            if "Usage: schedule" in reply or FAILURE_MARK in reply:
                raise UnsupportedConsoleCommand(
                    "this firmware has no `schedule begin`/`schedule put`")
            raise ConsoleError(f"schedule begin failed:\n{reply[-500:]}")

        sent = 0
        for offset in range(0, len(blob), SCHEDULE_PUT_CHUNK_BYTES):
            chunk = blob[offset:offset + SCHEDULE_PUT_CHUNK_BYTES]
            encoded = base64.b64encode(chunk).decode("ascii")
            reply = self.command(f"schedule put {encoded}", timeout=15.0)
            match = _SCHEDULE_PUT_RE.search(reply)
            if match is None or FAILURE_MARK in reply:
                self._schedule_abort_quietly()
                raise ConsoleError(f"schedule put failed at byte {sent}:\n{reply[-500:]}")
            sent += len(chunk)
            # The device reports the staged file's own size, so a silently
            # dropped chunk is caught here rather than by the digest at commit.
            if int(match.group(1)) != sent:
                self._schedule_abort_quietly()
                raise ConsoleError(
                    f"device staged {match.group(1)} bytes after {sent} were sent")
            if log is not None:
                log(f"Pushed {sent}/{len(blob)} bytes of {script_version}.")

        reply = self.command("schedule commit " + " ".join(args), timeout=15.0)
        if "schedule commit queued:" not in reply or FAILURE_MARK in reply:
            self._schedule_abort_quietly()
            raise ConsoleError(f"schedule commit failed:\n{reply[-500:]}")

    def _schedule_abort_quietly(self) -> None:
        """Drop a half-pushed staging file; never mask the original error."""
        try:
            self.command("schedule abort", timeout=10.0)
        except ConsoleError:
            pass

    def schedule_release(self, timeout: float = 10.0) -> ScheduleReleaseStatus:
        """Read the active /littlefs/schedule.yaml identity and runner state."""
        reply = self.command("schedule release", timeout=timeout)
        if FAILURE_MARK in reply:
            raise ConsoleError(f"Schedule release status failed:\n{reply[-500:]}")
        match = _SCHEDULE_RELEASE_RE.search(reply)
        if match is None:
            raise ConsoleError(f"Schedule release status not understood:\n{reply[-500:]}")

        def value(raw: str) -> str:
            return "" if raw == "-" else raw

        return ScheduleReleaseStatus(
            sha256=match.group(1).lower(),
            script_version=value(match.group(2)),
            built_against_fw=value(match.group(3)),
            installed_on_fw=value(match.group(4)),
            verified=match.group(5) == "true",
            running=match.group(6) == "true",
        )

    def schedule_status_workbook(self, timeout: float = 10.0) -> str | None:
        """The workbookVersionId the running schedule carries, from
        `schedule status`. Returns "" when the header line reports "-"
        (schedule has none) and None when the firmware predates the header
        print entirely — callers must not fail a board for being old."""
        reply = self.command("schedule status", timeout=timeout)
        if FAILURE_MARK in reply:
            raise ConsoleError(f"schedule status failed:\n{reply[-500:]}")
        match = _SCHEDULE_HDR_RE.search(reply)
        if match is None:
            return None
        value = match.group(1)
        return "" if value == "-" else value

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


def connect_after_boot(preferred_port: str | None, deadline_s: float = 180.0,
                       log=None) -> AmbyteConsole:
    """Console session on a freshly rebooted board.

    The ESP32-S3 native USB-Serial-JTAG re-enumerates to a NEW port name on
    every reset (and leaves ghost ports behind), and the CLI task only starts
    ~20-35 s into boot, so rescan every live JTAG port until one answers with
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
        f"no ambyte console answered within {deadline_s:.0f}s; the board may "
        "still be booting, or the USB port re-enumerated to a different name.")
