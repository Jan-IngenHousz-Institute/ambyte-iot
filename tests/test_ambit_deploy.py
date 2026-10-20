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

from tools.fleet_deploy import ambit_deploy


TAG = "v1.2.3"
APP_NAME = "ambit-fw-v1.2.3.bin"
DEVICE_A = "AMBYTE_00:11:22:33:44:55"
DEVICE_B = "AMBYTE_AA:BB:CC:DD:EE:FF"
STATUS_A = f"experiment/data_ingest/v1/site/multispeq/v1.0/{DEVICE_A}/status"
ROOT = Path(__file__).resolve().parents[1]


def release_fixture(
    *, tag: str = TAG, image: bytes | None = None, prerelease: bool = False
) -> tuple[dict, dict, bytes, bytes]:
    version = tag.removeprefix("v")
    app_name = f"ambit-fw-v{version}.bin"
    image = image if image is not None else b"\xe9" + b"A" * 31
    digest = hashlib.sha256(image).hexdigest()
    manifest = {
        "name": "ambit-iot",
        "version": version,
        "chip": "esp32c3",
        "flash": [
            {"file": "bootloader.bin", "offset": "0x0", "size": 12, "sha256": "1" * 64},
            {"file": app_name, "offset": "0x10000", "size": len(image), "sha256": digest},
        ],
        "ota": {"file": app_name},
    }
    manifest_bytes = json.dumps(manifest).encode()
    manifest_url = ambit_deploy._release_asset_url(
        ambit_deploy.AMBIT_REPOSITORY, tag, ambit_deploy.MANIFEST_NAME
    )
    app_url = ambit_deploy._release_asset_url(
        ambit_deploy.AMBIT_REPOSITORY, tag, app_name
    )
    release = {
        "id": 12345,
        "tag_name": tag,
        "target_commitish": "abc123",
        "published_at": "2026-08-05T10:00:00Z",
        "html_url": f"https://github.com/{ambit_deploy.AMBIT_REPOSITORY}/releases/tag/{tag}",
        "draft": False,
        "prerelease": prerelease,
        "immutable": True,
        "assets": [
            {
                "id": 10,
                "name": ambit_deploy.MANIFEST_NAME,
                "state": "uploaded",
                "size": len(manifest_bytes),
                "digest": f"sha256:{hashlib.sha256(manifest_bytes).hexdigest()}",
                "browser_download_url": manifest_url,
            },
            {
                "id": 11,
                "name": app_name,
                "state": "uploaded",
                "size": len(image),
                "digest": f"sha256:{digest}",
                "browser_download_url": app_url,
            },
        ],
    }
    return release, manifest, manifest_bytes, image


def verified_fixture(tag: str = TAG) -> tuple[dict, dict]:
    release, manifest, manifest_bytes, image = release_fixture(tag=tag)
    selected = ambit_deploy.validate_manifest(manifest, tag)
    proof = {
        "release_id": release["id"],
        "application": {
            "url": release["assets"][1]["browser_download_url"],
            "name": selected["app_name"],
            "offset": selected["offset"],
            "size": selected["size"],
            "sha256": selected["sha256"],
        },
    }
    return selected, proof


def channels(*versions: str | None) -> dict[str, dict]:
    values = list(versions) + [None] * (4 - len(versions))
    return {
        str(channel): {"present": version is not None, "version": version}
        for channel, version in enumerate(values[:4])
    }


def collect_version_replies(replies: list[dict]) -> dict[str, dict]:
    class Future:
        def result(self):
            return None

    class Connection:
        def __init__(self, callback):
            self.callback = callback

        def publish(self, **kwargs):
            query_id = json.loads(kwargs["payload"])["id"]
            for reply in replies:
                self.callback(
                    STATUS_A,
                    json.dumps({"type": "ambit_versions", "id": query_id, **reply}),
                    False, 1, False,
                )
            return Future(), 1

        def disconnect(self):
            return Future()

    fake_mqtt = types.SimpleNamespace(QoS=types.SimpleNamespace(AT_LEAST_ONCE=1))
    with mock.patch.dict(sys.modules, {"awscrt": types.SimpleNamespace(mqtt=fake_mqtt)}):
        reports, error = ambit_deploy.fleet_ambit_versions(
            object(), [DEVICE_A], "test-query", 1,
            connection_factory=lambda session, topic, callback, client_id: Connection(callback),
        )
    assert error is None, error
    return reports


class ReleaseValidationTest(unittest.TestCase):
    def test_anonymous_immutable_release_manifest_and_app_are_verified(self) -> None:
        release, manifest, manifest_bytes, image = release_fixture()
        responses = {
            release["assets"][0]["browser_download_url"]: manifest_bytes,
            release["assets"][1]["browser_download_url"]: image,
        }

        selected, fetched, proof = ambit_deploy.fetch_release(
            ambit_deploy.AMBIT_REPOSITORY,
            TAG,
            json_fetcher=lambda url: release,
            downloader=lambda url, **kwargs: responses[url],
        )

        self.assertEqual(fetched, image)
        self.assertEqual(selected["app_name"], APP_NAME)
        self.assertEqual(selected["offset"], "0x10000")
        self.assertEqual(proof["application"]["github_digest"], f"sha256:{selected['sha256']}")
        self.assertTrue(proof["anonymous_metadata_fetch"])
        self.assertTrue(proof["application"]["anonymous_download_verified"])

    def test_release_must_be_published_immutable_stable_by_default(self) -> None:
        cases = [
            ("draft", {"draft": True}),
            ("unpublished", {"published_at": None}),
            ("mutable", {"immutable": False}),
            ("prerelease", {"prerelease": True}),
            ("wrong tag", {"tag_name": "v9.9.9"}),
        ]
        for name, mutation in cases:
            with self.subTest(name=name):
                release, _, manifest_bytes, image = release_fixture()
                release.update(mutation)
                responses = {
                    release["assets"][0]["browser_download_url"]: manifest_bytes,
                    release["assets"][1]["browser_download_url"]: image,
                }
                with self.assertRaises(ambit_deploy.ReleaseError):
                    ambit_deploy.fetch_release(
                        ambit_deploy.AMBIT_REPOSITORY,
                        TAG,
                        json_fetcher=lambda url, release=release: release,
                        downloader=lambda url, **kwargs: responses[url],
                    )

    def test_explicit_prerelease_opt_in_uses_numeric_device_version(self) -> None:
        tag = "v1.2.4-recovery.1"
        release, _, manifest_bytes, image = release_fixture(tag=tag, prerelease=True)
        responses = {
            release["assets"][0]["browser_download_url"]: manifest_bytes,
            release["assets"][1]["browser_download_url"]: image,
        }
        selected, _, _ = ambit_deploy.fetch_release(
            ambit_deploy.AMBIT_REPOSITORY,
            tag,
            allow_prerelease=True,
            json_fetcher=lambda url: release,
            downloader=lambda url, **kwargs: responses[url],
        )
        numeric, _ = ambit_deploy.target_numeric_version(selected["version"])
        self.assertEqual(numeric, "1.2.4")

    def test_manifest_and_rest_asset_identity_fail_closed(self) -> None:
        mutations = [
            ("version", lambda m, r: m.__setitem__("version", "9.9.9")),
            ("offset", lambda m, r: m["flash"][1].__setitem__("offset", "0x0")),
            ("size", lambda m, r: m["flash"][1].__setitem__("size", "32")),
            ("sha", lambda m, r: m["flash"][1].__setitem__("sha256", "A" * 64)),
            ("rest size", lambda m, r: r["assets"][1].__setitem__("size", 99)),
            ("rest digest", lambda m, r: r["assets"][1].__setitem__("digest", "sha256:" + "0" * 64)),
            ("asset url", lambda m, r: r["assets"][1].__setitem__("browser_download_url", "https://example.test/app.bin")),
        ]
        for name, mutate in mutations:
            with self.subTest(name=name):
                release, manifest, _, image = release_fixture()
                mutate(manifest, release)
                manifest_bytes = json.dumps(manifest).encode()
                responses = {
                    release["assets"][0]["browser_download_url"]: manifest_bytes,
                    release["assets"][1]["browser_download_url"]: image,
                }
                with self.assertRaises(ambit_deploy.ReleaseError):
                    ambit_deploy.fetch_release(
                        ambit_deploy.AMBIT_REPOSITORY,
                        TAG,
                        json_fetcher=lambda url, release=release: release,
                        downloader=lambda url, **kwargs: responses[url],
                    )

    def test_downloaded_size_sha_and_esp_magic_are_checked(self) -> None:
        for name, image in (
            ("size", b"\xe9short"),
            ("sha", b"\xe9" + b"B" * 31),
            ("magic", b"X" + b"A" * 31),
        ):
            with self.subTest(name=name):
                release, _, manifest_bytes, _ = release_fixture()
                responses = {
                    release["assets"][0]["browser_download_url"]: manifest_bytes,
                    release["assets"][1]["browser_download_url"]: image,
                }
                with self.assertRaises(ambit_deploy.ReleaseError):
                    ambit_deploy.fetch_release(
                        ambit_deploy.AMBIT_REPOSITORY,
                        TAG,
                        json_fetcher=lambda url, release=release: release,
                        downloader=lambda url, **kwargs: responses[url],
                    )


class PreflightTest(unittest.TestCase):
    def test_parser_requires_all_unique_channels_and_correlated_shape(self) -> None:
        valid = {
            "channels": [
                {"ch": 0, "present": True, "version": "1.0.0"},
                {"ch": 1, "present": False},
                {"ch": 2, "present": False},
                {"ch": 3, "present": False},
            ]
        }
        parsed = ambit_deploy._parse_versions_report(valid)
        self.assertEqual(parsed["channels"]["0"]["version"], "1.0.0")
        self.assertIsNone(ambit_deploy._parse_versions_report({"channels": valid["channels"][:3]}))

    def test_gateway_decisions_respect_all_channel_atomicity(self) -> None:
        reports = {
            DEVICE_A: {"state": "complete", "channels": channels("1.2.3", "1.2.2")},
            DEVICE_B: {"state": "complete", "channels": channels("1.2.4")},
        }
        decisions, deploy = ambit_deploy.decide_gateways(
            [DEVICE_A, DEVICE_B], reports, "1.2.3", False
        )
        self.assertEqual(deploy, [DEVICE_A])
        self.assertTrue(decisions[DEVICE_A]["reflashes_current_channels"])
        self.assertEqual(decisions[DEVICE_B]["skip_reason"], "newer_ambit_present")

        decisions, deploy = ambit_deploy.decide_gateways(
            [DEVICE_A, DEVICE_B], reports, "1.2.3", True
        )
        self.assertEqual(deploy, [DEVICE_A, DEVICE_B])
        self.assertTrue(decisions[DEVICE_B]["downgrades_newer_channels"])

    def test_force_reflash_reapplies_current_only_but_keeps_newer_protection(self) -> None:
        reports = {
            DEVICE_A: {"state": "complete", "channels": channels("1.2.3")},
            DEVICE_B: {"state": "complete", "channels": channels("1.2.4")},
        }
        decisions, deploy = ambit_deploy.decide_gateways(
            [DEVICE_A, DEVICE_B], reports, "1.2.3", False, True
        )
        self.assertEqual(deploy, [DEVICE_A])
        self.assertTrue(decisions[DEVICE_A]["force_reflash"])
        self.assertFalse(decisions[DEVICE_A]["version_effect_can_prove_change"])
        self.assertEqual(decisions[DEVICE_B]["skip_reason"], "newer_ambit_present")

        decisions, deploy = ambit_deploy.decide_gateways(
            [DEVICE_A, DEVICE_B], reports, "1.2.3", True, True
        )
        self.assertEqual(deploy, [DEVICE_A, DEVICE_B])

    def test_no_ambit_current_unknown_busy_and_unreachable_reasons(self) -> None:
        reports = {
            DEVICE_A: {"state": "complete", "channels": channels()},
            DEVICE_B: {"state": "complete", "channels": channels("1.2.3")},
            "AMBYTE_12:34:56:78:9A:BC": {
                "state": "complete",
                "channels": {**channels(), "0": {"present": True, "version": None}},
            },
        }
        cohort = [*reports, "AMBYTE_10:20:30:40:50:60"]
        decisions, deploy = ambit_deploy.decide_gateways(cohort, reports, "1.2.3", False)
        self.assertEqual(deploy, [])
        self.assertEqual(decisions[DEVICE_A]["skip_reason"], "no_ambit_present")
        self.assertEqual(decisions[DEVICE_B]["skip_reason"], "all_ambits_up_to_date")
        self.assertEqual(decisions[cohort[2]]["skip_reason"], "unproven_ambit_version")
        self.assertEqual(decisions[cohort[3]]["skip_reason"], "no_preflight_reply")
        self.assertTrue(decisions[cohort[2]]["blocking"])
        self.assertFalse(decisions[cohort[3]]["blocking"])
        self.assertEqual(decisions[cohort[3]]["action"], "skipped_preflight")

    def test_unconfirmed_absence_without_presence_is_skipped(self) -> None:
        reports = {
            DEVICE_A: {
                "state": "ambiguous",
                "channels": channels(),
                "detail": "absent then no reply",
            }
        }
        decisions, deploy = ambit_deploy.decide_gateways(
            [DEVICE_A], reports, "1.2.3", False
        )
        self.assertEqual(deploy, [])
        self.assertEqual(decisions[DEVICE_A]["skip_reason"], "ambiguous_preflight")
        self.assertFalse(decisions[DEVICE_A]["blocking"])
        self.assertEqual(decisions[DEVICE_A]["action"], "skipped_preflight")


class ColdWakePreflightTest(unittest.TestCase):
    def test_absent_only_sweep_retries_and_recovers_present_channel(self) -> None:
        initial = {DEVICE_A: {"state": "complete", "channels": channels()}}
        retry = {DEVICE_A: {"state": "complete", "channels": channels("1.2.2")}}
        query = mock.Mock(return_value=(retry, None))
        sleep = mock.Mock()

        effective, attempt = ambit_deploy.retry_absent_preflight(
            object(),
            [DEVICE_A],
            initial,
            "1.2.3",
            90,
            3,
            versions_query=query,
            sleep=sleep,
        )

        sleep.assert_called_once_with(3)
        query.assert_called_once()
        self.assertEqual(attempt["devices"], [DEVICE_A])
        self.assertEqual(attempt["reports"], retry)
        self.assertTrue(effective[DEVICE_A]["channels"]["0"]["present"])
        self.assertEqual(effective[DEVICE_A]["preflight_source_attempt"], 2)
        self.assertFalse(effective[DEVICE_A]["absence_confirmed_after_retry"])

    def test_absent_then_no_reply_is_ambiguous_not_no_ambit(self) -> None:
        initial = {DEVICE_A: {"state": "complete", "channels": channels()}}
        effective, attempt = ambit_deploy.retry_absent_preflight(
            object(),
            [DEVICE_A],
            initial,
            "1.2.3",
            90,
            0,
            versions_query=mock.Mock(return_value=({}, None)),
        )
        self.assertIsNotNone(attempt)
        self.assertEqual(effective[DEVICE_A]["state"], "ambiguous")
        self.assertEqual(effective[DEVICE_A]["retry_state"], "no_reply")

    def test_second_absent_sweep_confirms_no_ambit(self) -> None:
        report = {DEVICE_A: {"state": "complete", "channels": channels()}}
        effective, _ = ambit_deploy.retry_absent_preflight(
            object(),
            [DEVICE_A],
            report,
            "1.2.3",
            90,
            0,
            versions_query=mock.Mock(return_value=(report, None)),
        )
        self.assertEqual(effective[DEVICE_A]["state"], "complete")
        self.assertTrue(effective[DEVICE_A]["absence_confirmed_after_retry"])

    def test_partial_measurement_busy_report_merges_retry_versions(self) -> None:
        initial = {
            DEVICE_A: {
                "state": "complete",
                "channels": {
                    "0": {"present": True, "version": "1.2.2"},
                    "1": {"present": True, "version": None},
                    "2": {"present": False, "version": None},
                    "3": {"present": False, "version": None},
                },
            }
        }
        retry = {
            DEVICE_A: {
                "state": "complete",
                "channels": {
                    "0": {"present": False, "version": None},
                    "1": {"present": True, "version": "1.2.2"},
                    "2": {"present": True, "version": "1.2.2"},
                    "3": {"present": False, "version": None},
                },
            }
        }
        effective, attempt = ambit_deploy.retry_absent_preflight(
            object(),
            [DEVICE_A],
            initial,
            "1.2.3",
            90,
            0,
            versions_query=mock.Mock(return_value=(retry, None)),
        )
        self.assertEqual(attempt["devices"], [DEVICE_A])
        self.assertEqual(
            effective[DEVICE_A]["channels"],
            {
                "0": {"present": True, "version": "1.2.2"},
                "1": {"present": True, "version": "1.2.2"},
                "2": {"present": True, "version": "1.2.2"},
                "3": {"present": False, "version": None},
            },
        )
        self.assertEqual(effective[DEVICE_A]["state"], "complete")

    def test_conflicting_positive_versions_remain_ambiguous(self) -> None:
        initial = {DEVICE_A: {"state": "complete", "channels": channels("1.2.1")}}
        retry = {DEVICE_A: {"state": "complete", "channels": channels("1.2.2")}}
        effective, _ = ambit_deploy.retry_absent_preflight(
            object(),
            [DEVICE_A],
            initial,
            "1.2.3",
            90,
            0,
            versions_query=mock.Mock(return_value=(retry, None)),
        )
        self.assertEqual(effective[DEVICE_A]["state"], "ambiguous")
        self.assertIn("conflicting versions", effective[DEVICE_A]["detail"])

    def test_four_proven_versions_do_not_retry(self) -> None:
        report = {
            DEVICE_A: {
                "state": "complete",
                "channels": channels("1.2.2", "1.2.2", "1.2.2", "1.2.2"),
            }
        }
        query = mock.Mock()
        effective, attempt = ambit_deploy.retry_absent_preflight(
            object(),
            [DEVICE_A],
            report,
            "1.2.3",
            90,
            15,
            versions_query=query,
        )
        self.assertEqual(effective, report)
        self.assertIsNone(attempt)
        query.assert_not_called()


class StatusTrackerTest(unittest.TestCase):
    def test_correlates_status_case_insensitively_but_keeps_requested_route(self) -> None:
        lower = DEVICE_A.replace("AMBYTE_", "ambyte_")
        tracker = ambit_deploy.AmbitStatusTracker([lower], "run-1")
        event = tracker.record(
            STATUS_A,
            json.dumps(
                {
                    "type": "ambit_ota_status",
                    "id": "run-1",
                    "channel": 255,
                    "state": "accepted",
                }
            ),
        )
        self.assertEqual(event, (lower, "accepted"))

    def test_correlates_campaign_gateway_channels_and_overall(self) -> None:
        tracker = ambit_deploy.AmbitStatusTracker([DEVICE_A], "run-1")
        ignored = [
            {"type": "ambit_ota_status", "id": "other", "channel": 0, "state": "success"},
            {"type": "ota_status", "id": "run-1", "channel": 0, "state": "success"},
        ]
        for payload in ignored:
            self.assertIsNone(tracker.record(STATUS_A, json.dumps(payload)))

        events = [
            {"type": "ambit_ota_status", "id": "run-1", "channel": 255, "state": "accepted"},
            {"type": "ambit_ota_status", "id": "run-1", "channel": 0, "state": "success"},
            {"type": "ambit_ota_status", "id": "run-1", "channel": 1, "state": "failed", "detail": "stream"},
            {"type": "ambit_ota_status", "id": "run-1", "channel": 2, "state": "absent"},
            {"type": "ambit_ota_status", "id": "run-1", "channel": 3, "state": "absent"},
            {"type": "ambit_ota_status", "id": "run-1", "channel": 255, "state": "failed"},
        ]
        for payload in events:
            self.assertIsNotNone(tracker.record(STATUS_A, json.dumps(payload)))
        self.assertTrue(tracker.complete())
        result = tracker.results()[DEVICE_A]
        self.assertTrue(result["accepted"])
        self.assertEqual(result["channels"]["1"]["detail"], "stream")
        self.assertEqual(result["overall"]["state"], "failed")
        self.assertIsNone(tracker.record(STATUS_A, json.dumps(events[1])))

    def test_busy_is_an_overall_terminal_failure_without_waiting_for_channels(self) -> None:
        tracker = ambit_deploy.AmbitStatusTracker([DEVICE_A], "run-1")
        event = tracker.record(
            STATUS_A,
            json.dumps(
                {
                    "type": "ambit_ota_status",
                    "id": "run-1",
                    "channel": 255,
                    "state": "busy",
                    "detail": "another maintenance op is in progress",
                }
            ),
        )
        self.assertEqual(event, (DEVICE_A, "overall_busy"))
        self.assertTrue(tracker.complete())
        result = tracker.results()[DEVICE_A]
        self.assertEqual(result["overall"]["state"], "busy")
        self.assertEqual(result["channels"]["0"]["state"], "timeout")

    def test_missing_per_channel_and_overall_become_timeouts_and_fail(self) -> None:
        tracker = ambit_deploy.AmbitStatusTracker([DEVICE_A], "run-1")
        result = tracker.results()
        decisions = {
            DEVICE_A: {
                "present_channels": ["0"],
                "version_effect_can_prove_change": True,
            }
        }
        assessments, failures = ambit_deploy.assess_post_verification(
            [DEVICE_A], decisions, result, {}, "1.2.3"
        )
        self.assertEqual(assessments[DEVICE_A]["outcome"], "indeterminate")
        self.assertTrue(failures)


class PostVerificationTest(unittest.TestCase):
    def terminal(
        self, *, overall: str = "timeout", channel0: str = "timeout"
    ) -> dict:
        return {
            DEVICE_A: {
                "accepted": overall != "timeout",
                "channels": {
                    "0": {"state": channel0, "detail": None},
                    "1": {"state": "absent", "detail": None},
                    "2": {"state": "absent", "detail": None},
                    "3": {"state": "absent", "detail": None},
                },
                "overall": {"state": overall, "detail": None},
            }
        }

    def decision(self, can_prove_change: bool = True) -> dict:
        return {
            DEVICE_A: {
                "present_channels": ["0"],
                "version_effect_can_prove_change": can_prove_change,
            }
        }

    def report(self, version: str = "1.2.3") -> dict:
        return {
            DEVICE_A: {"state": "complete", "channels": channels(version)}
        }

    def test_version_effect_confirms_success_when_terminal_reports_drop(self) -> None:
        assessments, failures = ambit_deploy.assess_post_verification(
            [DEVICE_A],
            self.decision(True),
            self.terminal(),
            self.report(),
            "1.2.3",
        )
        self.assertEqual(assessments[DEVICE_A]["outcome"], "confirmed_success")
        self.assertEqual(failures, [])

    def test_explicit_present_channel_failure_is_never_masked(self) -> None:
        assessments, failures = ambit_deploy.assess_post_verification(
            [DEVICE_A],
            self.decision(True),
            self.terminal(overall="failed", channel0="failed"),
            self.report(),
            "1.2.3",
        )
        self.assertEqual(assessments[DEVICE_A]["outcome"], "failed")
        self.assertIn("channel_0_failed", assessments[DEVICE_A]["reasons"])
        self.assertTrue(failures)

    def test_mismatch_fails_and_missing_verification_is_indeterminate(self) -> None:
        mismatch, mismatch_failures = ambit_deploy.assess_post_verification(
            [DEVICE_A],
            self.decision(True),
            self.terminal(overall="success", channel0="success"),
            self.report("1.2.2"),
            "1.2.3",
        )
        self.assertEqual(mismatch[DEVICE_A]["outcome"], "failed")
        self.assertTrue(mismatch_failures)

        missing, missing_failures = ambit_deploy.assess_post_verification(
            [DEVICE_A],
            self.decision(True),
            self.terminal(overall="success", channel0="success"),
            {},
            "1.2.3",
        )
        self.assertEqual(missing[DEVICE_A]["outcome"], "indeterminate")
        self.assertTrue(missing_failures)

    def test_same_numeric_force_reflash_needs_complete_terminal_success(self) -> None:
        indeterminate, failures = ambit_deploy.assess_post_verification(
            [DEVICE_A],
            self.decision(False),
            self.terminal(),
            self.report(),
            "1.2.3",
        )
        self.assertEqual(indeterminate[DEVICE_A]["outcome"], "indeterminate")
        self.assertTrue(failures)

        confirmed, failures = ambit_deploy.assess_post_verification(
            [DEVICE_A],
            self.decision(False),
            self.terminal(overall="success", channel0="success"),
            self.report(),
            "1.2.3",
        )
        self.assertEqual(confirmed[DEVICE_A]["outcome"], "confirmed_success")
        self.assertEqual(failures, [])


class RunnerTest(unittest.TestCase):
    def test_parser_uses_one_hour_terminal_budget(self) -> None:
        args = ambit_deploy.build_parser().parse_args(["--tag", TAG])
        self.assertEqual(args.final_seconds, 3600)
        self.assertEqual(args.verify_seconds, 120)
        self.assertEqual(args.absent_retry_delay_seconds, 65)

    def test_dry_run_preflights_but_never_publishes_ambit_ota(self) -> None:
        manifest, proof = verified_fixture()
        report = {
            DEVICE_A: {
                "state": "complete",
                "channels": channels("1.2.2", "1.2.2", "1.2.2", "1.2.2"),
            }
        }
        with tempfile.TemporaryDirectory() as temporary:
            result_path = Path(temporary) / "results.json"
            with (
                mock.patch.object(ambit_deploy, "fetch_release", return_value=(manifest, b"", proof)),
                mock.patch.object(ambit_deploy.fleet, "boto_session", return_value=object()),
                mock.patch.object(ambit_deploy.fleet, "fleet_ping", return_value={DEVICE_A: "1.5.1"}),
                mock.patch.object(ambit_deploy, "fleet_ambit_versions", return_value=(report, None)) as preflight,
                mock.patch.object(ambit_deploy, "fleet_ambit_ota") as ota,
            ):
                code = ambit_deploy.main(
                    ["--tag", TAG, "--devices", DEVICE_A, "--dry-run", "--results-json", str(result_path)]
                )
            rendered = json.loads(result_path.read_text())
        self.assertEqual(code, 0)
        preflight.assert_called_once()
        ota.assert_not_called()
        self.assertEqual(rendered["deployed_gateways"], [DEVICE_A])
        self.assertEqual(rendered["command"]["channel"], "all")

    def test_all_up_to_date_is_clean_live_success(self) -> None:
        manifest, proof = verified_fixture()
        report = {
            DEVICE_A: {
                "state": "complete",
                "channels": channels("1.2.3", "1.2.3", "1.2.3", "1.2.3"),
            }
        }
        with (
            mock.patch.object(ambit_deploy, "fetch_release", return_value=(manifest, b"", proof)),
            mock.patch.object(ambit_deploy.fleet, "boto_session", return_value=object()),
            mock.patch.object(ambit_deploy.fleet, "fleet_ping", return_value={DEVICE_A: "1.5.1"}),
            mock.patch.object(ambit_deploy, "fleet_ambit_versions", return_value=(report, None)),
            mock.patch.object(ambit_deploy, "fleet_ambit_ota") as ota,
        ):
            code = ambit_deploy.main(["--tag", TAG, "--devices", DEVICE_A])
        self.assertEqual(code, 0)
        ota.assert_not_called()

    def test_force_reflash_requires_exact_devices_and_full_percentage(self) -> None:
        cases = [
            ["--tag", TAG, "--force-reflash"],
            [
                "--tag",
                TAG,
                "--force-reflash",
                "--devices",
                DEVICE_A,
                "--percentage",
                "50",
            ],
        ]
        for argv in cases:
            with self.subTest(argv=argv), mock.patch.object(
                ambit_deploy, "fetch_release"
            ) as fetch:
                self.assertEqual(ambit_deploy.main(argv), 2)
                fetch.assert_not_called()

    def test_force_reflash_exact_current_device_is_planned(self) -> None:
        manifest, proof = verified_fixture()
        report = {
            DEVICE_A: {
                "state": "complete",
                "channels": channels("1.2.3", "1.2.3", "1.2.3", "1.2.3"),
            }
        }
        with tempfile.TemporaryDirectory() as temporary:
            result_path = Path(temporary) / "results.json"
            with (
                mock.patch.object(
                    ambit_deploy,
                    "fetch_release",
                    return_value=(manifest, b"", proof),
                ),
                mock.patch.object(
                    ambit_deploy.fleet, "boto_session", return_value=object()
                ),
                mock.patch.object(
                    ambit_deploy.fleet,
                    "fleet_ping",
                    return_value={DEVICE_A: "1.5.1"},
                ),
                mock.patch.object(
                    ambit_deploy,
                    "fleet_ambit_versions",
                    return_value=(report, None),
                ),
                mock.patch.object(ambit_deploy, "fleet_ambit_ota") as ota,
            ):
                code = ambit_deploy.main(
                    [
                        "--tag",
                        TAG,
                        "--devices",
                        DEVICE_A,
                        "--force-reflash",
                        "--dry-run",
                        "--results-json",
                        str(result_path),
                    ]
                )
            rendered = json.loads(result_path.read_text())
        self.assertEqual(code, 0)
        ota.assert_not_called()
        self.assertEqual(rendered["deployed_gateways"], [DEVICE_A])
        self.assertTrue(rendered["gateway_decisions"][DEVICE_A]["force_reflash"])

    def test_targeting_failure_preserves_exact_release_proof(self) -> None:
        manifest, proof = verified_fixture()
        with tempfile.TemporaryDirectory() as temporary:
            result_path = Path(temporary) / "results.json"
            with (
                mock.patch.object(
                    ambit_deploy,
                    "fetch_release",
                    return_value=(manifest, b"", proof),
                ),
                mock.patch.object(
                    ambit_deploy.fleet,
                    "boto_session",
                    side_effect=RuntimeError("AWS unavailable"),
                ),
            ):
                code = ambit_deploy.main(
                    [
                        "--tag",
                        TAG,
                        "--devices",
                        DEVICE_A,
                        "--results-json",
                        str(result_path),
                    ]
                )
            rendered = json.loads(result_path.read_text())
        self.assertEqual(code, 1)
        self.assertEqual(rendered["release_proof"]["release_id"], 12345)
        self.assertIn("AWS unavailable", rendered["error"])

    def test_live_zero_reachable_or_zero_present_fails_closed(self) -> None:
        manifest, proof = verified_fixture()
        for name, report in (
            ("unreachable", {}),
            ("no ambit", {DEVICE_A: {"state": "complete", "channels": channels()}}),
        ):
            with (
                self.subTest(name=name),
                mock.patch.object(ambit_deploy, "fetch_release", return_value=(manifest, b"", proof)),
                mock.patch.object(ambit_deploy.fleet, "boto_session", return_value=object()),
                mock.patch.object(ambit_deploy.fleet, "fleet_ping", return_value={DEVICE_A: "1.5.1"}),
                mock.patch.object(ambit_deploy, "fleet_ambit_versions", return_value=(report, None)),
            ):
                self.assertEqual(
                    ambit_deploy.main(
                        [
                            "--tag",
                            TAG,
                            "--devices",
                            DEVICE_A,
                            "--absent-retry-delay-seconds",
                            "0",
                        ]
                    ),
                    1,
                )

    def test_cold_wake_retry_recovers_gateway_and_records_both_attempts(self) -> None:
        manifest, proof = verified_fixture()
        absent = {DEVICE_A: {"state": "complete", "channels": channels()}}
        present = {DEVICE_A: {"state": "complete", "channels": channels("1.2.2")}}
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "results.json"
            with (
                mock.patch.object(
                    ambit_deploy, "fetch_release", return_value=(manifest, b"", proof)
                ),
                mock.patch.object(
                    ambit_deploy.fleet, "boto_session", return_value=object()
                ),
                mock.patch.object(
                    ambit_deploy.fleet,
                    "fleet_ping",
                    return_value={DEVICE_A: "1.5.1"},
                ),
                mock.patch.object(
                    ambit_deploy,
                    "fleet_ambit_versions",
                    side_effect=[(absent, None), (present, None)],
                ) as versions,
                mock.patch.object(ambit_deploy, "fleet_ambit_ota") as ota,
            ):
                code = ambit_deploy.main(
                    [
                        "--tag",
                        TAG,
                        "--devices",
                        DEVICE_A,
                        "--dry-run",
                        "--absent-retry-delay-seconds",
                        "0",
                        "--results-json",
                        str(path),
                    ]
                )
            rendered = json.loads(path.read_text())
        self.assertEqual(code, 0)
        self.assertEqual(versions.call_count, 2)
        ota.assert_not_called()
        self.assertEqual(len(rendered["preflight_attempts"]), 2)
        self.assertEqual(rendered["absent_retry_delay_seconds"], 0)
        self.assertEqual(
            rendered["preflight_versions"][DEVICE_A]["preflight_source_attempt"], 2
        )
        self.assertEqual(rendered["deployed_gateways"], [DEVICE_A])

    def test_unconfirmed_cold_wake_absence_skips_and_all_skipped_fails(self) -> None:
        manifest, proof = verified_fixture()
        absent = {DEVICE_A: {"state": "complete", "channels": channels()}}
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "results.json"
            with (
                mock.patch.object(
                    ambit_deploy, "fetch_release", return_value=(manifest, b"", proof)
                ),
                mock.patch.object(
                    ambit_deploy.fleet, "boto_session", return_value=object()
                ),
                mock.patch.object(
                    ambit_deploy.fleet,
                    "fleet_ping",
                    return_value={DEVICE_A: "1.5.1"},
                ),
                mock.patch.object(
                    ambit_deploy,
                    "fleet_ambit_versions",
                    side_effect=[(absent, None), ({}, None)],
                ),
                mock.patch.object(ambit_deploy, "fleet_ambit_ota") as ota,
            ):
                code = ambit_deploy.main(
                    [
                        "--tag",
                        TAG,
                        "--devices",
                        DEVICE_A,
                        "--absent-retry-delay-seconds",
                        "0",
                        "--results-json",
                        str(path),
                    ]
                )
            rendered = json.loads(path.read_text())
        self.assertEqual(code, 1)
        ota.assert_not_called()
        self.assertIn("all selected gateways were skipped_preflight", rendered["error"])
        self.assertEqual(
            rendered["gateway_decisions"][DEVICE_A]["skip_reason"],
            "ambiguous_preflight",
        )
        self.assertEqual(set(rendered["skipped_preflight"]), {DEVICE_A})

    def test_zero_universe_and_zero_selected_cohort_fail_dry_and_live(self) -> None:
        manifest, proof = verified_fixture()
        for dry_run in (False, True):
            with (
                self.subTest(case="zero universe", dry_run=dry_run),
                tempfile.TemporaryDirectory() as temporary,
            ):
                path = Path(temporary) / "results.json"
                argv = ["--tag", TAG, "--results-json", str(path)]
                if dry_run:
                    argv.append("--dry-run")
                with (
                    mock.patch.object(
                        ambit_deploy,
                        "fetch_release",
                        return_value=(manifest, b"", proof),
                    ),
                    mock.patch.object(
                        ambit_deploy.fleet, "boto_session", return_value=object()
                    ),
                    mock.patch.object(
                        ambit_deploy.fleet,
                        "discover_active_devices",
                        return_value=[],
                    ),
                ):
                    code = ambit_deploy.main(argv)
                rendered = json.loads(path.read_text())
                self.assertEqual(code, 1)
                self.assertIn("universe is empty", rendered["error"])

            with (
                self.subTest(case="zero cohort", dry_run=dry_run),
                tempfile.TemporaryDirectory() as temporary,
            ):
                path = Path(temporary) / "results.json"
                argv = [
                    "--tag",
                    TAG,
                    "--devices",
                    DEVICE_A,
                    "--version-op",
                    "gt",
                    "--version",
                    "9.0.0",
                    "--results-json",
                    str(path),
                ]
                if dry_run:
                    argv.append("--dry-run")
                with (
                    mock.patch.object(
                        ambit_deploy,
                        "fetch_release",
                        return_value=(manifest, b"", proof),
                    ),
                    mock.patch.object(
                        ambit_deploy.fleet, "boto_session", return_value=object()
                    ),
                    mock.patch.object(
                        ambit_deploy.fleet,
                        "fleet_ping",
                        return_value={DEVICE_A: "1.5.1"},
                    ),
                    mock.patch.object(ambit_deploy, "fleet_ambit_versions") as versions,
                ):
                    code = ambit_deploy.main(argv)
                rendered = json.loads(path.read_text())
                self.assertEqual(code, 1)
                self.assertIn("empty cohort", rendered["error"])
                versions.assert_not_called()

    def test_present_channel_failure_makes_live_run_fail(self) -> None:
        manifest, proof = verified_fixture()
        report = {
            DEVICE_A: {
                "state": "complete",
                "channels": channels("1.2.2", "1.2.2", "1.2.2", "1.2.2"),
            }
        }
        results = {
            DEVICE_A: {
                "accepted": True,
                "channels": {
                    "0": {"state": "failed", "detail": "stream"},
                    "1": {"state": "absent", "detail": None},
                    "2": {"state": "absent", "detail": None},
                    "3": {"state": "absent", "detail": None},
                },
                "overall": {"state": "failed", "detail": "one or more channels failed"},
            }
        }
        with (
            mock.patch.object(ambit_deploy, "fetch_release", return_value=(manifest, b"", proof)),
            mock.patch.object(ambit_deploy.fleet, "boto_session", return_value=object()),
            mock.patch.object(ambit_deploy.fleet, "fleet_ping", return_value={DEVICE_A: "1.5.1"}),
            mock.patch.object(ambit_deploy, "fleet_ambit_versions", return_value=(report, None)),
            mock.patch.object(ambit_deploy, "fleet_ambit_ota", return_value=(results, None)),
        ):
            code = ambit_deploy.main(["--tag", TAG, "--devices", DEVICE_A])
        self.assertEqual(code, 1)

    def test_post_verify_effect_rescues_dropped_terminal_reports(self) -> None:
        manifest, proof = verified_fixture()
        before = {
            DEVICE_A: {
                "state": "complete",
                "channels": channels("1.2.2", "1.2.2", "1.2.2", "1.2.2"),
            }
        }
        after = {
            DEVICE_A: {
                "state": "complete",
                "channels": channels("1.2.3", "1.2.3", "1.2.3", "1.2.3"),
            }
        }
        timeout_results = ambit_deploy.AmbitStatusTracker(
            [DEVICE_A], "campaign"
        ).results()
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "results.json"
            with (
                mock.patch.object(
                    ambit_deploy,
                    "fetch_release",
                    return_value=(manifest, b"", proof),
                ),
                mock.patch.object(
                    ambit_deploy.fleet, "boto_session", return_value=object()
                ),
                mock.patch.object(
                    ambit_deploy.fleet,
                    "fleet_ping",
                    return_value={DEVICE_A: "1.5.1"},
                ),
                mock.patch.object(
                    ambit_deploy,
                    "fleet_ambit_versions",
                    side_effect=[(before, None), (after, None)],
                ) as versions,
                mock.patch.object(
                    ambit_deploy,
                    "fleet_ambit_ota",
                    return_value=(timeout_results, "terminal reports dropped"),
                ),
            ):
                code = ambit_deploy.main(
                    [
                        "--tag",
                        TAG,
                        "--devices",
                        DEVICE_A,
                        "--results-json",
                        str(path),
                    ]
                )
            rendered = json.loads(path.read_text())
        self.assertEqual(code, 0)
        self.assertEqual(versions.call_count, 2)
        self.assertEqual(
            rendered["verification"][DEVICE_A]["outcome"], "confirmed_success"
        )
        self.assertEqual(rendered["tracking_error"], "terminal reports dropped")
        self.assertIsNone(rendered["error"])


class PreflightSkipRunnerTest(unittest.TestCase):
    def run_case(self, first, retry, *, devices=None, dry_run=False, retry_error=None):
        devices = devices or [DEVICE_A, DEVICE_B]
        manifest, proof = verified_fixture()
        after = {
            device: {"state": "complete", "channels": channels(*(["1.2.3"] * 4))}
            for device in devices
        }
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "results.json"
            summary_path = Path(temporary) / "summary.md"
            with (
                mock.patch.object(ambit_deploy, "fetch_release", return_value=(manifest, b"", proof)),
                mock.patch.object(ambit_deploy.fleet, "boto_session", return_value=object()),
                mock.patch.object(ambit_deploy.fleet, "fleet_ping", return_value=dict.fromkeys(devices, "2.6.1")),
                mock.patch.object(ambit_deploy, "fleet_ambit_versions", side_effect=[(first, None), (retry, retry_error), (after, None)]) as versions,
                mock.patch.object(ambit_deploy, "fleet_ambit_ota", return_value=({}, None)) as ota,
                mock.patch.dict("os.environ", {"GITHUB_STEP_SUMMARY": str(summary_path)}),
            ):
                argv = ["--tag", TAG, "--devices", ",".join(devices), "--absent-retry-delay-seconds", "0", "--results-json", str(path)]
                if dry_run:
                    argv.append("--dry-run")
                code = ambit_deploy.main(argv)
            rendered = json.loads(path.read_text())
            summary = summary_path.read_text()
        return code, rendered, summary, ota, versions

    def healthy(self, version="1.2.2"):
        return {"state": "complete", "channels": channels(*([version] * 4))}

    def test_mixed_skips_preserve_both_attempts_and_only_deploy_healthy(self):
        absent = {"state": "complete", "channels": channels()}
        busy = {"state": "busy", "channels": {}, "detail": "measurement busy"}
        incomplete = {"state": "incomplete", "channels": {}, "detail": "partial"}
        for first, second in (
            (absent, absent), (absent, None), (None, None),
            (busy, busy), (None, absent), (absent, busy),
            (incomplete, incomplete),
        ):
            for dry_run in (False, True):
                with self.subTest(first=first, second=second, dry_run=dry_run):
                    initial = {DEVICE_A: self.healthy()}
                    retry = {}
                    if first is not None:
                        initial[DEVICE_B] = first
                    if second is not None:
                        retry[DEVICE_B] = second
                    code, plan, summary, ota, versions = self.run_case(initial, retry, dry_run=dry_run)
                    self.assertEqual(code, 0)
                    self.assertIsNone(plan["error"])
                    self.assertEqual(plan["deployed_gateways"], [DEVICE_A])
                    self.assertEqual(set(plan["skipped_preflight"]), {DEVICE_B})
                    self.assertEqual(plan["gateway_decisions"][DEVICE_B]["action"], "skipped_preflight")
                    evidence = plan["skipped_preflight"][DEVICE_B]
                    self.assertEqual(evidence["inventory"], plan["preflight_versions"][DEVICE_B])
                    self.assertEqual(evidence["reason"], "no_ambit_present" if first == second == absent else "ambiguous_preflight")
                    for index, report in enumerate((first, second)):
                        attempt = evidence["attempts"][index]
                        self.assertEqual(attempt["attempt"], index + 1)
                        self.assertEqual(attempt["state"], report["state"] if report else "no_reply")
                        self.assertEqual(attempt["channels"], report["channels"] if report else {})
                        self.assertIsNone(attempt["error"])
                    self.assertEqual(plan["preflight_summary"]["reachable_gateways"], 1 + int(any(r and r["state"] in {"busy", "complete"} for r in (first, second))))
                    self.assertIn(f"preflight reachable {plan['preflight_summary']['reachable_gateways']}", summary)
                    general, skips = summary.split("### Skipped preflight gateways")
                    self.assertNotIn(DEVICE_B, general)
                    self.assertIn(DEVICE_B, skips)
                    self.assertIn(evidence["reason"], skips)
                    self.assertEqual(plan["live_failures"], [])
                    self.assertNotIn(DEVICE_B, plan["results"])
                    self.assertNotIn(DEVICE_B, plan["verification"])
                    self.assertEqual(versions.call_args_list[1].args[1], [DEVICE_B])
                    if dry_run:
                        ota.assert_not_called()
                        self.assertEqual(versions.call_count, 2)
                    else:
                        self.assertEqual(ota.call_args.args[1], [DEVICE_A])
                        self.assertEqual(versions.call_args_list[2].args[1], [DEVICE_A])
                        self.assertEqual(plan["verification"][DEVICE_A]["outcome"], "confirmed_success")

    def test_all_preflight_skipped_fails_dry_and_live(self):
        for report in (None, {"state": "complete", "channels": channels()}, {"state": "busy", "channels": {}}):
            for dry_run in (False, True):
                with self.subTest(report=report, dry_run=dry_run):
                    reports = dict.fromkeys((DEVICE_A, DEVICE_B), report) if report else {}
                    code, plan, summary, ota, versions = self.run_case(reports, reports, dry_run=dry_run)
                    self.assertEqual(code, 1)
                    ota.assert_not_called()
                    self.assertEqual(versions.call_count, 2)
                    self.assertEqual(set(plan["skipped_preflight"]), {DEVICE_A, DEVICE_B})
                    self.assertEqual(plan["live_failures"], [])
                    self.assertEqual(plan["results"], {})
                    self.assertIn("all selected gateways were skipped_preflight", plan["error"])
                    self.assertIn("a live run would fail closed", summary)

    def test_presence_in_either_inconclusive_attempt_blocks_whole_cohort(self):
        absent = {"state": "complete", "channels": channels()}
        for first, second in (
            ({"state": "complete", "channels": {**channels(), "0": {"present": True, "version": None}}}, absent),
            ({"state": "complete", "channels": channels("1.2.2")}, None),
            (absent, {"state": "complete", "channels": {**channels(), "0": {"present": True, "version": None}}}),
            ({"state": "busy", "channels": channels("1.2.2")}, absent),
            (absent, {"state": "busy", "channels": channels("1.2.2")}),
            ({"state": "incomplete", "channels": channels("1.2.2")}, absent),
            ({"state": "complete", "channels": channels("1.2.1")}, {"state": "complete", "channels": channels("1.2.2")}),
        ):
            for dry_run in (False, True):
                with self.subTest(first=first, second=second, dry_run=dry_run):
                    initial = {DEVICE_A: self.healthy(), DEVICE_B: first}
                    retry = {DEVICE_B: second} if second else {}
                    code, plan, summary, ota, _ = self.run_case(initial, retry, dry_run=dry_run)
                    self.assertEqual(code, 1)
                    ota.assert_not_called()
                    self.assertTrue(plan["gateway_decisions"][DEVICE_B]["blocking"])
                    self.assertEqual(plan["skipped_preflight"], {})
                    self.assertIn(DEVICE_B, plan["blocking_preflight"])
                    self.assertIn("ambiguous or unproven", plan["error"])
                    self.assertNotIn("### Skipped preflight gateways", summary)

    def test_non_null_unparseable_present_version_stays_blocking(self):
        report = {"state": "complete", "channels": channels("garbage")}
        decisions, deploy = ambit_deploy.decide_gateways([DEVICE_B], {DEVICE_B: report}, "1.2.3", False)
        self.assertEqual(deploy, [])
        self.assertEqual(decisions[DEVICE_B]["skip_reason"], "unproven_ambit_version")
        self.assertTrue(decisions[DEVICE_B]["blocking"])
        for dry_run in (False, True):
            code, plan, _, ota, _ = self.run_case({DEVICE_A: self.healthy(), DEVICE_B: report}, {DEVICE_B: report}, dry_run=dry_run)
            self.assertEqual(code, 1)
            ota.assert_not_called()
            self.assertEqual(plan["gateway_decisions"][DEVICE_B]["skip_reason"], "unproven_ambit_version")
            self.assertNotIn(DEVICE_B, plan["skipped_preflight"])

    def test_retry_transport_error_stays_fatal_and_visible(self):
        for dry_run in (False, True):
            for first in (None, {"state": "complete", "channels": channels()}):
                with self.subTest(dry_run=dry_run, first=first):
                    initial = {DEVICE_A: self.healthy()}
                    if first:
                        initial[DEVICE_B] = first
                    code, plan, summary, ota, _ = self.run_case(initial, {}, dry_run=dry_run, retry_error="RuntimeError: token=secret-value transport down")
                    self.assertEqual(code, 1)
                    ota.assert_not_called()
                    self.assertIn("transport down", plan["error"])
                    self.assertIn("transport down", summary)
                    self.assertIn("transport down", plan["skipped_preflight"][DEVICE_B]["attempts"][1]["error"])
                    self.assertNotIn("secret-value", json.dumps(plan) + summary)

    def test_policy_skip_beside_preflight_skip_is_successful_noop(self):
        for version, reason in (("1.2.3", "all_ambits_up_to_date"), ("1.2.4", "newer_ambit_present")):
            code, plan, summary, ota, _ = self.run_case({DEVICE_A: self.healthy(version)}, {})
            self.assertEqual(code, 0)
            self.assertIsNone(plan["error"])
            ota.assert_not_called()
            self.assertEqual(plan["deployed_gateways"], [])
            self.assertEqual(plan["gateway_decisions"][DEVICE_A]["skip_reason"], reason)
            self.assertEqual(set(plan["skipped_preflight"]), {DEVICE_B})
            self.assertIn(reason, summary)

    def test_three_gateway_mix_and_four_versions_only_on_retry(self):
        device_c = "AMBYTE_12:34:56:78:9A:BC"
        code, plan, _, ota, versions = self.run_case(
            {DEVICE_A: self.healthy(), device_c: self.healthy("1.2.3")}, {},
            devices=[DEVICE_A, DEVICE_B, device_c],
        )
        self.assertEqual(code, 0)
        self.assertEqual(ota.call_args.args[1], [DEVICE_A])
        self.assertEqual(versions.call_args_list[1].args[1], [DEVICE_B])
        self.assertEqual(set(plan["skipped_preflight"]), {DEVICE_B})
        self.assertEqual(plan["gateway_decisions"][device_c]["skip_reason"], "all_ambits_up_to_date")
        for dry_run in (False, True):
            code, plan, _, ota, _ = self.run_case(
                {}, {DEVICE_A: self.healthy()}, devices=[DEVICE_A], dry_run=dry_run,
            )
            self.assertEqual(code, 0)
            self.assertEqual(plan["skipped_preflight"], {})
            self.assertEqual(plan["preflight_summary"]["reachable_gateways"], 1)
            self.assertEqual(plan["deployed_gateways"], [DEVICE_A])
            if dry_run:
                ota.assert_not_called()
            else:
                self.assertEqual(ota.call_args.args[1], [DEVICE_A])

    def test_invalid_correlated_inventory_blocks_either_attempt(self):
        valid_channels = [{"ch": index, "present": index == 0, "version": "1.2.2" if index == 0 else None} for index in range(4)]
        bad_channels = (
            valid_channels[:3], valid_channels + [valid_channels[0]],
            [{**valid_channels[0], "present": "true"}, *valid_channels[1:]],
            [{**valid_channels[0], "version": 12}, *valid_channels[1:]],
            [{**valid_channels[0], "ch": True}, *valid_channels[1:]],
        )
        for raw_channels in bad_channels:
            collected = collect_version_replies([{"channels": raw_channels}])[DEVICE_A]
            self.assertEqual(collected["state"], "invalid_reply")
            for attempt in (1, 2):
                for dry_run in (False, True):
                    with self.subTest(raw=raw_channels, attempt=attempt, dry_run=dry_run):
                        first = {DEVICE_A: self.healthy(), DEVICE_B: collected} if attempt == 1 else {DEVICE_A: self.healthy()}
                        second = {} if attempt == 1 else {DEVICE_B: collected}
                        code, plan, _, ota, _ = self.run_case(first, second, dry_run=dry_run)
                        self.assertEqual(code, 1)
                        ota.assert_not_called()
                        self.assertEqual(plan["preflight_attempts"][attempt - 1]["reports"][DEVICE_B]["state"], "invalid_reply")
                        self.assertNotIn(DEVICE_B, plan["skipped_preflight"])
                        self.assertIn(DEVICE_B, plan["blocking_preflight"])

    def test_invalid_reply_evidence_is_bounded_redacted_and_cannot_be_washed_away(self):
        oversized = {"channels": [{"ch": 0, "present": True, "version": "token=sensitive-value " + "x" * 10000}], "detail": "password=secret-value"}
        valid = {"channels": [{"ch": index, "present": False} for index in range(4)]}
        for raw in ([oversized], [oversized, valid], [valid, oversized]):
            report = collect_version_replies(raw)[DEVICE_A]
            self.assertEqual(report["state"], "invalid_reply")
            code, plan, _, ota, _ = self.run_case({DEVICE_A: self.healthy(), DEVICE_B: report}, {DEVICE_B: self.healthy()})
            self.assertEqual(code, 1)
            ota.assert_not_called()
            evidence = plan["preflight_attempts"][0]["reports"][DEVICE_B]
            self.assertLessEqual(len(json.dumps(evidence).encode()), 2048)
            self.assertNotIn("sensitive-value", json.dumps(plan))
            self.assertNotIn("secret-value", json.dumps(plan))


class MqttOperationTest(unittest.TestCase):
    class Future:
        def result(self):
            return None

    def test_version_preflight_publishes_correlated_query(self) -> None:
        holder = {}

        class Connection:
            def __init__(self, callback):
                self.callback = callback
                self.publishes = []

            def publish(inner, **kwargs):
                inner.publishes.append(kwargs)
                command = json.loads(kwargs["payload"])
                inner.callback(
                    STATUS_A,
                    json.dumps(
                        {
                            "type": "ambit_versions",
                            "id": command["id"],
                            "channels": [
                                {"ch": 0, "present": True, "version": "1.2.2"},
                                {"ch": 1, "present": False},
                                {"ch": 2, "present": False},
                                {"ch": 3, "present": False},
                            ],
                        }
                    ).encode(),
                    False,
                    1,
                    False,
                )
                return MqttOperationTest.Future(), 1

            def disconnect(inner):
                return MqttOperationTest.Future()

        def factory(session, topic, callback, client_id):
            holder["connection"] = Connection(callback)
            return holder["connection"]

        fake_mqtt = types.SimpleNamespace(QoS=types.SimpleNamespace(AT_LEAST_ONCE=1))
        with mock.patch.dict(sys.modules, {"awscrt": types.SimpleNamespace(mqtt=fake_mqtt)}):
            reports, error = ambit_deploy.fleet_ambit_versions(
                object(), [DEVICE_A], "preflight-1", 1, connection_factory=factory
            )

        self.assertIsNone(error)
        self.assertEqual(reports[DEVICE_A]["channels"]["0"]["version"], "1.2.2")
        publish = holder["connection"].publishes[0]
        self.assertEqual(json.loads(publish["payload"]), {"type": "ambit_versions", "id": "preflight-1"})
        self.assertFalse(publish["retain"])

    def test_ota_publishes_one_all_channel_command_and_tracks_terminals(self) -> None:
        holder = {}

        class Connection:
            def __init__(self, callback):
                self.callback = callback
                self.publishes = []

            def publish(inner, **kwargs):
                inner.publishes.append(kwargs)
                command = json.loads(kwargs["payload"])
                for channel, state in ((255, "accepted"), (0, "success"), (1, "absent"), (2, "absent"), (3, "absent"), (255, "success")):
                    inner.callback(
                        STATUS_A,
                        json.dumps(
                            {
                                "type": "ambit_ota_status",
                                "id": command["id"],
                                "channel": channel,
                                "state": state,
                            }
                        ).encode(),
                        False,
                        1,
                        False,
                    )
                return MqttOperationTest.Future(), 1

            def disconnect(inner):
                return MqttOperationTest.Future()

        def factory(session, topic, callback, client_id):
            holder["connection"] = Connection(callback)
            return holder["connection"]

        fake_mqtt = types.SimpleNamespace(QoS=types.SimpleNamespace(AT_LEAST_ONCE=1))
        with mock.patch.dict(sys.modules, {"awscrt": types.SimpleNamespace(mqtt=fake_mqtt)}):
            results, error = ambit_deploy.fleet_ambit_ota(
                object(),
                [DEVICE_A],
                "campaign-unique-1",
                "https://github.com/example/app.bin",
                1,
                1,
                10,
                0,
                connection_factory=factory,
            )

        self.assertIsNone(error)
        self.assertEqual(results[DEVICE_A]["overall"]["state"], "success")
        self.assertEqual(results[DEVICE_A]["channels"]["0"]["state"], "success")
        command = json.loads(holder["connection"].publishes[0]["payload"])
        self.assertEqual(command["type"], "ambit_ota")
        self.assertEqual(command["id"], "campaign-unique-1")
        self.assertEqual(command["channel"], "all")
        self.assertFalse(holder["connection"].publishes[0]["retain"])

    def test_firmware_all_channel_dependency_reports_four_channels_including_absent(self) -> None:
        source = (ROOT / "components/ambit_ota/ambit_ota.c").read_text(
            encoding="utf-8"
        )
        start = source.index("/* Deferred per-channel results")
        end = source.index("if (ok) {", start)
        reporting_block = source[start:end]
        self.assertIn(
            "for (uint8_t c = 0; c < UART_SENSOR_NUM_CHANNELS; c++)",
            reporting_block,
        )
        self.assertIn('res[c] == OTA_ABSENT ? "absent"', reporting_block)
        self.assertIn("c, r->id", reporting_block)

    def test_firmware_versions_create_measurement_safe_idle_window(self) -> None:
        source = (ROOT / "components/ambit_ota/ambit_ota.c").read_text(
            encoding="utf-8"
        )
        helper = source.split("static void ambit_quiesce_for_idle", 1)[1].split(
            "static void ambit_do_ota", 1
        )[0]
        suspend = helper.index("s_cfg.workload_suspend();")
        settle = helper.index("vTaskDelay(pdMS_TO_TICKS(AMBIT_IDLE_SETTLE_MS));")
        invalidate = helper.index("s_cfg.ping_cache_invalidate()")
        self.assertLess(suspend, settle)
        self.assertLess(settle, invalidate)

        body = source.split("static void ambit_do_versions", 1)[1].split(
            "static void ambit_do_probe", 1
        )[0]
        quiesce = body.index("ambit_quiesce_for_idle();")
        sweep = body.index("for (uint8_t c = 0; c < UART_SENSOR_NUM_CHANNELS; c++)")
        publish = body.index("s_cfg.publish(s_cfg.status_topic")
        resume = body.index("s_cfg.workload_resume()")
        self.assertLess(quiesce, sweep)
        self.assertLess(sweep, publish)
        self.assertLess(publish, resume)
        self.assertIn("#define AMBIT_IDLE_SETTLE_MS   65000U", source)

        retry = source.split("static bool ambit_read_fw_version", 1)[1].split(
            "static void ambit_do_versions", 1
        )[0]
        self.assertIn(
            "attempt < AMBIT_VERSION_READ_ATTEMPTS", retry
        )
        read = retry.index("cmd_ambit_get_info(")
        delay = retry.index("vTaskDelay(pdMS_TO_TICKS(AMBIT_VERSION_RETRY_MS));")
        self.assertLess(read, delay)
        self.assertIn("#define AMBIT_VERSION_READ_ATTEMPTS 2U", source)
        self.assertIn("#define AMBIT_VERSION_RETRY_MS      250U", source)
        self.assertIn("ambit_read_fw_version(c, &fw)", body)
        self.assertNotIn("cmd_ambit_get_info(", body)

    def test_firmware_ota_waits_for_idle_and_invalidates_ping_cache(self) -> None:
        source = (ROOT / "components/ambit_ota/ambit_ota.c").read_text(
            encoding="utf-8"
        )
        body = source.split("static void ambit_do_ota", 1)[1].split(
            "static void ambit_do_versions", 1
        )[0]
        jitter = body.index("wait_for_fleet_slot()")
        quiesce = body.index("ambit_quiesce_for_idle();")
        comms = body.index("s_cfg.comms_suspend()")
        download = body.index("http_get_to_stage(")   # verified staging (ambit_stage.h)
        ping = body.index("cmd_uart_ping(c, &connected)")
        self.assertLess(jitter, quiesce)
        self.assertLess(quiesce, comms)
        self.assertLess(comms, download)
        self.assertLess(download, ping)

        uart_source = (ROOT / "components/uart_sensors/uart_sensors.c").read_text(
            encoding="utf-8"
        )
        invalidator = uart_source.split(
            "void uart_sensors_invalidate_ping_cache", 1
        )[1].split("/* ── Init", 1)[0]
        self.assertIn(
            "for (uint8_t channel = 0; channel < UART_SENSOR_NUM_CHANNELS; channel++)",
            invalidator,
        )
        self.assertIn("s_ch[channel].ping_ts = 0;", invalidator)


if __name__ == "__main__":
    unittest.main()
