from __future__ import annotations

import copy
import hashlib
import json
import tempfile
import unittest
import zipfile
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from tools.firmware_candidate.candidate import (
    APP_DESC_FIELD_SIZE,
    APP_DESC_MAGIC,
    APP_DESC_OFFSET,
    APP_DESC_VERSION_OFFSET,
    ESPTOOL_PACKAGE_VERSION,
    ESPTOOL_VERSION,
    CandidateError,
    artifact_name,
    extract_app_descriptor_version,
    package_candidate,
    parse_flasher_args,
    sha256_file,
    validate_metadata,
    validate_relative_path,
    validate_version,
    verify_app_descriptor,
    verify_candidate,
    verify_source_checkout,
)


HEAD_SHA = "deadbeefcafe" + "0" * 28
BASE_SHA = "1" * 40
TREE_SHA = "2" * 40
WORKFLOW_SHA = "3" * 40
TOOLS = {
    "platformio_core": "6.1.19",
    "platformio_platform": "espressif32@6.12.0",
    "esp_idf": "5.5.0",
    "esp_idf_package": "3.50500.0",
    "esptool": ESPTOOL_VERSION,
    "esptool_package": ESPTOOL_PACKAGE_VERSION,
}


def fake_firmware(version: str) -> bytes:
    data = bytearray(512)
    data[APP_DESC_OFFSET : APP_DESC_OFFSET + 4] = APP_DESC_MAGIC.to_bytes(4, "little")
    encoded = version.encode("ascii")
    data[APP_DESC_VERSION_OFFSET : APP_DESC_VERSION_OFFSET + len(encoded)] = encoded
    data[APP_DESC_VERSION_OFFSET + len(encoded)] = 0
    return bytes(data)


def metadata(notes: Path, *, release: bool = False) -> dict:
    return {
        "schema_version": 1,
        "repository": "Jan-IngenHousz-Institute/ambyte-iot",
        "pull_request": {"number": 42, "title": "feat: candidate packaging"},
        "source": {"head_sha": HEAD_SHA, "base_sha": BASE_SHA, "tree_sha": TREE_SHA},
        "analysis": {
            "decision": {
                "release": release,
                "bump": "minor" if release else None,
                "kind": "release" if release else "no-release",
                "reason": "type-feat" if release else "no-release-type",
            },
            "release_version": "1.1.0" if release else None,
            "notes_sha256": hashlib.sha256(notes.read_bytes()).hexdigest(),
            "latest_release_tag": "v1.0.5",
            "recovery_of_sha": None,
        },
        "workflow": {"sha": WORKFLOW_SHA, "run_id": 123456789, "run_attempt": 2},
    }


def make_build(root: Path, version: str = "pr-42-deadbeefcafe") -> Path:
    build = root / "build"
    (build / "bootloader").mkdir(parents=True)
    (build / "partition_table").mkdir()
    app = fake_firmware(version)
    (build / "firmware.bin").write_bytes(app)
    (build / "ambyte-iot.bin").write_bytes(app)
    (build / "bootloader" / "bootloader.bin").write_bytes(b"bootloader")
    (build / "partition_table" / "partition-table.bin").write_bytes(b"partition-table")
    (build / "ota_data_initial.bin").write_bytes(b"ota-initializer")
    flasher = {
        "write_flash_args": [
            "--flash_mode", "dio", "--flash_size", "16MB", "--flash_freq", "80m"
        ],
        "flash_settings": {"flash_mode": "dio", "flash_size": "16MB", "flash_freq": "80m"},
        "flash_files": {
            "0x0": "bootloader/bootloader.bin",
            "0x20000": "ambyte-iot.bin",
            "0x8000": "partition_table/partition-table.bin",
            "0xf000": "ota_data_initial.bin",
        },
        "bootloader": {"offset": "0x0", "file": "bootloader/bootloader.bin", "encrypted": "false"},
        "app": {"offset": "0x20000", "file": "ambyte-iot.bin", "encrypted": "false"},
        "partition-table": {
            "offset": "0x8000", "file": "partition_table/partition-table.bin", "encrypted": "false"
        },
        "otadata": {"offset": "0xf000", "file": "ota_data_initial.bin", "encrypted": "false"},
        "extra_esptool_args": {
            "after": "hard_reset", "before": "default_reset", "stub": True, "chip": "esp32s3"
        },
    }
    (build / "flasher_args.json").write_text(json.dumps(flasher), encoding="utf-8")
    return build


class VersionTests(unittest.TestCase):
    def test_accepts_settled_release_and_pr_formats(self) -> None:
        self.assertEqual(validate_version("0.0.0"), "release")
        self.assertEqual(validate_version("12.34.56"), "release")
        self.assertEqual(validate_version("pr-42-deadbeefcafe"), "pull-request")

    def test_rejects_invalid_ambiguous_or_overlong_versions(self) -> None:
        invalid = [
            "v1.2.3", "01.2.3", "1.02.3", "1.2.03", "1.2.3-rc1", "pr-0-deadbeefcafe",
            "pr-01-deadbeefcafe", "pr-42-DEADBEEFCAFE", "pr-42-deadbee", "pr-42-deadbeefcafe0",
            "1" * 32, "", "1.2.3\n",
        ]
        for value in invalid:
            with self.subTest(value=value), self.assertRaises(CandidateError):
                validate_version(value)

    def test_extracts_and_compares_app_descriptor_version(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            firmware = Path(tmp) / "firmware.bin"
            firmware.write_bytes(fake_firmware("pr-42-deadbeefcafe"))
            self.assertEqual(extract_app_descriptor_version(firmware), "pr-42-deadbeefcafe")
            self.assertEqual(verify_app_descriptor(firmware, "pr-42-deadbeefcafe"), "pr-42-deadbeefcafe")
            with self.assertRaises(CandidateError):
                verify_app_descriptor(firmware, "1.1.0")

    def test_rejects_wrong_descriptor_location_and_unterminated_field(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            firmware = Path(tmp) / "firmware.bin"
            firmware.write_bytes(b"x" * 512)
            with self.assertRaises(CandidateError):
                extract_app_descriptor_version(firmware)
            data = bytearray(fake_firmware("1.1.0"))
            data[APP_DESC_VERSION_OFFSET : APP_DESC_VERSION_OFFSET + APP_DESC_FIELD_SIZE] = b"1" * 32
            firmware.write_bytes(data)
            with self.assertRaises(CandidateError):
                extract_app_descriptor_version(firmware)


class MetadataTests(unittest.TestCase):
    def test_release_and_pr_versions_are_bound_to_analysis_and_head(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            notes = Path(tmp) / "notes.md"
            notes.write_text("notes\n", encoding="utf-8")
            validate_metadata(metadata(notes), "pr-42-deadbeefcafe", notes)
            validate_metadata(metadata(notes, release=True), "1.1.0", notes)
            with self.assertRaises(CandidateError):
                validate_metadata(metadata(notes), "pr-42-aaaaaaaaaaaa", notes)
            with self.assertRaises(CandidateError):
                validate_metadata(metadata(notes, release=True), "1.1.1", notes)

    def test_metadata_is_closed_and_notes_digest_is_exact(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            notes = Path(tmp) / "notes.md"
            notes.write_text("notes\n", encoding="utf-8")
            value = metadata(notes)
            value["artifact_id"] = 123
            with self.assertRaises(CandidateError):
                validate_metadata(value, "pr-42-deadbeefcafe", notes)
            value = metadata(notes)
            notes.write_text("changed\n", encoding="utf-8")
            with self.assertRaises(CandidateError):
                validate_metadata(value, "pr-42-deadbeefcafe", notes)

    def test_artifact_name_is_deterministic(self) -> None:
        self.assertEqual(artifact_name(42, HEAD_SHA), "firmware-candidate-pr-42-deadbeefcafe")


class SourceCheckoutTests(unittest.TestCase):
    def test_accepts_only_the_exact_clean_head_and_tree(self) -> None:
        source_metadata = {"source": {"head_sha": HEAD_SHA, "tree_sha": TREE_SHA}}
        command_results = [
            SimpleNamespace(stdout=f"{HEAD_SHA}\n"),
            SimpleNamespace(stdout=f"{TREE_SHA}\n"),
            SimpleNamespace(stdout=""),
            SimpleNamespace(stdout=" 0123456789abcdef components/littlefs\n"),
        ]
        with patch("tools.firmware_candidate.candidate._run", side_effect=command_results):
            verify_source_checkout(Path("/repo"), source_metadata)

    def test_rejects_dirty_or_mismatched_source(self) -> None:
        source_metadata = {"source": {"head_sha": HEAD_SHA, "tree_sha": TREE_SHA}}
        dirty_results = [
            SimpleNamespace(stdout=f"{HEAD_SHA}\n"),
            SimpleNamespace(stdout=f"{TREE_SHA}\n"),
            SimpleNamespace(stdout="?? generated.bin\n"),
        ]
        with patch("tools.firmware_candidate.candidate._run", side_effect=dirty_results):
            with self.assertRaises(CandidateError):
                verify_source_checkout(Path("/repo"), source_metadata)

        mismatched_results = [
            SimpleNamespace(stdout=f"{'a' * 40}\n"),
            SimpleNamespace(stdout=f"{TREE_SHA}\n"),
        ]
        with patch("tools.firmware_candidate.candidate._run", side_effect=mismatched_results):
            with self.assertRaises(CandidateError):
                verify_source_checkout(Path("/repo"), source_metadata)


class FlasherTests(unittest.TestCase):
    def test_normalizes_named_idf_schema_and_collects_ota_initializer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            build = make_build(Path(tmp))
            plan, sources = parse_flasher_args(
                build / "flasher_args.json", build, build / "firmware.bin"
            )
            self.assertEqual(
                [image["offset"] for image in plan["images"]],
                ["0x0", "0x8000", "0xf000", "0x20000"],
            )
            self.assertEqual(
                set(sources),
                {"bootloader.bin", "partition-table.bin", "ota_data_initial.bin", "firmware.bin"},
            )
            self.assertNotIn(str(build), json.dumps(plan))
            self.assertEqual(plan["esptool"]["command"][-2:], ["0x20000", "firmware.bin"])

    def test_accepts_only_role_specific_platformio_output_aliases(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            build = make_build(Path(tmp))
            (build / "bootloader.bin").write_bytes((build / "bootloader" / "bootloader.bin").read_bytes())
            (build / "partitions.bin").write_bytes(
                (build / "partition_table" / "partition-table.bin").read_bytes()
            )
            (build / "project_description.json").write_text(
                json.dumps({"app_bin": "ambyte-iot.bin"}), encoding="utf-8"
            )
            (build / "bootloader" / "bootloader.bin").unlink()
            (build / "partition_table" / "partition-table.bin").unlink()
            (build / "ambyte-iot.bin").unlink()
            plan, sources = parse_flasher_args(
                build / "flasher_args.json", build, build / "firmware.bin"
            )
            self.assertEqual(sources["bootloader.bin"], (build / "bootloader.bin").resolve())
            self.assertEqual(sources["partition-table.bin"], (build / "partitions.bin").resolve())
            self.assertEqual(sources["firmware.bin"], (build / "firmware.bin").resolve())
            self.assertEqual(len(plan["images"]), 4)

            raw = json.loads((build / "flasher_args.json").read_text())
            raw["bootloader"]["file"] = "renamed-bootloader.bin"
            raw["flash_files"]["0x0"] = "renamed-bootloader.bin"
            (build / "flasher_args.json").write_text(json.dumps(raw))
            with self.assertRaises(CandidateError):
                parse_flasher_args(build / "flasher_args.json", build, build / "firmware.bin")

    def test_rejects_nvs_unknown_roles_and_outside_paths(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = make_build(root)
            raw = json.loads((build / "flasher_args.json").read_text())
            raw["nvs"] = {"offset": "0x9000", "file": "nvs.bin", "encrypted": "false"}
            raw["flash_files"]["0x9000"] = "nvs.bin"
            (build / "nvs.bin").write_bytes(b"secret")
            (build / "flasher_args.json").write_text(json.dumps(raw))
            with self.assertRaises(CandidateError):
                parse_flasher_args(build / "flasher_args.json", build, build / "firmware.bin")

            build = make_build(root / "second")
            raw = json.loads((build / "flasher_args.json").read_text())
            outside = root / "outside.bin"
            outside.write_bytes(b"outside")
            raw["bootloader"]["file"] = str(outside)
            raw["flash_files"]["0x0"] = str(outside)
            (build / "flasher_args.json").write_text(json.dumps(raw))
            with self.assertRaises(CandidateError):
                parse_flasher_args(build / "flasher_args.json", build, build / "firmware.bin")

    def test_path_normalization_rejects_absolute_parent_and_secret_names(self) -> None:
        for value in ["/tmp/image.bin", "../image.bin", "a/../image.bin", "a\\image.bin", "nvs.bin", ".env"]:
            with self.subTest(value=value), self.assertRaises(CandidateError):
                validate_relative_path(value)


class PackageTests(unittest.TestCase):
    def test_round_trip_is_allowlisted_acyclic_and_portable(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = make_build(root)
            notes = root / "notes.md"
            notes.write_text("# Candidate\n\nDeterministic notes.\n", encoding="utf-8")
            output = root / "candidate"
            package_candidate(
                build_dir=build,
                metadata=metadata(notes),
                release_notes=notes,
                output_dir=output,
                firmware_version="pr-42-deadbeefcafe",
                tool_versions=TOOLS,
            )
            manifest = verify_candidate(output)
            self.assertEqual(manifest["artifact_name"], "firmware-candidate-pr-42-deadbeefcafe")
            self.assertEqual(
                {path.name for path in output.iterdir()},
                {
                    "firmware.bin", "ambyte-iot-pr-42-deadbeefcafe.zip", "release-notes.md",
                    "candidate-manifest.json", "candidate-SHA256SUMS",
                },
            )
            outer_sums = (output / "candidate-SHA256SUMS").read_text()
            self.assertNotIn("candidate-manifest.json", outer_sums)
            self.assertNotIn("candidate-SHA256SUMS", outer_sums)
            with zipfile.ZipFile(output / "ambyte-iot-pr-42-deadbeefcafe.zip") as archive:
                names = set(archive.namelist())
                self.assertTrue(all(info.date_time == (1980, 1, 1, 0, 0, 0) for info in archive.infolist()))
                self.assertTrue(all(info.compress_type == zipfile.ZIP_STORED for info in archive.infolist()))
                self.assertIn("ota_data_initial.bin", names)
                self.assertNotIn("nvs.bin", names)
                self.assertNotIn("bundle-manifest.json", archive.read("bundle-SHA256SUMS").decode())
                self.assertNotIn("bundle-SHA256SUMS", archive.read("bundle-SHA256SUMS").decode())
                plan = json.loads(archive.read("flash-plan.json"))
                self.assertTrue(all(not image["path"].startswith("/") for image in plan["images"]))

    def test_release_candidate_preserves_standalone_name_and_versioned_zip(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = make_build(root, "1.1.0")
            notes = root / "notes.md"
            notes.write_text("release\n")
            output = root / "candidate"
            package_candidate(
                build_dir=build,
                metadata=metadata(notes, release=True),
                release_notes=notes,
                output_dir=output,
                firmware_version="1.1.0",
                tool_versions=TOOLS,
            )
            self.assertTrue((output / "firmware.bin").is_file())
            self.assertTrue((output / "ambyte-iot-v1.1.0.zip").is_file())

    def test_verifier_rejects_tampering_and_unknown_files(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = make_build(root)
            notes = root / "notes.md"
            notes.write_text("notes\n")
            output = root / "candidate"
            package_candidate(
                build_dir=build,
                metadata=metadata(notes),
                release_notes=notes,
                output_dir=output,
                firmware_version="pr-42-deadbeefcafe",
                tool_versions=TOOLS,
            )
            (output / "unexpected.txt").write_text("bad")
            with self.assertRaises(CandidateError):
                verify_candidate(output)
            (output / "unexpected.txt").unlink()
            (output / "firmware.bin").write_bytes(b"tampered")
            with self.assertRaises(CandidateError):
                verify_candidate(output)

    def test_verifier_rejects_unknown_nested_manifest_fields(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = make_build(root)
            notes = root / "notes.md"
            notes.write_text("notes\n")
            output = root / "candidate"
            package_candidate(
                build_dir=build,
                metadata=metadata(notes),
                release_notes=notes,
                output_dir=output,
                firmware_version="pr-42-deadbeefcafe",
                tool_versions=TOOLS,
            )
            manifest_path = output / "candidate-manifest.json"
            manifest = json.loads(manifest_path.read_text())
            manifest["workflow"]["artifact_id"] = 999
            manifest_path.write_text(json.dumps(manifest))
            with self.assertRaises(CandidateError):
                verify_candidate(output)

    def test_json_schemas_are_closed_and_parseable(self) -> None:
        schemas = Path(__file__).resolve().parents[1] / "schemas"
        for path in schemas.glob("*.schema.json"):
            with self.subTest(schema=path.name):
                value = json.loads(path.read_text(encoding="utf-8"))
                self.assertEqual(value["$schema"], "https://json-schema.org/draft/2020-12/schema")
                self.assertFalse(value["additionalProperties"])


class RuntimeWiringTests(unittest.TestCase):
    def test_runtime_status_telemetry_and_ota_use_app_descriptor(self) -> None:
        repo = Path(__file__).resolve().parents[3]
        main = (repo / "main" / "app_main.c").read_text(encoding="utf-8")
        router = (repo / "components" / "command_router" / "command_router.c").read_text(encoding="utf-8")
        ota = (repo / "components" / "ota_update" / "ota_update.c").read_text(encoding="utf-8")
        self.assertIn("esp_app_get_description()", main)
        self.assertIn(".running_firmware_version = running_firmware_version", main)
        self.assertIn(".device_firmware        = running_firmware_version", main)
        self.assertNotIn("device_config_get_firmware_version(", main)
        self.assertNotIn("device_config_get_device_firmware(", main)
        self.assertIn("s_cfg.running_firmware_version", router)
        self.assertIn("esp_app_get_description()", ota)


if __name__ == "__main__":
    unittest.main()
