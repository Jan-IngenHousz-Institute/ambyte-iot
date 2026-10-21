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
  * set DTR/RTS false before opening. An open can still reset the app on
    some hosts; wait for the REPL before discovery and keep that descriptor
    open through boot polling
"""

from __future__ import annotations

import base64
import codecs
import json
import re
import time
from dataclasses import dataclass, field

import serial
from serial.tools import list_ports

from . import config
from .config import USB_JTAG_VID
from .service_client import (OperationJournal, ServiceClient, ServiceClientError,
                             decode_frame)

PROMPT = "ambyte> "
PROBE_READY_TIMEOUT_S = 45.0
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
    identity: ConsoleIdentity | None = None


@dataclass(frozen=True)
class ScheduleIdentity:
    source: str | None
    sha256: str | None
    workbook_version_id: str | None
    script_version: str | None


@dataclass(frozen=True)
class ConsoleIdentity:
    mac: str | None
    thing_name: str | None
    fw: str | None
    contract: int
    schedule: ScheduleIdentity
    capabilities: frozenset[str]
    signed: bool = False
    storage: dict | None = None


def _identity_object(obj: dict) -> ConsoleIdentity:
    """Validate v1 before exposing metadata to provisioning or write gates."""
    def nullable(value):
        if value is not None and (not isinstance(value, str) or not value):
            raise ValueError("expected nonempty string or null")
        if value is not None:
            value.encode("utf-8", "strict")
        return value

    if obj["contract"] != 1 or type(obj["contract"]) is not int:
        raise ValueError("unsupported identity contract")
    mac = nullable(obj["mac"])
    if mac is not None and _MAC_RE.fullmatch(mac) is None:
        raise ValueError("invalid MAC")
    schedule = obj["schedule"]
    source = nullable(schedule["source"])
    if source not in (None, "NONE", "INSTALLED", "EMBEDDED_DEFAULT", "EMBEDDED_FALLBACK"):
        raise ValueError("invalid loaded source")
    sha = nullable(schedule["sha256"])
    if sha is not None and re.fullmatch(r"[0-9a-fA-F]{64}", sha) is None:
        raise ValueError("invalid schedule digest")
    caps = obj["capabilities"]
    if (not isinstance(caps, list) or any(not isinstance(c, str) or not c for c in caps)
            or len(set(caps)) != len(caps)):
        raise ValueError("invalid capabilities")
    for cap in caps:
        cap.encode("utf-8", "strict")
    signed = obj.get("signed", False)
    if type(signed) is not bool:
        raise ValueError("signed must be a boolean")
    storage = obj.get("storage")
    if storage is not None:
        if not isinstance(storage, dict) or set(storage) != {"readable", "written", "fallback"}:
            raise ValueError("invalid storage manifest")
        readable, written, fallback = storage["readable"], storage["written"], storage["fallback"]
        if (not isinstance(readable, list) or not readable or
                any(type(v) is not int or not 1 <= v <= 31 for v in readable) or
                readable != sorted(set(readable)) or type(written) is not int or written not in readable or
                type(fallback) is not int or not 1 <= fallback <= 31):
            raise ValueError("invalid storage versions")
    return ConsoleIdentity(mac.upper() if mac else None,
        nullable(obj["thing_name"]), nullable(obj["fw"]), 1,
        ScheduleIdentity(source, sha.lower() if sha else None,
                         nullable(schedule["workbookVersionId"]),
                         nullable(schedule["script_version"])), frozenset(caps), signed, storage)


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
        self._decoder = codecs.getincrementaldecoder("utf-8")("surrogateescape")

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
        return self._decoder.decode(chunk) if chunk else ""

    def wait_prompt(self, timeout: float | None = None, *, recover_service=False) -> bool:
        """Nudge with empty lines until the `ambyte> ` prompt shows up.

        An empty line makes linenoise re-print the prompt (we may have
        connected long after it was first shown).
        """
        deadline = time.time() + (timeout or self._timeout)
        buf = ""
        self._ser.reset_input_buffer()
        self._decoder.reset()
        self._write_line("")
        last_nudge = time.time()
        while time.time() < deadline:
            got = self._read_available()
            if got:
                buf += got
                if PROMPT in _ANSI_RE.sub("", buf):
                    return True
                if recover_service:
                    for line in buf.splitlines(keepends=True):
                        try:
                            decode_frame(line.encode("utf-8"))
                        except (ServiceClientError, UnicodeError):
                            continue
                        # A disconnected client can leave svc active for 60s.
                        # Exit through a fresh read-only version/bind exchange;
                        # never let repeated console nudges keep it alive forever.
                        self._ser.reset_input_buffer()
                        session = ServiceClient(self._ser, None, clock=time.monotonic, sleep=time.sleep)
                        session._deadline = time.monotonic() + max(0.1, deadline - time.time())
                        try:
                            session.connect()
                            session.exit()
                        except ServiceClientError as exc:
                            try:
                                session.exit()
                            except ServiceClientError:
                                pass
                            raise ConsoleError(str(exc)) from exc
                        self._write_line("")
                        buf = ""
                        break
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
        self._decoder.reset()
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

    def command(self, cmd: str, timeout: float = 5.0, *, strip_ansi: bool = True, max_reply_bytes: int | None = None) -> str:
        """Send one command, return the ANSI-stripped reply text.

        The reply includes the re-printed prompt. Per-command parsing and the
        REPL's non-zero-exit marker are the caller's responsibility. Discovery
        keeps raw physical lines; other commands strip linenoise ANSI escapes.
        """
        self._ser.reset_input_buffer()
        self._decoder.reset()
        self._write_line(cmd)
        deadline = time.time() + timeout
        buf = ""
        while time.time() < deadline:
            buf += self._read_available()
            if max_reply_bytes is not None and len(buf.encode("utf-8", "surrogateescape")) > max_reply_bytes:
                raise ConsoleError("Console response exceeded its byte limit")
            plain = _ANSI_RE.sub("", buf)
            # The echo arrives first; the prompt we want is the one printed
            # AFTER the command output, i.e. following at least one newline.
            tail = plain.rfind(PROMPT)
            if tail > 0 and "\n" in plain[:tail]:
                return plain if strip_ansi else buf
        raise ConsoleError(f"'{cmd}' got no prompt back within {timeout:.0f}s")

    # ── commands ─────────────────────────────────────────────────────────
    def ota_policy(self, timeout: float = 5.0) -> dict | None:
        """Additive OTA diagnostic; an older ota_status response returns None."""
        reply = self.command("ota_status", timeout, strip_ansi=False, max_reply_bytes=2048)
        for line in reply.split("\n"):
            line = line.rstrip("\r")
            if not line.startswith("policy: "):
                continue
            try:
                line.encode("utf-8", "strict")
                obj = json.loads(line[8:])
                if (not isinstance(obj, dict) or type(obj.get("schema")) is not int or obj["schema"] != 1
                        or not {"schema", "readable", "written", "fallback", "refusal_state", "last_refusal"}.issubset(obj)
                        or obj.get("refusal_state") not in ("available", "missing", "unavailable", "corrupt")):
                    raise ValueError("invalid policy schema")
                readable, written, fallback = obj.get("readable"), obj.get("written"), obj.get("fallback")
                if readable is None:
                    if written is not None or fallback is not None:
                        raise ValueError("inconsistent unavailable manifest")
                elif (not isinstance(readable, list) or not readable or
                      any(type(v) is not int or not 1 <= v <= 31 for v in readable) or
                      readable != sorted(set(readable)) or type(written) is not int or written not in readable or
                      type(fallback) is not int or not 1 <= fallback <= 31):
                    raise ValueError("invalid storage versions")
                refusal = obj.get("last_refusal")
                if obj["refusal_state"] == "available":
                    if not isinstance(refusal, dict) or any(not isinstance(refusal.get(k), str) for k in ("id", "source", "reason")):
                        raise ValueError("invalid persisted refusal")
                    if len(refusal["id"]) > 71 or len(refusal["source"]) > 11 or len(refusal["reason"]) > 63:
                        raise ValueError("oversized persisted refusal")
                elif refusal is not None:
                    raise ValueError("unavailable refusal must be null")
                return obj
            except (ValueError, TypeError) as exc:
                raise ConsoleError(f"Invalid OTA policy diagnostic: {exc}") from exc
        return None

    def identity(self, timeout: float = 5.0) -> ConsoleIdentity:
        """First physical JSON-object line with an integer contract is the reply.

        Logs/malformed lines are ignored. Only an explicit old-image reply
        permits legacy discovery; silence and invalid v1 metadata fail closed.
        """
        reply = self.command("identity", timeout, strip_ansi=False)
        unsupported = False
        for line in reply.split("\n"):
            line = line.rstrip("\r")
            if line == "Unrecognized command":
                unsupported = True
            if not line.startswith("{") or not line.endswith("}"):
                continue
            try:
                line.encode("utf-8", "strict")
                obj = json.loads(line)
            except (ValueError, UnicodeError):
                continue
            if not isinstance(obj, dict) or type(obj.get("contract")) is not int:
                continue
            try:
                return _identity_object(obj)
            except (KeyError, TypeError, ValueError, UnicodeError) as exc:
                raise ConsoleError(f"Invalid identity response: {exc}") from exc
        if unsupported:
            raise UnsupportedConsoleCommand("identity: Unrecognized command")
        raise ConsoleError("identity returned no valid JSON response")

    def sensors(self, timeout: float = 5.0):
        """Fresh capability discovery and a cache-only attachment snapshot.

        Unsupported older firmware is explicitly unavailable (None). A failed
        advertised command, malformed snapshot or silence is an error.
        """
        from .sensors import decode_sensors, SensorsError
        try:
            identity = self.identity(timeout=timeout)
        except UnsupportedConsoleCommand:
            if self.status_mac() is None:
                raise ConsoleError("Older firmware did not provide a gateway MAC")
            return None
        if "sensors" not in identity.capabilities:
            return None
        reply = self.command("sensors", timeout, strip_ansi=False, max_reply_bytes=65536)
        for line in reply.split("\n"):
            line = line.rstrip("\r")
            if not line.startswith("{"):
                continue
            try:
                return decode_sensors(line)
            except SensorsError as exc:
                raise ConsoleError(str(exc)) from exc
        raise ConsoleError("sensors returned no valid JSON response")

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

    def schedule_service_push(self, blob: bytes, sha256: str, script_version: str,
                              built_against_fw: str, *, mac: str, workbook=None,
                              cancelled=lambda: False, log=None, deadline_s=360.0,
                              new_request=False):
        """Use the same journaled client as the bench on this open handle.

        Reconnection is read-only until strict identity proves the same gateway
        and capability. The new handle belongs to this console, so subsequent
        activation verification and close use it too.
        """
        journal = OperationJournal(config.SERVICE_OPERATIONS_DIR, mac)

        def reconnect(remaining):
            reconnect_deadline = time.monotonic() + remaining
            self.close()
            replacement = connect_after_boot(self.port, deadline_s=min(45.0, remaining),
                                             log=log, recover_service=True)
            try:
                remaining = reconnect_deadline - time.monotonic()
                if remaining <= 0:
                    raise ConsoleError("Transfer deadline reached while waiting for the gateway.")
                identity = replacement.identity(timeout=min(5.0, remaining))
                if identity.mac != journal.mac or "svc" not in identity.capabilities:
                    raise ConsoleError("Reconnected gateway identity or service capability changed; transfer preserved.")
            except Exception:
                replacement.close()
                raise
            self.port, self._ser = replacement.port, replacement._ser
            self._decoder.reset()
            return self._ser

        client = ServiceClient(self._ser, journal, reconnect=reconnect,
                               clock=time.monotonic, sleep=time.sleep)
        try:
            return client.upload_schedule(blob, sha256, script_version, built_against_fw,
                                          workbook, cancelled=cancelled, log=log,
                                          new_request=new_request, deadline_s=deadline_s)
        except ServiceClientError as exc:
            raise ConsoleError(str(exc)) from exc

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
def probe_device(port: str, timeout: float = PROBE_READY_TIMEOUT_S,
                 retries: int = 1) -> ProbeResult:
    """Keep one handle through boot, then prefer the versioned JSON contract.

    Only explicit `Unrecognized command` selects the old status parser. A
    ready console with corrupt discovery must be reported to the operator.
    `retries` remains accepted for callers; boot polling owns the time budget.
    """
    result = ProbeResult()
    try:
        con = AmbyteConsole(port)
    except (OSError, serial.SerialException):
        return result
    try:
        # Preflight is bound to the selected port; scanning unrelated boards
        # here could associate their MAC with the port later flashed by the GUI.
        try:
            if not con.wait_prompt(timeout):
                return result
        except ConsoleError:
            return result
        result.is_ambyte = True
        try:
            result.identity = con.identity()
            result.mac = result.identity.mac
        except UnsupportedConsoleCommand:
            result.raw = con.status()
            match = _STATUS_MAC_RE.search(result.raw)
            result.mac = match.group(1).upper() if match else None
        try:
            result.device_name = con.cfg_get("device_name")
        except ConsoleError:
            pass
        return result
    finally:
        con.close()


def connect_after_boot(preferred_port: str | None, deadline_s: float = 180.0,
                       log=None, *, recover_service=False) -> AmbyteConsole:
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
                options = {"recover_service": True} if recover_service else {}
                if con.wait_prompt(timeout=min(3.0, remaining), **options):
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
