#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Targeted deployment of an immutable Schedule release to the Ambyte fleet.

The selected release manifest and Schedule asset are downloaded and verified before
the tool opens an MQTT connection. Device discovery, normalization, firmware
ping/version handling, deterministic cohort ordering, and AWS IoT WebSocket
setup deliberately reuse :mod:`fleet_deploy`.
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
import urllib.request
from pathlib import Path
from typing import Any, Callable

try:  # Package import in tests; sibling import when invoked as a script.
    from . import fleet_deploy as fleet
    from .release_selection import SCHEDULE_TAG_RE
except ImportError:  # pragma: no cover - exercised by CLI invocation
    import fleet_deploy as fleet
    from release_selection import SCHEDULE_TAG_RE


MANIFEST_SCHEMA_VERSION = 1
SCHEDULE_MAX_BYTES = 16 * 1024  # SCHED_YAML_MAX_FILE_BYTES on the device
REPOSITORY_RE = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
SCRIPT_NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]*$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
SCRIPT_TERMINAL_STATES = {"applied", "failed", "busy"}
STATUS_FIELDS = (
    "detail",
    "app_version",
    "script_sha256",
    "script_version",
    "script_built_against_fw",
    "script_installed_on_fw",
    "script_metadata_verified",
)


class ManifestError(ValueError):
    """A Schedule release manifest does not satisfy the schema-1 contract."""


def _release_url(repository: str, tag: str, asset: str) -> str:
    return f"https://github.com/{repository}/releases/download/{tag}/{asset}"


def _require_string(obj: dict[str, Any], field: str) -> str:
    value = obj.get(field)
    if not isinstance(value, str) or not value:
        raise ManifestError(f"manifest field {field!r} must be a non-empty string")
    return value


def validate_manifest(
    manifest: Any, repository: str, tag: str, script_name: str = "default"
) -> dict[str, Any]:
    """Validate schema, release identity, immutable URL, and command equality."""
    if not REPOSITORY_RE.fullmatch(repository):
        raise ManifestError(f"invalid GitHub repository: {repository!r}")
    if SCRIPT_NAME_RE.fullmatch(script_name) is None:
        raise ManifestError(f"invalid Schedule script name: {script_name!r}")
    tag_match = SCHEDULE_TAG_RE.fullmatch(tag)
    if tag_match is None:
        raise ManifestError(f"Schedule release tag must match schedule-vX.Y.Z: {tag!r}")
    if not isinstance(manifest, dict):
        raise ManifestError("manifest root must be a JSON object")
    if manifest.get("schema_version") != MANIFEST_SCHEMA_VERSION:
        raise ManifestError(
            f"unsupported manifest schema_version: {manifest.get('schema_version')!r}"
        )

    version = _require_string(manifest, "script_version")
    if version != tag.removeprefix("schedule-v"):
        raise ManifestError(
            f"manifest script_version {version!r} does not match tag {tag!r}"
        )
    if _require_string(manifest, "tag") != tag:
        raise ManifestError("manifest tag does not match the requested release tag")
    manifest_script_name = manifest.get("script_name")
    if manifest_script_name is None and script_name == "default":
        pass  # Schema-1 manifests from before the script catalog.
    elif manifest_script_name != script_name:
        raise ManifestError("manifest script_name does not match the requested script")

    digest = _require_string(manifest, "sha256")
    if SHA256_RE.fullmatch(digest) is None:
        raise ManifestError("manifest sha256 must be 64 lowercase hexadecimal digits")
    size = manifest.get("size_bytes")
    if isinstance(size, bool) or not isinstance(size, int) or size < 0:
        raise ManifestError("manifest size_bytes must be a non-negative integer")
    built_against = _require_string(manifest, "built_against_fw")

    asset_name = f"{script_name}.yaml"
    expected_asset_url = _release_url(repository, tag, asset_name)
    if _require_string(manifest, "asset_url") != expected_asset_url:
        raise ManifestError(
            f"manifest asset_url is not the immutable {asset_name} URL for the requested release"
        )

    command = manifest.get("script_update")
    if not isinstance(command, dict):
        raise ManifestError("manifest script_update must be a JSON object")
    campaign_id = tag if script_name == "default" else f"{tag}:{script_name}"
    expected_command = {
        "type": "script_update",
        "id": campaign_id,
        "url": expected_asset_url,
        "checksum": digest,
        "script_version": version,
        "built_against_fw": built_against,
    }
    if command != expected_command:
        raise ManifestError("manifest script_update does not match the release identity")

    # Return a detached object so callers never mutate data owned by a fixture or
    # decoder while adding the per-run reboot setting.
    return json.loads(json.dumps(manifest))


def _download_bytes(url: str, *, max_bytes: int) -> bytes:
    request = urllib.request.Request(
        url, headers={"User-Agent": "ambyte-schedule-deploy"}
    )
    with urllib.request.urlopen(request, timeout=30) as response:
        length = response.headers.get("Content-Length")
        if length is not None and int(length) > max_bytes:
            raise RuntimeError(f"release asset exceeds {max_bytes} bytes")
        body = response.read(max_bytes + 1)
    if len(body) > max_bytes:
        raise RuntimeError(f"release asset exceeds {max_bytes} bytes")
    return body


def fetch_release(
    repository: str,
    tag: str,
    script_name: str = "default",
    *,
    downloader: Callable[..., bytes] = _download_bytes,
) -> tuple[dict[str, Any], bytes]:
    """Download and fully verify the release manifest and referenced asset."""
    # Validate the caller-controlled URL pieces before constructing any request.
    if not REPOSITORY_RE.fullmatch(repository):
        raise ManifestError(f"invalid GitHub repository: {repository!r}")
    if SCHEDULE_TAG_RE.fullmatch(tag) is None:
        raise ManifestError(f"Schedule release tag must match schedule-vX.Y.Z: {tag!r}")
    if SCRIPT_NAME_RE.fullmatch(script_name) is None:
        raise ManifestError(f"invalid Schedule script name: {script_name!r}")

    asset_name = f"{script_name}.yaml"
    manifest_url = _release_url(repository, tag, f"{asset_name}.manifest.json")
    raw_manifest = downloader(manifest_url, max_bytes=64 * 1024)
    try:
        decoded = json.loads(raw_manifest.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ManifestError("release manifest is not valid UTF-8 JSON") from exc
    manifest = validate_manifest(decoded, repository, tag, script_name)

    asset = downloader(manifest["asset_url"], max_bytes=SCHEDULE_MAX_BYTES)
    expected_size = manifest["size_bytes"]
    if len(asset) != expected_size:
        raise ManifestError(
            f"{asset_name} byte count mismatch: got {len(asset)}, expected {expected_size}"
        )
    digest = hashlib.sha256(asset).hexdigest()
    if digest != manifest["sha256"]:
        raise ManifestError(
            f"{asset_name} SHA-256 mismatch: got {digest}, expected {manifest['sha256']}"
        )
    return manifest, asset


def parse_devices(value: str) -> list[str]:
    """Normalize a comma/whitespace-separated exact-device list."""
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
    return fleet.unique_devices(devices)


def select_cohort(
    universe: list[str],
    firmware_by_device: dict[str, str | None],
    version_op: str,
    version: str | None,
    percentage: int,
) -> dict[str, Any]:
    """Apply firmware predicate and stable percentage selection."""
    if not 1 <= percentage <= 100:
        raise ValueError("percentage must be 1-100")
    if version_op not in {"any", *fleet.VERSION_OPS}:
        raise ValueError(f"unknown firmware version predicate: {version_op}")

    unproven: list[str] = []
    if version_op == "any":
        matching = list(universe)
    else:
        reference = fleet.parse_version(version or "")
        if reference is None:
            raise ValueError(
                f"firmware predicate {version_op!r} requires a parseable version"
            )
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
    cohort = sorted(matching[:take])
    return {"matching": matching, "unproven": sorted(unproven), "cohort": cohort}


class ScriptStatusTracker:
    """Correlate status-topic device, campaign ID, and first terminal report."""

    def __init__(self, devices: list[str], campaign_id: str):
        self._devices = list(devices)
        self._devices_by_identity = fleet.device_index(devices)
        self._campaign_id = campaign_id
        self._accepted = {device: False for device in devices}
        self._terminal: dict[str, dict[str, Any] | None] = {
            device: None for device in devices
        }

    def record(self, topic: str, payload: bytes | str) -> tuple[str, str] | None:
        if isinstance(payload, bytes):
            payload = payload.decode("utf-8", "replace")
        try:
            data = json.loads(payload)
        except (TypeError, json.JSONDecodeError):
            return None
        if not isinstance(data, dict) or data.get("type") != "script_status":
            return None
        if data.get("id") != self._campaign_id:
            return None
        device = fleet.requested_device_from_status_topic(
            topic, self._devices_by_identity
        )
        if device is None:
            return None
        state = data.get("state")
        if state == "accepted":
            if self._terminal[device] is not None or self._accepted[device]:
                return None
            self._accepted[device] = True
            return device, state
        if state not in SCRIPT_TERMINAL_STATES:
            return None
        if self._terminal[device] is not None:
            return None

        report = {"state": state}
        for field in STATUS_FIELDS:
            value = data.get(field)
            if isinstance(value, (str, bool, int, float)) or value is None:
                report[field] = value
        self._terminal[device] = report
        if state in {"applied", "failed"}:
            self._accepted[device] = True
        return device, state

    def any_accepted(self) -> bool:
        return any(self._accepted.values())

    def any_missing_terminal(self) -> bool:
        return any(value is None for value in self._terminal.values())

    def results(self) -> dict[str, dict[str, Any]]:
        result: dict[str, dict[str, Any]] = {}
        for device in sorted(self._devices):
            terminal = self._terminal[device] or {}
            result[device] = {"accepted": self._accepted[device], **terminal}
            result[device].setdefault("state", None)
        return result


def _redact_text(value: str) -> str:
    """Remove URL queries and common credential assignments from diagnostics."""
    value = re.sub(
        r"(?i)\b(token|secret|password|authorization|x-amz-signature)"
        r"\s*[=:]\s*[^\s,;&]+",
        r"\1=<redacted>",
        value,
    )
    return re.sub(r"(https?://[^\s?]+)\?[^\s]+", r"\1?<redacted>", value)


def redact(value: Any) -> Any:
    if isinstance(value, str):
        return _redact_text(value)
    if isinstance(value, dict):
        return {str(key): redact(item) for key, item in value.items()}
    if isinstance(value, list):
        return [redact(item) for item in value]
    return value


def fleet_script_update(
    session: Any,
    devices: list[str],
    command: dict[str, Any],
    ack_seconds: int,
    final_seconds: int,
    batch_size: int,
    stagger_seconds: int,
    *,
    connection_factory: Callable[..., Any] = fleet.mqtt_connection,
) -> tuple[dict[str, dict[str, Any]], str | None]:
    """Publish one verified command per device and preserve partial outcomes."""
    from awscrt import mqtt

    campaign_id = command["id"]
    payload = json.dumps(command, separators=(",", ":"))
    received: queue.Queue[tuple[str, bytes | str]] = queue.Queue()
    tracker = ScriptStatusTracker(devices, campaign_id)
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
            client_id="fleet-deploy-schedule",
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

        print(f"Fanned out campaign {campaign_id!r} to {len(devices)} device(s)")
        start = time.time()
        ack_deadline = start + ack_seconds
        final_deadline = start + final_seconds
        while time.time() < final_deadline:
            if not tracker.any_missing_terminal():
                break
            if time.time() >= ack_deadline and not tracker.any_accepted():
                break
            try:
                topic, message = received.get(timeout=0.2)
            except queue.Empty:
                continue
            event = tracker.record(topic, message)
            if event is not None:
                device, state = event
                print(f"  {state:<8} {device}")
    except Exception as exc:  # commands may already be live; never discard partials
        error = _redact_text(f"{type(exc).__name__}: {exc}")
        print(f"  ERROR mid-campaign: {error}")
    finally:
        # Status callbacks can race a failed publish future. Preserve anything
        # already received before reporting the connection/publish error.
        drain()
        if connection is not None:
            try:
                connection.disconnect().result()
            except Exception:
                pass

    return tracker.results(), error


def classify(record: dict[str, Any], expected_sha256: str | None = None) -> str:
    state = record.get("state")
    if state == "applied" and expected_sha256 is not None:
        reported = record.get("script_sha256")
        if not isinstance(reported, str) or reported.lower() != expected_sha256.lower():
            return "applied (sha mismatch)"
    if state in SCRIPT_TERMINAL_STATES:
        return str(state)
    if record.get("accepted"):
        return "accepted"
    return "no_reply"


def write_results(path: str | None, plan: dict[str, Any]) -> None:
    if not path:
        return
    Path(path).write_text(
        json.dumps(redact(plan), indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def write_summary(path: str | None, lines: list[str]) -> None:
    fleet.write_summary(path, [_redact_text(line) for line in lines])


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--repo", default="Jan-IngenHousz-Institute/ambyte-iot"
    )
    parser.add_argument("--tag", required=True, help="published schedule-vX.Y.Z tag")
    parser.add_argument(
        "--script-name",
        default="default",
        help="released Schedule asset name without the .yaml suffix",
    )
    parser.add_argument(
        "--version-op",
        choices=["any", *sorted(fleet.VERSION_OPS)],
        default="any",
        help="firmware-version predicate",
    )
    parser.add_argument("--version", default=None, help="reference firmware version")
    parser.add_argument("--percentage", type=int, default=100)
    parser.add_argument(
        "--devices", help="comma/space-separated exact client IDs or MACs"
    )
    parser.add_argument("--window-minutes", type=int, default=1440)
    parser.add_argument("--profile", default=None)
    parser.add_argument("--region", default="eu-central-1")
    parser.add_argument("--ping-wait", type=int, default=25)
    parser.add_argument("--ack-seconds", type=int, default=90)
    parser.add_argument("--final-seconds", type=int, default=2100)
    parser.add_argument("--batch", type=int, default=10)
    parser.add_argument("--stagger", type=int, default=30)
    parser.add_argument(
        "--reboot", action=argparse.BooleanOptionalAction, default=True
    )
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--results-json")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if not 1 <= args.percentage <= 100:
        parser.error("--percentage must be 1-100")
    if args.version_op != "any" and fleet.parse_version(args.version or "") is None:
        parser.error(
            f"--version-op {args.version_op} needs a parseable --version"
        )

    # This must finish before fleet_ping or the deployment connection can publish.
    print(f"Resolving and verifying {args.tag} ...")
    try:
        manifest, _asset = fetch_release(args.repo, args.tag, args.script_name)
    except (ManifestError, OSError, RuntimeError) as exc:
        error = _redact_text(str(exc))
        write_results(
            args.results_json,
            {
                "tag": args.tag,
                "script_name": args.script_name,
                "dry_run": args.dry_run,
                "results": {},
                "error": error,
            },
        )
        write_summary(
            os.environ.get("GITHUB_STEP_SUMMARY"),
            ["## Fleet deploy (Schedule): validation failed", "", f"**{error}**"],
        )
        print(f"Release validation failed: {error}", file=sys.stderr)
        return 2
    print(
        f"  Schedule {args.script_name} {manifest['script_version']}: "
        f"{manifest['size_bytes']} bytes, "
        f"sha256={manifest['sha256']} (built against fw "
        f"{manifest['built_against_fw']})"
    )

    session = fleet.boto_session(args.profile, args.region)
    if args.devices:
        try:
            universe = parse_devices(args.devices)
        except ValueError as exc:
            parser.error(str(exc))
        print(f"Explicit device list: {len(universe)} device(s)")
    else:
        print(f"Discovering devices active in the last {args.window_minutes} min ...")
        universe = fleet.discover_active_devices(session, args.window_minutes)
        print(f"  {len(universe)} active device(s)")

    if not universe:
        print("No devices to target.")
        write_results(
            args.results_json,
            {
                "tag": args.tag,
                "script_name": args.script_name,
                "campaign_id": manifest["script_update"]["id"],
                "sha256": manifest["sha256"],
                "script_version": manifest["script_version"],
                "dry_run": args.dry_run,
                "universe": 0,
                "cohort": [],
                "results": {},
                "error": None,
            },
        )
        write_summary(
            os.environ.get("GITHUB_STEP_SUMMARY"),
            [f"## Fleet deploy (Schedule): {args.tag}", "", "No devices to target."],
        )
        return 0

    print(f"Pinging {len(universe)} device(s) (up to {args.ping_wait}s) ...")
    firmware_by_device = fleet.fleet_ping(session, universe, args.ping_wait)
    selected = select_cohort(
        universe,
        firmware_by_device,
        args.version_op,
        args.version,
        args.percentage,
    )
    cohort = selected["cohort"]
    silent = sorted(device for device in universe if device not in firmware_by_device)
    if selected["unproven"]:
        print(
            f"  {len(selected['unproven'])} device(s) excluded: firmware version "
            "could not be proven"
        )

    command = dict(manifest["script_update"])
    command["reboot"] = args.reboot
    print(
        f"\nPlan: release {args.tag} campaign {command['id']!r}\n"
        f"  universe {len(universe)} | matching {len(selected['matching'])} | "
        f"cohort {args.percentage}% -> {len(cohort)}"
    )
    for device in cohort:
        print(f"    {device}  fw={firmware_by_device.get(device) or 'silent'}")

    plan: dict[str, Any] = {
        "tag": args.tag,
        "script_name": args.script_name,
        "campaign_id": command["id"],
        "asset_url": manifest["asset_url"],
        "sha256": manifest["sha256"],
        "size_bytes": manifest["size_bytes"],
        "script_version": manifest["script_version"],
        "built_against_fw": manifest["built_against_fw"],
        "reboot": args.reboot,
        "version_op": args.version_op,
        "version": args.version,
        "percentage": args.percentage,
        "dry_run": args.dry_run,
        "universe": len(universe),
        "matching": len(selected["matching"]),
        "cohort": cohort,
        "unproven_firmware": selected["unproven"],
        "silent_on_ping": silent,
        "firmware_by_device": firmware_by_device,
        "results": {},
        "error": None,
    }

    if args.dry_run:
        print(
            "\nDRY RUN: correlated ping commands were published for liveness/"
            "firmware targeting; no script_update commands were published."
        )
    elif cohort:
        print(f"\nDeploying to {len(cohort)} device(s) ...")
        results, error = fleet_script_update(
            session,
            cohort,
            command,
            args.ack_seconds,
            args.final_seconds,
            args.batch,
            args.stagger,
        )
        plan["results"] = results
        plan["error"] = error
    else:
        print("\nNo devices match the firmware filter; nothing to deploy.")

    write_results(args.results_json, plan)
    summary_lines = [
        f"## Fleet deploy (Schedule): {args.tag} "
        f"({'dry run' if args.dry_run else 'live'})",
        "",
        f"- Schedule script: `{args.script_name}`",
        f"- Schedule version: `{manifest['script_version']}` (`{manifest['sha256']}`)",
        f"- Built against firmware: `{manifest['built_against_fw']}` (provenance only)",
        f"- Targeting: `{args.version_op}"
        + (f" {args.version}" if args.version_op != "any" else "")
        + f"` at **{args.percentage}%**",
        f"- Universe {len(universe)} -> matching {len(selected['matching'])} -> "
        f"cohort {len(cohort)}",
        "",
        "| device | fw before | outcome | Schedule version | script SHA-256 | metadata verified | detail |",
        "|---|---|---|---|---|---|---|",
    ]
    if args.dry_run:
        summary_lines[6:6] = [
            "- Dry run published correlated ping commands only; it published no "
            "`script_update` command and made no device-state change.",
        ]
    for device in cohort:
        if args.dry_run:
            outcome, detail, reported_version, reported_sha, metadata_verified = (
                "would deploy",
                "",
                "",
                "",
                "",
            )
        else:
            record = plan["results"].get(device, {})
            outcome = classify(record, manifest["sha256"])
            detail = record.get("detail") or ""
            reported_version = record.get("script_version") or ""
            reported_sha = record.get("script_sha256") or ""
            metadata_verified = record.get("script_metadata_verified")
            if metadata_verified is None:
                metadata_verified = ""
        summary_lines.append(
            f"| {device} | {firmware_by_device.get(device) or 'silent'} | "
            f"{outcome} | {reported_version} | {reported_sha} | "
            f"{metadata_verified} | {detail} |"
        )
    if plan["error"]:
        summary_lines.extend(
            ["", f"**Campaign error (tracking incomplete): {plan['error']}**"]
        )
    write_summary(os.environ.get("GITHUB_STEP_SUMMARY"), summary_lines)

    if plan["error"]:
        return 1
    failed = [
        device
        for device, record in plan["results"].items()
        if classify(record, manifest["sha256"]) in {"failed", "applied (sha mismatch)"}
    ]
    if failed:
        print(f"\n{len(failed)} device(s) failed identity/application checks: "
              f"{', '.join(failed)}")
        return 1
    if not args.dry_run and cohort and not any(
        classify(record, manifest["sha256"]) == "applied"
        for record in plan["results"].values()
    ):
        print("\nNo target confirmed the expected Schedule SHA-256 as applied.")
        return 1
    print("\nDone.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
