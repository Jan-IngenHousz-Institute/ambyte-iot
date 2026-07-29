from __future__ import annotations

import hashlib
import json
import subprocess
import tempfile
import unittest
from copy import deepcopy
from pathlib import Path
from typing import Any

from tools.pr_gate.gate import (
    GateError,
    GitRepository,
    GitHubClient,
    PullRequestEvent,
    _ruleset_protects,
    collect_candidate_input,
    enforce_release_workflow_policy,
    parse_pull_request_event,
    prepare_candidate_files,
    recovery_target,
)


REPOSITORY = "Jan-IngenHousz-Institute/ambyte-iot"
ROOT = Path(__file__).resolve().parents[3]
FIXTURES = Path(__file__).with_name("fixtures")


def run(command: list[str], cwd: Path) -> str:
    result = subprocess.run(
        command,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode:
        raise AssertionError(
            f"command failed: {' '.join(command)}\n{result.stdout}{result.stderr}"
        )
    return result.stdout.strip()


def commit(repo: Path, subject: str) -> str:
    run(["git", "add", "--all"], repo)
    run(["git", "commit", "-m", subject], repo)
    return run(["git", "rev-parse", "HEAD"], repo)


def event_for(
    *, number: int, title: str, head_sha: str, base_sha: str, labels: list[str]
) -> dict[str, Any]:
    return {
        "action": "labeled" if labels else "edited",
        "number": number,
        "pull_request": {
            "number": number,
            "title": title,
            "html_url": f"https://github.com/{REPOSITORY}/pull/{number}",
            "head": {"sha": head_sha},
            "base": {"sha": base_sha},
            "labels": [{"name": label} for label in labels],
        },
    }


class FixtureGitHub:
    def __init__(self, releases: dict[str, dict[str, Any]], pulls: dict[str, Any]):
        self.releases = releases
        self.pulls = pulls
        self.associated_calls: list[str] = []

    def release_for_tag(self, tag: str) -> dict[str, Any] | None:
        return self.releases.get(tag)

    def associated_pull_request(self, sha: str) -> dict[str, Any] | None:
        self.associated_calls.append(sha)
        return self.pulls.get(sha)

    def active_tag_rulesets(self) -> list[dict[str, Any]]:
        return []


class FixtureRepository:
    def __init__(
        self,
        *,
        commits: list[dict[str, Any]],
        tags: list[tuple[str, str]],
        head_sha: str,
        head_tree: str,
    ):
        self.commits = commits
        self.tag_records = tags
        self.head_sha = head_sha
        self.head_tree = head_tree

    def assert_exact_head(self, expected_head: str) -> str:
        if expected_head != self.head_sha:
            raise GateError("fixture head mismatch")
        return expected_head

    def commit_exists(self, sha: str, label: str) -> None:
        known = {commit["sha"] for commit in self.commits} | {self.head_sha}
        if sha not in known:
            raise GateError(f"{label} is unavailable")

    def tree(self, sha: str) -> str:
        if sha == self.head_sha:
            return self.head_tree
        return next(commit["tree_sha"] for commit in self.commits if commit["sha"] == sha)

    def first_parent_chain(self, base_sha: str) -> list[dict[str, Any]]:
        if self.commits[-1]["sha"] != base_sha:
            raise GateError("fixture base mismatch")
        return deepcopy(self.commits)

    def tags(self) -> list[tuple[str, str]]:
        return list(self.tag_records)


class MalformedRulesetClient(GitHubClient):
    def __init__(self) -> None:
        super().__init__(repository=REPOSITORY, token="fixture-token")

    def get_json(self, path: str, *, optional: bool = False) -> Any:
        if path.startswith("/repos/"):
            return [
                {
                    "id": 17,
                    "enforcement": "active",
                    "_links": {"self": {"href": "https://api.github.com/rulesets/17"}},
                }
            ]
        return ["malformed-detail"]


def no_release_analysis(event: dict[str, Any], *, tree_sha: str, notes: str) -> dict[str, Any]:
    pr = event["pull_request"]
    digest = hashlib.sha256(notes.encode()).hexdigest()
    return {
        "ok": True,
        "candidate_identity": {
            "pull_request_number": pr["number"],
            "pull_request_url": pr["html_url"],
            "title": pr["title"],
            "head_sha": pr["head"]["sha"],
            "base_sha": pr["base"]["sha"],
            "tree_sha": tree_sha,
            "latest_release_tag": "v1.0.5",
            "release_tag": None,
            "notes_sha256": digest,
            "recovery_of_sha": None,
        },
        "decision": {
            "release": False,
            "bump": None,
            "kind": "no-release",
            "reason": "no-release-type",
        },
        "version": {"previous": "1.0.5", "next": None, "tag": None},
        "notes": {"markdown": notes, "sha256": digest},
    }


class RecoveryIntentTests(unittest.TestCase):
    def test_fixture_uses_only_exact_label_and_full_sha_title_scope(self) -> None:
        raw = json.loads((FIXTURES / "recovery-pull-request.json").read_text())
        event = parse_pull_request_event(raw, REPOSITORY)
        self.assertEqual(recovery_target(event), "a" * 40)

        without_label = PullRequestEvent(
            number=event.number,
            title=event.title,
            url=event.url,
            head_sha=event.head_sha,
            base_sha=event.base_sha,
            labels=frozenset(),
        )
        self.assertIsNone(recovery_target(without_label))

    def test_label_rejects_short_sha_or_mutable_text_linkage(self) -> None:
        for title in (
            "revert(release-aaaaaaaaaaaa): recover release",
            "revert: recover aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        ):
            event = PullRequestEvent(
                number=1,
                title=title,
                url=f"https://github.com/{REPOSITORY}/pull/1",
                head_sha="b" * 40,
                base_sha="c" * 40,
                labels=frozenset({"release-recovery"}),
            )
            with self.assertRaises(GateError):
                recovery_target(event)

    def test_active_tag_ruleset_must_restrict_create_update_and_delete(self) -> None:
        ruleset = {
            "target": "tag",
            "enforcement": "active",
            "conditions": {
                "ref_name": {"include": ["refs/tags/release-aborted/*"], "exclude": []}
            },
            "rules": [
                {"type": "creation"},
                {"type": "update"},
                {"type": "deletion"},
            ],
        }
        self.assertTrue(_ruleset_protects(ruleset, f"release-aborted/{'a' * 40}"))
        ruleset["rules"].pop()
        self.assertFalse(_ruleset_protects(ruleset, f"release-aborted/{'a' * 40}"))

    def test_malformed_ruleset_error_names_the_deterministic_ruleset_id(self) -> None:
        with self.assertRaisesRegex(GateError, r"ruleset 17 response is not an object"):
            MalformedRulesetClient().active_tag_rulesets()


class WorkflowChangePolicyTests(unittest.TestCase):
    def test_git_path_facts_cover_add_modify_delete_and_rename(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repo_path = root / "repo"
            repo_path.mkdir()
            run(["git", "init", "--initial-branch=main"], repo_path)
            run(["git", "config", "user.email", "tests@example.invalid"], repo_path)
            run(["git", "config", "user.name", "Path Fact Tests"], repo_path)
            workflows = repo_path / ".github" / "workflows"
            workflows.mkdir(parents=True)
            original = workflows / "original.yml"
            original.write_text("name: original\n")
            (repo_path / "firmware.c").write_text("v1\n")
            base = commit(repo_path, "base")

            original.write_text("name: modified\n")
            (repo_path / "firmware.c").write_text("v2\n")
            modified = commit(repo_path, "modify workflow and firmware")

            renamed = workflows / "renamed.yml"
            original.rename(renamed)
            renamed_sha = commit(repo_path, "rename workflow")

            renamed.unlink()
            deleted = commit(repo_path, "delete workflow")

            added = workflows / "added.yml"
            added.write_text("name: added\n")
            added_sha = commit(repo_path, "add workflow")

            repo = GitRepository(repo_path, root / "runner-temp")
            self.assertEqual(
                repo.workflow_changed_paths(base, modified),
                [".github/workflows/original.yml"],
            )
            self.assertEqual(
                repo.workflow_changed_paths(modified, renamed_sha),
                [
                    ".github/workflows/original.yml",
                    ".github/workflows/renamed.yml",
                ],
            )
            self.assertEqual(
                repo.workflow_changed_paths(renamed_sha, deleted),
                [".github/workflows/renamed.yml"],
            )
            self.assertEqual(
                repo.workflow_changed_paths(deleted, added_sha),
                [".github/workflows/added.yml"],
            )

    def test_release_mixed_with_workflow_change_is_rejected_but_ci_is_allowed(self) -> None:
        paths = [".github/workflows/firmware-pr-gate.yml"]
        release = {
            "release": True,
            "bump": "minor",
            "kind": "release",
            "reason": "type-feat",
        }
        with self.assertRaisesRegex(GateError, r"split.*no-release 'ci:' PR"):
            enforce_release_workflow_policy(release, paths)
        no_release = {
            "release": False,
            "bump": None,
            "kind": "no-release",
            "reason": "no-release-type",
        }
        enforce_release_workflow_policy(no_release, paths)


class LegacyBaselineCollectorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.older_sha = "40787cf000000000000000000000000000000000"
        self.baseline_sha = "508bca7c302c8a5e1b5214d5b03d243de6965ac6"
        self.rtc_sha = "b98ef489614e9c6d98af70020f1adaaf36b1630b"
        self.bootstrap_sha = "c" * 40
        self.head_sha = "f" * 40
        self.commits = [
            {
                "sha": self.older_sha,
                "parent_sha": "8f1ff74000000000000000000000000000000000",
                "tree_sha": "0" * 40,
                "subject": "legacy v1.0.4",
            },
            {
                "sha": self.baseline_sha,
                "parent_sha": self.older_sha,
                "tree_sha": "1" * 40,
                "subject": "fw 1.0.5",
            },
            {
                "sha": self.rtc_sha,
                "parent_sha": self.baseline_sha,
                "tree_sha": "2" * 40,
                "subject": "rtc ota",
            },
            {
                "sha": self.bootstrap_sha,
                "parent_sha": self.rtc_sha,
                "tree_sha": "3" * 40,
                "subject": "untrusted squash text",
            },
        ]

    def repository(self, tags: list[tuple[str, str]]) -> FixtureRepository:
        return FixtureRepository(
            commits=self.commits,
            tags=tags,
            head_sha=self.head_sha,
            head_tree="9" * 40,
        )

    @staticmethod
    def published_mutable_release(tag: str) -> dict[str, Any]:
        return {
            "tag_name": tag,
            "published_at": "2025-01-01T00:00:00Z",
            "draft": False,
            "prerelease": False,
            "immutable": False,
        }

    def event(self) -> dict[str, Any]:
        return event_for(
            number=54,
            title="ci: exercise firmware release canary",
            head_sha=self.head_sha,
            base_sha=self.bootstrap_sha,
            labels=[],
        )

    def test_exact_mutable_boundary_starts_post_baseline_pr_collection(self) -> None:
        github = FixtureGitHub(
            releases={
                "v1.0.4": self.published_mutable_release("v1.0.4"),
                "v1.0.5": self.published_mutable_release("v1.0.5"),
            },
            pulls={
                self.bootstrap_sha: {
                    "number": 52,
                    "title": "ci: add firmware release automation",
                    "url": f"https://github.com/{REPOSITORY}/pull/52",
                }
            },
        )
        facts = collect_candidate_input(
            event_raw=self.event(),
            repository=REPOSITORY,
            repo=self.repository(
                [("v1.0.4", self.older_sha), ("v1.0.5", self.baseline_sha)]
            ),  # type: ignore[arg-type]
            github=github,  # type: ignore[arg-type]
        )
        self.assertEqual(github.associated_calls, [self.rtc_sha, self.bootstrap_sha])
        self.assertEqual(
            facts["base"]["first_parent_commits"][-1]["pull_request"]["title"],
            "ci: add firmware release automation",
        )

        analyzer = subprocess.run(
            ["node", "tools/release-analysis/src/cli.mjs", "analyze-candidate"],
            cwd=ROOT,
            input=json.dumps(facts),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(analyzer.returncode, 0, analyzer.stdout + analyzer.stderr)
        self.assertEqual(json.loads(analyzer.stdout)["base_state"]["settled"], True)

    def test_near_match_mutable_boundaries_do_not_start_pr_collection(self) -> None:
        cases = [
            (
                [("v1.0.4", self.older_sha), ("v1.0.6", self.baseline_sha)],
                {
                    "v1.0.4": self.published_mutable_release("v1.0.4"),
                    "v1.0.6": self.published_mutable_release("v1.0.6"),
                },
            ),
            (
                [("v1.0.4", self.older_sha), ("v1.0.5", self.older_sha)],
                {
                    "v1.0.4": self.published_mutable_release("v1.0.4"),
                    "v1.0.5": self.published_mutable_release("v1.0.5"),
                },
            ),
            (
                [("v1.0.4", self.older_sha), ("v1.0.5", self.baseline_sha)],
                {
                    "v1.0.4": self.published_mutable_release("v1.0.4"),
                    "v1.0.5": {
                        **self.published_mutable_release("v1.0.5"),
                        "published_at": None,
                        "draft": True,
                    },
                },
            ),
        ]
        for tags, releases in cases:
            with self.subTest(tags=tags, releases=releases):
                github = FixtureGitHub(releases=releases, pulls={})
                collect_candidate_input(
                    event_raw=self.event(),
                    repository=REPOSITORY,
                    repo=self.repository(tags),  # type: ignore[arg-type]
                    github=github,  # type: ignore[arg-type]
                )
                self.assertEqual(github.associated_calls, [])


class GitRecoveryIntegrationTests(unittest.TestCase):
    def test_collector_and_pure_analyzer_verify_exact_recovery_tree(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repo_path = root / "repo"
            runner_temp = root / "runner-temp"
            repo_path.mkdir()
            run(["git", "init", "--initial-branch=main"], repo_path)
            run(["git", "config", "user.email", "tests@example.invalid"], repo_path)
            run(["git", "config", "user.name", "PR Gate Tests"], repo_path)

            (repo_path / "firmware.txt").write_text("baseline\n")
            release_sha = commit(repo_path, "baseline release")
            run(["git", "tag", "v1.0.0", release_sha], repo_path)

            (repo_path / "firmware.txt").write_text("failed release\n")
            failed_sha = commit(repo_path, "failed release merge")
            base_sha = failed_sha

            run(["git", "revert", "--no-edit", failed_sha], repo_path)
            head_sha = run(["git", "rev-parse", "HEAD"], repo_path)
            head_tree = run(["git", "rev-parse", "HEAD^{tree}"], repo_path)
            title = f"revert(release-{failed_sha}): recover failed firmware release"
            raw_event = event_for(
                number=7,
                title=title,
                head_sha=head_sha,
                base_sha=base_sha,
                labels=["release-recovery"],
            )
            github = FixtureGitHub(
                releases={
                    "v1.0.0": {
                        "tag_name": "v1.0.0",
                        "published_at": "2026-01-01T00:00:00Z",
                        "draft": False,
                        "prerelease": False,
                        "immutable": True,
                    }
                },
                pulls={
                    failed_sha: {
                        "number": 6,
                        "title": "feat: ship failed firmware",
                        "url": f"https://github.com/{REPOSITORY}/pull/6",
                    }
                },
            )
            facts = collect_candidate_input(
                event_raw=raw_event,
                repository=REPOSITORY,
                repo=GitRepository(repo_path, runner_temp),
                github=github,  # type: ignore[arg-type]
            )
            self.assertEqual(facts["candidate"]["tree_sha"], head_tree)
            self.assertEqual(facts["recovery"]["failed_sha"], failed_sha)
            self.assertEqual(facts["recovery"]["expected_revert_tree_sha"], head_tree)

            analyzer = subprocess.run(
                ["node", "tools/release-analysis/src/cli.mjs", "analyze-candidate"],
                cwd=ROOT,
                input=json.dumps(facts),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertEqual(analyzer.returncode, 0, analyzer.stdout + analyzer.stderr)
            output = json.loads(analyzer.stdout)
            self.assertEqual(output["decision"]["kind"], "recovery")
            self.assertFalse(output["decision"]["release"])
            self.assertEqual(output["candidate_identity"]["recovery_of_sha"], failed_sha)
            prepared = prepare_candidate_files(
                event_raw=raw_event,
                analysis=output,
                repository=REPOSITORY,
                workflow_sha="5" * 40,
                run_id=200,
                run_attempt=2,
                output_dir=runner_temp / "prepared",
            )
            self.assertEqual(prepared["firmware_version"], f"pr-7-{head_sha[:12]}")
            metadata = json.loads(Path(prepared["metadata_path"]).read_text())
            self.assertIsNone(metadata["analysis"]["release_version"])


class CandidatePreparationTests(unittest.TestCase):
    def test_release_candidate_embeds_predicted_bare_semver(self) -> None:
        event = json.loads((FIXTURES / "pull-request.json").read_text())
        event["pull_request"]["title"] = "feat: add release gate"
        notes = "## Features\n\n- add release gate\n"
        analysis = no_release_analysis(event, tree_sha="4" * 40, notes=notes)
        analysis["decision"] = {
            "release": True,
            "bump": "minor",
            "kind": "release",
            "reason": "type-feat",
        }
        analysis["version"] = {"previous": "1.0.5", "next": "1.1.0", "tag": "v1.1.0"}
        analysis["candidate_identity"]["release_tag"] = "v1.1.0"
        with tempfile.TemporaryDirectory() as directory:
            prepared = prepare_candidate_files(
                event_raw=event,
                analysis=analysis,
                repository=REPOSITORY,
                workflow_sha="5" * 40,
                run_id=99,
                run_attempt=1,
                output_dir=Path(directory) / "prepared",
            )
            self.assertEqual(prepared["firmware_version"], "1.1.0")
            metadata = json.loads(Path(prepared["metadata_path"]).read_text())
            self.assertEqual(metadata["analysis"]["release_version"], "1.1.0")

    def test_title_edit_same_head_has_distinct_run_bound_metadata(self) -> None:
        fixture = json.loads((FIXTURES / "pull-request.json").read_text())
        tree_sha = "4" * 40
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first_event = fixture
            first_notes = "## Continuous Integration\n\n- first title\n"
            first = prepare_candidate_files(
                event_raw=first_event,
                analysis=no_release_analysis(first_event, tree_sha=tree_sha, notes=first_notes),
                repository=REPOSITORY,
                workflow_sha="5" * 40,
                run_id=100,
                run_attempt=1,
                output_dir=root / "run-100",
            )

            second_event = json.loads(json.dumps(fixture))
            second_event["pull_request"]["title"] = "ci: edited candidate identity"
            second_notes = "## Continuous Integration\n\n- edited title\n"
            second = prepare_candidate_files(
                event_raw=second_event,
                analysis=no_release_analysis(second_event, tree_sha=tree_sha, notes=second_notes),
                repository=REPOSITORY,
                workflow_sha="5" * 40,
                run_id=101,
                run_attempt=1,
                output_dir=root / "run-101",
            )

            self.assertEqual(first["artifact_name"], second["artifact_name"])
            self.assertEqual(first["firmware_version"], "pr-42-333333333333")
            self.assertEqual(first["firmware_version"], second["firmware_version"])
            first_metadata = json.loads(Path(first["metadata_path"]).read_text())
            second_metadata = json.loads(Path(second["metadata_path"]).read_text())
            self.assertEqual(first_metadata["workflow"]["run_id"], 100)
            self.assertEqual(second_metadata["workflow"]["run_id"], 101)
            self.assertNotEqual(
                first_metadata["pull_request"]["title"],
                second_metadata["pull_request"]["title"],
            )
            self.assertNotIn("artifact_id", first_metadata)
            self.assertNotIn("artifact_id", second_metadata)

    def test_prepared_files_stay_under_requested_runner_temp_directory(self) -> None:
        event = json.loads((FIXTURES / "pull-request.json").read_text())
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "runner-temp" / "prepared"
            prepared = prepare_candidate_files(
                event_raw=event,
                analysis=no_release_analysis(event, tree_sha="4" * 40, notes="notes\n"),
                repository=REPOSITORY,
                workflow_sha="5" * 40,
                run_id=1,
                run_attempt=1,
                output_dir=output,
            )
            for key in ("metadata_path", "notes_path", "summary_path"):
                self.assertTrue(Path(prepared[key]).is_relative_to(output))


if __name__ == "__main__":
    unittest.main()
