import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from tools.fleet_deploy import ambit_candidate_deploy as candidate


DEVICE = "AMBYTE_AA:BB:CC:DD:EE:FF"
VERSION = "1.1.4"
URL = "https://downloads.example.org/ambit/candidate.bin"
IMAGE = bytes([0xE9]) + b"candidate-image"
DIGEST = hashlib.sha256(IMAGE).hexdigest()


def channels(version: str, *, mismatch: bool = False) -> dict[str, dict[str, object]]:
    return {
        "0": {"present": True, "version": "9.9.9" if mismatch else version},
        "1": {"present": False, "version": None},
        "2": {"present": False, "version": None},
        "3": {"present": False, "version": None},
    }


def terminal_success() -> dict[str, dict[str, object]]:
    return {
        DEVICE: {
            "accepted": True,
            "channels": {
                "0": {"state": "success", "detail": None},
                "1": {"state": "absent", "detail": None},
                "2": {"state": "absent", "detail": None},
                "3": {"state": "absent", "detail": None},
            },
            "overall": {"state": "success", "detail": None},
        }
    }


class CandidateValidationTest(unittest.TestCase):
    def test_requires_dev_exact_device_numeric_version_and_public_plain_https(self) -> None:
        with self.assertRaisesRegex(candidate.CandidateError, "environment"):
            candidate._require_environment("prod")
        with self.assertRaisesRegex(candidate.CandidateError, "git ref"):
            candidate._require_git_ref("refs/heads/main")
        for invalid in ("AA:BB:CC:DD:EE:FF", f"{DEVICE},{DEVICE}", "AMBYTE_bad"):
            with self.subTest(invalid=invalid), self.assertRaises(candidate.CandidateError):
                candidate.validate_device_id(invalid)
        for invalid in ("v1.1.4", "1.1", "1.1.4-rc1", "01.1.4"):
            with self.subTest(invalid=invalid), self.assertRaises(candidate.CandidateError):
                candidate.validate_candidate_version(invalid)
        for invalid in (
            "http://downloads.example.org/candidate.bin",
            "https://user:pass@downloads.example.org/candidate.bin",
            "https://downloads.example.org/candidate.bin?token=secret",
            "https://downloads.example.org/candidate.bin#sha",
            "https://downloads.example.org/candidate file.bin",
            "https://downloads.example.org\\@evil.example/candidate.bin",
            "https://localhost/candidate.bin",
            "https://127.0.0.1/candidate.bin",
            "https://downloads.example.org:8443/candidate.bin",
        ):
            with self.subTest(invalid=invalid), self.assertRaises(candidate.CandidateError):
                candidate.validate_candidate_url(invalid)
        self.assertEqual(candidate.validate_device_id(DEVICE), DEVICE)
        self.assertEqual(candidate.validate_candidate_version(VERSION), VERSION)
        self.assertEqual(candidate.validate_candidate_url(URL), URL)

    def test_exact_size_sha_and_esp_magic_are_required(self) -> None:
        candidate.verify_image(IMAGE, len(IMAGE), DIGEST)
        with self.assertRaisesRegex(candidate.CandidateError, "byte count"):
            candidate.verify_image(IMAGE, len(IMAGE) + 1, DIGEST)
        with self.assertRaisesRegex(candidate.CandidateError, "SHA-256"):
            candidate.verify_image(IMAGE, len(IMAGE), "0" * 64)
        bad_magic = b"\x00" + IMAGE[1:]
        with self.assertRaisesRegex(candidate.CandidateError, "ESP application"):
            candidate.verify_image(bad_magic, len(bad_magic), hashlib.sha256(bad_magic).hexdigest())
        for invalid in (DIGEST.upper(), "a" * 63, "g" * 64):
            with self.subTest(invalid=invalid), self.assertRaises(candidate.CandidateError):
                candidate.validate_sha256(invalid)

    def test_prepare_downloads_once_and_persists_pre_aws_proof(self) -> None:
        calls: list[tuple[str, int]] = []

        def download(url: str, size: int) -> tuple[bytes, str]:
            calls.append((url, size))
            return IMAGE, url

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            proof = candidate.prepare_candidate(
                environment="dev",
                git_ref=candidate.REQUIRED_GIT_REF,
                device_id=DEVICE,
                candidate_version=VERSION,
                image_url=URL,
                image_size=len(IMAGE),
                image_sha256=DIGEST,
                dry_run=True,
                prepared_dir=root / "prepared",
                proof_json=root / "proof.json",
                results_json=root / "results.json",
                downloader=download,
            )
            results = json.loads((root / "results.json").read_text())
            persisted = json.loads((root / "proof.json").read_text())
            self.assertEqual((root / "prepared" / candidate.IMAGE_NAME).read_bytes(), IMAGE)
        self.assertEqual(calls, [(URL, len(IMAGE))])
        self.assertEqual(persisted, proof)
        self.assertTrue(proof["application"]["verified_before_aws"])
        self.assertEqual(proof["application"]["download_count"], 1)
        self.assertEqual(proof["git_ref"], candidate.REQUIRED_GIT_REF)
        self.assertEqual(proof["targeting"]["explicit_device_count"], 1)
        self.assertEqual(proof["targeting"]["percentage"], 100)
        self.assertEqual(results["phase"], "prepared_before_aws")
        self.assertEqual(results["supply_chain_proof"], proof)

    def test_prepared_bytes_are_reverified_without_network(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            candidate.prepare_candidate(
                environment="dev",
                git_ref=candidate.REQUIRED_GIT_REF,
                device_id=DEVICE,
                candidate_version=VERSION,
                image_url=URL,
                image_size=len(IMAGE),
                image_sha256=DIGEST,
                dry_run=True,
                prepared_dir=root / "prepared",
                proof_json=root / "proof.json",
                results_json=None,
                downloader=lambda url, size: (IMAGE, url),
            )
            (root / "prepared" / candidate.IMAGE_NAME).write_bytes(IMAGE + b"tampered")
            with self.assertRaisesRegex(candidate.CandidateError, "byte count"):
                candidate._load_prepared(
                    environment="dev",
                    git_ref=candidate.REQUIRED_GIT_REF,
                    prepared_dir=root / "prepared",
                    proof_json=root / "proof.json",
                )

    def test_prepared_targeting_invariants_cannot_be_widened(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            candidate.prepare_candidate(
                environment="dev",
                git_ref=candidate.REQUIRED_GIT_REF,
                device_id=DEVICE,
                candidate_version=VERSION,
                image_url=URL,
                image_size=len(IMAGE),
                image_sha256=DIGEST,
                dry_run=True,
                prepared_dir=root / "prepared",
                proof_json=root / "proof.json",
                results_json=None,
                downloader=lambda url, size: (IMAGE, url),
            )
            proof = json.loads((root / "proof.json").read_text())
            proof["targeting"]["percentage"] = 50
            (root / "proof.json").write_text(json.dumps(proof))
            with self.assertRaisesRegex(candidate.CandidateError, "targeting"):
                candidate._load_prepared(
                    environment="dev",
                    git_ref=candidate.REQUIRED_GIT_REF,
                    prepared_dir=root / "prepared",
                    proof_json=root / "proof.json",
                )

    def test_cli_exposes_no_broad_targeting_or_release_escape_hatches(self) -> None:
        parser = candidate.build_parser()
        subparsers = next(
            action
            for action in parser._actions
            if isinstance(action, candidate.argparse._SubParsersAction)
        )
        options = {
            option
            for subparser in subparsers.choices.values()
            for action in subparser._actions
            for option in action.option_strings
        }
        self.assertTrue(
            {
                "--percentage",
                "--devices",
                "--window-minutes",
                "--allow-downgrade",
                "--force-reflash",
                "--tag",
                "--repo",
            }.isdisjoint(options)
        )


class CandidateDeployTest(unittest.TestCase):
    def prepare(self, root: Path, dry_run: bool) -> None:
        candidate.prepare_candidate(
            environment="dev",
            git_ref=candidate.REQUIRED_GIT_REF,
            device_id=DEVICE,
            candidate_version=VERSION,
            image_url=URL,
            image_size=len(IMAGE),
            image_sha256=DIGEST,
            dry_run=dry_run,
            prepared_dir=root / "prepared",
            proof_json=root / "proof.json",
            results_json=root / "results.json",
            downloader=lambda url, size: (IMAGE, url),
        )

    def run_deploy(self, root: Path) -> int:
        return candidate.deploy_candidate(
            environment="dev",
            git_ref=candidate.REQUIRED_GIT_REF,
            prepared_dir=root / "prepared",
            proof_json=root / "proof.json",
            results_json=root / "results.json",
            profile=None,
            region="eu-central-1",
            ping_wait=1,
            preflight_seconds=1,
            verify_seconds=1,
            ack_seconds=1,
            final_seconds=1,
        )

    def test_dry_run_uses_correlated_preflight_but_never_sends_ambit_ota(self) -> None:
        preflight = {DEVICE: {"state": "complete", "channels": channels("1.1.3")}}
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.prepare(root, dry_run=True)
            with (
                mock.patch.object(candidate.fleet, "boto_session", return_value=object()),
                mock.patch.object(
                    candidate.fleet,
                    "fleet_ping",
                    return_value={DEVICE: "1.5.1"},
                ) as ping,
                mock.patch.object(
                    candidate.ambit_deploy,
                    "fleet_ambit_versions",
                    return_value=(preflight, None),
                ) as versions,
                mock.patch.object(candidate.ambit_deploy, "fleet_ambit_ota") as ota,
            ):
                code = self.run_deploy(root)
            results = json.loads((root / "results.json").read_text())
        self.assertEqual(code, 0)
        ping.assert_called_once_with(mock.ANY, [DEVICE], 1)
        versions.assert_called_once()
        ota.assert_not_called()
        self.assertEqual(results["phase"], "dry_run_complete")
        self.assertEqual(results["command"]["channel"], "all")
        self.assertEqual(results["deployed_gateways"], [DEVICE])

    def test_dry_run_fails_when_no_ambit_is_proven_present(self) -> None:
        absent = {
            DEVICE: {
                "state": "complete",
                "channels": channels(VERSION),
            }
        }
        for entry in absent[DEVICE]["channels"].values():
            entry["present"] = False
            entry["version"] = None
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.prepare(root, dry_run=True)
            with (
                mock.patch.object(candidate.fleet, "boto_session", return_value=object()),
                mock.patch.object(
                    candidate.fleet,
                    "fleet_ping",
                    return_value={DEVICE: "1.5.1"},
                ),
                mock.patch.object(
                    candidate.ambit_deploy,
                    "fleet_ambit_versions",
                    return_value=(absent, None),
                ),
                mock.patch.object(candidate.ambit_deploy, "fleet_ambit_ota") as ota,
            ):
                code = self.run_deploy(root)
            results = json.loads((root / "results.json").read_text())
        self.assertEqual(code, 1)
        ota.assert_not_called()
        self.assertEqual(results["phase"], "preflight_failed")
        self.assertIn("no proven-present eligible AMBIT", results["error"])

    def test_dry_run_all_present_channels_already_target_is_success(self) -> None:
        current = {DEVICE: {"state": "complete", "channels": channels(VERSION)}}
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.prepare(root, dry_run=True)
            with (
                mock.patch.object(candidate.fleet, "boto_session", return_value=object()),
                mock.patch.object(
                    candidate.fleet,
                    "fleet_ping",
                    return_value={DEVICE: "1.5.1"},
                ),
                mock.patch.object(
                    candidate.ambit_deploy,
                    "fleet_ambit_versions",
                    return_value=(current, None),
                ),
                mock.patch.object(candidate.ambit_deploy, "fleet_ambit_ota") as ota,
            ):
                code = self.run_deploy(root)
            results = json.loads((root / "results.json").read_text())
        self.assertEqual(code, 0)
        ota.assert_not_called()
        self.assertEqual(results["phase"], "dry_run_complete")
        self.assertEqual(
            results["gateway_decisions"][DEVICE]["skip_reason"],
            "all_ambits_up_to_date",
        )

    def test_live_sends_one_all_channel_command_and_verifies_target(self) -> None:
        preflight = {DEVICE: {"state": "complete", "channels": channels("1.1.3")}}
        post = {DEVICE: {"state": "complete", "channels": channels(VERSION)}}
        terminal = terminal_success()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.prepare(root, dry_run=False)
            with (
                mock.patch.object(candidate.fleet, "boto_session", return_value=object()),
                mock.patch.object(candidate.fleet, "fleet_ping", return_value={DEVICE: "1.5.1"}),
                mock.patch.object(
                    candidate.ambit_deploy,
                    "fleet_ambit_versions",
                    side_effect=[(preflight, None), (post, None)],
                ) as versions,
                mock.patch.object(
                    candidate.ambit_deploy,
                    "fleet_ambit_ota",
                    return_value=(terminal, None),
                ) as ota,
            ):
                code = self.run_deploy(root)
            results = json.loads((root / "results.json").read_text())
        self.assertEqual(code, 0)
        self.assertEqual(versions.call_count, 2)
        ota.assert_called_once_with(mock.ANY, [DEVICE], mock.ANY, URL, 1, 1, 1, 0)
        self.assertEqual(results["phase"], "verified")
        self.assertEqual(results["verification"][DEVICE]["outcome"], "confirmed_success")
        self.assertIsNone(results["error"])

    def test_live_fails_when_preflight_present_channel_misses_target(self) -> None:
        preflight = {DEVICE: {"state": "complete", "channels": channels("1.1.3")}}
        post = {DEVICE: {"state": "complete", "channels": channels(VERSION, mismatch=True)}}
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.prepare(root, dry_run=False)
            with (
                mock.patch.object(candidate.fleet, "boto_session", return_value=object()),
                mock.patch.object(candidate.fleet, "fleet_ping", return_value={DEVICE: "1.5.1"}),
                mock.patch.object(
                    candidate.ambit_deploy,
                    "fleet_ambit_versions",
                    side_effect=[(preflight, None), (post, None)],
                ),
                mock.patch.object(
                    candidate.ambit_deploy,
                    "fleet_ambit_ota",
                    return_value=(terminal_success(), None),
                ),
            ):
                code = self.run_deploy(root)
            results = json.loads((root / "results.json").read_text())
        self.assertEqual(code, 1)
        self.assertEqual(results["phase"], "verification_failed")
        self.assertIn("did not verify", results["error"])


if __name__ == "__main__":
    unittest.main()
