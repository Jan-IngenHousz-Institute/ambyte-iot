# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only
"""Local USB Wi-Fi maintenance, independent of openJII and flashing.

Discovery and writes stay on the operator's selected port. Each write checks
the previously displayed MAC again on the same descriptor that receives the
credentials. Save uses the persistent wifi_join on all supported firmware;
Test & save explicitly selects the journaled service trial on newer firmware.
These are separate operator actions, never an automatic fallback after an error.
No console reply containing an echoed password leaves this module.
"""
from contextlib import contextmanager
from dataclasses import dataclass
import re
import time

from . import ambyte_serial, config
from .service_client import OperationJournal, ServiceClient, ServiceClientError, UnknownOutcome
from .service_wire import ServiceError, TransportError


class WifiSettingsError(RuntimeError):
    def __init__(self, message, *, uncertain=False):
        super().__init__(message)
        self.uncertain = uncertain


@dataclass(frozen=True)
class DeviceInfo:
    port: str
    mac: str
    name: str
    firmware: str | None
    service: bool
    connected: bool | None


@dataclass(frozen=True)
class WifiResult:
    ssid: str
    connected: bool


@dataclass(frozen=True)
class SavedSchedule:
    op: str
    store: str
    version: str
    state: str
    message: str
    action: str | None = None


def validate_credentials(ssid: str, password: str) -> None:
    try:
        ssid_bytes, password_bytes = ssid.encode("utf-8"), password.encode("ascii")
    except (AttributeError, UnicodeError):
        raise WifiSettingsError("Use a UTF-8 SSID and an ASCII Wi-Fi password.") from None
    # Both maintained firmware paths copy into a NUL-terminated 32-byte SSID
    # field. Reject 32 bytes instead of silently joining a truncated network.
    if not 1 <= len(ssid_bytes) <= 31 or any(ord(c) < 32 or ord(c) == 127 for c in ssid):
        raise WifiSettingsError("SSID must be 1–31 UTF-8 bytes, without control characters.")
    if (len(password_bytes) > 63 or any(c < 32 or c > 126 for c in password_bytes)
            or 0 < len(password_bytes) < 8):
        raise WifiSettingsError("Password must be 8–63 printable ASCII characters, or empty for an open network.")


@contextmanager
def _selected_console(port):
    # Do not use connect_after_boot's fleet scan here: two plugged-in devices
    # must never make the selected port silently mean a different gateway.
    with ambyte_serial.AmbyteConsole(port) as console:
        if not console.wait_prompt(ambyte_serial.PROBE_READY_TIMEOUT_S, recover_service=True):
            raise WifiSettingsError("No Ambyte console answered on the selected port. Refresh ports and read the device again.")
        yield console


def _read_info(console):
    try:
        identity = console.identity()
        mac, firmware = identity.mac, identity.fw
        service = "svc" in identity.capabilities
        default_name = identity.thing_name
    except ambyte_serial.UnsupportedConsoleCommand:
        mac, firmware, service, default_name = console.status_mac(), None, False, None
    if not mac:
        raise WifiSettingsError("The selected device did not provide an Ambyte MAC; no settings were changed.")
    try:
        name = console.cfg_get("device_name")
    except ambyte_serial.ConsoleError:
        name = None
    name = ambyte_serial.expand_mac_token(name or default_name or "AMBYTE_{MAC}", mac)
    return DeviceInfo(console.port, mac, name, firmware, service, console.wifi_connected())


def read_device(port: str) -> DeviceInfo:
    with _selected_console(port) as console:
        return _read_info(console)


def _checked_info(console, expected):
    current = _read_info(console)
    if current.mac != expected.mac:
        raise WifiSettingsError("Connected Ambyte changed. Read the device again and check its name and MAC before updating.")
    if current.service != expected.service:
        raise WifiSettingsError("The Ambyte's firmware capabilities changed. Read the device again before updating.")
    return current


_WIFI_FAILURES = {
    "busy": "The Ambyte is busy with another maintenance task. Wait for it to finish, then retry.",
    "wifi_auth_rejected": "The hub rejected the credentials. Check the password and hub settings.",
    "wifi_timeout": "The Ambyte could not get an IP address. Keep the destination hub in range and check its settings.",
    "wifi_failed": "The Wi-Fi connection failed. Check the destination hub and password.",
    "wifi_not_retrying": "Wi-Fi stopped retrying. Check the destination hub and try again.",
    "wifi_driver_unresolved": "The Wi-Fi driver could not finish an earlier attempt. Reboot the Ambyte before retrying.",
    "wifi_invalid_credentials": "The firmware rejected the Wi-Fi credential format.",
    "wifi_unavailable": "Wi-Fi is unavailable on this Ambyte right now.",
}


def _service_error(error, journal, *, recovering=False):
    message = _WIFI_FAILURES.get(error.reason, str(error)) if isinstance(error, ServiceError) else str(error)
    result = error.result if isinstance(error, ServiceError) else None
    if result and result["kind"] == "wifi":
        if result["saved"] is False:
            message += " New credentials were not saved."
        elif result["saved"] is True:
            message += " New credentials were saved."
        if result["restored"]:
            message += " Previous network restored."
            if result["restoration_connected"]:
                message += " The previous network is connected."
        elif not (result["saved"] is False and error.reason == "busy"):
            message += " Previous network restoration was not confirmed."
        if result["retrying"]:
            message += " Wi-Fi is still retrying."
    entry = journal.load()
    unresolved_wifi = bool(entry and entry["descriptor"]["kind"] == "wifi" and entry["phase"] != "complete")
    if unresolved_wifi:
        message += (" Use Start new Wi-Fi attempt after checking the device state."
                    if recovering and isinstance(error, UnknownOutcome)
                    else " Use Check saved result before another attempt.")
    return WifiSettingsError(message, uncertain=unresolved_wifi and isinstance(error, (UnknownOutcome, TransportError)))


def _service(console, info, action, *, recovering=False):
    journal = OperationJournal(config.SERVICE_OPERATIONS_DIR, info.mac)
    try:
        # Share the client's reentrant lease so the guard and the operation
        # cannot race another GUI. Explicit Wi-Fi retries must never archive
        # an unresolved schedule's only discoverable recovery record.
        with journal.lease():
            previous = journal.load()
            if previous and previous["phase"] != "complete" and previous["descriptor"]["kind"] != "wifi":
                raise WifiSettingsError("Resolve the saved schedule operation before updating Wi-Fi. Use Check saved result here or the schedule controls.")
            if recovering and (not previous or previous["descriptor"]["kind"] != "wifi"):
                raise WifiSettingsError("No saved Wi-Fi update is available for this Ambyte.")
            client = ServiceClient(console._ser, journal)
            try:
                result = action(client)
            except ServiceClientError as exc:
                raise _service_error(exc, journal, recovering=recovering) from None
    except ServiceClientError as exc:
        raise WifiSettingsError(str(exc)) from None
    return WifiResult(result["ssid"], result["connected"] is True)


def _quote(value):
    # ESP-IDF's esp_console_split_argv recognizes escaped quotes/backslashes,
    # and accepts an empty quoted argument for an open-network password.
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _legacy_join(console, ssid, password, *, require_connection=True):
    try:
        reply = console.command(f"wifi_join {_quote(ssid)} {_quote(password)}", timeout=45.0)
    except Exception:
        # AmbyteConsole.command's timeout includes the entire command. Do not
        # propagate it, its cause, or any part of the echoed serial capture.
        raise WifiSettingsError(
            "Wi-Fi update outcome is unknown after a USB error. Read the device again before another attempt.",
            uncertain=True) from None
    if any(line.strip() == "Unrecognized command" for line in reply.splitlines()):
        raise WifiSettingsError("This firmware does not support local wifi_join; no Wi-Fi update was confirmed.")
    pattern = re.compile(r'wifi_join "' + re.escape(ssid) + r'": ([A-Z][A-Z0-9_]+)')
    match = None
    # Physical ESP-IDF VFS emits CRCRLF for printf's CRLF. splitlines also
    # keeps input echoes separate from the handler result. Do not scan the
    # echoed password for FAILURE_MARK: a password can contain that text.
    for line in reply.splitlines():
        match = pattern.fullmatch(line.strip())
        if match:
            break
    if match is None:
        raise WifiSettingsError("Wi-Fi update outcome is unknown: the firmware returned no recognized result. Read the device again.", uncertain=True)
    # wifi_join writes esp_wifi's persistent config BEFORE waiting for an IP.
    # These two results are the documented post-save connection outcomes. Other
    # errors may come from set_config itself, so never label those as saved.
    if not require_connection and match[1] in {"ESP_ERR_TIMEOUT", "WIFI_MANAGER_ERR_AUTH_REJECTED"}:
        return WifiResult(ssid, False)
    if match[1] != "ESP_OK":
        if not require_connection:
            if match[1] == "ESP_ERR_WIFI_STATE":
                raise WifiSettingsError(
                    "Wi-Fi save could not be confirmed (ESP_ERR_WIFI_STATE). The Ambyte was busy trying to connect. "
                    "Wait a few seconds and explicitly Save Wi-Fi again; resending the same credentials is safe.",
                    uncertain=True)
            raise WifiSettingsError(
                f"Wi-Fi save could not be confirmed ({match[1]}). The credentials may have changed. "
                "Read the device again to check its identity, then explicitly Save Wi-Fi again with the same credentials if needed.",
                uncertain=True)
        raise WifiSettingsError(
            f"Wi-Fi connection failed ({match[1]}). Older firmware may have saved the new credentials and may keep retrying; check the hub and password.")
    # wifi_manager_connect returns ESP_OK only after getting an IP. Do not
    # mistake the unrelated 'connected' bit from before the command for proof.
    return WifiResult(ssid, True)



def _wait_service_startup(console, expected):
    # Opening native USB can reboot the board. Its console precedes the boot
    # AMBIT firmware sync, which holds the maintenance lock. Wait read-only on
    # this descriptor before sending credentials, never retry a rejected join.
    deadline = time.monotonic() + 180.0
    while True:
        identity = console.identity()
        if identity.mac != expected.mac or "svc" not in identity.capabilities:
            raise WifiSettingsError("Connected Ambyte changed during startup. Read the device again.")
        if identity.schedule.source not in {None, "NONE"}:
            return
        if time.monotonic() >= deadline:
            raise WifiSettingsError("Ambyte startup has not finished. No Wi-Fi credentials were sent. Wait and read the device again.")
        time.sleep(1.0)


def save_wifi(expected: DeviceInfo, ssid: str, password: str) -> WifiResult:
    """Persist deployment credentials even when the destination hub is absent.

    This explicitly uses wifi_join rather than the RAM-only service trial. A
    service-capable device is checked read-only first, so the local action does
    not bypass a saved or active schedule/Wi-Fi transaction. No password is sent
    until all checks finish, and a lost reply is never retried automatically.
    """
    validate_credentials(ssid, password)
    with _selected_console(expected.port) as console:
        info = _checked_info(console, expected)
        journal = OperationJournal(config.SERVICE_OPERATIONS_DIR, info.mac)
        try:
            with journal.lease():
                previous = journal.load()
                if previous and previous["phase"] != "complete":
                    raise WifiSettingsError("Resolve the saved operation with Check saved result before saving new Wi-Fi credentials.")
                if info.service:
                    _wait_service_startup(console, info)
                    client = ServiceClient(console._ser, journal)
                    try:
                        client.connect()
                        client._foreign()
                    finally:
                        client.exit()
                    if not console.wait_prompt(ambyte_serial.PROBE_READY_TIMEOUT_S):
                        raise WifiSettingsError("The Ambyte did not return to its console. No Wi-Fi credentials were sent.")
                    _checked_info(console, info)
                return _legacy_join(console, ssid, password, require_connection=False)
        except ServiceClientError as exc:
            raise WifiSettingsError(f"No Wi-Fi credentials were sent. {exc}") from None


def update_wifi(expected: DeviceInfo, ssid: str, password: str, *, new_request=False) -> WifiResult:
    validate_credentials(ssid, password)
    with _selected_console(expected.port) as console:
        info = _checked_info(console, expected)
        if info.service:
            _wait_service_startup(console, info)
            return _service(console, info, lambda client: client.join_wifi(ssid, password, new_request=new_request))
        return _legacy_join(console, ssid, password)


def recover_wifi(expected: DeviceInfo) -> WifiResult:
    """Read an earlier service result by ID; never retransmit credentials."""
    with _selected_console(expected.port) as console:
        info = _checked_info(console, expected)
        if not info.service:
            raise WifiSettingsError("Saved-result recovery is unavailable on this older firmware. Read the current device state instead.")
        return _service(console, info, lambda client: client.recover_wifi(), recovering=True)


def _schedule_result(client, entry):
    """Reconcile device evidence without installing or running a schedule."""
    try:
        result = client._read_intent(entry)
    except UnknownOutcome:
        result = None
    state = result["state"] if result else "unknown"
    action = None
    if state in {"succeeded", "failed", "aborted"}:
        try:
            client._finish(entry, result)
        except ServiceError:
            pass  # The validated terminal failure has been marked complete.
        message = (f"Schedule {entry['op']} recorded {state}. Its recovery record is resolved. "
                   "The active workbook was not rechecked; Wi-Fi maintenance can continue.")
    elif state == "staging":
        action = "cancel"
        message = f"Schedule {entry['op']} is staged and has not committed. You can explicitly cancel this saved transfer."
    elif state in {"committing", "persisting", "connecting"}:
        message = f"Schedule {entry['op']} is still {state}. Wait and check the saved result again."
    else:
        client._foreign()  # Never offer archive while any device operation is live.
        action = "archive"
        message = (f"Schedule {entry['op']} has an {state} result. Check the device state, then explicitly "
                   "archive this recovery record if you need to continue. This record will replace the single previous archive on disk.")
    return SavedSchedule(entry["op"], entry["store"], entry["descriptor"]["version"], state, message, action)


@contextmanager
def _saved_schedule_client(expected):
    with _selected_console(expected.port) as console:
        info = _checked_info(console, expected)
        if not info.service:
            raise WifiSettingsError("Saved schedule recovery requires service-capable firmware.")
        journal = OperationJournal(config.SERVICE_OPERATIONS_DIR, info.mac)
        with journal.lease():
            entry = journal.load()
            if not entry or entry["descriptor"]["kind"] != "schedule":
                raise WifiSettingsError("The saved operation changed. Check its result again.")
            client = ServiceClient(console._ser, journal)
            try:
                # A changed store must be observable for explicit host-side
                # archival. This only binds a read-only session, not a new op.
                client.connect(new_request=True)
                yield client, journal, entry
            except ServiceClientError as exc:
                raise WifiSettingsError(str(exc)) from None
            finally:
                try:
                    client.exit()
                except ServiceClientError:
                    pass


def check_saved_result(expected):
    """Also usable after app restart: no in-memory onboarding run is needed."""
    entry = OperationJournal(config.SERVICE_OPERATIONS_DIR, expected.mac).load()
    if entry and entry["descriptor"]["kind"] == "schedule":
        if entry["phase"] == "complete":
            return SavedSchedule(entry["op"], entry["store"], entry["descriptor"]["version"], "complete",
                                 "This schedule recovery record is already complete. Wi-Fi maintenance can continue; the active workbook was not rechecked.")
        with _saved_schedule_client(expected) as (client, _, saved):
            return _schedule_result(client, saved)
    return recover_wifi(expected)


def resolve_saved_schedule(expected, checked: SavedSchedule):
    """Explicit cancel/archive, bound to the exact saved record just shown."""
    with _saved_schedule_client(expected) as (client, journal, entry):
        if (entry["op"], entry["store"]) != (checked.op, checked.store):
            raise WifiSettingsError("The saved schedule changed. Check its result again before acting.")
        current = _schedule_result(client, entry)
        if current.action is None:
            return current
        if current.action != checked.action:
            raise WifiSettingsError("The schedule state changed. Check its result again before acting.")
        if current.action == "cancel":
            client.abort(entry)
            return SavedSchedule(entry["op"], entry["store"], current.version, "aborted",
                                 "The staged schedule transfer was cancelled. The active workbook was left unchanged; Wi-Fi maintenance can continue.")
        journal.preserve()
        return SavedSchedule(entry["op"], entry["store"], current.version, "archived",
                             "The unresolved schedule recovery record was saved as the previous archive, replacing any earlier archive. Wi-Fi maintenance can continue.")
