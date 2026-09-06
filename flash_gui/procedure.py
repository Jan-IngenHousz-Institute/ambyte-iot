# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""The per-device onboarding procedure: flash + provision + verify.

Step order is chosen so that every failure leaves the board in a recoverable,
clearly-reported state:

  1. check        pre-flash serial probe (2 s, one retry) → stored name;
                  fallback: MAC via esptool (works on unflashed chips)
     [GUI prompts for the device name here]
  2. credentials  openJII register + issue/rotate + onboard, BEFORE flashing,
                  so an API failure means nothing was written to the board
  3. nvs          bake the per-board NVS image (identity, certs, Wi-Fi,
                  timezone, MQTT, flash_time, schedule provenance) and the
                  littlefs image carrying the selected schedule as schedule.yaml
  4. flash        release images + nvs.bin + littlefs.bin in one esptool
                  session; a mid-way failure leaves the chip in the ROM
                  bootloader = re-flashable
  5. rtc          wait for the freshly booted console, set the exact UTC epoch
  6. schedule          push the session's schedule down this console (no device
                  network needed) and let the firmware verify + swap it; a
                  catalog asset falls back to the firmware's URL updater on
                  older firmware (a workbook-stamped schedule has no URL).
                  Previous script kept as .bak either way
  7. verify       read back name / timezone / RTC / schedule identity (+
                  workbook id when installed from a workbook); per-item
                  pass/fail

Retry paths (no full restart needed):
  * retry_flash()      re-runs steps 4-6 with the already-built images
  * retry_provision()  re-runs 5-7 only; first repairs a wrong name/timezone
                       over the console (`cfg set` + reboot), never re-flashes
"""

from __future__ import annotations

import hashlib
import time
import urllib.parse
from dataclasses import dataclass, field
from pathlib import Path

from . import ambyte_serial, esptool_ops, schedule_stamp, timezones
from .ambyte_serial import (ConsoleError, UnsupportedConsoleCommand,
                            expand_mac_token)
from .config import (CACHE_DIR, DEVICE_FIRMWARE, DEVICE_ID, DEVICE_VERSION,
                     LITTLEFS_OFFSET, MAX_NAME_LEN, NVS_OFFSET, PROTOCOL_ID,
                     OPENJII_DEVICE_TYPE, TOPIC_SENSOR_VERSION, Environment,
                     default_command_topic, default_status_topic)
from .littlefs_image import build_schedule_image
from .nvs_builder import ProvisioningPlan, build_nvs_image
from .openjii_client import DeviceIdentity, OpenJIIClient, WorkbookProgramming
from . import release_fetch
from .release_fetch import (ScheduleScriptRelease, ReleaseError,
                            ReleaseImages)

# Accept the RTC as correct within this window of host-now. Generous: it
# covers console latency and a slow verify loop, but still catches a
# seconds-vs-milliseconds or timezone-applied-twice class of error.
RTC_TOLERANCE_S = 120

# A normal post-reboot console appears in 20-35 s, but SD recovery and USB
# re-enumeration can push field devices past 90 s. Keep rescanning for three
# minutes before declaring the board unavailable.
CONSOLE_BOOT_DEADLINE_S = 180.0
# URL installs temporarily stop Schedule + MQTT, download over HTTPS, verify, swap,
# reconnect MQTT, and restart Schedule. Normal runs finish in seconds; tolerate a
# degraded Wi-Fi reconnect without turning a safe in-progress update into a
# false failure.
SCHEDULE_INSTALL_DEADLINE_S = 360.0
# The URL install is asynchronous. Give the worker time to download, hash,
# syntax-check, swap, and restart before asking the SD-backed identity reader.
# Querying immediately can block the CLI behind the same SD operation.
SCHEDULE_INSTALL_SETTLE_S = 15.0
# Nothing was printed between "install queued" and the verdict, so a healthy
# 6-minute wait for a slow download looked identical to a hung flasher, and the
# operator cannot attach a serial monitor to check because the GUI holds the
# port. Report what the device is reporting instead.
SCHEDULE_INSTALL_PROGRESS_S = 15.0
# The device fetches the script itself over HTTPS, so no Wi-Fi means no install.
# Association is not instant after the reboot in step 5, so wait briefly rather
# than failing on the first read; but wait here, where the cause is obvious,
# instead of burning the full SCHEDULE_INSTALL_DEADLINE_S on a board that was never
# going to download anything.
SCHEDULE_WIFI_DEADLINE_S = 60.0


class ProcedureError(RuntimeError):
    """A step failed; .step tells the GUI which retry to offer."""

    def __init__(self, step: str, message: str):
        super().__init__(message)
        self.step = step


def mqtt_uri_from_endpoint(endpoint: str) -> str:
    """Turn openJII's ATS broker host into the URI ESP-MQTT requires."""
    value = (endpoint or "").strip().rstrip("/")
    if "://" not in value:
        value = f"mqtts://{value}"
    try:
        parsed = urllib.parse.urlsplit(value)
        port = parsed.port or 8883
    except ValueError as exc:
        raise ValueError(f"invalid MQTT endpoint '{endpoint}'") from exc
    if (parsed.scheme != "mqtts" or not parsed.hostname or
            parsed.username is not None or parsed.password is not None or
            parsed.path not in ("", "/") or parsed.query or parsed.fragment):
        raise ValueError(f"unsupported MQTT endpoint '{endpoint}'")
    host = parsed.hostname
    if ":" in host:  # Preserve a valid URI when an IPv6 endpoint is ever used.
        host = f"[{host}]"
    return f"mqtts://{host}:{port}"


def clean_device_name(name: str) -> str | None:
    """Validated device name, or None if empty/invalid.

    Printable ASCII (space allowed) up to 63 bytes, minus quote/backslash
    (they complicate the console command line used for verification/repair).
    """
    n = (name or "").strip()
    if not n:
        return None
    if len(n.encode("utf-8")) > MAX_NAME_LEN:
        return None
    for ch in n:
        if ch < " " or ord(ch) > 0x7E or ch in '"\\':
            return None
    return n


@dataclass
class PreflightInfo:
    """What step 1 learned; the GUI builds the name prompt from this."""

    port: str
    mac: str
    had_console: bool
    stored_name: str | None            # expanded; None if default/unnamed

    @property
    def proposed_name(self) -> str:
        if self.stored_name:
            return self.stored_name
        return f"AMBYTE_{self.mac}"


@dataclass
class VerifyItem:
    label: str
    passed: bool
    detail: str


@dataclass
class SessionContext:
    """Session-level inputs, held constant across boards."""

    env: Environment
    client: OpenJIIClient
    release: ReleaseImages
    schedule_script: ScheduleScriptRelease
    experiment_id: str
    timezone: str
    wifi_ssid: str
    wifi_password: str
    # The selected experiment's workbook programming cell, resolved by the GUI
    # at experiment-selection time. When set, steps 3/6 install the stamped
    # workbook schedule and the catalog `schedule_script` is only the explicit
    # operator override/fallback.
    programming: WorkbookProgramming | None = None


@dataclass(frozen=True)
class ScheduleSource:
    """One resolved schedule to install: workbook programming (preferred) or
    the released catalog asset (fallback/explicit override).

    Identity rules (the sha256 threads through NVS provenance, the serial
    push, and step-7 verification):
      * workbook: sha256 of the STAMPED bytes; script_version "wb-v<n>";
        campaign_id "workbook-<workbookVersionId>"; built_against_fw is the
        firmware release being flashed (the workbook pins no firmware).
      * catalog: the manifest identity, exactly as before.
    `blob` is set for a workbook source (the stamped bytes); a catalog source
    leaves it None so the bytes keep coming from release_fetch's digest-checked
    cache.
    """

    origin: str                      # "workbook" | "catalog"
    label: str                       # human label for logs
    blob: bytes | None
    sha256: str
    script_version: str
    campaign_id: str
    built_against_fw: str
    asset_url: str | None            # catalog only; workbook has no URL
    workbook_version_id: str | None  # workbook only


def schedule_source(ctx: SessionContext, log=print) -> ScheduleSource:
    """Resolve which schedule this session installs, stamping if from a workbook.

    Stamping happens here — once per call, deterministically — so the NVS
    provenance (step 3), the littlefs image, the serial push (step 6) and the
    step-7 verify all agree on the same sha256.
    """
    # Tests duck-type the context (SimpleNamespace / object.__new__), so the
    # pre-workbook call sites legitimately lack the attribute.
    programming = getattr(ctx, "programming", None)
    script = ctx.schedule_script
    if programming is None:
        return ScheduleSource(
            origin="catalog",
            label=script.asset_name,
            blob=None,
            sha256=script.sha256,
            script_version=script.script_version,
            campaign_id=script.campaign_id,
            built_against_fw=script.built_against_fw,
            asset_url=getattr(script, "asset_url", None),
            workbook_version_id=None,
        )

    fw_version = getattr(ctx.release, "version", "") or ""
    if schedule_stamp.firmware_supports_macros(fw_version):
        macros = programming.macros
        log(f"Workbook programming: stamping workbookVersionId="
            f"{programming.workbook_version_id} and {len(macros)} macro(s) "
            f"(firmware {fw_version} ≥ "
            f"{schedule_stamp.MACROS_HEADER_MIN_FW} knows the macros header).")
    else:
        # Pre-stream-A firmware rejects the macros: key at boot and silently
        # falls back to the embedded default; stamping only the version id
        # keeps the schedule loadable there.
        macros = ()
        log(f"Workbook programming: stamping workbookVersionId="
            f"{programming.workbook_version_id} WITHOUT the macro list — "
            f"firmware {fw_version or 'unknown'} predates "
            f"{schedule_stamp.MACROS_HEADER_MIN_FW}, which introduced the "
            "macros header. Rows will carry the workbook id but no macros.")
    stamped = schedule_stamp.stamp_header(
        programming.yaml_text, programming.workbook_version_id, macros,
        log=log)
    blob = stamped.encode("utf-8")
    version_id = programming.workbook_version_id.strip()
    number = programming.workbook_version_number
    script_version = (f"wb-v{number}" if isinstance(number, int)
                      else f"wb-{version_id[:8]}")
    return ScheduleSource(
        origin="workbook",
        label=f"workbook v{number}" if isinstance(number, int)
              else f"workbook {version_id[:8]}",
        blob=blob,
        sha256=hashlib.sha256(blob).hexdigest(),
        script_version=script_version,
        campaign_id=f"workbook-{version_id}",
        built_against_fw=fw_version or "unknown",
        asset_url=None,
        workbook_version_id=version_id,
    )


@dataclass
class DeviceRun:
    """Mutable state of one board's procedure, kept for the retry paths."""

    port: str
    preflight: PreflightInfo
    name: str = ""
    identity: DeviceIdentity | None = None
    plan: ProvisioningPlan | None = None
    nvs_path: Path | None = None
    schedule_image_path: Path | None = None
    verify_results: list[VerifyItem] = field(default_factory=list)


# ── step 1: pre-flash check ──────────────────────────────────────────────────
def preflight(port: str, log=print) -> PreflightInfo:
    log("Checking for a live ambyte console (2 s probe)...")
    probe = ambyte_serial.probe_device(port, timeout=2.0, retries=1)

    mac = probe.mac
    if probe.is_ambyte and mac:
        log(f"Found running ambyte firmware, MAC {mac}.")
    else:
        if probe.is_ambyte:
            log("Console answered but gave no MAC, reading it via esptool.")
        else:
            log("No console answer, treating as unflashed; reading the MAC "
                "via esptool (this resets the board).")
        try:
            mac = esptool_ops.read_mac(port, log=None)
        except esptool_ops.EsptoolError as exc:
            raise ProcedureError("check", str(exc)) from exc
        log(f"MAC (esptool): {mac}")

    stored = None
    if probe.device_name:
        expanded = expand_mac_token(probe.device_name, mac)
        # The fleet default AMBYTE_{MAC}/AMBYTE_<mac> is "unnamed" for the
        # purpose of the rename prompt; the proposal falls back to it anyway.
        if expanded != f"AMBYTE_{mac.upper()}":
            stored = expanded
        log(f"Stored device name: {expanded}")

    return PreflightInfo(port=port, mac=mac, had_console=probe.is_ambyte,
                         stored_name=stored)


# ── steps 2-3: credentials + NVS image ───────────────────────────────────────
def prepare_provisioning(ctx: SessionContext, run: DeviceRun, log=print) -> None:
    name = clean_device_name(run.name)
    if not name:
        raise ProcedureError(
            "credentials",
            f"invalid device name '{run.name}' (max {MAX_NAME_LEN} chars, "
            "printable ASCII, no quotes or backslashes).")
    run.name = name

    # Fail before any openJII registration or flash: the firmware's `cfg set
    # timezone` validates against its compiled zone table and returns
    # ESP_ERR_INVALID_ARG for an unknown name, so an unsupported zone can only
    # end as a "Timezone: FAIL" no retry path can repair (America/La_Paz on the
    # Europe-only table, 2026-08).
    if not timezones.firmware_supports(ctx.timezone):
        raise ProcedureError(
            "credentials",
            f"timezone '{ctx.timezone}' is not in the firmware's zone table "
            f"(IANA tzdata {timezones.FIRMWARE_TZDATA_VERSION}) — the device "
            "would reject it and fail verification. Set this PC to a standard "
            "IANA zone, or regenerate the table with tools/gen_tz_table.py and "
            "release firmware built from it.")

    log("Requesting device identity, certificate, and onboarding config from openJII...")
    try:
        identity = ctx.client.provision_device(run.preflight.mac, name, log=log)
        onboarding = ctx.client.onboard_device(identity.device_id,
                                               ctx.experiment_id)
    except Exception as exc:
        raise ProcedureError("credentials", str(exc)) from exc
    run.identity = identity
    log(f"Thing name: {identity.thing_name} (this becomes the MQTT client id).")

    if onboarding.thing_name != identity.thing_name:
        raise ProcedureError(
            "credentials",
            "openJII onboarding returned a different Thing name "
            f"('{onboarding.thing_name}') than credential issuance "
            f"('{identity.thing_name}').")
    if onboarding.device_type != OPENJII_DEVICE_TYPE:
        raise ProcedureError(
            "credentials",
            "openJII onboarding returned device type "
            f"'{onboarding.device_type}', expected '{OPENJII_DEVICE_TYPE}'.")

    try:
        endpoint = mqtt_uri_from_endpoint(onboarding.endpoint)
    except ValueError as exc:
        raise ProcedureError(
            "credentials",
            str(exc)) from exc

    # openJII owns the prefix through sensorType. Its onboarding contract says
    # the device appends /{sensorVersion}/{sensorId}; the firmware appends the
    # final protocol segment when publishing.
    topic_root = (f"{onboarding.topic_prefix}/{TOPIC_SENSOR_VERSION}/"
                  f"{onboarding.thing_name}")
    log(f"openJII MQTT endpoint: {endpoint}")
    log(f"openJII ingest topic root: {topic_root}")

    source = schedule_source(ctx, log=log)
    plan = ProvisioningPlan(
        device_name=name,
        timezone=ctx.timezone,
        mqtt_uri=endpoint,
        client_id=identity.thing_name,
        topic_root=topic_root,
        command_topic=default_command_topic(identity.thing_name),
        status_topic=default_status_topic(topic_root),
        device_cert_pem=identity.certificate_pem,
        device_key_pem=identity.private_key_pem,
        wifi_ssid=ctx.wifi_ssid,
        wifi_password=ctx.wifi_password,
        device_id=DEVICE_ID,
        protocol_id=PROTOCOL_ID,
        device_version=DEVICE_VERSION,
        device_firmware=DEVICE_FIRMWARE,
        firmware_version=ctx.release.version,
        schedule_sha256=source.sha256,
        schedule_script_version=source.script_version,
        schedule_built_against_fw=source.built_against_fw,
        schedule_campaign_id=source.campaign_id,
    )
    run.plan = plan

    log("Baking the per-board NVS image...")
    out = CACHE_DIR / "nvs" / f"nvs-{run.preflight.mac.replace(':', '')}.bin"
    try:
        run.nvs_path = build_nvs_image(plan, out)
    except Exception as exc:
        raise ProcedureError("nvs", str(exc)) from exc
    log(f"NVS image ready: {out.name}")

    # Bake schedule.yaml into a littlefs image for the internal script partition:
    # first boot then finds the selected schedule already installed (and the NVS
    # provenance above makes `schedule release` verify it), so the Schedule step needs no
    # SD card and no download. Older flashed firmware simply ignores both.
    log(f"Baking {source.label} into the littlefs image...")
    schedule_out = CACHE_DIR / "littlefs" / f"littlefs-{run.preflight.mac.replace(':', '')}.bin"
    try:
        # Surface a cold-cache download in the same operator log as the rest of
        # provisioning. script_bytes still supports log=None for silent callers,
        # but there is no reason to hide useful progress in the GUI.
        blob = (source.blob if source.blob is not None
                else release_fetch.script_bytes(ctx.schedule_script, log=log))
        run.schedule_image_path = build_schedule_image(blob, schedule_out)
    except Exception as exc:
        raise ProcedureError("nvs", f"littlefs image: {exc}") from exc
    log(f"littlefs image ready: {schedule_out.name}")


# ── step 4: flash ────────────────────────────────────────────────────────────
def flash(ctx: SessionContext, run: DeviceRun, log=print) -> None:
    assert run.nvs_path, "prepare_provisioning must run first"
    images = list(ctx.release.flash_files) + [(NVS_OFFSET, run.nvs_path)]
    if run.schedule_image_path is not None:
        images.append((LITTLEFS_OFFSET, run.schedule_image_path))
    images.sort(key=lambda item: item[0])
    log(f"Flashing {ctx.release.tag} + provisioning "
        f"({len(images)} images) on {run.port}...")
    for off, path in images:
        log(f"  0x{off:06x}  {Path(path).name}")
    try:
        esptool_ops.flash_images(run.port, images,
                                 ctx.release.flash_settings, log=log)
    except esptool_ops.EsptoolError as exc:
        raise ProcedureError("flash", str(exc)) from exc
    log("Flash complete; board rebooting into the new firmware.")


# ── steps 5-6: RTC + verification ────────────────────────────────────────────
def provision_and_verify(ctx: SessionContext, run: DeviceRun,
                         log=print) -> list[VerifyItem]:
    """Set the exact RTC and verify name/timezone/RTC over the console."""
    assert run.plan, "prepare_provisioning must run first"
    plan = run.plan
    try:
        con = ambyte_serial.connect_after_boot(
            run.port, deadline_s=CONSOLE_BOOT_DEADLINE_S, log=log)
    except ConsoleError as exc:
        raise ProcedureError("provision", str(exc)) from exc

    try:
        now = int(time.time())
        log(f"Setting RTC to UTC epoch {now}...")
        con.rtc_set(now)

        results: list[VerifyItem] = []

        raw_name = con.cfg_get("device_name")
        got_name = expand_mac_token(raw_name or "", run.preflight.mac)
        results.append(VerifyItem(
            "Device name", got_name == plan.device_name,
            f"device reports '{got_name}', expected '{plan.device_name}'"))

        raw_tz = con.cfg_get("timezone") or ""
        results.append(VerifyItem(
            "Timezone", raw_tz == plan.timezone,
            f"device reports '{raw_tz}', expected '{plan.timezone}'"))

        host_now = int(time.time())
        try:
            rtc_now = con.rtc_read()
            drift = rtc_now - host_now
            results.append(VerifyItem(
                "RTC (UTC)", abs(drift) <= RTC_TOLERANCE_S,
                f"device clock is {drift:+d}s vs this PC "
                f"(tolerance ±{RTC_TOLERANCE_S}s)"))
        except ConsoleError as exc:
            results.append(VerifyItem("RTC (UTC)", False, str(exc)))

        # Informational, never a failure: since the event store and schedule.yaml
        # live on internal flash, the SD card only serves archive/logs/AMBIT
        # OTA. Operators used to read a missing card as an onboarding failure.
        try:
            sd = con.sd_mounted()
        except ConsoleError:
            sd = None
        if sd is True:
            log("SD card: mounted (archive/logs/AMBIT OTA roles only).")
        elif sd is False:
            log("SD card: absent — OK, Schedule and the event store are internal. "
                "Insert one only if this unit needs AMBIT OTA or log pulls.")
        else:
            log("SD card: not reported by this firmware (older than the "
                "internal-store release); a push install still works.")

        run.verify_results = results
        for item in results:
            log(("PASS  " if item.passed else "FAIL  ")
                + f"{item.label}: {item.detail}")
        return results
    except ConsoleError as exc:
        raise ProcedureError("provision", str(exc)) from exc
    finally:
        try:
            con.close()
        except Exception:
            pass


def _require_network(con, ssid: str, log=print) -> None:
    """Fail fast when the board has no IP, before queueing a download.

    `status` reports "Wi-Fi: connected" off the firmware's CONNECTED bit, which
    is set on IP_EVENT_STA_GOT_IP rather than on association, so this really
    does mean "associated and holding a DHCP lease".

    A board that reaches this state can still fail to reach GitHub, since a
    gateway with no uplink (or broken DNS) looks identical from here. This only
    removes the case that is knowable up front.
    """
    deadline = time.time() + SCHEDULE_WIFI_DEADLINE_S
    announced = False
    while True:
        try:
            state = con.wifi_connected()
        except ConsoleError as exc:
            # Never turn a flaky console read into a network verdict.
            log(f"Could not read Wi-Fi state ({exc}); continuing anyway.")
            return
        if state is None:
            return  # Firmware too old to report it; not our call to block.
        if state:
            log(f"Device is on Wi-Fi '{ssid}' with an IP.")
            return
        remaining = deadline - time.time()
        if remaining <= 0:
            raise ProcedureError(
                "schedule",
                f"the board never got an IP on '{ssid}' within "
                f"{SCHEDULE_WIFI_DEADLINE_S:.0f}s, so it cannot download the Schedule "
                "script. Check the SSID and password for "
                "this session, and that the access point is in range and "
                "handing out addresses. Nothing was changed on the board; "
                "use Retry provisioning once the network is up.")
        if not announced:
            log(f"Waiting for the board to join '{ssid}' "
                f"(up to {SCHEDULE_WIFI_DEADLINE_S:.0f}s)...")
            announced = True
        time.sleep(3.0)


def install_schedule_script(ctx: SessionContext, run: DeviceRun,
                       log=print) -> VerifyItem:
    """Install and positively verify the session's schedule (workbook or catalog).

    The console only submits bytes (serial push) or an immutable URL + manifest
    identity. All byte transfer, hashing, compile validation, atomic littlefs
    swap, .bak recovery, and runner restart remain firmware-owned in
    script_update — a device-side rejection of a stamped schedule IS the
    install failure, surfaced by the verify loop below never matching.

    A workbook source has no URL, so old firmware without the push commands
    cannot install it at all; that is reported, not silently fallen back.
    """
    source = schedule_source(ctx, log=log)
    script = ctx.schedule_script
    try:
        con = ambyte_serial.connect_after_boot(
            run.port, deadline_s=CONSOLE_BOOT_DEADLINE_S, log=log)
    except ConsoleError as exc:
        raise ProcedureError("schedule", str(exc)) from exc

    def matches(status: ambyte_serial.ScheduleReleaseStatus) -> bool:
        return (
            status.sha256 == source.sha256
            and status.script_version == source.script_version
            and status.built_against_fw == source.built_against_fw
            and status.verified
            and status.running
        )

    def workbook_check() -> VerifyItem | None:
        """Step-7 provenance check: `schedule status` workbook= must be the id
        we stamped. Returns None when the firmware does not report the header
        (too old to know it) — the sha identity already matched by then."""
        if source.workbook_version_id is None:
            return None
        try:
            reported = con.schedule_status_workbook()
        except ConsoleError as exc:
            return VerifyItem("Schedule workbook", False,
                              f"could not read schedule status: {exc}")
        if reported is None:
            log("This firmware's `schedule status` does not report the "
                "workbook header; skipping the workbook identity check.")
            return None
        ok = reported == source.workbook_version_id
        return VerifyItem(
            "Schedule workbook", ok,
            f"device reports workbook={reported or '(none)'}, expected "
            f"{source.workbook_version_id}")

    try:
        current = None
        try:
            current = con.schedule_release()
        except ConsoleError as exc:
            # A blank device legitimately has no /littlefs/schedule.yaml identity yet.
            # Submit the install anyway; unsupported older firmware will reject
            # the following `schedule install` command with an actionable reply.
            log(f"No active Schedule release identity yet ({exc}); installing the selection.")

        if current is not None and matches(current):
            log(f"Schedule {source.label} is already active and verified.")
            wb = workbook_check()
            if wb is not None and not wb.passed:
                return wb
            detail = f"{source.label}; sha256={source.sha256}"
            if wb is not None:
                detail += f"; workbook={source.workbook_version_id} confirmed"
            return VerifyItem("Schedule script", True, detail)

        # Preferred: stream the script down this console, so the board needs no
        # network of its own to be onboarded. Falls back to asking the device to
        # download it when the firmware predates the push commands — only
        # possible for a catalog asset, which has an immutable release URL.
        pushed = False
        blob = source.blob
        if blob is None:
            try:
                blob = release_fetch.script_bytes(script, log=log)
            except ReleaseError as exc:
                log(f"Cannot read {script.asset_name} locally ({exc}); "
                    "falling back to a device-side download.")

        if blob is not None:
            log(f"Pushing {source.label} ({len(blob)} bytes) over serial...")
            try:
                con.schedule_push(
                    blob,
                    source.sha256,
                    source.campaign_id,
                    source.script_version,
                    source.built_against_fw,
                    log=log,
                )
                pushed = True
            except UnsupportedConsoleCommand:
                if source.asset_url is None:
                    raise ProcedureError(
                        "schedule",
                        "this firmware predates the serial-push commands "
                        "(`schedule begin/put`), and a workbook-stamped "
                        "schedule has no release URL to fall back to. Flash a "
                        "current firmware release and retry.")
                log("This firmware cannot accept a serial push; "
                    "falling back to a device-side download.")
            except ConsoleError as exc:
                raise ProcedureError("schedule", str(exc)) from exc

        if not pushed:
            # Only this path needs the board on the network.
            _require_network(con, ctx.wifi_ssid, log=log)
            log(f"Installing {script.asset_name} from {script.tag} "
                f"({script.size_bytes} bytes)...")
            try:
                con.schedule_install(
                    script.asset_url,
                    script.sha256,
                    script.campaign_id,
                    script.script_version,
                    script.built_against_fw,
                )
            except ConsoleError as exc:
                raise ProcedureError("schedule", str(exc)) from exc

        deadline = time.time() + SCHEDULE_INSTALL_DEADLINE_S
        last_detail = "the previous script is still active"
        if pushed:
            # The bytes are already staged on the device; what remains is the
            # firmware's local hash + compile check + atomic swap, and our
            # serial read-back of `schedule release`. No network is involved.
            log(f"Schedule install queued; the firmware verifies the pushed script "
                f"on-device (up to {SCHEDULE_INSTALL_DEADLINE_S:.0f}s)...")
        else:
            log(f"Schedule install queued; the device downloads and verifies it over "
                f"Wi-Fi (up to {SCHEDULE_INSTALL_DEADLINE_S:.0f}s)...")
        next_progress = time.time() + SCHEDULE_INSTALL_SETTLE_S + SCHEDULE_INSTALL_PROGRESS_S

        def report_progress() -> None:
            nonlocal next_progress
            now = time.time()
            if now < next_progress:
                return
            next_progress = now + SCHEDULE_INSTALL_PROGRESS_S
            log(f"Still waiting for {source.label} "
                f"({deadline - now:.0f}s left): {last_detail}.")

        time.sleep(SCHEDULE_INSTALL_SETTLE_S)
        while time.time() < deadline:
            try:
                current = con.schedule_release(timeout=20.0)
            except ConsoleError as exc:
                # A busy SD identity read or asynchronous firmware log can make
                # one command miss its prompt. Keep the same serial handle: a
                # close/reopen can reset the ESP32 and abort the in-flight swap.
                last_detail = str(exc)
                if "went away" not in last_detail:
                    report_progress()
                    time.sleep(2.0)
                    continue

                # Reconnect only after a real USB I/O failure.
                con.close()
                replacement = None
                while replacement is None and time.time() < deadline:
                    remaining = deadline - time.time()
                    try:
                        replacement = ambyte_serial.connect_after_boot(
                            run.port, deadline_s=min(30.0, remaining), log=log)
                    except ConsoleError as reconnect_exc:
                        last_detail = str(reconnect_exc)
                if replacement is None:
                    break
                con = replacement
                continue

            if matches(current):
                log(f"Schedule install verified: {source.label}, "
                    f"sha256={current.sha256}.")
                wb = workbook_check()
                if wb is not None and not wb.passed:
                    return wb
                detail = f"{source.label}; sha256={current.sha256}"
                if wb is not None:
                    detail += (f"; workbook={source.workbook_version_id} "
                               "confirmed")
                return VerifyItem("Schedule script", True, detail)
            # Short digests: the log pane is narrow, and 64 hex chars per line
            # every 15 s buries the part that changes. The full sha256 is in the
            # success line and in the VerifyItem detail.
            last_detail = (
                f"device still on {current.script_version or '(untracked)'}"
                f"/{current.sha256[:12]}, want {source.script_version}"
                f"/{source.sha256[:12]} (verified={current.verified}, "
                f"running={current.running})")
            report_progress()
            time.sleep(2.0)

        raise ProcedureError(
            "schedule",
            f"{source.label} was not verified within "
            f"{SCHEDULE_INSTALL_DEADLINE_S:.0f}s ({last_detail}); the previous "
            "schedule.yaml remains recoverable")
    finally:
        con.close()


def _repair_cfg(con: ambyte_serial.AmbyteConsole, plan: ProvisioningPlan,
                log=print) -> bool:
    """Fix a wrong device_name/timezone over the console. Returns True when
    something was changed: cfg values are only read at boot, so the caller
    must then reboot + reconnect."""
    changed = False
    raw_name = con.cfg_get("device_name") or ""
    if raw_name != plan.device_name:
        log(f"Repairing device_name ('{raw_name}' → '{plan.device_name}')...")
        con.cfg_set("device_name", plan.device_name)
        changed = True
    raw_tz = con.cfg_get("timezone") or ""
    if raw_tz != plan.timezone:
        log(f"Repairing timezone ('{raw_tz}' → '{plan.timezone}')...")
        con.cfg_set("timezone", plan.timezone)
        changed = True
    return changed


def provision_and_verify_with_repair(ctx: SessionContext, run: DeviceRun,
                                     log=print) -> list[VerifyItem]:
    """The Retry-provisioning entry point: repair name/timezone over the
    console if needed (never a re-flash), then verify fresh."""
    assert run.plan
    try:
        con = ambyte_serial.connect_after_boot(
            run.port, deadline_s=CONSOLE_BOOT_DEADLINE_S, log=log)
    except ConsoleError as exc:
        raise ProcedureError("provision", str(exc)) from exc
    try:
        changed = _repair_cfg(con, run.plan, log)
        if changed:
            log("Rebooting to apply the repaired config...")
            con.reboot()
    except ConsoleError as exc:
        raise ProcedureError("provision", str(exc)) from exc
    finally:
        con.close()
    if changed:
        time.sleep(2.0)
        log("Waiting for the board to come back after the repair reboot...")
    return provision_and_verify(ctx, run, log=log)
