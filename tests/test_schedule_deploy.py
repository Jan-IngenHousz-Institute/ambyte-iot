# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

from flash_gui import release_fetch
from tools.build_schedule_release import build
from tools.fleet_deploy import fleet_deploy
from tools.fleet_deploy import schedule_deploy


REPOSITORY = "example/ambyte-iot"
TAG = "schedule-v1.2.3"
DEVICE_A = "AMBYTE_00:11:22:33:44:55"
DEVICE_B = "AMBYTE_AA:BB:CC:DD:EE:FF"
STATUS_A = f"experiment/data_ingest/v1/site/multispeq/v1.0/{DEVICE_A}/status"
MANIFEST_URL = (
    f"https://github.com/{REPOSITORY}/releases/download/{TAG}/default.yaml.manifest.json"
)
VALID_SCHEDULE = (
    b"schema: jii.ambyte-schedule/v1-draft\n"
    b"jobs:\n"
    b"  manual:\n"
    b"    on: dispatch\n"
    b"    steps: [ { uses: device/status-report } ]\n"
)


def release_fixture(body: bytes = VALID_SCHEDULE, script_name: str = "default") -> dict:
    digest = hashlib.sha256(body).hexdigest()
    asset_url = (
        f"https://github.com/{REPOSITORY}/releases/download/{TAG}/{script_name}.yaml"
    )
    campaign_id = TAG if script_name == "default" else f"{TAG}:{script_name}"
    return {
        "schema_version": 1,
        "script_name": script_name,
        "script_version": "1.2.3",
        "tag": TAG,
        "sha256": digest,
        "size_bytes": len(body),
        "built_against_fw": "4.5.6",
        "asset_url": asset_url,
        "script_update": {
            "type": "script_update",
            "id": campaign_id,
            "url": asset_url,
            "checksum": digest,
            "script_version": "1.2.3",
            "built_against_fw": "4.5.6",
        },
    }


class ManifestTest(unittest.TestCase):
    def test_validates_real_schema_and_downloaded_asset(self) -> None:
        body = VALID_SCHEDULE
        manifest = release_fixture(body)
        responses = {
            MANIFEST_URL: json.dumps(manifest).encode(),
            manifest["asset_url"]: body,
        }

        verified, asset = schedule_deploy.fetch_release(
            REPOSITORY,
            TAG,
            downloader=lambda url, **kwargs: responses[url],
        )

        self.assertEqual(verified, manifest)
        self.assertEqual(asset, body)

    def test_keeps_pre_catalog_default_manifests_deployable(self) -> None:
        manifest = release_fixture()
        del manifest["script_name"]
        self.assertEqual(
            schedule_deploy.validate_manifest(manifest, REPOSITORY, TAG), manifest
        )

    def test_validates_and_fetches_selected_script(self) -> None:
        script_name = "legacy_1Hz_spec"
        body = VALID_SCHEDULE.replace(b"manual", b"named")
        manifest = release_fixture(body, script_name)
        manifest_url = (
            f"https://github.com/{REPOSITORY}/releases/download/{TAG}/"
            f"{script_name}.yaml.manifest.json"
        )
        responses = {
            manifest_url: json.dumps(manifest).encode(),
            manifest["asset_url"]: body,
        }

        verified, asset = schedule_deploy.fetch_release(
            REPOSITORY,
            TAG,
            script_name,
            downloader=lambda url, **kwargs: responses[url],
        )

        self.assertEqual(verified["script_name"], script_name)
        self.assertEqual(verified["script_update"]["id"], f"{TAG}:{script_name}")
        self.assertEqual(asset, body)

    def test_rejects_schema_tag_version_url_and_command_drift(self) -> None:
        mutations = [
            ("schema", lambda m: m.__setitem__("schema_version", 2)),
            ("tag", lambda m: m.__setitem__("tag", "schedule-v9.9.9")),
            ("version", lambda m: m.__setitem__("script_version", "9.9.9")),
            ("url", lambda m: m.__setitem__("asset_url", "https://example.test/default.yaml")),
            (
                "command",
                lambda m: m["script_update"].__setitem__("checksum", "0" * 64),
            ),
        ]
        for name, mutate in mutations:
            with self.subTest(name=name):
                manifest = release_fixture()
                mutate(manifest)
                with self.assertRaises(schedule_deploy.ManifestError):
                    schedule_deploy.validate_manifest(manifest, REPOSITORY, TAG)

    def test_rejects_selected_script_manifest_drift_and_unsafe_name(self) -> None:
        manifest = release_fixture(script_name="legacy_1Hz_spec")
        with self.assertRaises(schedule_deploy.ManifestError):
            schedule_deploy.validate_manifest(
                manifest, REPOSITORY, TAG, "different_script"
            )
        with self.assertRaises(schedule_deploy.ManifestError):
            schedule_deploy.fetch_release(REPOSITORY, TAG, "../main")

    def test_rejects_asset_size_and_digest_mismatches(self) -> None:
        for name, body in (("size", b"short"), ("digest", b"xxxxxxxxxx")):
            with self.subTest(name=name):
                manifest = release_fixture()
                responses = {
                    MANIFEST_URL: json.dumps(manifest).encode(),
                    manifest["asset_url"]: body,
                }
                with self.assertRaises(schedule_deploy.ManifestError):
                    schedule_deploy.fetch_release(
                        REPOSITORY,
                        TAG,
                        downloader=lambda url, **kwargs: responses[url],
                    )

    def test_download_uses_the_firmware_parser_limit(self) -> None:
        manifest = release_fixture()
        requested_limits = {}

        def downloader(url, *, max_bytes):
            requested_limits[url] = max_bytes
            if url == MANIFEST_URL:
                return json.dumps(manifest).encode()
            return VALID_SCHEDULE

        schedule_deploy.fetch_release(REPOSITORY, TAG, downloader=downloader)
        self.assertEqual(
            requested_limits[manifest["asset_url"]],
            schedule_deploy.SCHEDULE_MAX_BYTES,
        )

    def test_builder_deployer_and_flash_gui_share_one_manifest_contract(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)
            manifest = build(
                Path("schedule/default.yaml"),
                output,
                "1.2.3",
                "4.5.6",
                REPOSITORY,
            )
            deployed = schedule_deploy.validate_manifest(
                manifest, REPOSITORY, TAG, "default"
            )
            gui_script = release_fetch._validated_schedule_script(
                TAG,
                {
                    "name": "default.yaml",
                    "size": manifest["size_bytes"],
                    "browser_download_url": manifest["asset_url"],
                },
                manifest,
            )

            self.assertEqual(deployed, manifest)
            self.assertEqual(gui_script.sha256, manifest["sha256"])
            self.assertEqual(gui_script.campaign_id,
                             manifest["script_update"]["id"])
            self.assertEqual(
                (output / "default.yaml").read_bytes(),
                Path("schedule/default.yaml").read_bytes(),
            )


class TargetingTest(unittest.TestCase):
    def test_device_normalization_reuses_fleet_contract(self) -> None:
        devices = schedule_deploy.parse_devices(
            "00:11:22:33:44:55, ambyte_aa:bb:cc:dd:ee:ff 00:11:22:33:44:55"
        )
        self.assertEqual(devices, [DEVICE_A, "ambyte_aa:bb:cc:dd:ee:ff"])
        with self.assertRaises(ValueError):
            schedule_deploy.parse_devices("not-a-device")

    def test_firmware_filter_and_deterministic_percentage(self) -> None:
        device_c = "AMBYTE_12:34:56:78:9A:BC"
        universe = [DEVICE_A, DEVICE_B, device_c]
        firmware = {DEVICE_A: "1.0.0", DEVICE_B: "2.0.0", device_c: None}
        selected = schedule_deploy.select_cohort(
            universe, firmware, "lt", "1.5.0", 50
        )
        self.assertEqual(selected["matching"], [DEVICE_A])
        self.assertEqual(selected["cohort"], [DEVICE_A])
        self.assertEqual(selected["unproven"], [device_c])

        all_selected = schedule_deploy.select_cohort(
            universe, firmware, "any", None, 67
        )
        ordered = sorted(universe, key=fleet_deploy.selection_key)
        self.assertEqual(all_selected["cohort"], sorted(ordered[:3]))

    def test_dry_run_pings_but_never_publishes_script_update(self) -> None:
        manifest = release_fixture()
        with (
            mock.patch.object(
                schedule_deploy, "fetch_release", return_value=(manifest, VALID_SCHEDULE)
            ),
            mock.patch.object(schedule_deploy.fleet, "boto_session", return_value=object()),
            mock.patch.object(
                schedule_deploy.fleet,
                "fleet_ping",
                return_value={DEVICE_A: "1.0.0"},
            ) as ping,
            mock.patch.object(schedule_deploy, "fleet_script_update") as update,
            mock.patch.object(schedule_deploy, "write_results"),
            mock.patch.object(schedule_deploy, "write_summary"),
        ):
            result = schedule_deploy.main(
                ["--tag", TAG, "--devices", DEVICE_A, "--dry-run"]
            )

        self.assertEqual(result, 0)
        ping.assert_called_once()
        update.assert_not_called()

    def test_live_run_requires_at_least_one_verified_applied_sha(self) -> None:
        manifest = release_fixture()
        cases = [
            (
                "verified",
                {
                    "accepted": True,
                    "state": "applied",
                    "script_sha256": manifest["sha256"],
                    "script_version": manifest["script_version"],
                    "script_metadata_verified": True,
                },
                0,
            ),
            (
                "mismatch",
                {
                    "accepted": True,
                    "state": "applied",
                    "script_sha256": "0" * 64,
                },
                1,
            ),
            ("busy", {"accepted": False, "state": "busy"}, 1),
            ("silent", {"accepted": False, "state": None}, 1),
        ]
        for name, record, expected in cases:
            with (
                self.subTest(name=name),
                mock.patch.object(
                    schedule_deploy, "fetch_release", return_value=(manifest, VALID_SCHEDULE)
                ),
                mock.patch.object(
                    schedule_deploy.fleet, "boto_session", return_value=object()
                ),
                mock.patch.object(
                    schedule_deploy.fleet,
                    "fleet_ping",
                    return_value={DEVICE_A: "1.0.0"},
                ),
                mock.patch.object(
                    schedule_deploy,
                    "fleet_script_update",
                    return_value=({DEVICE_A: record}, None),
                ),
                mock.patch.object(schedule_deploy, "write_results"),
                mock.patch.object(schedule_deploy, "write_summary"),
            ):
                result = schedule_deploy.main(["--tag", TAG, "--devices", DEVICE_A])
            self.assertEqual(result, expected)


class StatusTest(unittest.TestCase):
    def test_correlates_status_case_insensitively_but_keeps_requested_route(self) -> None:
        lower = DEVICE_A.replace("AMBYTE_", "ambyte_")
        tracker = schedule_deploy.ScriptStatusTracker([lower], TAG)
        terminal = {
            "type": "script_status",
            "id": TAG,
            "state": "applied",
            "script_sha256": "a" * 64,
        }
        self.assertEqual(
            tracker.record(STATUS_A, json.dumps(terminal)), (lower, "applied")
        )
        self.assertEqual(tracker.results()[lower]["state"], "applied")

    def test_correlates_topic_and_campaign_and_ignores_duplicate_out_of_order(self) -> None:
        tracker = schedule_deploy.ScriptStatusTracker([DEVICE_A], TAG)
        ignored = [
            (STATUS_A, {"type": "ota_status", "id": TAG, "state": "applied"}),
            (STATUS_A, {"type": "script_status", "id": "other", "state": "applied"}),
            (
                STATUS_A.replace(DEVICE_A, DEVICE_B),
                {"type": "script_status", "id": TAG, "state": "applied"},
            ),
        ]
        for topic, data in ignored:
            self.assertIsNone(tracker.record(topic, json.dumps(data)))

        terminal = {
            "type": "script_status",
            "id": TAG,
            "state": "applied",
            "script_sha256": "a" * 64,
            "script_version": "1.2.3",
        }
        self.assertEqual(tracker.record(STATUS_A, json.dumps(terminal)), (DEVICE_A, "applied"))
        # A delayed accepted and a duplicate failed report cannot regress or
        # overwrite the first terminal result.
        self.assertIsNone(
            tracker.record(
                STATUS_A,
                json.dumps(
                    {"type": "script_status", "id": TAG, "state": "accepted"}
                ),
            )
        )
        self.assertIsNone(
            tracker.record(
                STATUS_A,
                json.dumps({"type": "script_status", "id": TAG, "state": "failed"}),
            )
        )
        result = tracker.results()[DEVICE_A]
        self.assertEqual(result["state"], "applied")
        self.assertTrue(result["accepted"])
        self.assertEqual(result["script_sha256"], "a" * 64)

    def test_classification(self) -> None:
        cases = [
            (
                {"state": "applied", "accepted": True, "script_sha256": "a" * 64},
                "applied",
            ),
            (
                {"state": "applied", "accepted": True, "script_sha256": "b" * 64},
                "applied (sha mismatch)",
            ),
            ({"state": "applied", "accepted": True}, "applied (sha mismatch)"),
            ({"state": "failed", "accepted": True}, "failed"),
            ({"state": "busy", "accepted": False}, "busy"),
            ({"state": None, "accepted": True}, "accepted"),
            ({"state": None, "accepted": False}, "no_reply"),
        ]
        for record, expected in cases:
            with self.subTest(expected=expected):
                self.assertEqual(schedule_deploy.classify(record, "a" * 64), expected)

    def test_partial_results_survive_publish_error_and_error_is_redacted(self) -> None:
        class Future:
            def result(self):
                return None

        class Connection:
            def __init__(self, callback):
                self.callback = callback
                self.publishes = []

            def publish(self, **kwargs):
                self.publishes.append(kwargs)
                if len(self.publishes) == 1:
                    self.callback(
                        STATUS_A,
                        json.dumps(
                            {"type": "script_status", "id": TAG, "state": "applied"}
                        ).encode(),
                        False,
                        1,
                        False,
                    )
                    return Future(), 1
                raise RuntimeError(
                    "https://iot.example.test/path?token=fixture-secret password=hunter2"
                )

            def disconnect(self):
                return Future()

        connection_holder = {}

        def connection_factory(session, topic, callback, client_id):
            connection_holder["value"] = Connection(callback)
            return connection_holder["value"]

        fake_mqtt = types.SimpleNamespace(
            QoS=types.SimpleNamespace(AT_LEAST_ONCE=1)
        )
        with mock.patch.dict(sys.modules, {"awscrt": types.SimpleNamespace(mqtt=fake_mqtt)}):
            results, error = schedule_deploy.fleet_script_update(
                object(),
                [DEVICE_A, DEVICE_B],
                {"type": "script_update", "id": TAG},
                1,
                1,
                10,
                0,
                connection_factory=connection_factory,
            )

        self.assertEqual(results[DEVICE_A]["state"], "applied")
        self.assertEqual(results[DEVICE_B]["state"], None)
        self.assertNotIn("fixture-secret", error)
        self.assertNotIn("hunter2", error)
        first_publish = connection_holder["value"].publishes[0]
        self.assertEqual(first_publish["qos"], 1)
        self.assertIs(first_publish["retain"], False)

    def test_result_redaction_does_not_serialize_fixture_secrets(self) -> None:
        plan = {
            "error": "token=fixture-secret",
            "url": "https://example.test/default.yaml?x-amz-signature=fixture-signature",
        }
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "results.json"
            schedule_deploy.write_results(str(path), plan)
            rendered = path.read_text(encoding="utf-8")
        self.assertNotIn("fixture-secret", rendered)
        self.assertNotIn("fixture-signature", rendered)
        self.assertIn("<redacted>", rendered)


if __name__ == "__main__":
    unittest.main()
