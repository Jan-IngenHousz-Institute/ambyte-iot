# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Change one setting on an already on-boarded board, over its USB console.

On-boarding (procedure.py) is the only complete provisioning path: it flashes
the firmware and bakes identity, certificates and Wi-Fi into NVS. Three things
an operator wants to change afterwards need none of that, because the running
firmware already takes them on its console:

  Wi-Fi   `wifi_join <ssid> <password>`  applies now and survives reboots
  name    `cfg set device_name <name>`   read at boot, so reboot + read back
  clock   `rtc set <epoch>`              applies now (UTC); optionally
          `cfg set timezone <zone>`      read at boot, so reboot + read back

Nothing here talks to openJII or needs a firmware release: a rename changes
the name the board reports in its data, not its registry entry or its MQTT
identity (the Thing name).

One BoardSession holds one console open for the whole action, including any
confirmation dialog the GUI shows in between: closing and reopening the S3's
USB console can reset the board.
"""

from __future__ import annotations

import time
from dataclasses import dataclass

from . import ambyte_serial, timezones
from .ambyte_serial import (WIFI_JOIN_AUTH_REJECTED,
                            WIFI_JOIN_DRIVER_UNRESOLVED,
                            WIFI_JOIN_NOT_RETRYING, ConsoleError,
                            UnsupportedConsoleCommand, expand_mac_token)
from .config import MAX_NAME_LEN
from .procedure import (CONSOLE_BOOT_DEADLINE_S, RTC_TOLERANCE_S,
                        clean_device_name)

# 802.11 limits, as the firmware's wifi_config_t holds them.
WIFI_SSID_MAX_BYTES = 32
WIFI_PASSWORD_MIN_CHARS = 8
WIFI_PASSWORD_MAX_CHARS = 63

# The board is normally already running, but merely opening the S3's USB
# console can reset it, and the console then needs 20-35 s to come up.
CONNECT_DEADLINE_S = 90.0
# `wifi_join` itself waits 10 s for an IP. A slow DHCP server or an access
# point still holding a stale association resolves shortly after, so keep
# watching briefly before reporting "saved, not connected".
WIFI_SETTLE_S = 15.0
WIFI_POLL_S = 3.0
# Same pause the on-boarding repair path gives the USB port to drop.
REBOOT_SETTLE_S = 2.0


class SettingsError(RuntimeError):
    """The change was not made, or could not be confirmed."""


@dataclass(frozen=True)
class Outcome:
    """A change that was made. `warning` marks one the operator still has to
    act on (Wi-Fi stored but not connected)."""

    summary: str            # short: the GUI's result label
    detail: str = ""        # the rest, for the log and any dialog
    warning: bool = False


@dataclass(frozen=True)
class BoardIdentity:
    mac: str | None
    name: str                       # as the firmware reports it ({MAC} expanded)
    timezone: str
    wifi_connected: bool | None
    wifi_provisioned: bool | None

    def describe(self) -> str:
        wifi = {True: "connected", False: "not connected",
                None: "state unknown"}[self.wifi_connected]
        return (f"{self.name or '(unnamed)'}, MAC {self.mac or 'unknown'}, "
                f"timezone {self.timezone or '(unset)'}, Wi-Fi {wifi}")


def _printable_ascii(text: str) -> bool:
    return all(" " <= ch <= "~" for ch in text)


def wifi_problem(ssid: str, password: str) -> str | None:
    """Why these credentials cannot be sent, or None when they can.

    Stricter than 802.11 on one point: the console is a line editor that
    treats control and non-ASCII bytes as keys, so only printable ASCII can be
    typed into it. On-boarding bakes credentials into NVS and has no such
    limit.
    """
    if not ssid:
        return "enter the Wi-Fi SSID first."
    if not _printable_ascii(ssid) or not _printable_ascii(password):
        return ("the SSID and password must be printable ASCII to be sent "
                "over the console. Use 'On-board device' for a network whose "
                "name or password has other characters.")
    if len(ssid) > WIFI_SSID_MAX_BYTES:
        return f"the SSID is longer than {WIFI_SSID_MAX_BYTES} characters."
    if password and not (WIFI_PASSWORD_MIN_CHARS <= len(password)
                         <= WIFI_PASSWORD_MAX_CHARS):
        return (f"the password must be {WIFI_PASSWORD_MIN_CHARS}-"
                f"{WIFI_PASSWORD_MAX_CHARS} characters, or empty for an open "
                "network.")
    return None


def _drift_label(drift_s: int | None) -> str:
    return "unreadable" if drift_s is None else f"{drift_s:+d} s"


class BoardSession:
    """One console session on one identified board. Use as a context manager.

    Opening it connects and reads the board's identity; the three actions each
    return an Outcome or raise SettingsError.
    """

    def __init__(self, port: str, log=print):
        self._log = log
        #: Whether an action rebooted the board: its USB port may have
        #: re-enumerated, and `port` is then where it came back.
        self.rebooted = False
        try:
            self._con = ambyte_serial.connect_after_boot(
                port, deadline_s=CONNECT_DEADLINE_S, log=log)
        except ConsoleError as exc:
            raise SettingsError(str(exc)) from exc
        self.port = getattr(self._con, "port", port)
        try:
            self.identity = self._read_identity()
        except ConsoleError as exc:
            self.close()
            raise SettingsError(str(exc)) from exc

    def close(self) -> None:
        try:
            self._con.close()
        except Exception:
            pass

    def __enter__(self) -> "BoardSession":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    # ── shared ───────────────────────────────────────────────────────────
    def _expand(self, raw: str) -> str:
        """A raw NVS name the way the firmware renders it at boot."""
        mac = self.identity.mac
        return expand_mac_token(raw, mac) if mac else raw

    def _read_identity(self) -> BoardIdentity:
        status = self._con.board_status()
        raw_name = self._con.cfg_get("device_name") or ""
        zone = self._con.cfg_get("timezone") or ""
        name = expand_mac_token(raw_name, status.mac) if status.mac else raw_name
        return BoardIdentity(mac=status.mac, name=name, timezone=zone,
                             wifi_connected=status.wifi_connected,
                             wifi_provisioned=status.wifi_provisioned)

    def _reboot_and_reconnect(self) -> None:
        """Reboot (cfg values are read at boot) and get the same board back."""
        self._log("Rebooting the board to apply the change...")
        self._con.reboot()
        self._con.close()
        self.rebooted = True
        time.sleep(REBOOT_SETTLE_S)
        try:
            self._con = ambyte_serial.connect_after_boot(
                self.port, deadline_s=CONSOLE_BOOT_DEADLINE_S, log=self._log)
        except ConsoleError as exc:
            raise SettingsError(
                f"the change was written, but the board did not come back "
                f"after its reboot: {exc}") from exc
        self.port = getattr(self._con, "port", self.port)
        # The S3 re-enumerates on reset and the reconnect scans every ambyte
        # port, so with two boards attached it can land on the other one.
        mac = self._con.status_mac()
        if self.identity.mac and mac and mac != self.identity.mac:
            raise SettingsError(
                f"after the reboot a different board answered (MAC {mac}, "
                f"expected {self.identity.mac}). The change was written to "
                "the first board but not verified; connect one board at a "
                "time and repeat.")

    # ── Wi-Fi ────────────────────────────────────────────────────────────
    def set_wifi(self, ssid: str, password: str) -> Outcome:
        problem = wifi_problem(ssid, password)
        if problem:
            raise SettingsError(problem)
        if self.identity.wifi_provisioned is False:
            # `wifi_join` does not set the provisioned flag that on-boarding
            # bakes into NVS, so on such a board it would only half-work.
            raise SettingsError(
                "this board has never been on-boarded (its Wi-Fi provisioning "
                "flag is off). Use 'On-board device' instead.")

        self._log(f"Sending Wi-Fi '{ssid}' to the board (it waits up to 10 s "
                  "for an IP)...")
        try:
            result = self._con.wifi_join(ssid, password)
        except UnsupportedConsoleCommand as exc:
            raise SettingsError(
                "this board's firmware is too old to change Wi-Fi over the "
                "console (no `wifi_join`, added in 1.0.6). Use 'On-board "
                "device' to update it.") from exc
        except ConsoleError as exc:
            raise SettingsError(str(exc)) from exc

        joined = Outcome(f"Wi-Fi: joined '{ssid}'",
                         "The board is connected and has an IP.")
        if result.connected:
            return joined
        if result.retrying:
            if self._wait_for_ip():
                return joined
            if result.code == WIFI_JOIN_AUTH_REJECTED:
                return Outcome(
                    "Wi-Fi: saved, password rejected",
                    f"The access point rejected the password for '{ssid}'. "
                    "Check it and press Send Wi-Fi again; the board keeps "
                    "retrying in the meantime.",
                    warning=True)
            return Outcome(
                "Wi-Fi: saved, not connected yet",
                f"The board stored '{ssid}' but could not reach it now (out "
                "of range, or the access point did not answer). It keeps "
                "retrying and will join once the network is reachable.",
                warning=True)
        if result.code == WIFI_JOIN_NOT_RETRYING:
            raise SettingsError(
                f"the board stored '{ssid}' but its connect attempt failed "
                "and nothing is retrying. Press Send Wi-Fi again, or reboot "
                "the board.")
        if result.code == WIFI_JOIN_DRIVER_UNRESOLVED:
            raise SettingsError(
                "the board's Wi-Fi driver is stuck on an earlier attempt. "
                "Reboot the board, then press Send Wi-Fi again.")
        raise SettingsError(
            f"the board did not accept the Wi-Fi settings ({result.code}). "
            "Check the SSID and password and send again; if it repeats, "
            "reboot the board.")

    def _wait_for_ip(self) -> bool:
        deadline = time.time() + WIFI_SETTLE_S
        announced = False
        while True:
            try:
                state = self._con.wifi_connected()
            except ConsoleError:
                return False     # a flaky read is not a network verdict
            if state:
                return True
            if state is None or time.time() >= deadline:
                return False
            if not announced:
                self._log(f"Not connected yet; watching for up to "
                          f"{WIFI_SETTLE_S:.0f} s more...")
                announced = True
            time.sleep(WIFI_POLL_S)

    # ── name ─────────────────────────────────────────────────────────────
    def rename(self, name: str) -> Outcome:
        clean = clean_device_name(name)
        if not clean:
            raise SettingsError(
                f"invalid device name '{name}' (max {MAX_NAME_LEN} chars, "
                "printable ASCII, no quotes or backslashes).")
        wanted = self._expand(clean)
        current = self.identity.name
        if wanted == current:
            return Outcome(f"Name: already '{current}'", "Nothing was changed.")

        self._log(f"Renaming '{current}' to '{clean}'...")
        try:
            self._con.cfg_set("device_name", clean)
            self._reboot_and_reconnect()
            got = self._expand(self._con.cfg_get("device_name") or "")
        except ConsoleError as exc:
            raise SettingsError(str(exc)) from exc
        if got != wanted:
            raise SettingsError(
                f"after the reboot the board reports the name '{got}', "
                f"expected '{wanted}'.")
        return Outcome(f"Name: '{wanted}'",
                       f"Was '{current}'. Read back after the reboot.")

    # ── clock ────────────────────────────────────────────────────────────
    def set_clock(self, timezone: str | None = None) -> Outcome:
        """Set the RTC to this PC's UTC time; with `timezone`, also store that
        IANA zone (which costs a reboot when it differs)."""
        if timezone is not None and not timezones.firmware_supports(timezone):
            raise SettingsError(
                f"timezone '{timezone}' is not in the firmware's zone table "
                f"(IANA tzdata {timezones.FIRMWARE_TZDATA_VERSION}). Set this "
                "PC to a standard IANA zone, or leave the timezone unticked.")
        try:
            before = self._clock_drift()
            zone_note = ""
            if timezone is not None:
                zone_note = self._set_timezone(timezone)
            now = int(time.time())
            self._log(f"Setting the RTC to UTC epoch {now}...")
            self._con.rtc_set(now)
            after = self._con.rtc_read() - int(time.time())
        except ConsoleError as exc:
            raise SettingsError(str(exc)) from exc
        if abs(after) > RTC_TOLERANCE_S:
            raise SettingsError(
                f"the clock was set but reads {after:+d} s against this PC "
                f"(tolerance ±{RTC_TOLERANCE_S} s).")
        detail = f"Was {_drift_label(before)} against this PC."
        if zone_note:
            detail += f" {zone_note}"
        return Outcome(f"Clock: set, {after:+d} s vs this PC", detail)

    def _clock_drift(self) -> int | None:
        """RTC minus host, or None: a factory-fresh RTC refuses every read
        until it is set once (oscillator-stop flag)."""
        try:
            return self._con.rtc_read() - int(time.time())
        except ConsoleError:
            return None

    def _set_timezone(self, zone: str) -> str:
        current = self._con.cfg_get("timezone") or ""
        if current == zone:
            return f"Timezone already {zone}."
        self._log(f"Changing the timezone from '{current or '(unset)'}' to "
                  f"'{zone}'...")
        try:
            self._con.cfg_set("timezone", zone)
        except ConsoleError as exc:
            # The device validates against its own compiled zone table, which
            # on older firmware is smaller than the one this GUI ships.
            raise SettingsError(
                f"the board rejected the timezone '{zone}'; its firmware may "
                f"predate that zone. The clock was not changed. ({exc})"
            ) from exc
        self._reboot_and_reconnect()
        got = self._con.cfg_get("timezone") or ""
        if got != zone:
            raise SettingsError(
                f"after the reboot the board reports the timezone '{got}', "
                f"expected '{zone}'. The clock was not changed.")
        return (f"Timezone '{current or '(unset)'}' changed to '{zone}', "
                "read back after the reboot.")
