# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Experiment-scoped delivery of a workbook-stamped schedule over MQTT.

The flash GUI installs an experiment's schedule over USB: it resolves the
pinned workbook version, finds the ``schema: jii.ambyte-schedule/...`` command
cell, stamps ``workbookVersionId``/``macros``/``when:`` for the firmware being
flashed, and pushes the bytes. This module does the same resolve + stamp with
the GUI's own code (``flash_gui.openjii_client``, ``flash_gui.schedule_stamp``)
and hands :mod:`schedule_deploy` an inline ``script_update`` per firmware
class, so the fleet path and the bench path cannot drift apart.

Inline delivery (the ``script`` field the firmware accepts next to ``url``) is
what makes this possible without an artifact host: a stamped YAML is unique per
experiment and firmware class, so there is no immutable release URL for it.
The firmware caps an inline message at 16 KiB and needs a contiguous TLS
buffer to receive it, which is why the whole JSON payload is size-checked here
and why devices report ``busy``/``dropped`` under heap pressure instead of
applying.
"""

from __future__ import annotations

import hashlib
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable

try:
    from . import fleet_deploy as fleet
except ImportError:  # pragma: no cover - exercised by CLI invocation
    import fleet_deploy as fleet

API_KEY_ENV = "OPENJII_API_KEY"
# First firmware whose schedule runner reads /littlefs/schedule.yaml at all
# (v2.0.0 replaced the Lua host). Older devices need a firmware OTA first.
YAML_SCHEDULE_MIN_FW = "2.0.0"
# The firmware's inline script_update cap is the whole MQTT message.
INLINE_MAX_PAYLOAD_BYTES = 16 * 1024
# Stamping classes, newest first. Each is the minimum firmware for that header
# shape; the flash GUI applies exactly the same gates (procedure.schedule_source).
FIRMWARE_CLASSES = (
    ("when", "macros with when: routing"),
    ("macros", "macros without routing"),
    ("id-only", "workbookVersionId only"),
)


class WorkbookDeliveryError(RuntimeError):
    """The experiment's workbook cannot be delivered to the fleet as-is."""


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def gui_modules():
    """Import the flash GUI's resolve/stamp code lazily and from the repo root.

    Kept out of module scope so the catalog-release path of schedule_deploy
    never needs certifi (the GUI's TLS helper pulls it in).
    """
    root = str(repo_root())
    if root not in sys.path:
        sys.path.insert(0, root)
    from flash_gui import openjii_client, schedule_stamp  # noqa: WPS433
    from flash_gui.config import ENVIRONMENTS

    return openjii_client, schedule_stamp, ENVIRONMENTS


def make_client(env_key: str, api_key: str):
    openjii_client, _stamp, environments = gui_modules()
    if env_key not in environments:
        raise WorkbookDeliveryError(
            f"unknown openJII environment {env_key!r} (one of: "
            f"{', '.join(sorted(environments))})"
        )
    if not (api_key or "").strip():
        raise WorkbookDeliveryError(
            f"{API_KEY_ENV} is not set; a personal openJII API key (jii_...) for "
            f"the {env_key} environment is required to read the experiment"
        )
    return openjii_client.OpenJIIClient(environments[env_key], api_key)


def experiment_thing_names(client, experiment_id: str) -> tuple[list[str], list[str]]:
    """Ambyte thing names bound to the experiment -> (addressable, skipped).

    Names that are not fleet client IDs (other device families, or things not
    following ``ambyte_<MAC>``) are returned separately so the plan can show
    them instead of silently dropping them.
    """
    addressable: list[str] = []
    skipped: list[str] = []
    for entry in client.list_experiment_devices(experiment_id):
        name = entry.get("thingName")
        if not isinstance(name, str) or not name:
            continue
        normalized = fleet.normalize_device(name)
        if normalized is None or normalized.casefold() != name.casefold():
            skipped.append(name)
        else:
            addressable.append(name)
    return fleet.unique_devices(addressable), sorted(skipped)


def restrict_to_experiment(requested: list[str], experiment_devices: list[str]) -> list[str]:
    """Explicit --devices must be a subset of the experiment's things."""
    known = fleet.device_index(experiment_devices)
    outside = [d for d in requested if fleet.device_identity_key(d) not in known]
    if outside:
        raise WorkbookDeliveryError(
            "explicit devices are not bound to this experiment: " + ", ".join(outside)
        )
    return [known[fleet.device_identity_key(d)] for d in requested]


def firmware_class(fw: str | None, stamp) -> str:
    """Which stamping shape a device's reported firmware can take.

    Returns one of the FIRMWARE_CLASSES keys, or an exclusion reason:
    ``silent`` (no pong), ``unparseable`` (legacy NVS junk such as "1"),
    ``pre-yaml`` (Lua-era firmware; OTA it first).
    """
    if fw is None:
        return "silent"
    parsed = fleet.parse_version(fw)
    if parsed is None:
        return "unparseable"
    if fleet.cmp_version(parsed, fleet.parse_version(YAML_SCHEDULE_MIN_FW)) < 0:
        return "pre-yaml"
    if not stamp.firmware_supports_macros(fw):
        return "id-only"
    if not stamp.firmware_supports_macro_when(fw):
        return "macros"
    return "when"


@dataclass
class Variant:
    key: str
    description: str
    text: str
    sha256: str
    command: dict[str, Any]
    devices: list[str] = field(default_factory=list)


def script_version_label(programming) -> str:
    number = programming.workbook_version_number
    version_id = programming.workbook_version_id.strip()
    return f"wb-v{number}" if isinstance(number, int) else f"wb-{version_id[:8]}"


def stamped_variant(
    programming, key: str, stamp, *, reboot: bool, checker=None, log=print
) -> Variant:
    """Stamp the programming for one firmware class and wrap it as a command."""
    if key == "when":
        if not getattr(programming, "routing_compiled", False):
            raise WorkbookDeliveryError(
                "branch routing was not compiled for the workbook; refusing to "
                "stamp macros without when: onto firmware that evaluates it"
            )
        macros = tuple(programming.macros)
        built_against = stamp.MACRO_WHEN_MIN_FW
    elif key == "macros":
        macros = tuple(m.without_when() for m in programming.macros)
        built_against = stamp.MACROS_HEADER_MIN_FW
    elif key == "id-only":
        macros = ()
        built_against = YAML_SCHEDULE_MIN_FW
    else:
        raise ValueError(f"unknown firmware class {key!r}")

    text = stamp.stamp_header(
        programming.yaml_text,
        programming.workbook_version_id,
        macros,
        checker=checker,
        log=log,
    )
    blob = text.encode("utf-8")
    digest = hashlib.sha256(blob).hexdigest()
    version_id = programming.workbook_version_id.strip()
    command = {
        "type": "script_update",
        # The device latches applied ids in NVS and ignores repeats, so the
        # id must change whenever the stamped bytes do (a later firmware
        # class re-stamps the same workbook version differently).
        "id": f"workbook-{version_id}:{digest[:8]}",
        "script": text,
        "checksum": digest,
        "script_version": script_version_label(programming),
        "built_against_fw": built_against,
        "reboot": reboot,
    }
    payload = json.dumps(command, separators=(",", ":")).encode("utf-8")
    if len(payload) > INLINE_MAX_PAYLOAD_BYTES:
        raise WorkbookDeliveryError(
            f"stamped schedule for firmware class {key!r} is {len(payload)} bytes "
            f"as an inline command; the firmware accepts at most "
            f"{INLINE_MAX_PAYLOAD_BYTES}. Shorten the workbook's schedule cell or "
            "wait for URL-hosted delivery."
        )
    description = dict(FIRMWARE_CLASSES)[key]
    return Variant(key, description, text, digest, command)


def plan_variants(
    programming,
    cohort: list[str],
    firmware_by_device: dict[str, str | None],
    stamp,
    *,
    reboot: bool,
    checker=None,
    log: Callable[[str], None] = print,
) -> tuple[list[Variant], dict[str, str]]:
    """Group the cohort by firmware class and stamp one variant per class.

    Devices that cannot take a YAML schedule are returned in ``excluded`` with
    their reason; nothing is stamped for them. Stamping happens only for
    classes that have at least one device, and any stamping failure aborts the
    whole plan: a workbook the device contract cannot express must not be
    half-delivered.
    """
    grouped: dict[str, list[str]] = {}
    excluded: dict[str, str] = {}
    for device in cohort:
        key = firmware_class(firmware_by_device.get(device), stamp)
        if key in dict(FIRMWARE_CLASSES):
            grouped.setdefault(key, []).append(device)
        else:
            excluded[device] = key

    variants: list[Variant] = []
    for key, _description in FIRMWARE_CLASSES:
        devices = grouped.get(key)
        if not devices:
            continue
        variant = stamped_variant(
            programming, key, stamp, reboot=reboot, checker=checker, log=log
        )
        variant.devices = sorted(devices)
        variants.append(variant)
    return variants, excluded
