#!/usr/bin/env python3
"""Targeted AMBIT application OTA through selected Ambyte gateways.

The public AMBIT release, manifest, and application image are verified
anonymously before AWS is contacted. Selected gateways are then queried with a
correlated ``ambit_versions`` command. Only gateways with at least one older,
version-proven AMBIT are sent one ``ambit_ota`` command with ``channel=all``.

``channel=all`` is the only safe fleet primitive: channels share the UART and
the device firmware performs one staged download followed by a sequential
sweep. Consequently a mixed gateway can reflash a channel already on the target
when another channel needs the update. A gateway with any newer channel is
skipped unless ``--allow-downgrade`` is explicit, because the all-channel
command cannot exclude that channel.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import queue
import re
import sys
import time
import urllib.parse
import urllib.request
import uuid
from pathlib import Path
from typing import Any, Callable

try:  # Package import in tests; sibling import for direct CLI invocation.
    from . import fleet_deploy as fleet
    from .release_selection import AMBIT_TAG_RE
except ImportError:  # pragma: no cover
    import fleet_deploy as fleet
    from release_selection import AMBIT_TAG_RE


AMBIT_REPOSITORY = "Jan-IngenHousz-Institute/ambit"
MANIFEST_NAME = "manifest.json"
GITHUB_API = "https://api.github.com"
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
APP_OFFSET = "0x10000"
# Firmware dependency: components/ambit_ota/ambit_ota.c's channel=all path
# reports one terminal for every channel 0..3 after MQTT recovery, using
# ``absent`` for channels that did not answer, followed by an overall channel
# 255 terminal. The tracker deliberately waits for that complete four-channel
# set; tests pin the source-side contract so a firmware change cannot silently
# turn successful fleet runs into hour-long host-side timeouts.
CHANNELS = tuple(range(4))
OTA_CHANNEL_ALL = 255
AMBIT_OTA_TERMINAL_STATES = {"success", "failed", "absent"}


class ReleaseError(ValueError):
    """The selected release or its assets fail the public immutable contract."""


def _release_api_url(repository: str, tag: str) -> str:
    quoted = urllib.parse.quote(tag, safe="")
    return f"{GITHUB_API}/repos/{repository}/releases/tags/{quoted}"


def _release_asset_url(repository: str, tag: str, asset: str) -> str:
    return (
        f"https://github.com/{repository}/releases/download/"
        f"{urllib.parse.quote(tag, safe='')}/"
        f"{urllib.parse.quote(asset, safe='')}"
    )


def _download_bytes(url: str, *, max_bytes: int) -> bytes:
    request = urllib.request.Request(
        url,
        headers={
            "Accept": "application/octet-stream",
            "User-Agent": "ambyte-ambit-fleet-deploy",
        },
    )
    with urllib.request.urlopen(request, timeout=60) as response:
        length = response.headers.get("Content-Length")
        if length is not None and int(length) > max_bytes:
            raise ReleaseError(f"release asset exceeds {max_bytes} bytes")
        body = response.read(max_bytes + 1)
    if len(body) > max_bytes:
        raise ReleaseError(f"release asset exceeds {max_bytes} bytes")
    return body


def _download_json(url: str) -> Any:
    request = urllib.request.Request(
        url,
        headers={
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
            "User-Agent": "ambyte-ambit-fleet-deploy",
        },
    )
    with urllib.request.urlopen(request, timeout=30) as response:
        body = response.read(2 * 1024 * 1024 + 1)
    if len(body) > 2 * 1024 * 1024:
        raise ReleaseError("GitHub release response is unexpectedly large")
    try:
        return json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ReleaseError("GitHub release response is not valid UTF-8 JSON") from exc


def _require_string(obj: dict[str, Any], field: str, context: str) -> str:
    value = obj.get(field)
    if not isinstance(value, str) or not value:
        raise ReleaseError(f"{context} field {field!r} must be a non-empty string")
    return value


def _asset_by_name(release: dict[str, Any], name: str) -> dict[str, Any]:
    assets = release.get("assets")
    if not isinstance(assets, list):
        raise ReleaseError("GitHub release assets must be an array")
    matches = [asset for asset in assets if isinstance(asset, dict) and asset.get("name") == name]
    if len(matches) != 1:
        raise ReleaseError(f"release must contain exactly one {name!r} asset")
    asset = matches[0]
    if asset.get("state") not in (None, "uploaded"):
        raise ReleaseError(f"release asset {name!r} is not uploaded")
    return asset


def _validate_asset_url(
    asset: dict[str, Any], repository: str, tag: str, name: str
) -> str:
    url = _require_string(asset, "browser_download_url", f"asset {name!r}")
    expected = _release_asset_url(repository, tag, name)
    if url != expected:
        raise ReleaseError(
            f"asset {name!r} does not use the anonymous immutable release URL"
        )
    return url


def validate_manifest(manifest: Any, tag: str) -> dict[str, Any]:
    """Validate the existing AMBIT multi-image manifest and select its app image."""
    if AMBIT_TAG_RE.fullmatch(tag) is None:
        raise ReleaseError(f"AMBIT release tag must match vX.Y.Z[-suffix]: {tag!r}")
    if not isinstance(manifest, dict):
        raise ReleaseError("manifest root must be a JSON object")
    expected_version = tag.removeprefix("v")
    if _require_string(manifest, "version", "manifest") != expected_version:
        raise ReleaseError("manifest version does not match the selected release tag")

    ota = manifest.get("ota")
    if not isinstance(ota, dict):
        raise ReleaseError("manifest ota must be a JSON object")
    app_name = _require_string(ota, "file", "manifest ota")
    # A release asset name, never a path or URL.
    if app_name in {".", ".."} or "/" in app_name or "\\" in app_name:
        raise ReleaseError("manifest ota.file must be a plain release asset name")

    flash = manifest.get("flash")
    if not isinstance(flash, list):
        raise ReleaseError("manifest flash must be an array")
    app_entries = [
        entry
        for entry in flash
        if isinstance(entry, dict) and entry.get("file") == app_name
    ]
    if len(app_entries) != 1:
        raise ReleaseError("manifest must contain exactly one flash entry for ota.file")
    app = app_entries[0]
    if app.get("offset") != APP_OFFSET:
        raise ReleaseError(f"AMBIT app image offset must be exactly {APP_OFFSET!r}")
    size = app.get("size")
    if isinstance(size, bool) or not isinstance(size, int) or size <= 0:
        raise ReleaseError("AMBIT app image size must be a positive integer")
    digest = app.get("sha256")
    if not isinstance(digest, str) or SHA256_RE.fullmatch(digest) is None:
        raise ReleaseError("AMBIT app image sha256 must be lowercase 64-digit hex")

    return {
        "name": manifest.get("name"),  # legacy "ambit-iot" is intentionally allowed
        "version": expected_version,
        "chip": manifest.get("chip"),
        "app_name": app_name,
        "offset": APP_OFFSET,
        "size": size,
        "sha256": digest,
    }


def _validate_release_metadata(
    release: Any, tag: str, allow_prerelease: bool
) -> dict[str, Any]:
    if not isinstance(release, dict):
        raise ReleaseError("GitHub release response must be a JSON object")
    if release.get("tag_name") != tag:
        raise ReleaseError("GitHub release tag does not match the requested tag")
    if release.get("draft") is not False:
        raise ReleaseError("selected AMBIT release is a draft")
    if not isinstance(release.get("published_at"), str) or not release["published_at"]:
        raise ReleaseError("selected AMBIT release is not published")
    if release.get("immutable") is not True:
        raise ReleaseError("selected AMBIT release is not immutable")
    is_prerelease = release.get("prerelease") is True
    if is_prerelease and not allow_prerelease:
        raise ReleaseError("selected AMBIT release is a prerelease; explicit opt-in required")
    return release


def fetch_release(
    repository: str,
    tag: str,
    *,
    allow_prerelease: bool = False,
    json_fetcher: Callable[[str], Any] = _download_json,
    downloader: Callable[..., bytes] = _download_bytes,
) -> tuple[dict[str, Any], bytes, dict[str, Any]]:
    """Anonymously fetch and verify release metadata, manifest, and app image."""
    if repository != AMBIT_REPOSITORY:
        raise ReleaseError(
            f"AMBIT releases must come from the fixed public repository {AMBIT_REPOSITORY}"
        )
    if AMBIT_TAG_RE.fullmatch(tag) is None:
        raise ReleaseError(f"invalid AMBIT release tag: {tag!r}")

    api_url = _release_api_url(repository, tag)
    release = _validate_release_metadata(
        json_fetcher(api_url), tag, allow_prerelease
    )
    manifest_asset = _asset_by_name(release, MANIFEST_NAME)
    manifest_url = _validate_asset_url(
        manifest_asset, repository, tag, MANIFEST_NAME
    )
    manifest_bytes = downloader(manifest_url, max_bytes=256 * 1024)
    try:
        decoded = json.loads(manifest_bytes.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ReleaseError("manifest.json is not valid UTF-8 JSON") from exc
    selected = validate_manifest(decoded, tag)

    app_asset = _asset_by_name(release, selected["app_name"])
    app_url = _validate_asset_url(
        app_asset, repository, tag, selected["app_name"]
    )
    rest_size = app_asset.get("size")
    if isinstance(rest_size, bool) or not isinstance(rest_size, int):
        raise ReleaseError("GitHub app asset size must be an integer")
    if rest_size != selected["size"]:
        raise ReleaseError(
            f"GitHub app asset size {rest_size} does not match manifest {selected['size']}"
        )
    expected_asset_digest = f"sha256:{selected['sha256']}"
    if app_asset.get("digest") != expected_asset_digest:
        raise ReleaseError(
            "GitHub immutable app asset digest does not match manifest sha256"
        )

    image = downloader(app_url, max_bytes=16 * 1024 * 1024)
    if len(image) != selected["size"]:
        raise ReleaseError(
            f"AMBIT app byte count mismatch: got {len(image)}, expected {selected['size']}"
        )
    actual_digest = hashlib.sha256(image).hexdigest()
    if actual_digest != selected["sha256"]:
        raise ReleaseError(
            f"AMBIT app SHA-256 mismatch: got {actual_digest}, expected {selected['sha256']}"
        )
    if not image or image[0] != 0xE9:
        first = image[:1].hex() or "none"
        raise ReleaseError(
            f"AMBIT app is not an ESP application image (first byte {first}, expected e9)"
        )

    manifest_digest = hashlib.sha256(manifest_bytes).hexdigest()
    proof = {
        "repository": repository,
        "release_api_url": api_url,
        "release_id": release.get("id"),
        "tag": tag,
        "target_commitish": release.get("target_commitish"),
        "published_at": release["published_at"],
        "html_url": release.get("html_url"),
        "draft": False,
        "prerelease": release.get("prerelease") is True,
        "immutable": True,
        "anonymous_metadata_fetch": True,
        "manifest": {
            "asset_id": manifest_asset.get("id"),
            "name": MANIFEST_NAME,
            "url": manifest_url,
            "size": len(manifest_bytes),
            "sha256": manifest_digest,
            "github_digest": manifest_asset.get("digest"),
        },
        "application": {
            "asset_id": app_asset.get("id"),
            "name": selected["app_name"],
            "url": app_url,
            "offset": selected["offset"],
            "size": selected["size"],
            "sha256": selected["sha256"],
            "github_digest": app_asset["digest"],
            "esp_app_magic": "0xe9",
            "anonymous_download_verified": True,
        },
    }
    return selected, image, proof


def parse_devices(value: str) -> list[str]:
    devices: list[str] = []
    invalid: list[str] = []
    for token in re.split(r"[\s,]+", value):
        if not token:
            continue
        normalized = fleet.normalize_device(token)
        if normalized is None:
            invalid.append(token)
        else:
            devices.append(normalized)
    if invalid:
        raise ValueError(f"unrecognized device tokens: {invalid}")
    return sorted(set(devices))


def select_cohort(
    universe: list[str],
    firmware_by_device: dict[str, str | None],
    version_op: str,
    version: str | None,
    percentage: int,
) -> dict[str, Any]:
    if not 1 <= percentage <= 100:
        raise ValueError("percentage must be 1-100")
    unproven: list[str] = []
    if version_op == "any":
        matching = list(universe)
    else:
        reference = fleet.parse_version(version or "")
        if reference is None:
            raise ValueError(f"firmware predicate {version_op!r} needs a version")
        predicate = fleet.VERSION_OPS[version_op]
        matching = []
        for device in universe:
            current = fleet.parse_version(firmware_by_device.get(device) or "")
            if current is None:
                unproven.append(device)
            elif predicate(fleet.cmp_version(current, reference)):
                matching.append(device)
    matching.sort(key=fleet.selection_key)
    take = math.ceil(len(matching) * percentage / 100)
    return {
        "matching": matching,
        "unproven": sorted(unproven),
        "cohort": sorted(matching[:take]),
    }


def _new_id(prefix: str, version: str) -> str:
    clean = re.sub(r"[^0-9A-Za-z.-]+", "-", version)[:24]
    # Firmware accepts at most 63 characters including the terminating NUL.
    return f"{prefix}-{clean}-{uuid.uuid4().hex[:16]}"[:63]


def _parse_versions_report(data: dict[str, Any]) -> dict[str, Any] | None:
    if data.get("state") == "busy":
        return {"state": "busy", "detail": data.get("detail"), "channels": {}}
    raw_channels = data.get("channels")
    if not isinstance(raw_channels, list):
        return None
    channels: dict[str, dict[str, Any]] = {}
    seen: set[int] = set()
    for entry in raw_channels:
        if not isinstance(entry, dict):
            return None
        channel = entry.get("ch")
        present = entry.get("present")
        if channel not in CHANNELS or channel in seen or not isinstance(present, bool):
            return None
        seen.add(channel)
        version = entry.get("version")
        if version is not None and not isinstance(version, str):
            return None
        channels[str(channel)] = {"present": present, "version": version}
    if seen != set(CHANNELS):
        return None
    return {"state": "complete", "channels": channels}


def fleet_ambit_versions(
    session: Any,
    devices: list[str],
    query_id: str,
    wait_seconds: int,
    *,
    connection_factory: Callable[..., Any] = fleet.mqtt_connection,
) -> tuple[dict[str, dict[str, Any]], str | None]:
    """Publish correlated AMBIT inventory queries and retain one valid reply/device."""
    from awscrt import mqtt

    payload = json.dumps(
        {"type": "ambit_versions", "id": query_id}, separators=(",", ":")
    )
    received: queue.Queue[tuple[str, bytes | str]] = queue.Queue()
    reports: dict[str, dict[str, Any]] = {}
    error = None
    connection = None

    def record_message(topic: str, message: bytes | str) -> None:
        if isinstance(message, bytes):
            message = message.decode("utf-8", "replace")
        try:
            data = json.loads(message)
        except (TypeError, json.JSONDecodeError):
            return
        if not isinstance(data, dict) or data.get("type") != "ambit_versions":
            return
        if data.get("id") != query_id:
            return
        device = fleet.device_from_status_topic(topic)
        if device not in devices or device in reports:
            return
        report = _parse_versions_report(data)
        if report is not None:
            reports[device] = report
            print(f"  ambit_versions {device}  {report['state']}")

    try:
        connection = connection_factory(
            session,
            fleet.STATUS_TOPIC,
            lambda topic, payload, dup, qos, retain, **kwargs: received.put(
                (topic, payload)
            ),
            client_id="fleet-deploy-ambit-preflight",
        )
        publications = []
        for device in devices:
            publication, _ = connection.publish(
                topic=fleet.COMMAND_TOPIC_FMT.format(device=device),
                payload=payload,
                qos=mqtt.QoS.AT_LEAST_ONCE,
                retain=False,
            )
            publications.append(publication)
        for publication in publications:
            publication.result()

        deadline = time.time() + wait_seconds
        while len(reports) < len(devices) and time.time() < deadline:
            try:
                topic, message = received.get(timeout=0.2)
            except queue.Empty:
                continue
            record_message(topic, message)
    except Exception as exc:
        error = _redact_text(f"{type(exc).__name__}: {exc}")
    finally:
        while True:
            try:
                topic, message = received.get_nowait()
            except queue.Empty:
                break
            record_message(topic, message)
        if connection is not None:
            try:
                connection.disconnect().result()
            except Exception:
                pass
    return reports, error


def retry_absent_preflight(
    session: Any,
    devices: list[str],
    initial_reports: dict[str, dict[str, Any]],
    release_version: str,
    wait_seconds: int,
    delay_seconds: float,
    *,
    versions_query: Callable[
        ..., tuple[dict[str, dict[str, Any]], str | None]
    ] = fleet_ambit_versions,
    sleep: Callable[[float], None] = time.sleep,
) -> tuple[
    dict[str, dict[str, Any]],
    dict[str, Any] | None,
]:
    """Retry inconclusive inventory and reconcile channel evidence fail closed.

    A measuring AMBIT is deliberately silent on the shared UART. A version
    sweep that overlaps SS/MPF can therefore report that channel as absent or
    present-without-version even though the application is healthy. Cold wake
    has the same observable result. Retry every report that is not four proven
    versions, then merge only compatible positive evidence. Absence requires
    two matching observations; a missing retry or conflicting version remains
    ambiguous and blocks live deployment.
    """

    def proven_version(entry: dict[str, Any]) -> str | None:
        if not entry.get("present"):
            return None
        version = entry.get("version")
        return version if fleet.parse_version(version or "") is not None else None

    expected_channels = {str(channel) for channel in CHANNELS}

    def report_has_four_proven_versions(report: dict[str, Any]) -> bool:
        channels = report.get("channels", {})
        return (
            report.get("state") == "complete"
            and set(channels) == expected_channels
            and all(
                proven_version(channels[channel]) is not None
                for channel in expected_channels
            )
        )

    retry_devices = sorted(
        device
        for device in devices
        if (report := initial_reports.get(device)) is None
        or not report_has_four_proven_versions(report)
    )
    effective = dict(initial_reports)
    if not retry_devices:
        return effective, None

    if delay_seconds:
        print(
            f"AMBIT inventory was inconclusive on {len(retry_devices)} gateway(s); "
            f"waiting {delay_seconds:g}s before a measurement-safe retry ..."
        )
        sleep(delay_seconds)
    retry_id = _new_id("ambit-preflight-retry", release_version)
    retry_reports, retry_error = versions_query(
        session, retry_devices, retry_id, wait_seconds
    )
    for device in retry_devices:
        initial = initial_reports.get(device)
        retry = retry_reports.get(device)
        if retry is None or retry.get("state") != "complete":
            effective[device] = {
                "state": "ambiguous",
                "channels": (initial or {}).get("channels", {}),
                "detail": "inventory retry did not return a complete report",
                "initial_state": initial.get("state") if initial else "no_reply",
                "retry_state": retry.get("state") if retry else "no_reply",
            }
            continue

        retry_channels = retry.get("channels", {})
        if initial is None or initial.get("state") != "complete":
            if report_has_four_proven_versions(retry):
                effective[device] = {
                    **retry,
                    "preflight_source_attempt": 2,
                    "absence_confirmed_after_retry": False,
                }
            else:
                effective[device] = {
                    "state": "ambiguous",
                    "channels": retry_channels,
                    "detail": (
                        "only the retry replied and one or more channels were "
                        "absent or unversioned"
                    ),
                    "initial_state": initial.get("state") if initial else "no_reply",
                    "retry_state": retry.get("state"),
                }
            continue

        initial_channels = initial.get("channels", {})
        merged: dict[str, dict[str, Any]] = {}
        conflicts: list[str] = []
        unknown: list[str] = []
        for channel in map(str, CHANNELS):
            first = initial_channels.get(channel, {"present": False, "version": None})
            second = retry_channels.get(channel, {"present": False, "version": None})
            known = [
                version
                for entry in (first, second)
                if (version := proven_version(entry)) is not None
            ]
            if len(set(known)) > 1:
                conflicts.append(channel)
                merged[channel] = {"present": True, "version": None}
            elif known:
                merged[channel] = {"present": True, "version": known[-1]}
            elif first.get("present") or second.get("present"):
                unknown.append(channel)
                merged[channel] = {"present": True, "version": None}
            else:
                # Two complete reports both observed absence.
                merged[channel] = {"present": False, "version": None}

        if conflicts or unknown:
            detail_parts = []
            if conflicts:
                detail_parts.append("conflicting versions on channels " + ",".join(conflicts))
            if unknown:
                detail_parts.append("unversioned present channels " + ",".join(unknown))
            effective[device] = {
                "state": "ambiguous",
                "channels": merged,
                "detail": "; ".join(detail_parts),
                "initial_state": initial.get("state"),
                "retry_state": retry.get("state"),
            }
        else:
            present = any(entry["present"] for entry in merged.values())
            effective[device] = {
                "state": "complete",
                "channels": merged,
                "preflight_source_attempt": 2,
                "absence_confirmed_after_retry": not present,
            }
    return effective, {
        "attempt": 2,
        "id": retry_id,
        "devices": retry_devices,
        "reports": retry_reports,
        "error": retry_error,
    }


def target_numeric_version(version: str) -> tuple[str, tuple[Any, ...]]:
    parsed = fleet.parse_version(version)
    if parsed is None:
        raise ValueError(f"unparseable AMBIT release version: {version!r}")
    numeric = ".".join(str(part) for part in parsed[0])
    numeric_parsed = fleet.parse_version(numeric)
    assert numeric_parsed is not None
    return numeric, numeric_parsed


def decide_gateways(
    cohort: list[str],
    reports: dict[str, dict[str, Any]],
    release_version: str,
    allow_downgrade: bool,
    force_reflash: bool = False,
) -> tuple[dict[str, dict[str, Any]], list[str]]:
    """Classify preflight inventory and return gateway decisions + deploy list."""
    numeric, target = target_numeric_version(release_version)
    decisions: dict[str, dict[str, Any]] = {}
    deploy: list[str] = []
    for device in cohort:
        report = reports.get(device)
        if report is None:
            decisions[device] = {
                "action": "skip",
                "skip_reason": "no_preflight_reply",
                "blocking": True,
            }
            continue
        if report.get("state") == "busy":
            decisions[device] = {
                "action": "skip",
                "skip_reason": "preflight_busy",
                "detail": report.get("detail"),
                "blocking": True,
            }
            continue
        if report.get("state") != "complete":
            decisions[device] = {
                "action": "skip",
                "skip_reason": "ambiguous_preflight",
                "detail": report.get("detail"),
                "blocking": True,
            }
            continue
        present = {
            channel: entry
            for channel, entry in report["channels"].items()
            if entry["present"]
        }
        if not present:
            decisions[device] = {"action": "skip", "skip_reason": "no_ambit_present"}
            continue

        current: list[str] = []
        older: list[str] = []
        newer: list[str] = []
        unknown: list[str] = []
        for channel, entry in present.items():
            parsed = fleet.parse_version(entry.get("version") or "")
            if parsed is None:
                unknown.append(channel)
                continue
            comparison = fleet.cmp_version(parsed, target)
            if comparison == 0:
                current.append(channel)
            elif comparison < 0:
                older.append(channel)
            else:
                newer.append(channel)

        base = {
            "target_numeric_version": numeric,
            "present_channels": sorted(present, key=int),
            "current_channels": sorted(current, key=int),
            "older_channels": sorted(older, key=int),
            "newer_channels": sorted(newer, key=int),
            "unknown_version_channels": sorted(unknown, key=int),
        }
        if unknown:
            decisions[device] = {
                **base,
                "action": "skip",
                "skip_reason": "unproven_ambit_version",
                "blocking": True,
            }
        elif newer and not allow_downgrade:
            decisions[device] = {
                **base,
                "action": "skip",
                "skip_reason": "newer_ambit_present",
            }
        elif not older and not (newer and allow_downgrade) and not force_reflash:
            decisions[device] = {
                **base,
                "action": "skip",
                "skip_reason": "all_ambits_up_to_date",
            }
        else:
            decisions[device] = {
                **base,
                "action": "deploy",
                "skip_reason": None,
                "force_reflash": force_reflash and not older and not newer,
                "reflashes_current_channels": bool(current),
                "downgrades_newer_channels": bool(newer),
                "version_effect_can_prove_change": bool(older or newer),
            }
            deploy.append(device)
    return decisions, deploy


class AmbitStatusTracker:
    """Correlate gateway, campaign, per-channel terminals, and overall terminal."""

    def __init__(self, devices: list[str], campaign_id: str):
        self._devices = set(devices)
        self._campaign_id = campaign_id
        self._records = {
            device: {
                "accepted": False,
                "channels": {str(channel): None for channel in CHANNELS},
                "overall": {"state": None, "detail": None},
            }
            for device in devices
        }

    def record(self, topic: str, payload: bytes | str) -> tuple[str, str] | None:
        if isinstance(payload, bytes):
            payload = payload.decode("utf-8", "replace")
        try:
            data = json.loads(payload)
        except (TypeError, json.JSONDecodeError):
            return None
        if not isinstance(data, dict) or data.get("type") != "ambit_ota_status":
            return None
        if data.get("id") != self._campaign_id:
            return None
        device = fleet.device_from_status_topic(topic)
        if device not in self._devices:
            return None
        channel = data.get("channel")
        state = data.get("state")
        record = self._records[device]
        if channel == OTA_CHANNEL_ALL:
            if state == "accepted":
                if record["accepted"] or record["overall"]["state"] is not None:
                    return None
                record["accepted"] = True
                return device, "accepted"
            if state not in {"success", "failed", "busy"} or record["overall"]["state"] is not None:
                return None
            record["overall"] = {
                "state": state,
                "detail": data.get("detail") if isinstance(data.get("detail"), str) else None,
            }
            if state == "success":
                record["accepted"] = True
            return device, f"overall_{state}"
        if channel not in CHANNELS or state not in AMBIT_OTA_TERMINAL_STATES:
            return None
        key = str(channel)
        if record["channels"][key] is not None:
            return None
        record["channels"][key] = {
            "state": state,
            "detail": data.get("detail") if isinstance(data.get("detail"), str) else None,
        }
        if state in {"success", "failed"}:
            record["accepted"] = True
        return device, f"channel_{channel}_{state}"

    def any_accepted(self) -> bool:
        return any(record["accepted"] for record in self._records.values())

    def complete(self) -> bool:
        return all(
            record["overall"]["state"] in {"failed", "busy"}
            or (
                record["overall"]["state"] == "success"
                and all(value is not None for value in record["channels"].values())
            )
            for record in self._records.values()
        )

    def results(self) -> dict[str, dict[str, Any]]:
        result = json.loads(json.dumps(self._records))
        for record in result.values():
            for key, value in record["channels"].items():
                if value is None:
                    record["channels"][key] = {"state": "timeout", "detail": None}
            if record["overall"]["state"] is None:
                record["overall"] = {"state": "timeout", "detail": None}
        return result


def fleet_ambit_ota(
    session: Any,
    devices: list[str],
    campaign_id: str,
    image_url: str,
    ack_seconds: int,
    final_seconds: int,
    batch_size: int,
    stagger_seconds: int,
    *,
    connection_factory: Callable[..., Any] = fleet.mqtt_connection,
) -> tuple[dict[str, dict[str, Any]], str | None]:
    """Send one all-channel command/gateway and preserve complete partial results."""
    from awscrt import mqtt

    command = {
        "type": "ambit_ota",
        "id": campaign_id,
        "channel": "all",
        "url": image_url,
    }
    payload = json.dumps(command, separators=(",", ":"))
    received: queue.Queue[tuple[str, bytes | str]] = queue.Queue()
    tracker = AmbitStatusTracker(devices, campaign_id)
    connection = None
    error = None

    def drain() -> None:
        while True:
            try:
                topic, message = received.get_nowait()
            except queue.Empty:
                return
            tracker.record(topic, message)

    try:
        connection = connection_factory(
            session,
            fleet.STATUS_TOPIC,
            lambda topic, payload, dup, qos, retain, **kwargs: received.put(
                (topic, payload)
            ),
            client_id="fleet-deploy-ambit",
        )
        publications = []
        for index, device in enumerate(devices):
            publication, _ = connection.publish(
                topic=fleet.COMMAND_TOPIC_FMT.format(device=device),
                payload=payload,
                qos=mqtt.QoS.AT_LEAST_ONCE,
                retain=False,
            )
            publications.append(publication)
            if (
                batch_size
                and (index + 1) % batch_size == 0
                and (index + 1) < len(devices)
            ):
                for pending in publications:
                    pending.result()
                publications = []
                print(
                    f"  ...fanned out {index + 1}/{len(devices)}, "
                    f"pausing {stagger_seconds}s"
                )
                time.sleep(stagger_seconds)
        for pending in publications:
            pending.result()
        print(f"Fanned out AMBIT campaign {campaign_id!r} to {len(devices)} gateway(s)")

        start = time.time()
        ack_deadline = start + ack_seconds
        final_deadline = start + final_seconds
        while time.time() < final_deadline:
            if tracker.complete():
                break
            if time.time() >= ack_deadline and not tracker.any_accepted():
                break
            try:
                topic, message = received.get(timeout=0.2)
            except queue.Empty:
                continue
            event = tracker.record(topic, message)
            if event is not None:
                print(f"  {event[1]:<20} {event[0]}")
    except Exception as exc:
        error = _redact_text(f"{type(exc).__name__}: {exc}")
    finally:
        drain()
        if connection is not None:
            try:
                connection.disconnect().result()
            except Exception:
                pass
    return tracker.results(), error


def _redact_text(value: str) -> str:
    value = re.sub(
        r"(?i)\b(token|secret|password|authorization|x-amz-signature)"
        r"\s*[=:]\s*[^\s,;&]+",
        r"\1=<redacted>",
        value,
    )
    return re.sub(r"(https?://[^\s?]+)\?[^\s]+", r"\1?<redacted>", value)


def _redact(value: Any) -> Any:
    if isinstance(value, str):
        return _redact_text(value)
    if isinstance(value, dict):
        return {str(key): _redact(item) for key, item in value.items()}
    if isinstance(value, list):
        return [_redact(item) for item in value]
    return value


def write_results(path: str | None, plan: dict[str, Any]) -> None:
    if path:
        Path(path).write_text(
            json.dumps(_redact(plan), indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )


def _expected_present_channels(
    decisions: dict[str, dict[str, Any]], device: str
) -> set[str]:
    return set(decisions[device].get("present_channels", []))


def assess_post_verification(
    deployed: list[str],
    decisions: dict[str, dict[str, Any]],
    terminal_results: dict[str, dict[str, Any]],
    verification_reports: dict[str, dict[str, Any]],
    expected_numeric_version: str,
) -> tuple[dict[str, dict[str, Any]], list[str]]:
    """Combine terminal reports with an independent verify-by-effect sweep.

    Version effect can rescue missing/drop-report terminal evidence only when a
    channel was expected to change numeric version. It can never erase an
    explicit channel/overall failure. A forced same-version reflash has no
    observable version effect, so it additionally requires complete successful
    terminal reporting.
    """
    expected = fleet.parse_version(expected_numeric_version)
    if expected is None:  # defensive; target_numeric_version already validated
        raise ValueError(f"invalid expected numeric version: {expected_numeric_version}")

    assessments: dict[str, dict[str, Any]] = {}
    failures: list[str] = []
    for device in deployed:
        decision = decisions[device]
        expected_present = _expected_present_channels(decisions, device)
        terminal = terminal_results.get(device, {})
        terminal_channels = terminal.get("channels", {})
        overall_state = terminal.get("overall", {}).get("state") or "timeout"

        explicit_terminal_failures: list[str] = []
        if overall_state in {"failed", "busy"}:
            explicit_terminal_failures.append(f"overall_{overall_state}")
        for channel, channel_result in terminal_channels.items():
            if channel_result.get("state") == "failed":
                explicit_terminal_failures.append(f"channel_{channel}_failed")
        for channel in sorted(expected_present, key=int):
            state = terminal_channels.get(channel, {}).get("state") or "timeout"
            if state == "absent":
                explicit_terminal_failures.append(f"channel_{channel}_absent")

        terminal_complete_success = (
            overall_state == "success"
            and all(
                terminal_channels.get(str(channel), {}).get("state")
                in {"success", "absent"}
                for channel in CHANNELS
            )
            and all(
                terminal_channels.get(channel, {}).get("state") == "success"
                for channel in expected_present
            )
        )

        report = verification_reports.get(device)
        actual_channels = report.get("channels", {}) if report else {}
        matching: list[str] = []
        mismatches: list[dict[str, Any]] = []
        unknown: list[str] = []
        actual_present: set[str] = set()
        for channel, entry in actual_channels.items():
            if not entry.get("present"):
                continue
            actual_present.add(channel)
            parsed = fleet.parse_version(entry.get("version") or "")
            if parsed is None:
                unknown.append(channel)
            elif fleet.cmp_version(parsed, expected) == 0:
                matching.append(channel)
            else:
                mismatches.append(
                    {"channel": channel, "actual": entry.get("version")}
                )
        missing_expected = sorted(expected_present - actual_present, key=int)

        if report is None or report.get("state") != "complete":
            version_effect = "indeterminate"
            effect_reason = (
                "no_post_verify_reply"
                if report is None
                else f"post_verify_{report.get('state') or 'invalid'}"
            )
        elif missing_expected:
            version_effect = "failed"
            effect_reason = "preflight_present_channel_missing_after_ota"
        elif mismatches:
            version_effect = "failed"
            effect_reason = "post_verify_version_mismatch"
        elif unknown:
            version_effect = "indeterminate"
            effect_reason = "post_verify_version_unproven"
        elif not actual_present:
            version_effect = "failed"
            effect_reason = "no_ambit_present_after_ota"
        else:
            version_effect = "confirmed"
            effect_reason = None

        can_prove_change = bool(decision.get("version_effect_can_prove_change"))
        reasons: list[str] = []
        if explicit_terminal_failures:
            outcome = "failed"
            reasons.extend(explicit_terminal_failures)
        elif version_effect == "failed":
            outcome = "failed"
            reasons.append(effect_reason or "version_effect_failed")
        elif version_effect == "confirmed" and (
            can_prove_change or terminal_complete_success
        ):
            outcome = "confirmed_success"
        else:
            outcome = "indeterminate"
            reasons.append(effect_reason or "same_version_reflash_not_proven")
            if not terminal_complete_success:
                reasons.append("terminal_evidence_incomplete")

        assessments[device] = {
            "expected_numeric_version": expected_numeric_version,
            "expected_present_channels": sorted(expected_present, key=int),
            "actual_channels": actual_channels,
            "matching_present_channels": sorted(matching, key=int),
            "mismatched_present_channels": mismatches,
            "unknown_version_channels": sorted(unknown, key=int),
            "missing_expected_channels": missing_expected,
            "terminal_complete_success": terminal_complete_success,
            "explicit_terminal_failures": explicit_terminal_failures,
            "version_effect_can_prove_change": can_prove_change,
            "version_effect": version_effect,
            "outcome": outcome,
            "reasons": reasons,
        }
        if outcome != "confirmed_success":
            failures.append(f"{device}:{outcome}:" + ",".join(reasons))
    return assessments, failures


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--repo", default=AMBIT_REPOSITORY)
    parser.add_argument("--tag", required=True)
    parser.add_argument(
        "--version-op", choices=["any", *sorted(fleet.VERSION_OPS)], default="any"
    )
    parser.add_argument("--version")
    parser.add_argument("--percentage", type=int, default=100)
    parser.add_argument("--devices")
    parser.add_argument("--window-minutes", type=int, default=1440)
    parser.add_argument("--profile")
    parser.add_argument("--region", default="eu-central-1")
    parser.add_argument("--ping-wait", type=int, default=25)
    parser.add_argument("--preflight-seconds", type=int, default=90)
    parser.add_argument(
        "--absent-retry-delay-seconds",
        type=float,
        default=65,
        help="bounded delay before retrying cold-wake or measurement-busy inventory",
    )
    parser.add_argument("--verify-seconds", type=int, default=120)
    parser.add_argument("--ack-seconds", type=int, default=90)
    parser.add_argument(
        "--final-seconds",
        type=int,
        default=3600,
        help=(
            "covers 0..899 s jitter, degraded HTTPS/SD, four sequential "
            "channels, and MQTT recovery"
        ),
    )
    parser.add_argument("--batch", type=int, default=10)
    parser.add_argument("--stagger", type=int, default=30)
    parser.add_argument("--allow-downgrade", action="store_true")
    parser.add_argument("--allow-prerelease", action="store_true")
    parser.add_argument("--force-reflash", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--results-json")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if not 1 <= args.percentage <= 100:
        parser.error("--percentage must be 1-100")
    if (
        args.final_seconds <= 0
        or args.verify_seconds <= 0
        or args.preflight_seconds <= 0
        or args.absent_retry_delay_seconds < 0
    ):
        parser.error(
            "--final-seconds, --verify-seconds, and --preflight-seconds must be positive; "
            "--absent-retry-delay-seconds must be non-negative"
        )
    if args.version_op != "any" and fleet.parse_version(args.version or "") is None:
        parser.error(f"--version-op {args.version_op} needs a parseable --version")

    if args.force_reflash and (not args.devices or args.percentage != 100):
        error = (
            "--force-reflash requires an explicit --devices list and "
            "--percentage 100; discovered or percentage cohorts are forbidden"
        )
        write_results(
            args.results_json,
            {
                "kind": "ambit",
                "requested_tag": args.tag,
                "force_reflash": True,
                "dry_run": args.dry_run,
                "results": {},
                "error": error,
            },
        )
        fleet.write_summary(
            os.environ.get("GITHUB_STEP_SUMMARY"),
            ["## Fleet deploy (AMBIT): unsafe force-reflash request", "", f"**{error}**"],
        )
        print(error, file=sys.stderr)
        return 2

    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    print(f"Resolving and anonymously verifying AMBIT {args.tag} ...")
    try:
        manifest, _image, release_proof = fetch_release(
            args.repo, args.tag, allow_prerelease=args.allow_prerelease
        )
    except (ReleaseError, OSError, RuntimeError) as exc:
        error = _redact_text(str(exc))
        write_results(
            args.results_json,
            {
                "kind": "ambit",
                "requested_tag": args.tag,
                "dry_run": args.dry_run,
                "release_proof": None,
                "results": {},
                "error": error,
            },
        )
        fleet.write_summary(
            summary_path,
            ["## Fleet deploy (AMBIT): validation failed", "", f"**{error}**"],
        )
        print(f"Release validation failed: {error}", file=sys.stderr)
        return 2
    target_version, _ = target_numeric_version(manifest["version"])
    print(
        f"  {manifest['version']} -> device version {target_version}; "
        f"{manifest['size']} bytes, sha256={manifest['sha256']}"
    )

    # Persist the verified supply-chain proof before touching AWS. If discovery
    # or MQTT setup fails unexpectedly, the always-uploaded artifact still says
    # exactly which public bytes were selected instead of falling back to the
    # composite action's generic placeholder.
    base_result: dict[str, Any] = {
        "kind": "ambit",
        "tag": args.tag,
        "target_numeric_version": target_version,
        "release_proof": release_proof,
        "allow_prerelease": args.allow_prerelease,
        "allow_downgrade": args.allow_downgrade,
        "force_reflash": args.force_reflash,
        "dry_run": args.dry_run,
        "results": {},
        "error": "targeting did not start",
    }
    write_results(args.results_json, base_result)

    try:
        session = fleet.boto_session(args.profile, args.region)
        if args.devices:
            universe = parse_devices(args.devices)
            print(f"Explicit device list: {len(universe)} gateway(s)")
        else:
            print(
                f"Discovering gateways active in the last {args.window_minutes} min ..."
            )
            universe = fleet.discover_active_devices(session, args.window_minutes)
            print(f"  {len(universe)} active gateway(s)")

        firmware_by_device: dict[str, str | None] = {}
        selected = {"matching": [], "unproven": [], "cohort": []}
        preflight_id = _new_id("ambit-preflight", manifest["version"])
        reports: dict[str, dict[str, Any]] = {}
        preflight_error = None
        preflight_attempts: list[dict[str, Any]] = []
        if universe:
            print(f"Pinging {len(universe)} gateway(s) (up to {args.ping_wait}s) ...")
            firmware_by_device = fleet.fleet_ping(session, universe, args.ping_wait)
            selected = select_cohort(
                universe,
                firmware_by_device,
                args.version_op,
                args.version,
                args.percentage,
            )
            cohort = selected["cohort"]
            if cohort:
                print(
                    f"Querying AMBIT versions on {len(cohort)} selected gateway(s) ..."
                )
                initial_reports, preflight_error = fleet_ambit_versions(
                    session, cohort, preflight_id, args.preflight_seconds
                )
                preflight_attempts.append(
                    {
                        "attempt": 1,
                        "id": preflight_id,
                        "devices": cohort,
                        "reports": initial_reports,
                        "error": preflight_error,
                    }
                )
                reports, retry_attempt = retry_absent_preflight(
                    session,
                    cohort,
                    initial_reports,
                    manifest["version"],
                    args.preflight_seconds,
                    args.absent_retry_delay_seconds,
                    versions_query=fleet_ambit_versions,
                    sleep=time.sleep,
                )
                if retry_attempt is not None:
                    preflight_attempts.append(retry_attempt)
                    retry_error = retry_attempt["error"]
                    if retry_error:
                        preflight_error = (
                            f"{preflight_error}; retry: {retry_error}"
                            if preflight_error
                            else f"retry: {retry_error}"
                        )
    except Exception as exc:  # preserve the verified release proof on AWS/MQTT failures
        error = _redact_text(f"{type(exc).__name__}: {exc}")
        failure = {**base_result, "error": error}
        write_results(args.results_json, failure)
        fleet.write_summary(
            summary_path,
            ["## Fleet deploy (AMBIT): targeting failed", "", f"**{error}**"],
        )
        print(f"Targeting failed: {error}", file=sys.stderr)
        return 1
    cohort = selected["cohort"]
    decisions, to_deploy = decide_gateways(
        cohort,
        reports,
        manifest["version"],
        args.allow_downgrade,
        args.force_reflash,
    )
    campaign_id = _new_id("ambit-ota", manifest["version"])
    blocking_preflight = sorted(
        device
        for device, decision in decisions.items()
        if decision.get("blocking")
    )

    reachable = len(reports)
    present_gateways = sum(
        1
        for report in reports.values()
        if any(entry["present"] for entry in report.get("channels", {}).values())
    )
    present_channels = sum(
        1
        for report in reports.values()
        for entry in report.get("channels", {}).values()
        if entry["present"]
    )
    print(
        f"\nPlan: AMBIT {manifest['version']} campaign {campaign_id!r}\n"
        f"  universe {len(universe)} | matching {len(selected['matching'])} | "
        f"cohort {len(cohort)} | preflight replies {reachable} | "
        f"present channels {present_channels} | deploying gateways {len(to_deploy)}"
    )
    for device in cohort:
        decision = decisions[device]
        print(
            f"    {device}  {decision['action']}"
            + (f" ({decision['skip_reason']})" if decision.get("skip_reason") else "")
        )

    plan: dict[str, Any] = {
        "kind": "ambit",
        "tag": args.tag,
        "target_numeric_version": target_version,
        "release_proof": release_proof,
        "preflight_id": preflight_id,
        "campaign_id": campaign_id,
        "command": {
            "type": "ambit_ota",
            "id": campaign_id,
            "channel": "all",
            "url": release_proof["application"]["url"],
        },
        "allow_prerelease": args.allow_prerelease,
        "allow_downgrade": args.allow_downgrade,
        "force_reflash": args.force_reflash,
        "version_op": args.version_op,
        "version": args.version,
        "percentage": args.percentage,
        "dry_run": args.dry_run,
        "discovery_window_minutes": args.window_minutes,
        "universe": universe,
        "matching": selected["matching"],
        "cohort": cohort,
        "unproven_gateway_firmware": selected["unproven"],
        "silent_on_ping": sorted(d for d in universe if d not in firmware_by_device),
        "gateway_firmware": firmware_by_device,
        "preflight_versions": reports,
        "preflight_attempts": preflight_attempts,
        "absent_retry_delay_seconds": args.absent_retry_delay_seconds,
        "preflight_error": preflight_error,
        "preflight_summary": {
            "reachable_gateways": reachable,
            "gateways_with_ambit": present_gateways,
            "present_channels": present_channels,
        },
        "gateway_decisions": decisions,
        "deployed_gateways": to_deploy,
        "terminal_observation_seconds": args.final_seconds,
        "post_verify_wait_seconds": args.verify_seconds,
        "post_verify_id": None,
        "post_verify_versions": {},
        "post_verify_attempts": [],
        "post_verify_error": None,
        "verification": {},
        "results": {},
        "error": preflight_error,
    }

    exit_code = 0
    if not universe:
        plan["error"] = "no gateways selected: discovery/explicit device universe is empty"
        exit_code = 1
    elif not cohort:
        plan["error"] = "no gateways selected: gateway firmware filter produced an empty cohort"
        exit_code = 1
    elif blocking_preflight:
        plan["error"] = (
            "preflight is ambiguous or unproven for selected gateways: "
            + ", ".join(blocking_preflight)
        )
        exit_code = 1
    elif args.dry_run:
        print(
            "\nDRY RUN: ping and ambit_versions preflight commands were published; "
            "no ambit_ota command was published."
        )
        if preflight_error:
            exit_code = 1
    elif preflight_error:
        exit_code = 1
    elif reachable == 0:
        plan["error"] = "live deployment has zero preflight-reachable gateways"
        exit_code = 1
    elif present_channels == 0:
        plan["error"] = "live deployment has zero present AMBIT channels"
        exit_code = 1
    elif to_deploy:
        print(f"\nDeploying through {len(to_deploy)} gateway(s) ...")
        results, error = fleet_ambit_ota(
            session,
            to_deploy,
            campaign_id,
            release_proof["application"]["url"],
            args.ack_seconds,
            args.final_seconds,
            args.batch,
            args.stagger,
        )
        plan["results"] = results
        plan["tracking_error"] = error

        # Terminal reports are best-effort after a long MQTT blackout. Always
        # verify every executed gateway by effect with a fresh correlated
        # versions sweep, even when tracking timed out or returned an error.
        post_verify_id = _new_id("ambit-verify", manifest["version"])
        print(
            f"\nVerifying AMBIT versions on {len(to_deploy)} executed gateway(s) ..."
        )
        initial_post_reports, post_error = fleet_ambit_versions(
            session, to_deploy, post_verify_id, args.verify_seconds
        )
        post_attempts = [
            {
                "attempt": 1,
                "id": post_verify_id,
                "devices": to_deploy,
                "reports": initial_post_reports,
                "error": post_error,
            }
        ]
        post_reports, post_retry_attempt = retry_absent_preflight(
            session,
            to_deploy,
            initial_post_reports,
            manifest["version"],
            args.verify_seconds,
            args.absent_retry_delay_seconds,
            versions_query=fleet_ambit_versions,
            sleep=time.sleep,
        )
        if post_retry_attempt is not None:
            post_attempts.append(post_retry_attempt)
            post_retry_error = post_retry_attempt["error"]
            if post_retry_error:
                post_error = (
                    f"{post_error}; retry: {post_retry_error}"
                    if post_error
                    else f"retry: {post_retry_error}"
                )
        verification, failures = assess_post_verification(
            to_deploy,
            decisions,
            results,
            post_reports,
            target_version,
        )
        plan["post_verify_id"] = post_verify_id
        plan["post_verify_versions"] = post_reports
        plan["post_verify_attempts"] = post_attempts
        plan["post_verify_error"] = post_error
        plan["verification"] = verification
        plan["live_failures"] = failures
        if failures:
            plan["error"] = "one or more executed gateways failed or remain indeterminate"
            exit_code = 1
        else:
            # A version-effect confirmation can safely recover dropped terminal
            # reports. Preserve the transport diagnostics without making the
            # successfully verified campaign look failed.
            plan["error"] = None
    else:
        if all(
            decision.get("skip_reason") == "all_ambits_up_to_date"
            for decision in decisions.values()
        ):
            print("\nAll selected AMBIT channels are already up to date.")
        else:
            print("\nNothing eligible to deploy; policy skip reasons are recorded.")

    write_results(args.results_json, plan)
    summary = [
        f"## Fleet deploy (AMBIT): {args.tag} "
        f"({'dry run' if args.dry_run else 'live'})",
        "",
        f"- Immutable public release: `{release_proof['release_id']}`",
        f"- Application: `{manifest['app_name']}` at `{manifest['offset']}` "
        f"(`{manifest['sha256']}`)",
        f"- Target device version: `{target_version}`",
        f"- Terminal observation: **{args.final_seconds} s**; "
        f"post-OTA verification: **{args.verify_seconds} s**",
        f"- Force reflash: **{args.force_reflash}**",
        f"- Universe {len(universe)} -> matching {len(selected['matching'])} -> "
        f"cohort {len(cohort)} -> preflight reachable {reachable} -> "
        f"present AMBIT channels {present_channels} -> deploying gateways {len(to_deploy)}",
        "",
        "| gateway | Ambyte fw | decision | skip reason | AMBIT channels before "
        "| overall | verify-by-effect |",
        "|---|---|---|---|---|---|---|",
    ]
    for device in cohort:
        decision = decisions[device]
        channels = reports.get(device, {}).get("channels", {})
        versions = ", ".join(
            f"{channel}:"
            f"{entry.get('version') or ('present/unknown' if entry['present'] else 'absent')}"
            for channel, entry in sorted(channels.items(), key=lambda item: int(item[0]))
        )
        overall = plan["results"].get(device, {}).get("overall", {}).get("state", "")
        verified = plan["verification"].get(device, {}).get("outcome", "")
        summary.append(
            f"| {device} | {firmware_by_device.get(device) or 'silent'} | "
            f"{decision['action']} | {decision.get('skip_reason') or ''} | "
            f"{versions} | {overall} | {verified} |"
        )
    if plan["error"]:
        summary.extend(["", f"**Deployment error: {_redact_text(plan['error'])}**"])
    fleet.write_summary(summary_path, summary)
    if exit_code:
        print(f"\nAMBIT deployment failed closed: {plan['error'] or plan.get('live_failures')}")
    else:
        print("\nDone.")
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
