"""The per-device onboarding procedure: flash + provision + verify.

Step order is chosen so that every failure leaves the board in a recoverable,
clearly-reported state:

  1. check        pre-flash serial probe (2 s, one retry) → stored name;
                  fallback: MAC via esptool (works on unflashed chips)
     [GUI prompts for the device name here]
  2. credentials  openJII register + issue/rotate — BEFORE flashing, so an
                  API failure means nothing was written to the board at all
  3. nvs          bake the per-board NVS image (identity, certs, Wi-Fi,
                  timezone, MQTT, flash_time)
  4. flash        release images + nvs.bin in one esptool session; a mid-way
                  failure leaves the chip in the ROM bootloader = re-flashable
  5. rtc          wait for the freshly booted console, set the exact UTC epoch
  6. lua          install the selected latest-catalog release asset through the
                  firmware's verified URL updater; previous script kept as .bak
  7. verify       read back name / timezone / RTC / Lua identity; per-item
                  pass/fail

Retry paths (no full restart needed):
  * retry_flash()      re-runs steps 4-6 with the already-built images
  * retry_provision()  re-runs 5-7 only; first repairs a wrong name/timezone
                       over the console (`cfg set` + reboot), never re-flashes
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from pathlib import Path

from . import ambyte_serial, esptool_ops
from .ambyte_serial import ConsoleError, expand_mac_token
from .config import (CACHE_DIR, DEVICE_FIRMWARE, DEVICE_ID, DEVICE_VERSION,
                     MAX_NAME_LEN, NVS_OFFSET, PROTOCOL_ID, Environment,
                     default_command_topic, default_status_topic)
from .nvs_builder import ProvisioningPlan, build_nvs_image
from .openjii_client import DeviceIdentity, OpenJIIClient
from .release_fetch import LuaScriptRelease, ReleaseImages

# Accept the RTC as correct within this window of host-now. Generous: it
# covers console latency and a slow verify loop, but still catches a
# seconds-vs-milliseconds or timezone-applied-twice class of error.
RTC_TOLERANCE_S = 120

# A normal post-reboot console appears in 20-35 s, but SD recovery and USB
# re-enumeration can push field devices past 90 s. Keep rescanning for three
# minutes before declaring the board unavailable.
CONSOLE_BOOT_DEADLINE_S = 180.0
# URL installs temporarily stop Lua + MQTT, download over HTTPS, verify, swap,
# reconnect MQTT, and restart Lua. Normal runs finish in seconds; tolerate a
# degraded Wi-Fi reconnect without turning a safe in-progress update into a
# false failure.
LUA_INSTALL_DEADLINE_S = 360.0


class ProcedureError(RuntimeError):
    """A step failed; .step tells the GUI which retry to offer."""

    def __init__(self, step: str, message: str):
        super().__init__(message)
        self.step = step


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
    lua_script: LuaScriptRelease
    topic_root_template: str       # may contain {thingName}
    timezone: str
    wifi_ssid: str
    wifi_password: str


@dataclass
class DeviceRun:
    """Mutable state of one board's procedure, kept for the retry paths."""

    port: str
    preflight: PreflightInfo
    name: str = ""
    identity: DeviceIdentity | None = None
    plan: ProvisioningPlan | None = None
    nvs_path: Path | None = None
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
            log("Console answered but gave no MAC — reading it via esptool.")
        else:
            log("No console answer — treating as unflashed; reading the MAC "
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
        # purpose of the rename prompt — the proposal falls back to it anyway.
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

    if ctx.env.mqtt_uri is None:
        raise ProcedureError(
            "credentials",
            f"the MQTT broker endpoint for '{ctx.env.key}' is not configured "
            "(see the TODO in flash_gui/config.py) — refusing to provision a "
            "board that could never connect.")

    log("Requesting device identity + certificate from openJII...")
    try:
        identity = ctx.client.provision_device(run.preflight.mac, name, log=log)
    except Exception as exc:
        raise ProcedureError("credentials", str(exc)) from exc
    run.identity = identity
    log(f"Thing name: {identity.thing_name} (this becomes the MQTT client id).")

    topic_root = ctx.topic_root_template.replace(
        "{thingName}", identity.thing_name).strip().strip("/")
    plan = ProvisioningPlan(
        device_name=name,
        timezone=ctx.timezone,
        mqtt_uri=ctx.env.mqtt_uri,
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
    )
    run.plan = plan

    log("Baking the per-board NVS image...")
    out = CACHE_DIR / "nvs" / f"nvs-{run.preflight.mac.replace(':', '')}.bin"
    try:
        run.nvs_path = build_nvs_image(plan, out)
    except Exception as exc:
        raise ProcedureError("nvs", str(exc)) from exc
    log(f"NVS image ready: {out.name}")


# ── step 4: flash ────────────────────────────────────────────────────────────
def flash(ctx: SessionContext, run: DeviceRun, log=print) -> None:
    assert run.nvs_path, "prepare_provisioning must run first"
    images = list(ctx.release.flash_files) + [(NVS_OFFSET, run.nvs_path)]
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


def install_lua_script(ctx: SessionContext, run: DeviceRun,
                       log=print) -> VerifyItem:
    """Install and positively verify the selected released Lua asset.

    The console only submits an immutable URL + manifest identity. All byte
    transfer, hashing, syntax validation, atomic SD swap, .bak recovery, and
    runner restart remain firmware-owned in script_update.
    """
    script = ctx.lua_script
    try:
        con = ambyte_serial.connect_after_boot(
            run.port, deadline_s=CONSOLE_BOOT_DEADLINE_S, log=log)
    except ConsoleError as exc:
        raise ProcedureError("lua", str(exc)) from exc

    def matches(status: ambyte_serial.LuaReleaseStatus) -> bool:
        return (
            status.sha256 == script.sha256
            and status.script_version == script.script_version
            and status.built_against_fw == script.built_against_fw
            and status.verified
            and status.running
        )

    try:
        current = None
        try:
            current = con.lua_release()
        except ConsoleError as exc:
            # A blank/new SD legitimately has no /sdcard/main.lua identity yet.
            # Submit the install anyway; unsupported older firmware will reject
            # the following `lua install` command with an actionable reply.
            log(f"No active Lua release identity yet ({exc}); installing the selection.")

        if current is not None and matches(current):
            log(f"Lua {script.asset_name} ({script.tag}) is already active and verified.")
            return VerifyItem(
                "Lua script", True,
                f"{script.asset_name} from {script.tag}; sha256={script.sha256}")

        log(f"Installing {script.asset_name} from {script.tag} "
            f"({script.size_bytes} bytes)...")
        try:
            con.lua_install(
                script.asset_url,
                script.sha256,
                script.campaign_id,
                script.script_version,
                script.built_against_fw,
            )
        except ConsoleError as exc:
            raise ProcedureError("lua", str(exc)) from exc

        deadline = time.time() + LUA_INSTALL_DEADLINE_S
        last_detail = "the previous script is still active"
        while time.time() < deadline:
            time.sleep(2.0)
            try:
                current = con.lua_release(timeout=15.0)
            except ConsoleError as exc:
                # The in-place path should retain USB, but recover from a
                # transient re-enumeration instead of abandoning a valid update.
                last_detail = str(exc)
                con.close()
                remaining = deadline - time.time()
                if remaining <= 0:
                    break
                try:
                    con = ambyte_serial.connect_after_boot(
                        run.port, deadline_s=min(30.0, remaining), log=log)
                except ConsoleError as reconnect_exc:
                    last_detail = str(reconnect_exc)
                continue

            if matches(current):
                log(f"Lua install verified: {script.asset_name} from {script.tag}, "
                    f"sha256={current.sha256}.")
                return VerifyItem(
                    "Lua script", True,
                    f"{script.asset_name} from {script.tag}; sha256={current.sha256}")
            last_detail = (
                f"active sha256={current.sha256}, version="
                f"{current.script_version or '(untracked)'}, "
                f"verified={current.verified}, running={current.running}")

        raise ProcedureError(
            "lua",
            f"{script.asset_name} was not verified within "
            f"{LUA_INSTALL_DEADLINE_S:.0f}s ({last_detail}); the previous "
            "main.lua remains recoverable")
    finally:
        con.close()


def _repair_cfg(con: ambyte_serial.AmbyteConsole, plan: ProvisioningPlan,
                log=print) -> bool:
    """Fix a wrong device_name/timezone over the console. Returns True when
    something was changed — cfg values are only read at boot, so the caller
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
