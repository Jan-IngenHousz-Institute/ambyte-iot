# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only
"""Firmware maintenance: preserve configured identity, NVS and filesystem data.

No registry calls, certificates, private-key reads, NVS image, or littlefs image
belong to this path. A failed maintenance operation never falls back to initial
provisioning. Physical writes bypass OTA admission, so the host must check the
release signer, version, storage contract and on-device partition table itself.
"""
from dataclasses import dataclass
import re
import time
from urllib.parse import urlsplit

from . import ambyte_serial, esptool_ops
from .firmware_metadata import ImageError, inspect_image, StorageManifest
from .service_client import ServiceClient, ServiceClientError

# Public, reviewed trust pin; intentionally bundled as code in the GUI. Tests
# pin these to tools/keys so release-key rotation cannot silently diverge.
FIRST_SIGNED_RELEASE = (2, 12, 0)
RELEASE_KEY_ID = "fw-rsa3072:224fa9409f97a2116f637cf4d07f6c734ec3e2c9a442ecac581be92d8108530f"
CONFIG_KEYS = ("mqtt_uri", "mqtt_client_id", "mqtt_topic_root", "command_topic",
               "status_topic", "device_name", "timezone")
# This maintenance path preserves the currently deployed dual-OTA layout.
# Verify the table byte-for-byte on the chip before erasing any sector. Each
# image is bounded to its own region, including erase-sector rounding.
IMAGE_REGIONS = {0x0: 0x8000, 0x8000: 0x9000, 0xF000: 0x11000, 0x20000: 0x310000}


class FirmwareUpdateError(RuntimeError):
    pass


@dataclass(frozen=True)
class ConfiguredDevice:
    port: str
    identity: ambyte_serial.ConsoleIdentity
    settings: dict[str, str | None]


@dataclass(frozen=True)
class FirmwareUpdateResult:
    mac: str
    firmware: str
    changed: bool
    maintenance: object = None


def _version(value):
    match = re.fullmatch(r"v?(\d+)\.(\d+)\.(\d+)(?:[-+][0-9A-Za-z.+-]+)?", value or "")
    if match is None:
        raise FirmwareUpdateError("Cannot determine a semantic firmware version; no firmware was written.")
    return tuple(int(v) for v in match.groups())


def _snapshot(console):
    try:
        identity = console.identity()
    except ambyte_serial.UnsupportedConsoleCommand:
        raise FirmwareUpdateError("This firmware cannot report its compiled identity. Firmware-only maintenance requires identity-capable firmware; no credentials were changed.") from None
    if not identity.mac or not identity.thing_name or not identity.fw:
        raise FirmwareUpdateError("The Ambyte did not provide its MAC, client identity and compiled firmware version.")
    settings = {key: console.cfg_get(key) for key in CONFIG_KEYS}
    if any(not settings[key] for key in ("mqtt_uri", "mqtt_client_id", "mqtt_topic_root")):
        raise FirmwareUpdateError("The device has no complete MQTT configuration. Use Initial setup explicitly for a new device.")
    try:
        uri = urlsplit(settings["mqtt_uri"])
        valid_uri = uri.scheme in {"mqtt", "mqtts"} and uri.hostname and not uri.username and not uri.password
    except ValueError:
        valid_uri = False
    if not valid_uri:
        raise FirmwareUpdateError("The existing MQTT broker configuration is invalid; no credentials were changed.")
    client = ambyte_serial.expand_mac_token(settings["mqtt_client_id"], identity.mac)
    if client != identity.thing_name:
        raise FirmwareUpdateError("Stored client ID differs from the running identity. Reboot and inspect the device before updating.")
    return ConfiguredDevice(console.port, identity, settings)


def _candidate(release):
    images = list(release.flash_files)
    if len(images) != len(IMAGE_REGIONS) or {off for off, _ in images} != set(IMAGE_REGIONS):
        raise FirmwareUpdateError("This release does not contain the supported firmware-only image layout. NVS and filesystem images are never written by maintenance.")
    for off, path in images:
        size = path.stat().st_size
        if not size or off + ((size + 4095) // 4096) * 4096 > IMAGE_REGIONS[off]:
            raise FirmwareUpdateError("A release image overlaps a preserved device region; nothing was written.")
    files = dict(images)
    signed_release = _version(release.version) >= FIRST_SIGNED_RELEASE
    try:
        metadata = inspect_image(files[0x20000].read_bytes(),
                                 require_signed=signed_release, require_manifest=signed_release)
    except ImageError as exc:
        raise FirmwareUpdateError(f"Firmware release refused: {exc}") from None
    if metadata.version != release.version.removeprefix("v"):
        raise FirmwareUpdateError("Application version does not match the selected release.")
    if metadata.storage and not metadata.signature_present:
        raise FirmwareUpdateError("A modern firmware image must be signed before flashing.")
    if metadata.signature_present and metadata.key_id != RELEASE_KEY_ID:
        raise FirmwareUpdateError("Firmware signer does not match the pinned release key.")
    return images, files[0x8000].read_bytes(), metadata


def _compatible(identity, candidate):
    running = identity.storage or dict(readable=[1], written=1, fallback=1)
    target = candidate.storage or StorageManifest((1,), 1, 1)
    # The durable union of store versions is not exposed over USB yet. With
    # format 1 the entire deployed history is known. Refuse store-changing
    # releases rather than guessing that the running writer is the whole set.
    if running["written"] != 1 or target.written != 1:
        raise FirmwareUpdateError("USB maintenance for a storage-format change is not qualified; existing measurements were left untouched.")
    if (running["written"] not in target.readable or target.written not in running["readable"]
            or target.fallback != running["written"]):
        raise FirmwareUpdateError("Firmware storage contracts are incompatible; existing measurements were left untouched.")


def _idle(console, identity):
    if "svc" not in identity.capabilities:
        return
    client = ServiceClient(console._ser, None)
    try:
        client.connect()
        client._foreign()  # Read-only; rejects any live schedule or Wi-Fi operation.
    finally:
        try:
            client.exit()
        except ServiceClientError:
            pass


def _settled_schedule(console, snapshot, log, *, before_flash):
    deadline = time.monotonic() + 180.0
    next_progress = time.monotonic() + 15.0
    mac = snapshot.identity.mac
    while snapshot.identity.schedule.source in {None, "NONE"} and time.monotonic() < deadline:
        if time.monotonic() >= next_progress:
            log("Firmware is responding; waiting for schedule startup to finish…")
            next_progress = time.monotonic() + 15.0
        time.sleep(1.0)
        snapshot = _snapshot(console)
        if snapshot.identity.mac != mac:
            raise FirmwareUpdateError("The connected Ambyte changed during schedule discovery.")
    if snapshot.identity.schedule.source in {None, "NONE"}:
        stage = "No firmware was written" if before_flash else "Firmware was written; post-boot verification needs attention"
        raise FirmwareUpdateError(f"{stage}: schedule startup did not provide a loaded identity.")
    return snapshot


def update_firmware(port, release, *, client=None, confirm_timezone=None, log=print):
    """Update an existing configured device; initial provisioning is separate."""
    try:
        images, table, metadata = _candidate(release)
        with ambyte_serial.AmbyteConsole(port) as console:
            if not console.wait_prompt(45.0, recover_service=True):
                raise FirmwareUpdateError("No Ambyte answered on the selected USB port. No firmware or credentials were changed.")
            before = _snapshot(console)
            if _version(before.identity.fw) > _version(release.version):
                raise FirmwareUpdateError("This would downgrade the running firmware. Maintenance refuses downgrades by default.")
            if before.identity.fw.removeprefix("v") == release.version.removeprefix("v"):
                log(f"{before.identity.mac}: firmware {release.version} is already installed; nothing was flashed.")
                # No flash, but this is still a maintenance session.
                from . import maintenance
                extra = maintenance.check_session(console, before.identity.mac, client,
                    confirm_timezone=confirm_timezone, log=log)
                return FirmwareUpdateResult(before.identity.mac, before.identity.fw, False, extra)
            before = _settled_schedule(console, before, log, before_flash=True)
            if before.identity.schedule.source == "INSTALLED" and not before.identity.schedule.sha256:
                raise FirmwareUpdateError("The installed schedule identity is not ready. Wait for startup and retry; nothing was flashed.")
            log(f"Pre-update schedule: {before.identity.schedule.source}; sha256={before.identity.schedule.sha256 or 'unavailable'}")
            _compatible(before.identity, metadata)
            _idle(console, before.identity)
        log(f"Updating {before.identity.mac} to {release.version}; preserving NVS, credentials, Wi-Fi, topics, schedule and stored measurements.")
        log("Configuration is present; certificate presence and validity cannot be checked offline.")
        esptool_ops.flash_images(port, images, release.flash_settings, log=log,
                                 expected_mac=before.identity.mac,
                                 expected_partition_table=table)
        log("Firmware flashed. Waiting for the same Ambyte to boot and verifying its existing configuration…")
        with ambyte_serial.connect_after_boot(port, deadline_s=180.0, log=log,
                                             recover_service=True) as console:
            after = _snapshot(console)
            after = _settled_schedule(console, after, log, before_flash=False)
            if (after.identity.mac != before.identity.mac or
                    after.identity.thing_name != before.identity.thing_name):
                raise FirmwareUpdateError("Post-update device identity differs. Firmware was written; no provisioning fallback was attempted.")
            if after.identity.fw.removeprefix("v") != release.version.removeprefix("v"):
                raise FirmwareUpdateError("The expected firmware did not start. Device credentials were preserved.")
            changed = [key for key in CONFIG_KEYS if after.settings[key] != before.settings[key]]
            if changed:
                raise FirmwareUpdateError("Firmware was updated but configuration verification failed for: " + ", ".join(changed))
            old, new = before.identity.schedule, after.identity.schedule
            if (new.source in {None, "NONE", "EMBEDDED_FALLBACK"}
                    or new.source == "INSTALLED" and not new.sha256
                    or old.source == "INSTALLED" and new != old):
                raise FirmwareUpdateError("Firmware was updated, but the existing installed schedule was not confirmed active. Install a compatible workbook separately; credentials were preserved.")
        log("Verified firmware, MAC, client identity and existing configuration. No registration or certificate issuance was performed.")
        from . import maintenance
        extra = maintenance.run(after.port, after.identity.mac, client, confirm_timezone=confirm_timezone, log=log)
        return FirmwareUpdateResult(after.identity.mac, after.identity.fw, True, extra)
    except FirmwareUpdateError:
        raise
    except (ambyte_serial.ConsoleError, esptool_ops.EsptoolError, ServiceClientError) as exc:
        raise FirmwareUpdateError(str(exc)) from None
