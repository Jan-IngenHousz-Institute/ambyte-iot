from __future__ import annotations

import fnmatch
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any


GIT_SHA_RE = re.compile(r"^[0-9a-f]{40}$")
SEMVER_TAG_RE = re.compile(r"^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
RECOVERY_MARKER_RE = re.compile(r"^release-aborted/([0-9a-f]{40})$")
RECOVERY_TITLE_RE = re.compile(
    r"^revert\(release-([0-9a-f]{40})\): ([^\r\n]+)$"
)
RECOVERY_LABEL = "release-recovery"
COMMENT_MARKER = "<!-- ambyte-firmware-release-candidate -->"
WORKFLOW_DIRECTORY = ".github/workflows/"
APPROVED_LEGACY_RELEASE_TAG = "v1.0.5"
APPROVED_LEGACY_RELEASE_SHA = "508bca7c302c8a5e1b5214d5b03d243de6965ac6"


class GateError(ValueError):
    """The event or collected repository facts violate the PR-gate contract."""


def canonical_json(value: Any) -> bytes:
    return (
        json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":"))
        + "\n"
    ).encode("utf-8")


def write_canonical_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(canonical_json(value))


def _git_sha(value: Any, label: str) -> str:
    if not isinstance(value, str) or not GIT_SHA_RE.fullmatch(value):
        raise GateError(f"{label} must be exactly 40 lowercase hexadecimal characters")
    return value


def _positive_int(value: Any, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise GateError(f"{label} must be a positive integer")
    return value


def _one_line(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value or "\n" in value or "\r" in value:
        raise GateError(f"{label} must be one non-empty line")
    return value


def _repository(value: Any) -> str:
    if not isinstance(value, str) or not re.fullmatch(
        r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", value
    ):
        raise GateError("repository must have owner/name syntax")
    return value


def _run(
    command: list[str], *, cwd: Path, env: dict[str, str] | None = None
) -> str:
    result = subprocess.run(
        command,
        cwd=cwd,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise GateError(
            f"command failed ({result.returncode}): {' '.join(command)}"
            + (f"\n{detail}" if detail else "")
        )
    return result.stdout


@dataclass(frozen=True)
class PullRequestEvent:
    number: int
    title: str
    url: str
    head_sha: str
    base_sha: str
    labels: frozenset[str]


def parse_pull_request_event(raw: Any, repository: str) -> PullRequestEvent:
    repository = _repository(repository)
    if not isinstance(raw, dict) or not isinstance(raw.get("pull_request"), dict):
        raise GateError("event must contain a pull_request object")
    pr = raw["pull_request"]
    number = _positive_int(pr.get("number") or raw.get("number"), "pull_request.number")
    title = _one_line(pr.get("title"), "pull_request.title")
    head = pr.get("head")
    base = pr.get("base")
    if not isinstance(head, dict) or not isinstance(base, dict):
        raise GateError("pull_request must contain head and base objects")
    head_sha = _git_sha(head.get("sha"), "pull_request.head.sha")
    base_sha = _git_sha(base.get("sha"), "pull_request.base.sha")
    labels_raw = pr.get("labels", [])
    if not isinstance(labels_raw, list):
        raise GateError("pull_request.labels must be an array")
    labels: set[str] = set()
    for item in labels_raw:
        if not isinstance(item, dict) or not isinstance(item.get("name"), str):
            raise GateError("each pull_request label must contain a string name")
        labels.add(item["name"])
    url = f"https://github.com/{repository}/pull/{number}"
    event_url = pr.get("html_url")
    if event_url is not None and event_url != url:
        raise GateError("pull_request.html_url does not match repository and PR number")
    return PullRequestEvent(
        number=number,
        title=title,
        url=url,
        head_sha=head_sha,
        base_sha=base_sha,
        labels=frozenset(labels),
    )


def recovery_target(event: PullRequestEvent) -> str | None:
    """Return a recovery target only for the fixed label plus exact title scope.

    The label is an intent/re-evaluation signal. The returned SHA is not trusted
    until the pure analyzer proves it is the sole unresolved release and the Git
    helper proves the exact revert tree.
    """

    if RECOVERY_LABEL not in event.labels:
        return None
    match = RECOVERY_TITLE_RE.fullmatch(event.title)
    if not match:
        raise GateError(
            "release-recovery requires title "
            "'revert(release-<40 lowercase hex SHA>): <non-empty subject>'"
        )
    return match.group(1)


class GitRepository:
    def __init__(self, root: Path, runner_temp: Path):
        self.root = root.resolve()
        self.runner_temp = runner_temp.resolve()
        if not (self.root / ".git").exists():
            # Linked worktrees store .git as a file.
            if not (self.root / ".git").is_file():
                raise GateError(f"not a Git checkout: {self.root}")
        self.runner_temp.mkdir(parents=True, exist_ok=True)

    def git(self, *args: str) -> str:
        return _run(["git", *args], cwd=self.root)

    def assert_exact_head(self, expected_head: str) -> str:
        expected_head = _git_sha(expected_head, "expected head SHA")
        actual = self.git("rev-parse", "HEAD").strip()
        if actual != expected_head:
            raise GateError(
                f"checked-out HEAD mismatch: expected {expected_head}, got {actual}"
            )
        return actual

    def commit_exists(self, sha: str, label: str) -> None:
        _git_sha(sha, label)
        result = subprocess.run(
            ["git", "cat-file", "-e", f"{sha}^{{commit}}"],
            cwd=self.root,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        if result.returncode != 0:
            raise GateError(f"{label} is not available as a commit: {sha}")

    def tree(self, sha: str) -> str:
        self.commit_exists(sha, "commit SHA")
        return _git_sha(self.git("rev-parse", f"{sha}^{{tree}}").strip(), "tree SHA")

    def changed_paths(self, base_sha: str, head_sha: str) -> list[str]:
        """Return every old/new path changed by base..head, including renames.

        `--name-status -z` is intentionally used instead of a path-limited diff:
        a rename into or out of a protected directory must expose both names.
        """

        self.commit_exists(base_sha, "diff base SHA")
        self.commit_exists(head_sha, "diff head SHA")
        raw = subprocess.run(
            [
                "git",
                "diff",
                "--name-status",
                "-z",
                "--find-renames",
                base_sha,
                head_sha,
                "--",
            ],
            cwd=self.root,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if raw.returncode != 0:
            detail = raw.stderr.decode("utf-8", errors="replace").strip()
            raise GateError(f"cannot collect changed paths: {detail}")
        fields = raw.stdout.split(b"\0")
        if fields and fields[-1] == b"":
            fields.pop()
        paths: list[str] = []
        index = 0
        while index < len(fields):
            try:
                status = fields[index].decode("ascii")
            except UnicodeDecodeError as exc:
                raise GateError("Git diff status is not ASCII") from exc
            index += 1
            path_count = 2 if status.startswith(("R", "C")) else 1
            if index + path_count > len(fields):
                raise GateError("Git diff emitted an incomplete name-status record")
            for raw_path in fields[index : index + path_count]:
                try:
                    path = raw_path.decode("utf-8")
                except UnicodeDecodeError as exc:
                    raise GateError("Git diff path is not valid UTF-8") from exc
                if not path or path.startswith("/") or "\0" in path:
                    raise GateError("Git diff emitted an invalid repository path")
                paths.append(path)
            index += path_count
        return sorted(set(paths))

    def workflow_changed_paths(self, base_sha: str, head_sha: str) -> list[str]:
        return [
            path
            for path in self.changed_paths(base_sha, head_sha)
            if path.startswith(WORKFLOW_DIRECTORY)
        ]

    def first_parent_chain(self, base_sha: str) -> list[dict[str, Any]]:
        self.commit_exists(base_sha, "base SHA")
        shas = [
            line
            for line in self.git("rev-list", "--first-parent", "--reverse", base_sha).splitlines()
            if line
        ]
        commits: list[dict[str, Any]] = []
        for sha in shas:
            parent_line = self.git("rev-list", "--parents", "-n", "1", sha).strip().split()
            parent = parent_line[1] if len(parent_line) > 1 else None
            subject = self.git("show", "-s", "--format=%s", sha).rstrip("\n")
            commits.append(
                {
                    "sha": _git_sha(sha, "commit SHA"),
                    "parent_sha": _git_sha(parent, "parent SHA") if parent else None,
                    "tree_sha": self.tree(sha),
                    "subject": _one_line(subject, "commit subject"),
                }
            )
        return commits

    def tags(self) -> list[tuple[str, str]]:
        names = [
            line
            for line in self.git("tag", "--list", "--sort=refname").splitlines()
            if line
        ]
        return [
            (name, self.git("rev-parse", f"refs/tags/{name}^{{commit}}").strip())
            for name in names
        ]

    def expected_revert_tree(self, base_sha: str, failed_sha: str) -> str:
        """Compute `git revert <failed_sha>` in an isolated clone under RUNNER_TEMP."""

        self.commit_exists(base_sha, "revert base SHA")
        self.commit_exists(failed_sha, "failed release SHA")
        temp = Path(tempfile.mkdtemp(prefix="ambyte-revert-", dir=self.runner_temp))
        clone = temp / "repo"
        try:
            _run(
                [
                    "git",
                    "clone",
                    "--quiet",
                    "--no-checkout",
                    "--no-hardlinks",
                    str(self.root),
                    str(clone),
                ],
                cwd=temp,
            )
            _run(["git", "checkout", "--quiet", "--detach", base_sha], cwd=clone)
            _run(
                [
                    "git",
                    "-c",
                    "core.hooksPath=/dev/null",
                    "revert",
                    "--no-commit",
                    failed_sha,
                ],
                cwd=clone,
            )
            return _git_sha(
                _run(["git", "write-tree"], cwd=clone).strip(),
                "expected revert tree SHA",
            )
        except GateError as exc:
            raise GateError(
                f"isolated git revert verification failed for {failed_sha}: {exc}"
            ) from exc
        finally:
            shutil.rmtree(temp, ignore_errors=True)


class GitHubClient:
    def __init__(
        self,
        *,
        repository: str,
        token: str,
        api_url: str = "https://api.github.com",
    ):
        self.repository = _repository(repository)
        if not token:
            raise GateError("GITHUB_TOKEN is required for read-only GitHub fact collection")
        self.token = token
        self.api_url = api_url.rstrip("/")

    def get_json(self, path: str, *, optional: bool = False) -> Any:
        if path.startswith("https://"):
            if not path.startswith(f"{self.api_url}/"):
                raise GateError("refusing a GitHub API link outside GITHUB_API_URL")
            url = path
        else:
            url = f"{self.api_url}{path}"
        request = urllib.request.Request(
            url,
            headers={
                "Accept": "application/vnd.github+json",
                "Authorization": f"Bearer {self.token}",
                "X-GitHub-Api-Version": "2022-11-28",
                "User-Agent": "ambyte-firmware-pr-gate",
            },
            method="GET",
        )
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                return json.load(response)
        except urllib.error.HTTPError as exc:
            if optional and exc.code == 404:
                return None
            raise GateError(f"GitHub API GET {path} failed with HTTP {exc.code}") from exc
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
            raise GateError(f"GitHub API GET {path} failed: {exc}") from exc

    def release_for_tag(self, tag: str) -> dict[str, Any] | None:
        encoded = urllib.parse.quote(tag, safe="")
        value = self.get_json(
            f"/repos/{self.repository}/releases/tags/{encoded}", optional=True
        )
        if value is not None and not isinstance(value, dict):
            raise GateError(f"GitHub release response for {tag} is not an object")
        return value

    def associated_pull_request(self, sha: str) -> dict[str, Any] | None:
        sha = _git_sha(sha, "associated commit SHA")
        value = self.get_json(
            f"/repos/{self.repository}/commits/{sha}/pulls?per_page=100"
        )
        if not isinstance(value, list):
            raise GateError(f"associated PR response for {sha} is not an array")
        matches = []
        for pr in value:
            base_repo = ((pr.get("base") or {}).get("repo") or {}).get("full_name")
            if (
                pr.get("merged_at")
                and pr.get("merge_commit_sha") == sha
                and isinstance(base_repo, str)
                and base_repo.casefold() == self.repository.casefold()
            ):
                matches.append(pr)
        if len(matches) > 1:
            raise GateError(f"commit {sha} has more than one exact merged PR association")
        if not matches:
            return None
        pr = matches[0]
        number = _positive_int(pr.get("number"), "associated pull_request.number")
        title = _one_line(pr.get("title"), "associated pull_request.title")
        return {
            "number": number,
            "title": title,
            "url": f"https://github.com/{self.repository}/pull/{number}",
        }

    def active_tag_rulesets(self) -> list[dict[str, Any]]:
        summaries = self.get_json(
            f"/repos/{self.repository}/rulesets?includes_parents=true&targets=tag&per_page=100"
        )
        if not isinstance(summaries, list):
            raise GateError("GitHub rulesets response is not an array")
        details = []
        for summary in summaries:
            if summary.get("enforcement") != "active":
                continue
            ruleset_id = _positive_int(summary.get("id"), "ruleset.id")
            link = ((summary.get("_links") or {}).get("self") or {}).get("href")
            if not isinstance(link, str):
                raise GateError("active tag ruleset is missing its canonical API link")
            detail = self.get_json(link)
            if not isinstance(detail, dict):
                raise GateError(f"GitHub ruleset {ruleset_id} response is not an object")
            details.append(detail)
        return details


def _ruleset_protects(ruleset: dict[str, Any], tag_name: str) -> bool:
    if ruleset.get("target") != "tag" or ruleset.get("enforcement") != "active":
        return False
    conditions = ruleset.get("conditions") or {}
    ref_name = conditions.get("ref_name") or {}
    includes = ref_name.get("include") or []
    excludes = ref_name.get("exclude") or []
    full_ref = f"refs/tags/{tag_name}"
    if not any(pattern == "~ALL" or fnmatch.fnmatchcase(full_ref, pattern) for pattern in includes):
        return False
    if any(pattern == "~ALL" or fnmatch.fnmatchcase(full_ref, pattern) for pattern in excludes):
        return False
    rule_types = {
        rule.get("type") for rule in ruleset.get("rules", []) if isinstance(rule, dict)
    }
    return {"creation", "update", "deletion"}.issubset(rule_types)


def _release_facts(
    repo: GitRepository, github: GitHubClient
) -> tuple[list[dict[str, Any]], list[tuple[str, str]]]:
    releases: list[dict[str, Any]] = []
    markers: list[tuple[str, str]] = []
    for name, target_sha in repo.tags():
        target_sha = _git_sha(target_sha, f"tag {name} target")
        if SEMVER_TAG_RE.fullmatch(name):
            release = github.release_for_tag(name)
            # A bare v* tag is not a release authority. Existing GitHub release
            # records are included even when untrusted so the analyzer fails closed.
            if release is None:
                continue
            if release.get("tag_name") != name:
                raise GateError(f"GitHub release tag mismatch for {name}")
            releases.append(
                {
                    "name": name,
                    "target_sha": target_sha,
                    "published": bool(release.get("published_at"))
                    and release.get("draft") is False
                    and release.get("prerelease") is False,
                    "immutable": release.get("immutable") is True,
                }
            )
        elif RECOVERY_MARKER_RE.fullmatch(name):
            markers.append((name, target_sha))
    return releases, markers


def _is_approved_legacy_boundary(tag: dict[str, Any]) -> bool:
    return (
        tag.get("name") == APPROVED_LEGACY_RELEASE_TAG
        and tag.get("target_sha") == APPROVED_LEGACY_RELEASE_SHA
        and tag.get("published") is True
    )


def collect_base_state_input(
    *,
    base_sha: str,
    repo: GitRepository,
    github: GitHubClient,
) -> dict[str, Any]:
    base_sha = _git_sha(base_sha, "base SHA")
    commits = repo.first_parent_chain(base_sha)
    index_by_sha = {commit["sha"]: index for index, commit in enumerate(commits)}

    release_tags, marker_refs = _release_facts(repo, github)
    authority_positions = [
        index_by_sha[tag["target_sha"]]
        for tag in release_tags
        if (
            (tag["published"] and tag["immutable"])
            or _is_approved_legacy_boundary(tag)
        )
        and tag["target_sha"] in index_by_sha
    ]
    association_start = max(authority_positions) + 1 if authority_positions else len(commits)
    for commit in commits[association_start:]:
        pull_request = github.associated_pull_request(commit["sha"])
        if pull_request is not None:
            commit["pull_request"] = pull_request

    rulesets = github.active_tag_rulesets() if marker_refs else []
    recovery_markers: list[dict[str, Any]] = []
    for name, target_sha in marker_refs:
        recovery_index = index_by_sha.get(target_sha)
        # Tags on descendants are not state reachable from this predecessor.
        # This is essential for an idempotent rerun of the recovery publisher:
        # the marker at the recovery commit is not reachable from its parent.
        if recovery_index is None:
            continue
        recovery_markers.append(
            {
                "name": name,
                "target_sha": target_sha,
                "protected": any(_ruleset_protects(item, name) for item in rulesets),
            }
        )
        failed_sha = RECOVERY_MARKER_RE.fullmatch(name).group(1)  # type: ignore[union-attr]
        failed_index = index_by_sha.get(failed_sha)
        if failed_index is None or failed_index >= recovery_index:
            raise GateError(
                f"recovery marker {name} does not target an earlier first-parent release"
            )
        recovery_commit = commits[recovery_index]
        recovery_parent = recovery_commit["parent_sha"]
        failed_parent = commits[failed_index]["parent_sha"]
        if recovery_parent is None or failed_parent is None:
            raise GateError(f"recovery marker {name} cannot target a root commit")
        expected_tree = repo.expected_revert_tree(recovery_parent, failed_sha)
        if expected_tree != recovery_commit["tree_sha"]:
            raise GateError(
                f"recovery marker {name} target tree does not equal isolated git revert tree"
            )
        recovery_commit["recovery"] = {
            "failed_sha": failed_sha,
            "failed_parent_sha": failed_parent,
            "expected_revert_tree_sha": expected_tree,
        }

    return {
        "schema_version": 1,
        "base_sha": base_sha,
        "first_parent_commits": commits,
        "release_tags": release_tags,
        "recovery_markers": recovery_markers,
    }


def collect_candidate_input(
    *,
    event_raw: Any,
    repository: str,
    repo: GitRepository,
    github: GitHubClient,
) -> dict[str, Any]:
    event = parse_pull_request_event(event_raw, repository)
    repo.assert_exact_head(event.head_sha)
    repo.commit_exists(event.base_sha, "pull_request.base.sha")
    candidate_tree = repo.tree(event.head_sha)
    base = collect_base_state_input(base_sha=event.base_sha, repo=repo, github=github)
    commits = base["first_parent_commits"]
    index_by_sha = {commit["sha"]: index for index, commit in enumerate(commits)}

    candidate: dict[str, Any] = {
        "schema_version": 1,
        "pull_request": {
            "number": event.number,
            "title": event.title,
            "url": event.url,
        },
        "candidate": {
            "head_sha": event.head_sha,
            "base_sha": event.base_sha,
            "tree_sha": candidate_tree,
        },
        "base": base,
    }

    failed_sha = recovery_target(event)
    if failed_sha is not None:
        failed_index = index_by_sha.get(failed_sha)
        if failed_index is None:
            raise GateError("release-recovery target is not on the PR base first-parent chain")
        failed_parent = commits[failed_index]["parent_sha"]
        if failed_parent is None:
            raise GateError("release-recovery cannot target a root commit")
        expected_tree = repo.expected_revert_tree(event.base_sha, failed_sha)
        if expected_tree != candidate_tree:
            raise GateError(
                "release-recovery PR head tree does not equal the isolated git revert tree"
            )
        candidate["recovery"] = {
            "failed_sha": failed_sha,
            "failed_parent_sha": failed_parent,
            "expected_revert_tree_sha": expected_tree,
        }
    return candidate


def _sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def _artifact_name(number: int, head_sha: str) -> str:
    return f"firmware-candidate-pr-{number}-{head_sha[:12]}"


def prepare_candidate_files(
    *,
    event_raw: Any,
    analysis: Any,
    repository: str,
    workflow_sha: str,
    run_id: int,
    run_attempt: int,
    output_dir: Path,
    workflow_changed_paths: list[str] | tuple[str, ...] = (),
) -> dict[str, Any]:
    event = parse_pull_request_event(event_raw, repository)
    workflow_sha = _git_sha(workflow_sha, "workflow SHA")
    run_id = _positive_int(run_id, "workflow run ID")
    run_attempt = _positive_int(run_attempt, "workflow run attempt")
    if not isinstance(analysis, dict) or analysis.get("ok") is not True:
        raise GateError("analysis must be a successful candidate-analysis output")
    identity = analysis.get("candidate_identity")
    if not isinstance(identity, dict):
        raise GateError("analysis is missing candidate_identity")
    expected_identity = {
        "pull_request_number": event.number,
        "pull_request_url": event.url,
        "title": event.title,
        "head_sha": event.head_sha,
        "base_sha": event.base_sha,
    }
    for key, expected in expected_identity.items():
        if identity.get(key) != expected:
            raise GateError(f"analysis candidate_identity.{key} does not match the event")
    tree_sha = _git_sha(identity.get("tree_sha"), "analysis tree SHA")
    decision = analysis.get("decision")
    version = analysis.get("version")
    notes = analysis.get("notes")
    if not isinstance(decision, dict) or not isinstance(version, dict) or not isinstance(notes, dict):
        raise GateError("analysis is missing decision, version, or notes")
    notes_markdown = notes.get("markdown")
    notes_digest = notes.get("sha256")
    if not isinstance(notes_markdown, str) or not notes_markdown.endswith("\n"):
        raise GateError("analysis notes must be newline-terminated Markdown")
    if notes_digest != _sha256_text(notes_markdown):
        raise GateError("analysis notes digest does not match canonical Markdown")
    latest_release_tag = identity.get("latest_release_tag")
    if not isinstance(latest_release_tag, str) or not SEMVER_TAG_RE.fullmatch(
        latest_release_tag
    ):
        raise GateError("analysis candidate_identity.latest_release_tag is not exact vX.Y.Z")
    if identity.get("notes_sha256") != notes_digest:
        raise GateError("analysis candidate_identity.notes_sha256 does not match notes")
    if identity.get("release_tag") != version.get("tag"):
        raise GateError("analysis candidate_identity.release_tag does not match version.tag")
    recovery_of_sha = identity.get("recovery_of_sha")
    if decision.get("kind") == "recovery":
        _git_sha(recovery_of_sha, "analysis recovery_of_sha")
    elif recovery_of_sha is not None:
        raise GateError("only verified recovery analysis may carry recovery_of_sha")

    release = decision.get("release") is True
    enforce_release_workflow_policy(decision, workflow_changed_paths)
    if release:
        firmware_version = version.get("next")
        if not isinstance(firmware_version, str) or not re.fullmatch(
            r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)",
            firmware_version,
        ):
            raise GateError("release candidate must embed the predicted bare semver")
        release_version: str | None = firmware_version
    else:
        if version.get("next") is not None or version.get("tag") is not None:
            raise GateError("non-release analysis must not reserve a release version or tag")
        firmware_version = f"pr-{event.number}-{event.head_sha[:12]}"
        release_version = None

    artifact_name = _artifact_name(event.number, event.head_sha)
    metadata = {
        "schema_version": 1,
        "repository": repository,
        "pull_request": {"number": event.number, "title": event.title},
        "source": {
            "head_sha": event.head_sha,
            "base_sha": event.base_sha,
            "tree_sha": tree_sha,
        },
        "analysis": {
            "decision": decision,
            "release_version": release_version,
            "notes_sha256": notes_digest,
            "latest_release_tag": latest_release_tag,
            "recovery_of_sha": recovery_of_sha,
        },
        "workflow": {
            "sha": workflow_sha,
            "run_id": run_id,
            "run_attempt": run_attempt,
        },
    }

    output_dir = output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    if any(output_dir.iterdir()):
        raise GateError(f"prepared output directory is not empty: {output_dir}")
    notes_path = output_dir / "release-notes.md"
    metadata_path = output_dir / "candidate-metadata.json"
    summary_path = output_dir / "preview.md"
    notes_path.write_text(notes_markdown, encoding="utf-8", newline="\n")
    write_canonical_json(metadata_path, metadata)

    decision_label = (
        f"release `{firmware_version}` ({decision.get('bump')} bump)"
        if release
        else (
            "verified recovery (no release)"
            if decision.get("kind") == "recovery"
            else "no release"
        )
    )
    summary = (
        f"{COMMENT_MARKER}\n"
        "## Firmware release candidate\n\n"
        f"**Decision:** {decision_label}\n\n"
        "| Identity | Value |\n"
        "|---|---|\n"
        f"| Pull request | [#{event.number}]({event.url}) |\n"
        f"| Exact head | `{event.head_sha}` |\n"
        f"| Base | `{event.base_sha}` |\n"
        f"| Tree | `{tree_sha}` |\n"
        f"| Artifact | `{artifact_name}` |\n"
        f"| Workflow SHA | `{workflow_sha}` |\n"
        f"| Run | `{run_id}` attempt `{run_attempt}` |\n\n"
        "### Canonical notes\n\n"
        f"{notes_markdown}"
    )
    summary_path.write_text(summary, encoding="utf-8", newline="\n")
    prepared = {
        "artifact_name": artifact_name,
        "firmware_version": firmware_version,
        "metadata_path": str(metadata_path),
        "notes_path": str(notes_path),
        "summary_path": str(summary_path),
    }
    write_canonical_json(output_dir / "prepared.json", prepared)
    return prepared


def enforce_release_workflow_policy(
    decision: Any, workflow_changed_paths: list[str] | tuple[str, ...]
) -> None:
    if not isinstance(decision, dict) or not isinstance(decision.get("release"), bool):
        raise GateError("analysis decision must contain a boolean release value")
    paths = list(workflow_changed_paths)
    for path in paths:
        if not isinstance(path, str) or not path.startswith(WORKFLOW_DIRECTORY):
            raise GateError("workflow changed paths must be normalized .github/workflows paths")
    if decision["release"] and paths:
        joined = ", ".join(sorted(set(paths)))
        raise GateError(
            "release-bearing PRs cannot change .github/workflows/** because the minimal "
            "publisher token cannot tag commits that modify workflow files; split the "
            f"workflow changes into a separate no-release 'ci:' PR (changed: {joined})"
        )


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise GateError(f"cannot read JSON {path}: {exc}") from exc


def append_github_outputs(path: Path, prepared: dict[str, Any]) -> None:
    mapping = {
        "artifact-name": prepared["artifact_name"],
        "firmware-version": prepared["firmware_version"],
        "metadata-path": prepared["metadata_path"],
        "notes-path": prepared["notes_path"],
        "summary-path": prepared["summary_path"],
    }
    with path.open("a", encoding="utf-8", newline="\n") as stream:
        for key, value in mapping.items():
            if "\n" in value or "\r" in value:
                raise GateError(f"unsafe multiline GitHub output for {key}")
            stream.write(f"{key}={value}\n")


def token_from_environment() -> str:
    return os.environ.get("GITHUB_TOKEN", "")
