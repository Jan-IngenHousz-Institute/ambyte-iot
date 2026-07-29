from __future__ import annotations

import copy
import hashlib
import io
import json
import shutil
import tempfile
import unittest
import zipfile
import subprocess
from pathlib import Path
from typing import Any
from unittest import mock

from tools.firmware_candidate.candidate import (
    APP_DESC_MAGIC,
    APP_DESC_OFFSET,
    APP_DESC_VERSION_OFFSET,
    ESPTOOL_PACKAGE_VERSION,
    ESPTOOL_VERSION,
    package_candidate,
)
from tools.main_publisher.publisher import (
    FirmwarePublisher,
    GitHubPublisherClient,
    MergeIdentity,
    PublishRequest,
    PublisherError,
    PublisherRepository,
    analyze_pull_request,
)


ROOT = Path(__file__).resolve().parents[3]
REPOSITORY = "Jan-IngenHousz-Institute/ambyte-iot"
BASE_SHA = "b98ef489614e9c6d98af70020f1adaaf36b1630b"
TARGET_SHA = "5" * 40
HEAD_SHA = "3" * 40
TREE_SHA = "4" * 40
WORKFLOW_SHA = "6" * 40
DEFAULT_TITLE = "feat: ship exact firmware"
RUN_ID = 7001
ARTIFACT_ID = 8001
TOOLS = {
    "platformio_core": "6.1.19",
    "platformio_platform": "espressif32@6.12.0",
    "esp_idf": "5.5.0",
    "esp_idf_package": "3.50500.0",
    "esptool": ESPTOOL_VERSION,
    "esptool_package": ESPTOOL_PACKAGE_VERSION,
}


def fake_firmware(version: str) -> bytes:
    value = bytearray(512)
    value[APP_DESC_OFFSET : APP_DESC_OFFSET + 4] = APP_DESC_MAGIC.to_bytes(4, "little")
    encoded = version.encode("ascii")
    value[APP_DESC_VERSION_OFFSET : APP_DESC_VERSION_OFFSET + len(encoded)] = encoded
    value[APP_DESC_VERSION_OFFSET + len(encoded)] = 0
    return bytes(value)


def make_build(root: Path, version: str) -> Path:
    build = root / "build"
    (build / "bootloader").mkdir(parents=True)
    (build / "partition_table").mkdir()
    firmware = fake_firmware(version)
    (build / "firmware.bin").write_bytes(firmware)
    (build / "ambyte-iot.bin").write_bytes(firmware)
    (build / "bootloader" / "bootloader.bin").write_bytes(b"bootloader")
    (build / "partition_table" / "partition-table.bin").write_bytes(b"partition-table")
    (build / "ota_data_initial.bin").write_bytes(b"ota-initializer")
    flasher = {
        "write_flash_args": [
            "--flash_mode",
            "dio",
            "--flash_size",
            "16MB",
            "--flash_freq",
            "80m",
        ],
        "flash_settings": {
            "flash_mode": "dio",
            "flash_size": "16MB",
            "flash_freq": "80m",
        },
        "flash_files": {
            "0x0": "bootloader/bootloader.bin",
            "0x20000": "ambyte-iot.bin",
            "0x8000": "partition_table/partition-table.bin",
            "0xf000": "ota_data_initial.bin",
        },
        "bootloader": {
            "offset": "0x0",
            "file": "bootloader/bootloader.bin",
            "encrypted": "false",
        },
        "app": {
            "offset": "0x20000",
            "file": "ambyte-iot.bin",
            "encrypted": "false",
        },
        "partition-table": {
            "offset": "0x8000",
            "file": "partition_table/partition-table.bin",
            "encrypted": "false",
        },
        "otadata": {
            "offset": "0xf000",
            "file": "ota_data_initial.bin",
            "encrypted": "false",
        },
        "extra_esptool_args": {
            "after": "hard_reset",
            "before": "default_reset",
            "stub": True,
            "chip": "esp32s3",
        },
    }
    (build / "flasher_args.json").write_text(json.dumps(flasher))
    return build


def artifact_zip(candidate: Path) -> bytes:
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w", compression=zipfile.ZIP_STORED) as archive:
        for path in sorted(candidate.iterdir()):
            archive.writestr(path.name, path.read_bytes())
    return stream.getvalue()


def settled_base() -> dict[str, Any]:
    return json.loads(
        (ROOT / "tools/release-analysis/test/fixtures/settled-bootstrap.json").read_text()
    )


def unsettled_base() -> dict[str, Any]:
    return json.loads(
        (ROOT / "tools/release-analysis/test/fixtures/unsettled-release.json").read_text()
    )


class FakeRepository:
    def __init__(
        self,
        merge: MergeIdentity,
        base_state: dict[str, Any],
        *,
        workflow_changes: list[str] | None = None,
        recovery_failed_sha: str | None = None,
    ):
        self.merge = merge
        self.base_state = base_state
        self.workflow_changes = workflow_changes or []
        self.recovery_failed_sha = recovery_failed_sha

    def publish_target(self, target_sha: str, main_ref: str) -> MergeIdentity:
        if target_sha != self.merge.sha or main_ref != "refs/remotes/origin/main":
            raise PublisherError("fake target mismatch")
        return self.merge

    def workflow_changed_paths(self, base_sha: str, head_sha: str) -> list[str]:
        if self.recovery_failed_sha and head_sha == self.recovery_failed_sha:
            return []
        return list(self.workflow_changes)

    def base_state_input(self, base_sha: str, github: Any) -> dict[str, Any]:
        if base_sha != self.merge.parent_sha:
            raise PublisherError("fake predecessor mismatch")
        return copy.deepcopy(self.base_state)

    def expected_revert_tree(self, base_sha: str, failed_sha: str) -> str:
        if base_sha != self.merge.parent_sha or failed_sha != self.recovery_failed_sha:
            raise PublisherError("fake recovery proof mismatch")
        return self.merge.tree_sha

    def git(self, *args: str) -> str:
        if args[:4] == ("rev-list", "--parents", "-n", "1") and args[4] == self.recovery_failed_sha:
            return f"{self.recovery_failed_sha} {BASE_SHA}\n"
        raise PublisherError(f"unexpected fake git call: {args}")


def git(command: list[str], cwd: Path) -> str:
    result = subprocess.run(
        ["git", *command],
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)
    return result.stdout.strip()


def git_commit(repo: Path, subject: str, value: str) -> str:
    (repo / "state.txt").write_text(value)
    git(["add", "--all"], repo)
    git(["commit", "-m", subject], repo)
    return git(["rev-parse", "HEAD"], repo)


class FakeGitHub:
    def __init__(self, pull_request: dict[str, Any]):
        self.pull_request = pull_request
        self.runs: list[dict[str, Any]] = []
        self.artifacts: dict[int, list[dict[str, Any]]] = {}
        self.artifact_bytes: dict[int, bytes] = {}
        self.releases: list[dict[str, Any]] = []
        self.assets: dict[int, list[dict[str, Any]]] = {}
        self.asset_bytes: dict[int, bytes] = {}
        self.tags: dict[str, str] = {}
        self.immutable = (200, {"enabled": True, "enforced_by_owner": False})
        self.rulesets: list[dict[str, Any]] = []
        self.mutations: list[str] = []
        self.next_release_id = 9001
        self.next_asset_id = 10001
        self.pull_merge_refs: dict[int, str] = {
            pull_request["number"]: WORKFLOW_SHA
        }

    def exact_merged_pull_request(self, sha: str) -> dict[str, Any]:
        if sha != TARGET_SHA and sha != "c" * 40:
            raise PublisherError("fake associated PR target mismatch")
        return copy.deepcopy(self.pull_request)

    def workflow_runs(self, head_sha: str) -> list[dict[str, Any]]:
        return copy.deepcopy(self.runs)

    def pull_request_merge_ref_sha(self, pr_number: int) -> str:
        value = self.pull_merge_refs.get(pr_number)
        if value is None:
            raise PublisherError(f"GitHub Git ref API did not resolve refs/pull/{pr_number}/merge")
        return value

    def run_artifacts(self, run_id: int) -> list[dict[str, Any]]:
        return copy.deepcopy(self.artifacts.get(run_id, []))

    def download_artifact(self, artifact_id: int) -> bytes:
        return self.artifact_bytes[artifact_id]

    def immutable_release_capability(self) -> tuple[int, Any]:
        return copy.deepcopy(self.immutable)

    def releases_for_tag(self, tag: str) -> list[dict[str, Any]]:
        return [copy.deepcopy(item) for item in self.releases if item["tag_name"] == tag]

    def tag_target(self, tag: str) -> str | None:
        return self.tags.get(tag)

    def create_draft(self, *, tag: str, target_sha: str, body: str) -> dict[str, Any]:
        release = {
            "id": self.next_release_id,
            "tag_name": tag,
            "target_commitish": target_sha,
            "name": tag,
            "body": body,
            "draft": True,
            "prerelease": False,
            "immutable": False,
            "upload_url": "https://uploads.github.com/fake{?name,label}",
        }
        self.next_release_id += 1
        self.releases.append(release)
        self.assets[release["id"]] = []
        self.mutations.append("create-draft")
        return copy.deepcopy(release)

    def release_assets(self, release_id: int) -> list[dict[str, Any]]:
        return copy.deepcopy(self.assets.get(release_id, []))

    def upload_release_asset(
        self, release: dict[str, Any], name: str, content_type: str, value: bytes
    ) -> dict[str, Any]:
        if any(item["name"] == name for item in self.assets[release["id"]]):
            raise PublisherError("fake no-clobber collision")
        asset = {
            "id": self.next_asset_id,
            "name": name,
            "state": "uploaded",
            "content_type": content_type,
            "size": len(value),
            "digest": f"sha256:{hashlib.sha256(value).hexdigest()}",
        }
        self.next_asset_id += 1
        self.assets[release["id"]].append(asset)
        self.asset_bytes[asset["id"]] = value
        self.mutations.append(f"upload:{name}")
        return copy.deepcopy(asset)

    def download_release_asset(self, asset_id: int) -> bytes:
        return self.asset_bytes[asset_id]

    def publish_release(self, release_id: int) -> dict[str, Any]:
        release = next(item for item in self.releases if item["id"] == release_id)
        release["draft"] = False
        release["immutable"] = True
        self.tags[release["tag_name"]] = release["target_commitish"]
        self.mutations.append("publish")
        return copy.deepcopy(release)

    def get_release(self, release_id: int) -> dict[str, Any]:
        return copy.deepcopy(next(item for item in self.releases if item["id"] == release_id))

    def active_tag_rulesets(self) -> list[dict[str, Any]]:
        return copy.deepcopy(self.rulesets)

    def create_tag(self, tag: str, target_sha: str) -> None:
        if tag in self.tags:
            raise PublisherError("fake tag collision")
        self.tags[tag] = target_sha
        self.mutations.append(f"tag:{tag}")

    def seed_draft(self, body: str, target_sha: str, candidate: Path, names: list[str]) -> None:
        release = self.create_draft(tag="v1.1.0", target_sha=target_sha, body=body)
        self.mutations.clear()
        for name in names:
            self.upload_release_asset(
                release,
                name,
                "application/octet-stream",
                (candidate / name).read_bytes(),
            )
        self.mutations.clear()


def pull_request(
    *,
    number: int = 42,
    title: str = DEFAULT_TITLE,
    head_sha: str = HEAD_SHA,
) -> dict[str, Any]:
    return {
        "number": number,
        "title": title,
        "url": f"https://github.com/{REPOSITORY}/pull/{number}",
        "head_sha": head_sha,
    }


def build_candidate(
    root: Path,
    pr: dict[str, Any],
    merge: MergeIdentity,
    *,
    run_id: int = RUN_ID,
    run_attempt: int = 1,
    recovery_of_sha: str | None = None,
    workflow_sha: str = WORKFLOW_SHA,
) -> Path:
    analysis = analyze_pull_request(ROOT, pr)
    if recovery_of_sha:
        decision = {
            "release": False,
            "bump": None,
            "kind": "recovery",
            "reason": "verified-recovery-revert",
        }
        version = f"pr-{pr['number']}-{pr['head_sha'][:12]}"
        release_version = None
    elif analysis["decision"]["release"]:
        decision = analysis["decision"]
        version = "1.1.0"
        release_version = "1.1.0"
    else:
        decision = analysis["decision"]
        version = f"pr-{pr['number']}-{pr['head_sha'][:12]}"
        release_version = None
    notes = root / "notes.md"
    notes.write_text(analysis["notes"]["markdown"])
    metadata = {
        "schema_version": 1,
        "repository": REPOSITORY,
        "pull_request": {"number": pr["number"], "title": pr["title"]},
        "source": {
            "head_sha": pr["head_sha"],
            "base_sha": merge.parent_sha,
            "tree_sha": merge.tree_sha,
        },
        "analysis": {
            "decision": decision,
            "release_version": release_version,
            "notes_sha256": analysis["notes"]["sha256"],
            "latest_release_tag": "v1.0.5",
            "recovery_of_sha": recovery_of_sha,
        },
        "workflow": {"sha": workflow_sha, "run_id": run_id, "run_attempt": run_attempt},
    }
    candidate = root / "candidate"
    package_candidate(
        build_dir=make_build(root, version),
        metadata=metadata,
        release_notes=notes,
        output_dir=candidate,
        firmware_version=version,
        tool_versions=TOOLS,
    )
    return candidate


def attach_candidate(
    github: FakeGitHub,
    candidate: Path,
    pr: dict[str, Any],
    *,
    run_id: int = RUN_ID,
    artifact_id: int = ARTIFACT_ID,
    expired: bool = False,
) -> None:
    archive = artifact_zip(candidate)
    github.runs.append(
        {
            "id": run_id,
            "run_attempt": 1,
            "path": f".github/workflows/firmware-pr-gate.yml@refs/pull/{pr['number']}/merge",
            "display_title": pr["title"],
            "event": "pull_request",
            "status": "completed",
            "conclusion": "success",
            "head_sha": pr["head_sha"],
            "pull_requests": [{"number": pr["number"]}],
        }
    )
    github.artifacts[run_id] = [
        {
            "id": artifact_id,
            "name": f"firmware-candidate-pr-{pr['number']}-{pr['head_sha'][:12]}",
            "expired": expired,
            "digest": f"sha256:{hashlib.sha256(archive).hexdigest()}",
            "workflow_run": {
                "id": run_id,
                "head_sha": pr["head_sha"],
            },
        }
    ]
    github.artifact_bytes[artifact_id] = archive


class PublisherHarnessTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.merge = MergeIdentity(
            TARGET_SHA,
            BASE_SHA,
            TREE_SHA,
            f"{DEFAULT_TITLE} (#42)",
        )
        self.pr = pull_request()
        self.candidate = build_candidate(self.root, self.pr, self.merge)
        self.github = FakeGitHub(self.pr)
        attach_candidate(self.github, self.candidate, self.pr)
        self.repo = FakeRepository(self.merge, settled_base())
        self.publisher = FirmwarePublisher(
            repository=REPOSITORY,
            repo_root=ROOT,
            runner_temp=self.root / "runner",
            repo=self.repo,
            github=self.github,
        )

    def tearDown(self) -> None:
        self.temp.cleanup()

    def push(self) -> PublishRequest:
        return PublishRequest("push", "refs/heads/main", TARGET_SHA, TARGET_SHA)

    def test_fresh_publish_preserves_all_candidate_assets_and_is_idempotent(self) -> None:
        first = self.publisher.run(self.push())
        self.assertEqual(first["outcome"], "published")
        self.assertEqual(self.github.tags["v1.1.0"], TARGET_SHA)
        release = self.github.releases[0]
        self.assertTrue(release["immutable"])
        self.assertEqual(
            {item["name"] for item in self.github.assets[release["id"]]},
            {path.name for path in self.candidate.iterdir()},
        )

        mutations = list(self.github.mutations)
        second = self.publisher.run(self.push())
        self.assertEqual(second["outcome"], "already-published")
        self.assertEqual(self.github.mutations, mutations)

    def test_post_merge_pr_title_edit_cannot_change_release_identity(self) -> None:
        self.github.pull_request["title"] = "fix: mutable title edited after merge"
        result = self.publisher.run(self.push())
        self.assertEqual(result["outcome"], "published")
        self.assertEqual(self.github.tags, {"v1.1.0": TARGET_SHA})
        self.assertIn("**feat:** ship exact firmware", self.github.releases[0]["body"])

    def test_squash_subject_must_use_exact_pr_title_form(self) -> None:
        for subject in (
            DEFAULT_TITLE,
            f"{DEFAULT_TITLE} (#41)",
            f"{DEFAULT_TITLE} (#042)",
        ):
            with self.subTest(subject=subject):
                merge = MergeIdentity(TARGET_SHA, BASE_SHA, TREE_SHA, subject)
                publisher = FirmwarePublisher(
                    repository=REPOSITORY,
                    repo_root=ROOT,
                    runner_temp=self.root / "runner-bad-squash-form",
                    repo=FakeRepository(merge, settled_base()),
                    github=copy.deepcopy(self.github),
                )
                with self.assertRaisesRegex(PublisherError, "PR_TITLE"):
                    publisher.run(self.push())
                self.assertEqual(publisher.github.mutations, [])

    def test_squash_title_override_cannot_replace_candidate_authority(self) -> None:
        override = "fix: override the validated squash title"
        merge = MergeIdentity(TARGET_SHA, BASE_SHA, TREE_SHA, f"{override} (#42)")
        github = copy.deepcopy(self.github)
        github.runs[0]["display_title"] = override
        publisher = FirmwarePublisher(
            repository=REPOSITORY,
            repo_root=ROOT,
            runner_temp=self.root / "runner-squash-override",
            repo=FakeRepository(merge, settled_base()),
            github=github,
        )
        with self.assertRaisesRegex(PublisherError, "candidate identity mismatch"):
            publisher.run(self.push())
        self.assertEqual(github.mutations, [])

    def test_selected_run_display_title_must_equal_frozen_title(self) -> None:
        self.github.runs[0]["display_title"] = "fix: stale pre-merge run title"
        with self.assertRaisesRegex(PublisherError, "display_title"):
            self.publisher.run(self.push())
        self.assertEqual(self.github.mutations, [])

    def test_wrong_or_missing_pull_merge_ref_fails_closed(self) -> None:
        for value in ("7" * 40, None):
            with self.subTest(value=value):
                github = copy.deepcopy(self.github)
                if value is None:
                    github.pull_merge_refs.clear()
                else:
                    github.pull_merge_refs[42] = value
                publisher = FirmwarePublisher(
                    repository=REPOSITORY,
                    repo_root=ROOT,
                    runner_temp=self.root / "runner-bad-merge-ref",
                    repo=self.repo,
                    github=github,
                )
                with self.assertRaisesRegex(PublisherError, r"workflow.sha|refs/pull/42/merge"):
                    publisher.run(self.push())
                self.assertEqual(github.mutations, [])

    def test_wrong_manifest_workflow_sha_fails_closed(self) -> None:
        root = self.root / "wrong-manifest-workflow-sha"
        root.mkdir()
        candidate = build_candidate(
            root,
            self.pr,
            self.merge,
            workflow_sha="7" * 40,
        )
        github = FakeGitHub(self.pr)
        attach_candidate(github, candidate, self.pr)
        publisher = FirmwarePublisher(
            repository=REPOSITORY,
            repo_root=ROOT,
            runner_temp=root / "runner",
            repo=self.repo,
            github=github,
        )
        with self.assertRaisesRegex(PublisherError, "workflow.sha"):
            publisher.run(self.push())
        self.assertEqual(github.mutations, [])

    def test_partial_matching_draft_resumes_without_clobber(self) -> None:
        notes = (self.candidate / "release-notes.md").read_text()
        self.github.seed_draft(notes, TARGET_SHA, self.candidate, ["firmware.bin"])
        result = self.publisher.run(self.push())
        self.assertEqual(result["outcome"], "published")
        self.assertNotIn("upload:firmware.bin", self.github.mutations)
        self.assertEqual(self.github.mutations[-1], "publish")

    def test_conflicting_release_tag_body_or_asset_fails_without_mutation(self) -> None:
        notes = (self.candidate / "release-notes.md").read_text()
        cases = ("body", "tag", "asset", "asset-bytes")
        for case in cases:
            with self.subTest(case=case):
                github = copy.deepcopy(self.github)
                github.seed_draft(notes, TARGET_SHA, self.candidate, ["firmware.bin"])
                release = github.releases[0]
                if case == "body":
                    release["body"] = "conflicting\n"
                elif case == "tag":
                    github.tags["v1.1.0"] = "9" * 40
                else:
                    asset = github.assets[release["id"]][0]
                    if case == "asset":
                        asset["digest"] = f"sha256:{'9' * 64}"
                    else:
                        github.asset_bytes[asset["id"]] = b"tampered remote bytes"
                publisher = FirmwarePublisher(
                    repository=REPOSITORY,
                    repo_root=ROOT,
                    runner_temp=self.root / f"runner-{case}",
                    repo=self.repo,
                    github=github,
                )
                with self.assertRaises(PublisherError):
                    publisher.run(self.push())
                self.assertEqual(github.mutations, [])

    def test_manual_retry_requires_exact_run_and_artifact_ids(self) -> None:
        request = PublishRequest(
            "workflow_dispatch",
            "refs/heads/main",
            TARGET_SHA,
            pr_workflow_run_id=RUN_ID,
            artifact_id=ARTIFACT_ID,
        )
        self.assertEqual(self.publisher.run(request)["outcome"], "published")

        github = FakeGitHub(self.pr)
        attach_candidate(github, self.candidate, self.pr)
        publisher = FirmwarePublisher(
            repository=REPOSITORY,
            repo_root=ROOT,
            runner_temp=self.root / "runner-manual-bad",
            repo=self.repo,
            github=github,
        )
        with self.assertRaisesRegex(PublisherError, "artifact_id"):
            publisher.run(
                PublishRequest(
                    "workflow_dispatch",
                    "refs/heads/main",
                    TARGET_SHA,
                    pr_workflow_run_id=RUN_ID,
                    artifact_id=9999,
                )
            )

    def test_fork_run_with_empty_api_pull_array_remains_manifest_bound(self) -> None:
        self.github.runs[0]["pull_requests"] = []
        self.assertEqual(self.publisher.run(self.push())["outcome"], "published")

    def test_expired_artifact_never_rebuilds_or_mutates(self) -> None:
        self.github.artifacts[RUN_ID][0]["expired"] = True
        with self.assertRaisesRegex(PublisherError, r"never rebuild on main"):
            self.publisher.run(self.push())
        self.assertEqual(self.github.mutations, [])

    def test_wrong_candidate_identity_version_notes_or_run_never_mutates(self) -> None:
        mutations = {
            "pr": lambda manifest: manifest["pull_request"].__setitem__("number", 99),
            "head": lambda manifest: manifest["source"].__setitem__("head_sha", "8" * 40),
            "base": lambda manifest: manifest["source"].__setitem__("base_sha", "8" * 40),
            "tree": lambda manifest: manifest["source"].__setitem__("tree_sha", "8" * 40),
            "title": lambda manifest: manifest["pull_request"].__setitem__(
                "title", "feat: wrong title"
            ),
            "run": lambda manifest: manifest["workflow"].__setitem__("run_id", RUN_ID + 1),
            "version": lambda manifest: manifest["analysis"].__setitem__(
                "release_version", "1.1.1"
            ),
            "notes": lambda manifest: manifest["analysis"].__setitem__(
                "notes_sha256", "9" * 64
            ),
        }
        for name, mutate in mutations.items():
            with self.subTest(identity=name):
                candidate = self.root / f"candidate-{name}"
                shutil.copytree(self.candidate, candidate)
                manifest_path = candidate / "candidate-manifest.json"
                manifest = json.loads(manifest_path.read_text())
                mutate(manifest)
                manifest_path.write_text(json.dumps(manifest))
                github = FakeGitHub(self.pr)
                attach_candidate(github, candidate, self.pr)
                publisher = FirmwarePublisher(
                    repository=REPOSITORY,
                    repo_root=ROOT,
                    runner_temp=self.root / f"runner-identity-{name}",
                    repo=self.repo,
                    github=github,
                )
                with self.assertRaises(PublisherError):
                    publisher.run(self.push())
                self.assertEqual(github.mutations, [])

    def test_wrong_workflow_identity_fails_before_candidate_download(self) -> None:
        self.github.runs[0]["path"] = ".github/workflows/other.yml@refs/pull/42/merge"
        with self.assertRaisesRegex(PublisherError, "exact PR gate workflow"):
            self.publisher.run(self.push())
        self.assertEqual(self.github.mutations, [])

    def test_automatic_selection_fails_when_two_run_bound_candidates_match(self) -> None:
        second_root = self.root / "second-run"
        second_root.mkdir()
        second = build_candidate(
            second_root, self.pr, self.merge, run_id=RUN_ID + 1
        )
        attach_candidate(
            self.github,
            second,
            self.pr,
            run_id=RUN_ID + 1,
            artifact_id=ARTIFACT_ID + 1,
        )
        with self.assertRaisesRegex(PublisherError, "more than one exact run-bound candidate"):
            self.publisher.run(self.push())
        self.assertEqual(self.github.mutations, [])

    def test_release_workflow_change_bypass_is_rejected_on_main(self) -> None:
        self.repo.workflow_changes = [".github/workflows/firmware-pr-gate.yml"]
        with self.assertRaisesRegex(PublisherError, r"separate no-release 'ci:' PR"):
            self.publisher.run(self.push())
        self.assertEqual(self.github.mutations, [])

    def test_immutable_preflight_accepts_only_the_exact_supported_enabled_response(self) -> None:
        bad = [
            (403, None),
            (404, None),
            (200, {"enabled": False, "enforced_by_owner": False}),
            (200, {"enabled": True}),
            (200, {"enabled": True, "enforced_by_owner": False, "future": True}),
            (200, {"enabled": "true", "enforced_by_owner": False}),
            (201, {"enabled": True, "enforced_by_owner": False}),
        ]
        for response in bad:
            with self.subTest(response=response):
                github = copy.deepcopy(self.github)
                github.immutable = response
                publisher = FirmwarePublisher(
                    repository=REPOSITORY,
                    repo_root=ROOT,
                    runner_temp=self.root / "runner-preflight",
                    repo=self.repo,
                    github=github,
                )
                with self.assertRaisesRegex(PublisherError, r"Ticket 6|immutable releases"):
                    publisher.run(self.push())
                self.assertEqual(github.mutations, [])

    def test_bootstrap_no_release_exits_without_candidate_tag_or_release(self) -> None:
        title = "ci: add firmware release automation"
        pr = pull_request(title=title)
        merge = MergeIdentity(TARGET_SHA, BASE_SHA, TREE_SHA, f"{title} (#42)")
        github = FakeGitHub(pr)
        github.pull_request["title"] = "feat: mutable post-merge title edit"
        publisher = FirmwarePublisher(
            repository=REPOSITORY,
            repo_root=ROOT,
            runner_temp=self.root / "runner-bootstrap",
            repo=FakeRepository(merge, settled_base()),
            github=github,
        )
        result = publisher.run(self.push())
        self.assertEqual(result["outcome"], "no-release")
        self.assertEqual(github.mutations, [])
        self.assertEqual(github.artifacts, {})

    def test_manual_no_release_still_binds_supplied_run_and_artifact_ids(self) -> None:
        root = self.root / "manual-no-release"
        root.mkdir()
        title = "ci: update publisher documentation"
        pr = pull_request(title=title)
        merge = MergeIdentity(TARGET_SHA, BASE_SHA, TREE_SHA, f"{title} (#42)")
        candidate = build_candidate(root, pr, merge)
        github = FakeGitHub(pr)
        attach_candidate(github, candidate, pr)
        github.pull_request["title"] = "feat: mutable post-merge title edit"
        publisher = FirmwarePublisher(
            repository=REPOSITORY,
            repo_root=ROOT,
            runner_temp=root / "runner",
            repo=FakeRepository(merge, settled_base()),
            github=github,
        )
        request = PublishRequest(
            "workflow_dispatch",
            "refs/heads/main",
            TARGET_SHA,
            pr_workflow_run_id=RUN_ID,
            artifact_id=ARTIFACT_ID,
        )
        self.assertEqual(publisher.run(request)["outcome"], "no-release")
        self.assertEqual(github.mutations, [])

        github.runs[0]["display_title"] = "ci: stale run title"
        with self.assertRaisesRegex(PublisherError, "display_title"):
            publisher.run(request)
        github.runs[0]["display_title"] = title

        github.pull_merge_refs[42] = "7" * 40
        with self.assertRaisesRegex(PublisherError, "workflow.sha"):
            publisher.run(request)
        github.pull_merge_refs[42] = WORKFLOW_SHA

        with self.assertRaisesRegex(PublisherError, "artifact_id"):
            publisher.run(
                PublishRequest(
                    "workflow_dispatch",
                    "refs/heads/main",
                    TARGET_SHA,
                    pr_workflow_run_id=RUN_ID,
                    artifact_id=9999,
                )
            )

    def test_manual_no_release_rejects_squash_title_override(self) -> None:
        root = self.root / "manual-no-release-squash-override"
        root.mkdir()
        title = "ci: update publisher documentation"
        pr = pull_request(title=title)
        candidate_merge = MergeIdentity(
            TARGET_SHA,
            BASE_SHA,
            TREE_SHA,
            f"{title} (#42)",
        )
        candidate = build_candidate(root, pr, candidate_merge)
        github = FakeGitHub(pr)
        attach_candidate(github, candidate, pr)
        override = "docs: override the validated squash title"
        github.runs[0]["display_title"] = override
        publisher = FirmwarePublisher(
            repository=REPOSITORY,
            repo_root=ROOT,
            runner_temp=root / "runner",
            repo=FakeRepository(
                MergeIdentity(TARGET_SHA, BASE_SHA, TREE_SHA, f"{override} (#42)"),
                settled_base(),
            ),
            github=github,
        )
        request = PublishRequest(
            "workflow_dispatch",
            "refs/heads/main",
            TARGET_SHA,
            pr_workflow_run_id=RUN_ID,
            artifact_id=ARTIFACT_ID,
        )
        with self.assertRaisesRegex(PublisherError, "candidate identity mismatch"):
            publisher.run(request)
        self.assertEqual(github.mutations, [])


class GitHubRefApiTests(unittest.TestCase):
    def setUp(self) -> None:
        self.client = GitHubPublisherClient(
            repository=REPOSITORY,
            token="test-token",
            api_url="https://api.github.com",
        )

    def test_resolves_exact_pull_merge_ref_through_git_ref_api(self) -> None:
        response = {
            "ref": "refs/pull/42/merge",
            "object": {"type": "commit", "sha": WORKFLOW_SHA},
        }
        with mock.patch.object(self.client, "get_json", return_value=response) as get_json:
            self.assertEqual(self.client.pull_request_merge_ref_sha(42), WORKFLOW_SHA)
        get_json.assert_called_once_with(
            f"/repos/{REPOSITORY}/git/ref/pull/42/merge",
            optional=True,
        )

    def test_missing_or_malformed_pull_merge_ref_response_fails_closed(self) -> None:
        bad = (
            None,
            [],
            {"ref": "refs/pull/41/merge", "object": {"type": "commit", "sha": WORKFLOW_SHA}},
            {"ref": "refs/pull/42/merge", "object": {"type": "tag", "sha": WORKFLOW_SHA}},
            {"ref": "refs/pull/42/merge", "object": {"type": "commit", "sha": "bad"}},
        )
        for response in bad:
            with self.subTest(response=response):
                with mock.patch.object(self.client, "get_json", return_value=response):
                    with self.assertRaisesRegex(PublisherError, r"refs/pull/42/merge"):
                        self.client.pull_request_merge_ref_sha(42)


class TargetValidationTests(unittest.TestCase):
    def test_full_target_must_be_linear_and_on_current_main_first_parent(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            git(["init", "--initial-branch=main"], root)
            git(["config", "user.email", "tests@example.invalid"], root)
            git(["config", "user.name", "Publisher Target Tests"], root)
            base = git_commit(root, "base", "base\n")
            original_push = git_commit(root, "main one", "main-one\n")
            git(["branch", "feature", base], root)
            main_tip = git_commit(root, "main two", "main-two\n")
            repo = PublisherRepository(root, root / "runner")

            # A queued rerun may target an older push, but it must still be on
            # the current first-parent chain.
            git(["checkout", "--detach", original_push], root)
            identity = repo.publish_target(original_push, "refs/heads/main")
            self.assertEqual(identity.sha, original_push)
            self.assertEqual(identity.subject, "main one")
            self.assertNotEqual(identity.sha, main_tip)

            git(["checkout", "feature"], root)
            feature = git_commit(root, "feature", "feature\n")
            git(["checkout", "--detach", feature], root)
            with self.assertRaisesRegex(PublisherError, "first-parent chain"):
                repo.publish_target(feature, "refs/heads/main")


class RecoveryHarnessTests(unittest.TestCase):
    def test_verified_recovery_creates_only_the_protected_exact_marker(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            failed_sha = "a" * 40
            target_sha = "c" * 40
            tree_sha = "d" * 40
            head_sha = "f" * 40
            title = f"revert(release-{failed_sha}): recover failed firmware release"
            pr = pull_request(number=43, title=title, head_sha=head_sha)
            merge = MergeIdentity(
                target_sha,
                failed_sha,
                tree_sha,
                f"{title} (#43)",
            )
            candidate = build_candidate(
                root, pr, merge, recovery_of_sha=failed_sha
            )
            github = FakeGitHub(pr)
            attach_candidate(github, candidate, pr)
            github.rulesets = [
                {
                    "target": "tag",
                    "enforcement": "active",
                    "conditions": {
                        "ref_name": {
                            "include": ["refs/tags/release-aborted/*"],
                            "exclude": [],
                        }
                    },
                    "rules": [
                        {"type": "creation"},
                        {"type": "update"},
                        {"type": "deletion"},
                    ],
                }
            ]
            repo = FakeRepository(
                merge,
                unsettled_base(),
                recovery_failed_sha=failed_sha,
            )
            publisher = FirmwarePublisher(
                repository=REPOSITORY,
                repo_root=ROOT,
                runner_temp=root / "runner",
                repo=repo,
                github=github,
            )
            request = PublishRequest("push", "refs/heads/main", target_sha, target_sha)
            result = publisher.run(request)
            marker = f"release-aborted/{failed_sha}"
            self.assertEqual(result["outcome"], "recovery-marked")
            self.assertEqual(github.tags, {marker: target_sha})
            self.assertEqual(github.releases, [])
            self.assertEqual(github.mutations, [f"tag:{marker}"])

            result = publisher.run(request)
            self.assertEqual(result["outcome"], "recovery-already-marked")
            self.assertEqual(github.mutations, [f"tag:{marker}"])


if __name__ == "__main__":
    unittest.main()
