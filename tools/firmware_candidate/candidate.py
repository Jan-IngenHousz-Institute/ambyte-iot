from __future__ import annotations

import configparser
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
import zipfile
from pathlib import Path, PurePosixPath
from typing import Any, Iterable


ENVIRONMENT = "esp32-s3-devkitm-1"
PLATFORMIO_CORE_VERSION = "6.1.19"
PLATFORM_SPEC = "espressif32@6.12.0"
ESP_IDF_VERSION = "5.5.0"
ESP_IDF_PACKAGE_VERSION = "3.50500.0"
ESPTOOL_VERSION = "4.9.0"
ESPTOOL_PACKAGE_VERSION = "2.40900.250804"
ZIP_TIMESTAMP = (1980, 1, 1, 0, 0, 0)
PINNED_TOOL_VERSIONS = {
    "platformio_core": PLATFORMIO_CORE_VERSION,
    "platformio_platform": PLATFORM_SPEC,
    "esp_idf": ESP_IDF_VERSION,
    "esp_idf_package": ESP_IDF_PACKAGE_VERSION,
    "esptool": ESPTOOL_VERSION,
    "esptool_package": ESPTOOL_PACKAGE_VERSION,
}
APP_DESC_MAGIC = 0xABCD5432
APP_DESC_OFFSET = 32
APP_DESC_VERSION_OFFSET = APP_DESC_OFFSET + 16
APP_DESC_FIELD_SIZE = 32
HEX_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
GIT_SHA_RE = re.compile(r"^[0-9a-f]{40}$")
SEMVER_RE = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
SEMVER_TAG_RE = re.compile(r"^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
PR_VERSION_RE = re.compile(r"^pr-([1-9][0-9]*)-([0-9a-f]{12})$")
REPOSITORY_RE = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
ARTIFACT_NAME_RE = re.compile(r"^firmware-candidate-pr-([1-9][0-9]*)-([0-9a-f]{12})$")
OFFSET_RE = re.compile(r"^0x(?:0|[1-9a-f][0-9a-f]*)$")
FORBIDDEN_NAME_PARTS = (
    ".env",
    "nvs",
    "certificate",
    "credentials",
    "private-key",
    "private_key",
    "secret",
)
ALLOWED_FLASH_ROLES = {
    "bootloader": "bootloader.bin",
    "app": "firmware.bin",
    "partition-table": "partition-table.bin",
    "otadata": "ota_data_initial.bin",
}
PLATFORMIO_LOGICAL_FLASH_PATHS = {
    "bootloader": "bootloader/bootloader.bin",
    "partition-table": "partition_table/partition-table.bin",
}
REQUIRED_FLASH_ROLES = {"bootloader", "app", "partition-table"}
FLASHER_TOP_LEVEL_KEYS = {
    "write_flash_args",
    "flash_settings",
    "flash_files",
    "bootloader",
    "app",
    "partition-table",
    "otadata",
    "extra_esptool_args",
}


class CandidateError(ValueError):
    """A candidate input or artifact violated the closed release contract."""


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise CandidateError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_reject_duplicate_keys)
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise CandidateError(f"cannot read JSON {path}: {exc}") from exc


def canonical_json(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":")) + "\n").encode()


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def file_record(path: Path, relative_path: str | None = None) -> dict[str, Any]:
    return {
        "path": relative_path or path.name,
        "sha256": sha256_file(path),
        "size": path.stat().st_size,
    }


def validate_version(version: str) -> str:
    if not isinstance(version, str):
        raise CandidateError("firmware version must be a string")
    try:
        encoded = version.encode("ascii")
    except UnicodeEncodeError as exc:
        raise CandidateError("firmware version must contain ASCII characters only") from exc
    if not encoded or len(encoded) > 31:
        raise CandidateError("firmware version must contain 1..31 ASCII characters")
    if SEMVER_RE.fullmatch(version):
        return "release"
    if PR_VERSION_RE.fullmatch(version):
        return "pull-request"
    raise CandidateError(
        "firmware version must be bare M.m.p without leading zeroes or "
        "pr-<positive PR>-<12 lowercase hex>"
    )


def expected_pr_version(pr_number: int, head_sha: str) -> str:
    _positive_int(pr_number, "pull_request.number")
    _git_sha(head_sha, "source.head_sha")
    return f"pr-{pr_number}-{head_sha[:12]}"


def artifact_name(pr_number: int, head_sha: str) -> str:
    _positive_int(pr_number, "pull_request.number")
    _git_sha(head_sha, "source.head_sha")
    return f"firmware-candidate-pr-{pr_number}-{head_sha[:12]}"


def extract_app_descriptor_version(firmware: Path) -> str:
    try:
        data = firmware.read_bytes()
    except OSError as exc:
        raise CandidateError(f"cannot read firmware image {firmware}: {exc}") from exc
    minimum = APP_DESC_VERSION_OFFSET + APP_DESC_FIELD_SIZE
    if len(data) < minimum:
        raise CandidateError("firmware image is too small to contain an ESP app descriptor")
    magic = int.from_bytes(data[APP_DESC_OFFSET : APP_DESC_OFFSET + 4], "little")
    if magic != APP_DESC_MAGIC:
        raise CandidateError(
            f"ESP app descriptor magic missing at image offset {APP_DESC_OFFSET}"
        )
    raw = data[APP_DESC_VERSION_OFFSET : APP_DESC_VERSION_OFFSET + APP_DESC_FIELD_SIZE]
    if b"\0" not in raw:
        raise CandidateError("ESP app descriptor version is not NUL terminated")
    value = raw.split(b"\0", 1)[0]
    try:
        version = value.decode("ascii")
    except UnicodeDecodeError as exc:
        raise CandidateError("ESP app descriptor version is not ASCII") from exc
    validate_version(version)
    return version


def verify_app_descriptor(firmware: Path, expected_version: str) -> str:
    validate_version(expected_version)
    actual = extract_app_descriptor_version(firmware)
    if actual != expected_version:
        raise CandidateError(
            f"firmware app descriptor version mismatch: expected {expected_version!r}, got {actual!r}"
        )
    return actual


def _require_exact_keys(value: Any, expected: set[str], label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise CandidateError(f"{label} must be a JSON object")
    actual = set(value)
    if actual != expected:
        missing = sorted(expected - actual)
        unknown = sorted(actual - expected)
        raise CandidateError(f"{label} keys mismatch; missing={missing}, unknown={unknown}")
    return value


def _positive_int(value: Any, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise CandidateError(f"{label} must be a positive integer")
    return value


def _git_sha(value: Any, label: str) -> str:
    if not isinstance(value, str) or not GIT_SHA_RE.fullmatch(value):
        raise CandidateError(f"{label} must be exactly 40 lowercase hexadecimal characters")
    return value


def _sha256(value: Any, label: str) -> str:
    if not isinstance(value, str) or not HEX_SHA256_RE.fullmatch(value):
        raise CandidateError(f"{label} must be a lowercase SHA-256 digest")
    return value


def validate_metadata(raw: Any, firmware_version: str, notes: Path) -> dict[str, Any]:
    version_kind = validate_version(firmware_version)
    metadata = _require_exact_keys(
        raw,
        {"schema_version", "repository", "pull_request", "source", "analysis", "workflow"},
        "candidate metadata",
    )
    if metadata["schema_version"] != 1:
        raise CandidateError("candidate metadata schema_version must be 1")
    if not isinstance(metadata["repository"], str) or not REPOSITORY_RE.fullmatch(metadata["repository"]):
        raise CandidateError("repository must have owner/name syntax")

    pull_request = _require_exact_keys(metadata["pull_request"], {"number", "title"}, "pull_request")
    pr_number = _positive_int(pull_request["number"], "pull_request.number")
    title = pull_request["title"]
    if not isinstance(title, str) or not title or "\n" in title or "\r" in title:
        raise CandidateError("pull_request.title must be one non-empty line")

    source = _require_exact_keys(metadata["source"], {"head_sha", "base_sha", "tree_sha"}, "source")
    for key in source:
        _git_sha(source[key], f"source.{key}")

    analysis = _require_exact_keys(
        metadata["analysis"],
        {
            "decision",
            "release_version",
            "notes_sha256",
            "latest_release_tag",
            "recovery_of_sha",
        },
        "analysis",
    )
    decision = _require_exact_keys(
        analysis["decision"], {"release", "bump", "kind", "reason"}, "analysis.decision"
    )
    if not isinstance(decision["release"], bool):
        raise CandidateError("analysis.decision.release must be boolean")
    if decision["kind"] not in {"release", "no-release", "recovery"}:
        raise CandidateError("analysis.decision.kind is not supported")
    if not isinstance(decision["reason"], str) or not decision["reason"]:
        raise CandidateError("analysis.decision.reason must be non-empty")
    if decision["bump"] not in {None, "major", "minor", "patch"}:
        raise CandidateError("analysis.decision.bump is not supported")
    if not isinstance(analysis["latest_release_tag"], str) or not SEMVER_TAG_RE.fullmatch(
        analysis["latest_release_tag"]
    ):
        raise CandidateError("analysis.latest_release_tag must be an exact vX.Y.Z tag")
    if decision["release"]:
        if decision["kind"] != "release" or decision["bump"] is None:
            raise CandidateError("release decisions require kind=release and a bump")
        if version_kind != "release" or analysis["release_version"] != firmware_version:
            raise CandidateError("release candidate firmware must equal the predicted bare semantic version")
    else:
        if decision["kind"] == "release" or decision["bump"] is not None:
            raise CandidateError("non-release decisions require a non-release kind and null bump")
        if analysis["release_version"] is not None:
            raise CandidateError("non-release candidate release_version must be null")
        expected = expected_pr_version(pr_number, source["head_sha"])
        if firmware_version != expected:
            raise CandidateError(f"non-release firmware version must be {expected!r}")
    recovery_of_sha = analysis["recovery_of_sha"]
    if decision["kind"] == "recovery":
        _git_sha(recovery_of_sha, "analysis.recovery_of_sha")
    elif recovery_of_sha is not None:
        raise CandidateError("only verified recovery decisions may carry recovery_of_sha")
    _sha256(analysis["notes_sha256"], "analysis.notes_sha256")
    actual_notes_digest = sha256_file(notes)
    if analysis["notes_sha256"] != actual_notes_digest:
        raise CandidateError("release notes digest does not match candidate metadata")

    workflow = _require_exact_keys(metadata["workflow"], {"sha", "run_id", "run_attempt"}, "workflow")
    _git_sha(workflow["sha"], "workflow.sha")
    _positive_int(workflow["run_id"], "workflow.run_id")
    _positive_int(workflow["run_attempt"], "workflow.run_attempt")
    return metadata


def validate_tool_versions(value: Any) -> dict[str, str]:
    tools = _require_exact_keys(value, set(PINNED_TOOL_VERSIONS), "tool_versions")
    if tools != PINNED_TOOL_VERSIONS:
        raise CandidateError(f"tool_versions must equal the pinned set {PINNED_TOOL_VERSIONS}")
    return tools


def validate_provisioning(value: Any) -> dict[str, str]:
    provisioning = _require_exact_keys(value, {"nvs", "device_secrets"}, "provisioning")
    if provisioning != {"nvs": "excluded", "device_secrets": "excluded"}:
        raise CandidateError("candidate provisioning must exclude NVS and device secrets")
    return provisioning


def validate_relative_path(value: Any, label: str = "path") -> str:
    if not isinstance(value, str) or not value or "\\" in value or "\0" in value:
        raise CandidateError(f"{label} is not a normalized POSIX relative path")
    pure = PurePosixPath(value)
    if pure.is_absolute() or value != pure.as_posix() or any(part in {"", ".", ".."} for part in pure.parts):
        raise CandidateError(f"{label} is not a normalized POSIX relative path")
    lowered = value.lower()
    if any(part in lowered for part in FORBIDDEN_NAME_PARTS):
        raise CandidateError(f"{label} contains a forbidden provisioning/secret name")
    if lowered.endswith((".pem", ".key", ".p12", ".pfx", ".crt", ".cer")):
        raise CandidateError(f"{label} names certificate or key material")
    return value


def _safe_source(build_dir: Path, value: str, label: str) -> Path:
    source = Path(value)
    resolved = (source if source.is_absolute() else build_dir / source).resolve()
    build_resolved = build_dir.resolve()
    if resolved != build_resolved and build_resolved not in resolved.parents:
        raise CandidateError(f"{label} resolves outside the firmware build directory")
    if not resolved.is_file():
        raise CandidateError(f"{label} does not exist: {value}")
    if resolved.suffix.lower() != ".bin":
        raise CandidateError(f"{label} must reference a .bin image")
    validate_relative_path(resolved.name, label)
    return resolved


def _platformio_role_source(
    build_dir: Path,
    role: str,
    flasher_path: str,
    standalone_firmware: Path,
) -> Path:
    """Resolve an IDF flasher role after PlatformIO's standard output renames.

    ESP-IDF writes logical paths into flasher_args.json. PlatformIO consumes the
    same build and then renames/removes three of those files for its public
    outputs. The role and offset still come exclusively from IDF metadata; only
    the source filename is mapped through this closed, target-specific alias.
    """
    try:
        return _safe_source(build_dir, flasher_path, f"{role}.file")
    except CandidateError as original:
        path = Path(flasher_path)
        if path.is_absolute():
            raise
        validate_relative_path(path.as_posix(), f"{role}.file")
        aliases = {
            "bootloader": build_dir / "bootloader.bin",
            "app": standalone_firmware,
            "partition-table": build_dir / "partitions.bin",
        }
        alias = aliases.get(role)
        if alias is None or not alias.is_file():
            raise original
        if role in PLATFORMIO_LOGICAL_FLASH_PATHS:
            if path.as_posix() != PLATFORMIO_LOGICAL_FLASH_PATHS[role]:
                raise CandidateError(f"unsupported missing PlatformIO path for {role}")
        elif role == "app":
            description = load_json(build_dir / "project_description.json")
            if description.get("app_bin") != flasher_path:
                raise CandidateError(
                    "missing IDF app path does not match project_description.json app_bin"
                )
        return alias.resolve()


def _normalize_offset(value: Any, label: str) -> str:
    if not isinstance(value, str) or not OFFSET_RE.fullmatch(value):
        raise CandidateError(f"{label} must be normalized lowercase hexadecimal, such as 0x8000")
    number = int(value, 16)
    normalized = hex(number)
    if normalized != value:
        raise CandidateError(f"{label} is not normalized")
    return normalized


def _validate_write_flash_args(value: Any, flash_settings: Any) -> list[str]:
    if not isinstance(value, list) or not value or any(not isinstance(item, str) for item in value):
        raise CandidateError("write_flash_args must be a non-empty string array")
    if len(value) % 2:
        raise CandidateError("write_flash_args must contain flag/value pairs")
    settings = _require_exact_keys(
        flash_settings, {"flash_mode", "flash_size", "flash_freq"}, "flash_settings"
    )
    allowed = {"--flash_mode": "flash_mode", "--flash_size": "flash_size", "--flash_freq": "flash_freq"}
    seen: set[str] = set()
    for index in range(0, len(value), 2):
        flag, item = value[index], value[index + 1]
        if flag not in allowed or flag in seen:
            raise CandidateError(f"unsupported or duplicate esptool write flag: {flag}")
        seen.add(flag)
        if settings[allowed[flag]] != item:
            raise CandidateError(f"{flag} does not match flash_settings")
        if not re.fullmatch(r"[A-Za-z0-9]+", item):
            raise CandidateError(f"unsafe esptool value for {flag}")
    if seen != set(allowed):
        raise CandidateError("write_flash_args must define flash mode, size, and frequency")
    return list(value)


def parse_flasher_args(
    flasher_args_path: Path,
    build_dir: Path,
    standalone_firmware: Path,
    esptool_version: str = ESPTOOL_VERSION,
) -> tuple[dict[str, Any], dict[str, Path]]:
    raw = load_json(flasher_args_path)
    if not isinstance(raw, dict):
        raise CandidateError("flasher_args.json must contain an object")
    unknown = set(raw) - FLASHER_TOP_LEVEL_KEYS
    missing = {"write_flash_args", "flash_settings", "flash_files", "extra_esptool_args"} - set(raw)
    if unknown or missing:
        raise CandidateError(f"unsupported flasher_args.json schema; missing={sorted(missing)}, unknown={sorted(unknown)}")

    roles = set(raw) & set(ALLOWED_FLASH_ROLES)
    if not REQUIRED_FLASH_ROLES.issubset(roles):
        raise CandidateError("flasher schema is missing bootloader, app, or partition-table")
    flash_files = raw["flash_files"]
    if not isinstance(flash_files, dict) or len(flash_files) != len(roles):
        raise CandidateError("flash_files must match the named generic flasher roles exactly")
    write_args = _validate_write_flash_args(raw["write_flash_args"], raw["flash_settings"])
    extra = _require_exact_keys(
        raw["extra_esptool_args"], {"after", "before", "stub", "chip"}, "extra_esptool_args"
    )
    if extra["chip"] != "esp32s3":
        raise CandidateError("flash plan is only valid for chip esp32s3")
    if extra["before"] not in {"default_reset", "usb_reset", "no_reset", "no_reset_no_sync"}:
        raise CandidateError("unsupported esptool before-reset mode")
    if extra["after"] not in {"hard_reset", "soft_reset", "no_reset", "no_reset_stub"}:
        raise CandidateError("unsupported esptool after-reset mode")
    if not isinstance(extra["stub"], bool):
        raise CandidateError("extra_esptool_args.stub must be boolean")

    image_sources: dict[str, Path] = {}
    images: list[dict[str, Any]] = []
    seen_offsets: set[int] = set()
    for role in sorted(roles):
        entry = _require_exact_keys(raw[role], {"offset", "file", "encrypted"}, f"flasher role {role}")
        if entry["encrypted"] not in {False, "false"}:
            raise CandidateError("encrypted flash images are outside this unsigned generic candidate contract")
        offset = _normalize_offset(entry["offset"], f"{role}.offset")
        if offset not in flash_files or flash_files[offset] != entry["file"]:
            raise CandidateError(f"named flasher role {role} does not match flash_files")
        numeric_offset = int(offset, 16)
        if numeric_offset in seen_offsets:
            raise CandidateError("duplicate flash offset")
        seen_offsets.add(numeric_offset)
        if not isinstance(entry["file"], str):
            raise CandidateError(f"{role}.file must be a path string")
        source = _platformio_role_source(
            build_dir, role, entry["file"], standalone_firmware
        )
        destination = ALLOWED_FLASH_ROLES[role]
        validate_relative_path(destination)
        image_sources[destination] = source
        images.append(
            {
                "offset": offset,
                "path": destination,
                "role": role,
                "sha256": sha256_file(source),
                "size": source.stat().st_size,
            }
        )

    if set(flash_files) != {image["offset"] for image in images}:
        raise CandidateError("flash_files contains an unnamed or uncollected image")
    if not standalone_firmware.is_file():
        raise CandidateError("PlatformIO did not produce standalone firmware.bin")
    app_source = image_sources["firmware.bin"]
    if sha256_file(app_source) != sha256_file(standalone_firmware):
        raise CandidateError("flasher app image and PlatformIO firmware.bin are not byte-identical")
    image_sources["firmware.bin"] = standalone_firmware.resolve()
    for image in images:
        if image["path"] == "firmware.bin":
            image["sha256"] = sha256_file(standalone_firmware)
            image["size"] = standalone_firmware.stat().st_size

    images.sort(key=lambda image: int(image["offset"], 16))
    command = [
        "esptool.py",
        "--chip",
        extra["chip"],
        "--before",
        extra["before"],
        "--after",
        extra["after"],
        "write_flash",
        *write_args,
    ]
    if not extra["stub"]:
        command.append("--no-stub")
    for image in images:
        command.extend([image["offset"], image["path"]])
    plan = {
        "schema_version": 1,
        "chip": extra["chip"],
        "esptool": {
            "package_version": ESPTOOL_PACKAGE_VERSION,
            "version": esptool_version,
            "command": command,
        },
        "images": images,
    }
    _assert_no_absolute_strings(plan, "flash-plan.json")
    return plan, image_sources


def _assert_no_absolute_strings(value: Any, label: str) -> None:
    if isinstance(value, dict):
        for item in value.values():
            _assert_no_absolute_strings(item, label)
    elif isinstance(value, list):
        for item in value:
            _assert_no_absolute_strings(item, label)
    elif isinstance(value, str):
        if value.startswith("/") or re.match(r"^[A-Za-z]:[\\/]", value):
            raise CandidateError(f"{label} contains an absolute runner path")


def _checksums(records: Iterable[dict[str, Any]]) -> bytes:
    return "".join(f"{record['sha256']}  {record['path']}\n" for record in records).encode()


def _write_deterministic_zip(source_dir: Path, names: list[str], destination: Path) -> None:
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_STORED, strict_timestamps=True) as archive:
        for name in sorted(names):
            validate_relative_path(name, "ZIP member")
            info = zipfile.ZipInfo(name, date_time=ZIP_TIMESTAMP)
            info.compress_type = zipfile.ZIP_STORED
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            archive.writestr(info, (source_dir / name).read_bytes())


def _zip_name(version: str) -> str:
    kind = validate_version(version)
    return f"ambyte-iot-v{version}.zip" if kind == "release" else f"ambyte-iot-{version}.zip"


def package_candidate(
    *,
    build_dir: Path,
    metadata: dict[str, Any],
    release_notes: Path,
    output_dir: Path,
    firmware_version: str,
    tool_versions: dict[str, str],
) -> Path:
    validate_metadata(metadata, firmware_version, release_notes)
    validate_tool_versions(tool_versions)
    build_dir = build_dir.resolve()
    standalone = build_dir / "firmware.bin"
    verify_app_descriptor(standalone, firmware_version)
    plan, image_sources = parse_flasher_args(
        build_dir / "flasher_args.json", build_dir, standalone, tool_versions["esptool"]
    )

    output_dir = output_dir.resolve()
    output_dir.parent.mkdir(parents=True, exist_ok=True)
    if output_dir.exists() and any(output_dir.iterdir()):
        raise CandidateError(f"candidate output directory is not empty: {output_dir}")
    if output_dir.exists() and not output_dir.is_dir():
        raise CandidateError(f"candidate output path is not a directory: {output_dir}")
    stage = Path(tempfile.mkdtemp(prefix=".firmware-candidate-", dir=output_dir.parent))
    try:
        bundle = stage / "bundle"
        bundle.mkdir()
        for destination, source in image_sources.items():
            shutil.copyfile(source, bundle / destination)
        (bundle / "flash-plan.json").write_bytes(canonical_json(plan))
        payload_names = sorted([*image_sources, "flash-plan.json"])
        payload_records = [file_record(bundle / name, name) for name in payload_names]
        bundle_manifest = {
            "schema_version": 1,
            "firmware_version": firmware_version,
            "provisioning": {"nvs": "excluded", "device_secrets": "excluded"},
            "tool_versions": tool_versions,
            "files": payload_records,
        }
        (bundle / "bundle-manifest.json").write_bytes(canonical_json(bundle_manifest))
        (bundle / "bundle-SHA256SUMS").write_bytes(_checksums(payload_records))
        zip_name = _zip_name(firmware_version)
        zip_path = stage / zip_name
        bundle_names = [*payload_names, "bundle-manifest.json", "bundle-SHA256SUMS"]
        _write_deterministic_zip(bundle, bundle_names, zip_path)

        shutil.copyfile(standalone, stage / "firmware.bin")
        shutil.copyfile(release_notes, stage / "release-notes.md")
        outer_payload_names = sorted(["firmware.bin", zip_name, "release-notes.md"])
        outer_records = [file_record(stage / name, name) for name in outer_payload_names]
        candidate_manifest = {
            "schema_version": 1,
            "artifact_name": artifact_name(
                metadata["pull_request"]["number"], metadata["source"]["head_sha"]
            ),
            "repository": metadata["repository"],
            "pull_request": metadata["pull_request"],
            "source": metadata["source"],
            "analysis": {
                **metadata["analysis"],
                "firmware_version": firmware_version,
            },
            "workflow": metadata["workflow"],
            "provisioning": {"nvs": "excluded", "device_secrets": "excluded"},
            "tool_versions": tool_versions,
            "files": outer_records,
        }
        _assert_no_absolute_strings(candidate_manifest, "candidate-manifest.json")
        (stage / "candidate-manifest.json").write_bytes(canonical_json(candidate_manifest))
        (stage / "candidate-SHA256SUMS").write_bytes(_checksums(outer_records))
        shutil.rmtree(bundle)

        verify_candidate(stage)
        if output_dir.exists():
            output_dir.rmdir()
        stage.rename(output_dir)
        return output_dir
    except Exception:
        shutil.rmtree(stage, ignore_errors=True)
        raise


def _parse_checksum_file(data: bytes, label: str) -> dict[str, str]:
    try:
        text = data.decode("ascii")
    except UnicodeDecodeError as exc:
        raise CandidateError(f"{label} must be ASCII") from exc
    result: dict[str, str] = {}
    for line in text.splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  ([^\r\n]+)", line)
        if not match:
            raise CandidateError(f"invalid checksum line in {label}")
        digest, name = match.groups()
        validate_relative_path(name, f"{label} member")
        if name in result:
            raise CandidateError(f"duplicate checksum member in {label}: {name}")
        result[name] = digest
    if not result:
        raise CandidateError(f"{label} must not be empty")
    return result


def _validate_records(value: Any, label: str) -> list[dict[str, Any]]:
    if not isinstance(value, list) or not value:
        raise CandidateError(f"{label} must be a non-empty array")
    records: list[dict[str, Any]] = []
    seen: set[str] = set()
    for index, item in enumerate(value):
        record = _require_exact_keys(item, {"path", "sha256", "size"}, f"{label}[{index}]")
        path = validate_relative_path(record["path"], f"{label}[{index}].path")
        _sha256(record["sha256"], f"{label}[{index}].sha256")
        if isinstance(record["size"], bool) or not isinstance(record["size"], int) or record["size"] < 0:
            raise CandidateError(f"{label}[{index}].size must be a non-negative integer")
        if path in seen:
            raise CandidateError(f"duplicate file record: {path}")
        seen.add(path)
        records.append(record)
    if [record["path"] for record in records] != sorted(seen):
        raise CandidateError(f"{label} must be sorted by normalized path")
    return records


def _verify_record_bytes(records: list[dict[str, Any]], read: Any, label: str) -> None:
    for record in records:
        value = read(record["path"])
        if len(value) != record["size"] or sha256_bytes(value) != record["sha256"]:
            raise CandidateError(f"{label} digest/size mismatch for {record['path']}")


def verify_candidate(candidate_dir: Path) -> dict[str, Any]:
    candidate_dir = candidate_dir.resolve()
    manifest = load_json(candidate_dir / "candidate-manifest.json")
    manifest = _require_exact_keys(
        manifest,
        {
            "schema_version", "artifact_name", "repository", "pull_request", "source",
            "analysis", "workflow", "provisioning", "tool_versions", "files",
        },
        "candidate manifest",
    )
    if manifest["schema_version"] != 1:
        raise CandidateError("candidate manifest schema_version must be 1")
    _assert_no_absolute_strings(manifest, "candidate-manifest.json")
    validate_provisioning(manifest["provisioning"])
    validate_tool_versions(manifest["tool_versions"])
    analysis = _require_exact_keys(
        manifest["analysis"],
        {
            "decision",
            "release_version",
            "notes_sha256",
            "firmware_version",
            "latest_release_tag",
            "recovery_of_sha",
        },
        "candidate manifest analysis",
    )
    firmware_version = analysis["firmware_version"]
    metadata = {
        "schema_version": 1,
        "repository": manifest["repository"],
        "pull_request": manifest["pull_request"],
        "source": manifest["source"],
        "analysis": {
            "decision": analysis["decision"],
            "release_version": analysis["release_version"],
            "notes_sha256": analysis["notes_sha256"],
            "latest_release_tag": analysis["latest_release_tag"],
            "recovery_of_sha": analysis["recovery_of_sha"],
        },
        "workflow": manifest["workflow"],
    }
    validate_metadata(metadata, firmware_version, candidate_dir / "release-notes.md")
    expected_artifact = artifact_name(manifest["pull_request"]["number"], manifest["source"]["head_sha"])
    if manifest["artifact_name"] != expected_artifact or not ARTIFACT_NAME_RE.fullmatch(manifest["artifact_name"]):
        raise CandidateError("candidate artifact_name is not deterministic for PR/head")
    outer_records = _validate_records(manifest["files"], "candidate manifest files")
    zip_names = [record["path"] for record in outer_records if record["path"].endswith(".zip")]
    expected_zip = _zip_name(firmware_version)
    if zip_names != [expected_zip] or set(record["path"] for record in outer_records) != {
        "firmware.bin", "release-notes.md", expected_zip
    }:
        raise CandidateError("candidate payload allowlist must contain firmware, notes, and one ZIP")
    expected_outer = {
        *(record["path"] for record in outer_records),
        "candidate-manifest.json",
        "candidate-SHA256SUMS",
    }
    actual_outer = {path.name for path in candidate_dir.iterdir() if path.is_file()}
    if actual_outer != expected_outer or any(path.is_dir() for path in candidate_dir.iterdir()):
        raise CandidateError("candidate directory contains unknown or missing files")
    _verify_record_bytes(outer_records, lambda name: (candidate_dir / name).read_bytes(), "candidate")
    outer_sums = _parse_checksum_file((candidate_dir / "candidate-SHA256SUMS").read_bytes(), "candidate-SHA256SUMS")
    if outer_sums != {record["path"]: record["sha256"] for record in outer_records}:
        raise CandidateError("candidate-SHA256SUMS does not exactly cover outer payload files")
    if manifest["analysis"].get("notes_sha256") != sha256_file(candidate_dir / "release-notes.md"):
        raise CandidateError("candidate notes digest mismatch")
    verify_app_descriptor(candidate_dir / "firmware.bin", firmware_version)

    zip_path = candidate_dir / zip_names[0]
    with zipfile.ZipFile(zip_path, "r") as archive:
        infos = archive.infolist()
        names = [info.filename for info in infos]
        if len(names) != len(set(names)):
            raise CandidateError("ZIP contains duplicate member names")
        for info in infos:
            validate_relative_path(info.filename, "ZIP member")
            if info.is_dir():
                raise CandidateError("ZIP must not contain directories")
            if info.compress_type != zipfile.ZIP_STORED or info.date_time != ZIP_TIMESTAMP:
                raise CandidateError("ZIP members must use the deterministic stored representation")
            if info.flag_bits & 0x1:
                raise CandidateError("ZIP members must not be encrypted")
        required_meta = {"bundle-manifest.json", "bundle-SHA256SUMS", "flash-plan.json"}
        if not required_meta.issubset(names):
            raise CandidateError("ZIP is missing bundle metadata")
        bundle_manifest = json.loads(
            archive.read("bundle-manifest.json").decode("utf-8"),
            object_pairs_hook=_reject_duplicate_keys,
        )
        bundle_manifest = _require_exact_keys(
            bundle_manifest,
            {"schema_version", "firmware_version", "provisioning", "tool_versions", "files"},
            "bundle manifest",
        )
        if bundle_manifest["schema_version"] != 1 or bundle_manifest["firmware_version"] != firmware_version:
            raise CandidateError("bundle identity does not match candidate")
        _assert_no_absolute_strings(bundle_manifest, "bundle-manifest.json")
        validate_provisioning(bundle_manifest["provisioning"])
        validate_tool_versions(bundle_manifest["tool_versions"])
        if bundle_manifest["tool_versions"] != manifest["tool_versions"]:
            raise CandidateError("bundle and candidate tool versions differ")
        bundle_records = _validate_records(bundle_manifest["files"], "bundle manifest files")
        expected_bundle = {record["path"] for record in bundle_records} | {
            "bundle-manifest.json", "bundle-SHA256SUMS"
        }
        if set(names) != expected_bundle:
            raise CandidateError("ZIP contains unknown or missing files")
        _verify_record_bytes(bundle_records, archive.read, "bundle")
        bundle_sums = _parse_checksum_file(archive.read("bundle-SHA256SUMS"), "bundle-SHA256SUMS")
        if bundle_sums != {record["path"]: record["sha256"] for record in bundle_records}:
            raise CandidateError("bundle-SHA256SUMS does not exactly cover bundle payload files")
        plan = json.loads(
            archive.read("flash-plan.json").decode("utf-8"),
            object_pairs_hook=_reject_duplicate_keys,
        )
        _verify_flash_plan(plan, bundle_records, archive.read)
        if archive.read("firmware.bin") != (candidate_dir / "firmware.bin").read_bytes():
            raise CandidateError("standalone and bundled firmware.bin differ")
    return manifest


def _verify_flash_plan(plan: Any, bundle_records: list[dict[str, Any]], read: Any) -> None:
    plan = _require_exact_keys(plan, {"schema_version", "chip", "esptool", "images"}, "flash plan")
    if plan["schema_version"] != 1 or plan["chip"] != "esp32s3":
        raise CandidateError("unsupported flash plan identity")
    esptool = _require_exact_keys(
        plan["esptool"], {"package_version", "version", "command"}, "flash plan esptool"
    )
    if esptool["package_version"] != ESPTOOL_PACKAGE_VERSION or esptool["version"] != ESPTOOL_VERSION:
        raise CandidateError("flash plan esptool version is not pinned")
    if not isinstance(esptool["command"], list) or any(not isinstance(item, str) for item in esptool["command"]):
        raise CandidateError("flash plan command must be a string array")
    if not isinstance(plan["images"], list) or not plan["images"]:
        raise CandidateError("flash plan images must be non-empty")
    record_map = {record["path"]: record for record in bundle_records}
    offsets: list[int] = []
    image_paths: set[str] = set()
    for index, image in enumerate(plan["images"]):
        image = _require_exact_keys(
            image, {"offset", "path", "role", "sha256", "size"}, f"flash plan image {index}"
        )
        offset = _normalize_offset(image["offset"], f"flash plan image {index} offset")
        path = validate_relative_path(image["path"], f"flash plan image {index} path")
        if image["role"] not in ALLOWED_FLASH_ROLES or ALLOWED_FLASH_ROLES[image["role"]] != path:
            raise CandidateError("flash plan contains an unknown image role/path")
        if path not in record_map or path in image_paths:
            raise CandidateError("flash plan image is missing from bundle or duplicated")
        value = read(path)
        if image["sha256"] != sha256_bytes(value) or image["size"] != len(value):
            raise CandidateError("flash plan image digest/size mismatch")
        offsets.append(int(offset, 16))
        image_paths.add(path)
    if offsets != sorted(offsets) or len(offsets) != len(set(offsets)):
        raise CandidateError("flash plan offsets must be unique and sorted")
    binary_records = {path for path in record_map if path.endswith(".bin")}
    if image_paths != binary_records:
        raise CandidateError("flash plan must reference every and only bundled binary image")
    command = esptool["command"]
    expected_pairs: list[str] = []
    for image in plan["images"]:
        expected_pairs.extend([image["offset"], image["path"]])
    expected_command = [
        "esptool.py", "--chip", "esp32s3", "--before", "default_reset",
        "--after", "hard_reset", "write_flash", "--flash_mode", "dio",
        "--flash_size", "16MB", "--flash_freq", "80m", *expected_pairs,
    ]
    if command != expected_command:
        raise CandidateError("flash plan command is not the exact pinned esptool invocation")
    if set(record_map) != binary_records | {"flash-plan.json"}:
        raise CandidateError("bundle payload contains a file outside the flash/image allowlist")


def _run(command: list[str], *, cwd: Path, env: dict[str, str] | None = None, capture: bool = False) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        cwd=cwd,
        env=env,
        text=True,
        capture_output=capture,
        check=False,
    )
    if result.returncode != 0:
        detail = (result.stdout or "") + (result.stderr or "") if capture else ""
        raise CandidateError(f"command failed ({result.returncode}): {' '.join(command)}\n{detail}")
    return result


def verify_source_checkout(repo_root: Path, metadata: dict[str, Any]) -> None:
    expected_head = metadata["source"]["head_sha"]
    expected_tree = metadata["source"]["tree_sha"]
    actual_head = _run(["git", "rev-parse", "HEAD"], cwd=repo_root, capture=True).stdout.strip()
    actual_tree = _run(["git", "rev-parse", "HEAD^{tree}"], cwd=repo_root, capture=True).stdout.strip()
    if actual_head != expected_head or actual_tree != expected_tree:
        raise CandidateError("checked-out HEAD/tree does not match candidate source identity")
    status = _run(
        ["git", "status", "--porcelain=v1", "--untracked-files=all"], cwd=repo_root, capture=True
    ).stdout
    if status:
        raise CandidateError("candidate checkout contains tracked or untracked source changes")
    submodules = _run(["git", "submodule", "status", "--recursive"], cwd=repo_root, capture=True).stdout
    bad = [line for line in submodules.splitlines() if line and not line.startswith(" ")]
    if bad:
        raise CandidateError("candidate checkout has uninitialized or mismatched submodules")


def _platform_configuration(repo_root: Path) -> None:
    parser = configparser.ConfigParser(interpolation=None, inline_comment_prefixes=(";", "#"))
    parser.read(repo_root / "platformio.ini", encoding="utf-8")
    section = parser[f"env:{ENVIRONMENT}"]
    if section.get("platform", "").strip() != PLATFORM_SPEC:
        raise CandidateError(f"PlatformIO platform must remain pinned to {PLATFORM_SPEC}")
    packages = {line.strip() for line in section.get("platform_packages", "").splitlines() if line.strip()}
    expected = {
        f"platformio/framework-espidf@{ESP_IDF_PACKAGE_VERSION}",
        f"platformio/tool-esptoolpy@{ESPTOOL_PACKAGE_VERSION}",
    }
    if packages != expected:
        raise CandidateError(f"PlatformIO framework/esptool package pins must be exactly {sorted(expected)}")


def _platformio_info(pio: Path, repo_root: Path) -> dict[str, Any]:
    result = _run([str(pio), "system", "info", "--json-output"], cwd=repo_root, capture=True)
    try:
        info = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise CandidateError("PlatformIO system info was not valid JSON") from exc
    if info.get("core_version", {}).get("value") != PLATFORMIO_CORE_VERSION:
        raise CandidateError(f"PlatformIO Core must be exactly {PLATFORMIO_CORE_VERSION}")
    return info


def _verify_esptool(info: dict[str, Any], firmware: Path, repo_root: Path) -> None:
    core_dir = Path(info["core_dir"]["value"])
    python_exe = info["python_exe"]["value"]
    matches: list[Path] = []
    for package_dir in (core_dir / "packages").glob("tool-esptoolpy*"):
        package_json = package_dir / "package.json"
        if package_json.is_file() and load_json(package_json).get("version") == ESPTOOL_PACKAGE_VERSION:
            matches.append(package_dir / "esptool.py")
    matches = [path for path in matches if path.is_file()]
    if len(matches) != 1:
        raise CandidateError(f"expected one pinned esptool package, found {len(matches)}")
    version = _run([python_exe, str(matches[0]), "version"], cwd=repo_root, capture=True)
    if ESPTOOL_VERSION not in version.stdout:
        raise CandidateError(f"esptool must report version {ESPTOOL_VERSION}")
    _run(
        [python_exe, str(matches[0]), "--chip", "esp32s3", "image_info", str(firmware)],
        cwd=repo_root,
        capture=True,
    )


def build_and_package(
    *,
    repo_root: Path,
    pio: Path,
    metadata_path: Path,
    release_notes: Path,
    output_dir: Path,
    firmware_version: str,
) -> Path:
    metadata = load_json(metadata_path)
    validate_metadata(metadata, firmware_version, release_notes)
    verify_source_checkout(repo_root, metadata)
    _platform_configuration(repo_root)
    pio_info = _platformio_info(pio, repo_root)
    env = os.environ.copy()
    env["AMBYTE_NVS_SKIP"] = "1"
    env["AMBYTE_PROJECT_VER"] = firmware_version
    _run([str(pio), "run", "-e", ENVIRONMENT, "-t", "clean"], cwd=repo_root, env=env)
    _run([str(pio), "run", "-e", ENVIRONMENT], cwd=repo_root, env=env)
    build_dir = repo_root / ".pio" / "build" / ENVIRONMENT
    forbidden_nvs = list(build_dir.rglob("nvs.bin"))
    if forbidden_nvs:
        raise CandidateError("unprovisioned build unexpectedly produced nvs.bin")
    firmware = build_dir / "firmware.bin"
    verify_app_descriptor(firmware, firmware_version)
    description = load_json(build_dir / "project_description.json")
    if description.get("project_version") != firmware_version:
        raise CandidateError("ESP-IDF project description version does not match requested firmware version")
    if description.get("git_revision") != ESP_IDF_VERSION:
        raise CandidateError(f"ESP-IDF must report version {ESP_IDF_VERSION}")
    _verify_esptool(pio_info, firmware, repo_root)
    return package_candidate(
        build_dir=build_dir,
        metadata=metadata,
        release_notes=release_notes,
        output_dir=output_dir,
        firmware_version=firmware_version,
        tool_versions=PINNED_TOOL_VERSIONS,
    )
