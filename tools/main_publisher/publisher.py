from __future__ import annotations

import hashlib
import io
import json
import mimetypes
import os
import re
import subprocess
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from tools.firmware_candidate.candidate import CandidateError, verify_candidate
from tools.pr_gate.gate import (
    GateError,
    GitHubClient,
    GitRepository,
    _ruleset_protects,
    collect_base_state_input,
)


GIT_SHA_RE = re.compile(r"^[0-9a-f]{40}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
SEMVER_TAG_RE = re.compile(r"^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
RECOVERY_TITLE_RE = re.compile(
    r"^revert\(release-([0-9a-f]{40})\): ([^\r\n]+)$"
)
PR_WORKFLOW_FILE = "firmware-pr-gate.yml"
PR_WORKFLOW_PATH = f".github/workflows/{PR_WORKFLOW_FILE}"
PUBLISHER_API_VERSION = "2026-03-10"
MAX_ARTIFACT_ARCHIVE_BYTES = 256 * 1024 * 1024
MAX_ARTIFACT_FILES = 16
MAX_ARTIFACT_EXPANDED_BYTES = 256 * 1024 * 1024
IMMUTABLE_PREFLIGHT_MESSAGE = (
    "Ticket 6 staged activation must enable immutable releases and exercise this "
    "endpoint with the actual Actions token before publishing"
)


class PublisherError(ValueError):
    """The trusted publisher cannot prove an exact, safe state transition."""


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req: Any, fp: Any, code: int, msg: str, headers: Any, newurl: str) -> None:
        return None


def _git_sha(value: Any, label: str) -> str:
    if not isinstance(value, str) or not GIT_SHA_RE.fullmatch(value):
        raise PublisherError(f"{label} must be exactly 40 lowercase hexadecimal characters")
    return value


def _positive_int(value: Any, label: str) -> int:
    if isinstance(value, str):
        if not re.fullmatch(r"[1-9][0-9]*", value):
            raise PublisherError(f"{label} must be a positive integer")
        value = int(value)
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise PublisherError(f"{label} must be a positive integer")
    return value


def _one_line(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value or "\r" in value or "\n" in value:
        raise PublisherError(f"{label} must be one non-empty line")
    return value


def _sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _canonical_pr_url(repository: str, number: int) -> str:
    return f"https://github.com/{repository}/pull/{number}"


def _run_json(command: list[str], *, cwd: Path, value: Any, label: str) -> dict[str, Any]:
    result = subprocess.run(
        command,
        cwd=cwd,
        input=json.dumps(value, ensure_ascii=True, separators=(",", ":")),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    try:
        parsed = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        detail = result.stderr.strip() or result.stdout.strip()
        raise PublisherError(f"{label} did not return structured JSON: {detail}") from exc
    if result.returncode != 0:
        message = parsed.get("message") if isinstance(parsed, dict) else None
        code = parsed.get("code") if isinstance(parsed, dict) else None
        raise PublisherError(f"{label} failed closed ({code or 'UNKNOWN'}): {message or parsed}")
    if not isinstance(parsed, dict):
        raise PublisherError(f"{label} output must be a JSON object")
    return parsed


def analyze_pull_request(repo_root: Path, pull_request: dict[str, Any]) -> dict[str, Any]:
    return _run_json(
        ["node", "tools/main_publisher/analyze_pr.mjs"],
        cwd=repo_root,
        value={
            "number": pull_request["number"],
            "title": pull_request["title"],
            "url": pull_request["url"],
        },
        label="Ticket 1 pull-request analysis",
    )


@dataclass(frozen=True)
class PublishRequest:
    event_name: str
    event_ref: str
    target_sha: str
    push_sha: str | None = None
    pr_workflow_run_id: int | str | None = None
    artifact_id: int | str | None = None

    def validated(self) -> "PublishRequest":
        target_sha = _git_sha(self.target_sha, "target_sha")
        if self.event_name == "push":
            if self.event_ref != "refs/heads/main":
                raise PublisherError("push publisher runs must originate from refs/heads/main")
            push_sha = _git_sha(self.push_sha, "push github.sha")
            if target_sha != push_sha:
                raise PublisherError("push target_sha must equal the exact github.sha")
            if self.pr_workflow_run_id not in {None, ""} or self.artifact_id not in {None, ""}:
                raise PublisherError("push runs must not override PR workflow run or artifact IDs")
            return PublishRequest("push", self.event_ref, target_sha, push_sha)
        if self.event_name != "workflow_dispatch":
            raise PublisherError("publisher event must be push or workflow_dispatch")
        run_id = _positive_int(self.pr_workflow_run_id, "pr_workflow_run_id")
        artifact_id = _positive_int(self.artifact_id, "artifact_id")
        return PublishRequest(
            "workflow_dispatch",
            self.event_ref,
            target_sha,
            None,
            run_id,
            artifact_id,
        )


@dataclass(frozen=True)
class MergeIdentity:
    sha: str
    parent_sha: str
    tree_sha: str
    subject: str


class PublisherRepository(GitRepository):
    def publish_target(self, target_sha: str, main_ref: str) -> MergeIdentity:
        target_sha = _git_sha(target_sha, "target_sha")
        self.assert_exact_head(target_sha)
        try:
            main_sha = self.git("rev-parse", "--verify", f"{main_ref}^{{commit}}").strip()
        except GateError as exc:
            raise PublisherError(f"main ref is unavailable: {main_ref}") from exc
        _git_sha(main_sha, "main ref SHA")
        first_parent = set(self.git("rev-list", "--first-parent", main_sha).splitlines())
        if target_sha not in first_parent:
            raise PublisherError(
                "target_sha is not on the current main first-parent chain; no branch-tip default is allowed"
            )
        parents = self.git("rev-list", "--parents", "-n", "1", target_sha).strip().split()
        if len(parents) != 2 or parents[0] != target_sha:
            raise PublisherError("target_sha must have exactly one parent (squash/linear main only)")
        subject = self.git("show", "-s", "--format=%s", target_sha).rstrip("\n")
        return MergeIdentity(
            target_sha,
            _git_sha(parents[1], "target parent SHA"),
            self.tree(target_sha),
            _one_line(subject, "target squash commit subject"),
        )

    def base_state_input(self, base_sha: str, github: Any) -> dict[str, Any]:
        return collect_base_state_input(base_sha=base_sha, repo=self, github=github)


class GitHubPublisherClient(GitHubClient):
    def __init__(self, *, repository: str, token: str, api_url: str):
        super().__init__(repository=repository, token=token, api_url=api_url)
        self._api_origin = urllib.parse.urlsplit(self.api_url)

    def _validated_url(self, path: str, *, allow_upload: bool = False) -> str:
        if not path.startswith("https://"):
            return f"{self.api_url}{path}"
        parsed = urllib.parse.urlsplit(path)
        same_origin = (
            parsed.scheme == self._api_origin.scheme
            and parsed.netloc == self._api_origin.netloc
        )
        github_upload = (
            allow_upload
            and self._api_origin.netloc == "api.github.com"
            and parsed.scheme == "https"
            and parsed.netloc == "uploads.github.com"
        )
        if not same_origin and not github_upload:
            raise PublisherError("refusing a GitHub API link outside the configured API/upload origins")
        return path

    def _request(
        self,
        method: str,
        path: str,
        *,
        json_body: Any | None = None,
        raw_body: bytes | None = None,
        accept: str = "application/vnd.github+json",
        content_type: str | None = None,
        optional_statuses: set[int] | frozenset[int] = frozenset(),
        allow_upload: bool = False,
    ) -> tuple[int, bytes, Any]:
        url = self._validated_url(path, allow_upload=allow_upload)
        if json_body is not None and raw_body is not None:
            raise PublisherError("internal API request cannot contain two bodies")
        body = raw_body
        if json_body is not None:
            body = json.dumps(json_body, ensure_ascii=True, separators=(",", ":")).encode()
            content_type = "application/json"
        headers = {
            "Accept": accept,
            "Authorization": f"Bearer {self.token}",
            "X-GitHub-Api-Version": PUBLISHER_API_VERSION,
            "User-Agent": "ambyte-firmware-main-publisher",
        }
        if content_type:
            headers["Content-Type"] = content_type
        request = urllib.request.Request(url, data=body, headers=headers, method=method)
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                return response.status, response.read(), response.headers
        except urllib.error.HTTPError as exc:
            if exc.code in optional_statuses:
                return exc.code, exc.read(), exc.headers
            raise PublisherError(f"GitHub API {method} {path} failed with HTTP {exc.code}") from exc
        except (urllib.error.URLError, TimeoutError) as exc:
            raise PublisherError(f"GitHub API {method} {path} failed: {exc}") from exc

    def _json_request(self, method: str, path: str, **kwargs: Any) -> tuple[int, Any]:
        status, raw, _ = self._request(method, path, **kwargs)
        if not raw:
            return status, None
        try:
            return status, json.loads(raw)
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise PublisherError(f"GitHub API {method} {path} returned malformed JSON") from exc

    def _download(self, path: str, *, accept: str, maximum: int) -> bytes:
        """Follow GitHub's signed redirect without forwarding the bearer token."""

        url = self._validated_url(path)
        request = urllib.request.Request(
            url,
            headers={
                "Accept": accept,
                "Authorization": f"Bearer {self.token}",
                "X-GitHub-Api-Version": PUBLISHER_API_VERSION,
                "User-Agent": "ambyte-firmware-main-publisher",
            },
            method="GET",
        )
        opener = urllib.request.build_opener(_NoRedirect)
        try:
            response = opener.open(request, timeout=60)
        except urllib.error.HTTPError as exc:
            if exc.code not in {301, 302, 303, 307, 308}:
                raise PublisherError(f"GitHub download {path} failed with HTTP {exc.code}") from exc
            location = exc.headers.get("Location")
            if not isinstance(location, str):
                raise PublisherError("GitHub download redirect omitted Location") from exc
            parsed = urllib.parse.urlsplit(location)
            if parsed.scheme != "https" or not parsed.netloc or parsed.username or parsed.password:
                raise PublisherError("GitHub download redirect is not a safe HTTPS URL") from exc
            request = urllib.request.Request(
                location,
                headers={"User-Agent": "ambyte-firmware-main-publisher"},
                method="GET",
            )
            try:
                response = urllib.request.urlopen(request, timeout=60)
            except (urllib.error.HTTPError, urllib.error.URLError, TimeoutError) as download_exc:
                raise PublisherError(f"signed GitHub download failed: {download_exc}") from download_exc
        except (urllib.error.URLError, TimeoutError) as exc:
            raise PublisherError(f"GitHub download {path} failed: {exc}") from exc
        with response:
            if response.status != 200:
                raise PublisherError(f"GitHub download {path} did not return HTTP 200")
            value = response.read(maximum + 1)
        if len(value) > maximum:
            raise PublisherError(f"GitHub download {path} exceeds the closed size limit")
        return value

    def get_json(self, path: str, *, optional: bool = False) -> Any:
        statuses = {404} if optional else set()
        status, value = self._json_request("GET", path, optional_statuses=statuses)
        return None if optional and status == 404 else value

    def immutable_release_capability(self) -> tuple[int, Any]:
        return self._json_request(
            "GET",
            f"/repos/{self.repository}/immutable-releases",
            optional_statuses={403, 404},
        )

    def exact_merged_pull_request(self, sha: str) -> dict[str, Any]:
        sha = _git_sha(sha, "merged commit SHA")
        value = self.get_json(f"/repos/{self.repository}/commits/{sha}/pulls?per_page=100")
        if not isinstance(value, list):
            raise PublisherError("associated pull-request response is not an array")
        matches = []
        for raw in value:
            if not isinstance(raw, dict):
                raise PublisherError("associated pull-request entry is not an object")
            base = raw.get("base") or {}
            head = raw.get("head") or {}
            base_repo = (base.get("repo") or {}).get("full_name")
            if (
                raw.get("merged_at")
                and raw.get("merge_commit_sha") == sha
                and base.get("ref") == "main"
                and isinstance(base_repo, str)
                and base_repo.casefold() == self.repository.casefold()
            ):
                matches.append(raw)
        if len(matches) != 1:
            raise PublisherError(
                f"target commit must have exactly one merged PR association on main; found {len(matches)}"
            )
        raw = matches[0]
        number = _positive_int(raw.get("number"), "pull_request.number")
        url = _canonical_pr_url(self.repository, number)
        if raw.get("html_url") not in {None, url}:
            raise PublisherError("associated pull-request URL is not canonical")
        return {
            "number": number,
            "url": url,
            "head_sha": _git_sha((raw.get("head") or {}).get("sha"), "pull_request.head.sha"),
        }

    def pull_request_merge_ref_sha(self, pr_number: int) -> str:
        pr_number = _positive_int(pr_number, "pull_request.number")
        ref = f"refs/pull/{pr_number}/merge"
        value = self.get_json(
            f"/repos/{self.repository}/git/ref/pull/{pr_number}/merge",
            optional=True,
        )
        if value is None:
            raise PublisherError(f"GitHub Git ref API did not resolve {ref}")
        if not isinstance(value, dict) or value.get("ref") != ref:
            raise PublisherError(f"GitHub Git ref API returned the wrong identity for {ref}")
        target = value.get("object")
        if not isinstance(target, dict) or target.get("type") != "commit":
            raise PublisherError(f"GitHub Git ref API did not return a commit for {ref}")
        return _git_sha(target.get("sha"), f"{ref} commit SHA")

    def _pages(self, path: str, key: str) -> list[dict[str, Any]]:
        results: list[dict[str, Any]] = []
        for page in range(1, 101):
            separator = "&" if "?" in path else "?"
            value = self.get_json(f"{path}{separator}per_page=100&page={page}")
            if not isinstance(value, dict) or not isinstance(value.get(key), list):
                raise PublisherError(f"paginated GitHub response is missing {key}")
            batch = value[key]
            if any(not isinstance(item, dict) for item in batch):
                raise PublisherError(f"paginated GitHub response {key} contains a non-object")
            results.extend(batch)
            if len(batch) < 100:
                return results
        raise PublisherError(f"refusing to truncate GitHub pagination for {key}")

    def workflow_runs(self, head_sha: str) -> list[dict[str, Any]]:
        query = urllib.parse.urlencode(
            {"event": "pull_request", "status": "success", "head_sha": head_sha}
        )
        return self._pages(
            f"/repos/{self.repository}/actions/workflows/{PR_WORKFLOW_FILE}/runs?{query}",
            "workflow_runs",
        )

    def run_artifacts(self, run_id: int) -> list[dict[str, Any]]:
        return self._pages(
            f"/repos/{self.repository}/actions/runs/{run_id}/artifacts", "artifacts"
        )

    def download_artifact(self, artifact_id: int) -> bytes:
        return self._download(
            f"/repos/{self.repository}/actions/artifacts/{artifact_id}/zip",
            accept="application/vnd.github+json",
            maximum=MAX_ARTIFACT_ARCHIVE_BYTES,
        )

    def releases_for_tag(self, tag: str) -> list[dict[str, Any]]:
        matches: list[dict[str, Any]] = []
        for page in range(1, 101):
            value = self.get_json(
                f"/repos/{self.repository}/releases?per_page=100&page={page}"
            )
            if not isinstance(value, list) or any(not isinstance(item, dict) for item in value):
                raise PublisherError("release listing response is not an object array")
            matches.extend(item for item in value if item.get("tag_name") == tag)
            if len(value) < 100:
                return matches
        raise PublisherError("refusing to truncate GitHub release pagination")

    def tag_target(self, tag: str) -> str | None:
        encoded = urllib.parse.quote(tag, safe="")
        value = self.get_json(f"/repos/{self.repository}/git/ref/tags/{encoded}", optional=True)
        if value is None:
            return None
        if not isinstance(value, dict) or (value.get("object") or {}).get("type") != "commit":
            raise PublisherError(f"tag {tag} is not a lightweight commit reference")
        return _git_sha((value.get("object") or {}).get("sha"), f"tag {tag} target")

    def create_tag(self, tag: str, target_sha: str) -> None:
        status, _ = self._json_request(
            "POST",
            f"/repos/{self.repository}/git/refs",
            json_body={"ref": f"refs/tags/{tag}", "sha": target_sha},
        )
        if status != 201:
            raise PublisherError(f"tag creation for {tag} did not return HTTP 201")

    def create_draft(self, *, tag: str, target_sha: str, body: str) -> dict[str, Any]:
        status, value = self._json_request(
            "POST",
            f"/repos/{self.repository}/releases",
            json_body={
                "tag_name": tag,
                "target_commitish": target_sha,
                "name": tag,
                "body": body,
                "draft": True,
                "prerelease": False,
            },
        )
        if status != 201 or not isinstance(value, dict):
            raise PublisherError("draft release creation did not return HTTP 201/object")
        return value

    def release_assets(self, release_id: int) -> list[dict[str, Any]]:
        results = []
        for page in range(1, 101):
            value = self.get_json(
                f"/repos/{self.repository}/releases/{release_id}/assets?per_page=100&page={page}"
            )
            if not isinstance(value, list) or any(not isinstance(item, dict) for item in value):
                raise PublisherError("release asset response is not an object array")
            results.extend(value)
            if len(value) < 100:
                return results
        raise PublisherError("refusing to truncate release asset pagination")

    def upload_release_asset(
        self, release: dict[str, Any], name: str, content_type: str, value: bytes
    ) -> dict[str, Any]:
        template = release.get("upload_url")
        if not isinstance(template, str):
            raise PublisherError("draft release is missing upload_url")
        base = template.split("{", 1)[0]
        url = f"{base}?{urllib.parse.urlencode({'name': name})}"
        status, raw, _ = self._request(
            "POST",
            url,
            raw_body=value,
            content_type=content_type,
            allow_upload=True,
        )
        try:
            parsed = json.loads(raw)
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise PublisherError("release asset upload returned malformed JSON") from exc
        if status != 201 or not isinstance(parsed, dict):
            raise PublisherError("release asset upload did not return HTTP 201/object")
        return parsed

    def download_release_asset(self, asset_id: int) -> bytes:
        return self._download(
            f"/repos/{self.repository}/releases/assets/{asset_id}",
            accept="application/octet-stream",
            maximum=MAX_ARTIFACT_EXPANDED_BYTES,
        )

    def publish_release(self, release_id: int) -> dict[str, Any]:
        status, value = self._json_request(
            "PATCH",
            f"/repos/{self.repository}/releases/{release_id}",
            json_body={"draft": False, "prerelease": False, "make_latest": "true"},
        )
        if status != 200 or not isinstance(value, dict):
            raise PublisherError("release publication did not return HTTP 200/object")
        return value

    def get_release(self, release_id: int) -> dict[str, Any]:
        value = self.get_json(f"/repos/{self.repository}/releases/{release_id}")
        if not isinstance(value, dict):
            raise PublisherError("release response is not an object")
        return value


def _frozen_pull_request(
    merge: MergeIdentity, pull_request: dict[str, Any]
) -> dict[str, Any]:
    number = _positive_int(pull_request.get("number"), "pull_request.number")
    subject = _one_line(merge.subject, "target squash commit subject")
    suffix = f" (#{number})"
    if not subject.endswith(suffix):
        raise PublisherError(
            "target squash commit subject must use the configured PR_TITLE form "
            f"'<validated title> (#{number})'"
        )
    frozen_title = subject[: -len(suffix)]
    if not frozen_title:
        raise PublisherError("target squash commit subject has an empty frozen PR title")
    return {**pull_request, "number": number, "title": frozen_title}


def _validated_run(raw: Any, *, head_sha: str, pr_number: int) -> dict[str, Any] | None:
    if not isinstance(raw, dict):
        raise PublisherError("workflow run entry is not an object")
    run_id = _positive_int(raw.get("id"), "workflow run id")
    attempt = _positive_int(raw.get("run_attempt"), "workflow run attempt")
    path = raw.get("path")
    if not isinstance(path, str) or path.split("@", 1)[0] != PR_WORKFLOW_PATH:
        raise PublisherError(f"workflow run {run_id} is not the exact PR gate workflow")
    if raw.get("event") != "pull_request" or raw.get("status") != "completed":
        raise PublisherError(f"workflow run {run_id} is not a completed pull_request run")
    if raw.get("conclusion") != "success" or raw.get("head_sha") != head_sha:
        raise PublisherError(f"workflow run {run_id} is not successful for the exact PR head")
    display_title = _one_line(raw.get("display_title"), f"workflow run {run_id} display_title")
    pulls = raw.get("pull_requests")
    if not isinstance(pulls, list):
        raise PublisherError(f"workflow run {run_id} pull_requests is not an array")
    numbers = []
    for item in pulls:
        if not isinstance(item, dict):
            raise PublisherError(f"workflow run {run_id} has malformed pull-request identity")
        number = item.get("number")
        if isinstance(number, bool) or not isinstance(number, int) or number <= 0:
            raise PublisherError(f"workflow run {run_id} has malformed pull-request number")
        numbers.append(number)
    # GitHub may return an empty pull_requests array for fork-origin runs. The
    # exact workflow/head filter plus the run-bound manifest still proves the
    # PR identity. When GitHub does provide identities, they must be unambiguous.
    if numbers and pr_number not in numbers:
        return None
    if numbers and set(numbers) != {pr_number}:
        raise PublisherError(f"workflow run {run_id} is associated with multiple PR identities")
    return {
        **raw,
        "id": run_id,
        "run_attempt": attempt,
        "display_title": display_title,
    }


def _artifact_record(raw: Any, *, run_id: int, head_sha: str) -> dict[str, Any]:
    if not isinstance(raw, dict):
        raise PublisherError("artifact entry is not an object")
    artifact_id = _positive_int(raw.get("id"), "artifact id")
    name = _one_line(raw.get("name"), "artifact name")
    if not isinstance(raw.get("expired"), bool):
        raise PublisherError(f"artifact {artifact_id} expired flag is not boolean")
    workflow_run = raw.get("workflow_run")
    if workflow_run is not None:
        if not isinstance(workflow_run, dict):
            raise PublisherError(f"artifact {artifact_id} workflow_run is malformed")
        if workflow_run.get("id") != run_id or workflow_run.get("head_sha") != head_sha:
            raise PublisherError(f"artifact {artifact_id} is not bound to the selected run/head")
    digest = raw.get("digest")
    if not isinstance(digest, str) or not digest.startswith("sha256:") or not SHA256_RE.fullmatch(
        digest.removeprefix("sha256:")
    ):
        raise PublisherError(f"artifact {artifact_id} lacks an exact SHA-256 transport digest")
    return {**raw, "id": artifact_id, "name": name}


def _extract_artifact(archive: bytes, destination: Path) -> None:
    if not archive or len(archive) > MAX_ARTIFACT_ARCHIVE_BYTES:
        raise PublisherError("artifact archive size is outside the supported closed limit")
    destination.mkdir(parents=True, exist_ok=False)
    try:
        with zipfile.ZipFile(io.BytesIO(archive), "r") as bundle:
            infos = bundle.infolist()
            if not infos or len(infos) > MAX_ARTIFACT_FILES:
                raise PublisherError("artifact archive file count is outside the supported limit")
            if len({info.filename for info in infos}) != len(infos):
                raise PublisherError("artifact archive contains duplicate member names")
            expanded = 0
            for info in infos:
                name = info.filename
                if (
                    info.is_dir()
                    or "/" in name
                    or "\\" in name
                    or name in {"", ".", ".."}
                    or info.flag_bits & 0x1
                    or ((info.external_attr >> 16) & 0o170000) == 0o120000
                ):
                    raise PublisherError("artifact archive contains an unsafe/non-flat member")
                expanded += info.file_size
                if expanded > MAX_ARTIFACT_EXPANDED_BYTES:
                    raise PublisherError("artifact archive expands beyond the supported limit")
                value = bundle.read(info)
                if len(value) != info.file_size:
                    raise PublisherError("artifact archive member size changed while reading")
                (destination / name).write_bytes(value)
    except zipfile.BadZipFile as exc:
        raise PublisherError("artifact download is not a valid ZIP archive") from exc


@dataclass(frozen=True)
class SelectedCandidate:
    run: dict[str, Any]
    artifact: dict[str, Any]
    directory: Path
    manifest: dict[str, Any]


class FirmwarePublisher:
    def __init__(
        self,
        *,
        repository: str,
        repo_root: Path,
        runner_temp: Path,
        repo: Any,
        github: Any,
        main_ref: str = "refs/remotes/origin/main",
    ):
        self.repository = repository
        self.repo_root = repo_root.resolve()
        self.runner_temp = runner_temp.resolve()
        self.runner_temp.mkdir(parents=True, exist_ok=True)
        self.repo = repo
        self.github = github
        self.main_ref = main_ref

    def run(self, request: PublishRequest) -> dict[str, Any]:
        request = request.validated()
        merge = self.repo.publish_target(request.target_sha, self.main_ref)
        pull_request = _frozen_pull_request(
            merge, self.github.exact_merged_pull_request(merge.sha)
        )
        title_analysis = analyze_pull_request(self.repo_root, pull_request)
        if title_analysis.get("decision", {}).get("release") is False:
            if request.event_name == "workflow_dispatch":
                with tempfile.TemporaryDirectory(
                    prefix="ambyte-main-publisher-", dir=self.runner_temp
                ) as directory:
                    self._select_candidate(
                        request,
                        merge,
                        pull_request,
                        title_analysis,
                        Path(directory),
                    )
            return {
                "schema_version": 1,
                "ok": True,
                "outcome": "no-release",
                "target_sha": merge.sha,
                "pull_request_number": pull_request["number"],
                "decision": title_analysis["decision"],
                "mutations": [],
            }

        workflow_changes = self.repo.workflow_changed_paths(merge.parent_sha, merge.sha)
        if workflow_changes:
            raise PublisherError(
                "release-bearing main commit changes .github/workflows/**; this bypassed the "
                "PR gate and cannot be published with the minimal token. Split workflow changes "
                f"into a separate no-release 'ci:' PR (changed: {', '.join(workflow_changes)})"
            )

        with tempfile.TemporaryDirectory(
            prefix="ambyte-main-publisher-", dir=self.runner_temp
        ) as directory:
            selected = self._select_candidate(
                request, merge, pull_request, title_analysis, Path(directory)
            )
            first_check = self._rederive(selected.manifest, merge, pull_request)
            decision = selected.manifest["analysis"]["decision"]
            if decision["kind"] == "recovery":
                return self._publish_recovery(
                    selected, merge, pull_request, first_check
                )
            if decision.get("release") is not True:
                raise PublisherError(
                    "candidate is neither a normal release nor a verified recovery"
                )
            return self._publish_release(selected, merge, pull_request, first_check)

    def _select_candidate(
        self,
        request: PublishRequest,
        merge: MergeIdentity,
        pull_request: dict[str, Any],
        title_analysis: dict[str, Any],
        temp: Path,
    ) -> SelectedCandidate:
        expected_name = (
            f"firmware-candidate-pr-{pull_request['number']}-{pull_request['head_sha'][:12]}"
        )
        expected_workflow_sha = self.github.pull_request_merge_ref_sha(
            pull_request["number"]
        )
        valid_runs = []
        for raw in self.github.workflow_runs(pull_request["head_sha"]):
            run = _validated_run(
                raw,
                head_sha=pull_request["head_sha"],
                pr_number=pull_request["number"],
            )
            if run is not None:
                valid_runs.append(run)
        if request.event_name == "workflow_dispatch":
            matches = [
                run for run in valid_runs if run["id"] == request.pr_workflow_run_id
            ]
            if len(matches) != 1:
                raise PublisherError(
                    "manual pr_workflow_run_id is not one successful exact-head run of the PR gate"
                )
            run = matches[0]
            if run["display_title"] != pull_request["title"]:
                raise PublisherError(
                    "selected workflow run display_title does not equal the frozen squash title"
                )
            artifacts = [
                _artifact_record(raw, run_id=run["id"], head_sha=pull_request["head_sha"])
                for raw in self.github.run_artifacts(run["id"])
            ]
            matches = [item for item in artifacts if item["id"] == request.artifact_id]
            if len(matches) != 1:
                raise PublisherError("manual artifact_id does not belong to the supplied PR run")
            artifact = matches[0]
            if artifact["name"] != expected_name:
                raise PublisherError("manual artifact_id does not have the deterministic candidate name")
            if artifact["expired"]:
                raise PublisherError(
                    "the exact candidate artifact expired; never rebuild on main—use verified revert, "
                    "release-aborted marker, and a fresh re-land"
                )
            return self._inspect_candidate(
                run,
                artifact,
                merge,
                pull_request,
                title_analysis,
                expected_workflow_sha,
                temp / "manual",
            )

        exact: list[SelectedCandidate] = []
        diagnostics = [
            f"run {run['id']}: display_title does not equal the frozen squash title"
            for run in valid_runs
            if run["display_title"] != pull_request["title"]
        ]
        runs = [
            run
            for run in valid_runs
            if run["display_title"] == pull_request["title"]
        ]
        for run in runs:
            for raw in self.github.run_artifacts(run["id"]):
                artifact = _artifact_record(
                    raw, run_id=run["id"], head_sha=pull_request["head_sha"]
                )
                if artifact["name"] != expected_name:
                    continue
                if artifact["expired"]:
                    diagnostics.append(f"run {run['id']} artifact {artifact['id']}: expired")
                    continue
                try:
                    candidate = self._inspect_candidate(
                        run,
                        artifact,
                        merge,
                        pull_request,
                        title_analysis,
                        expected_workflow_sha,
                        temp / f"artifact-{artifact['id']}",
                    )
                except (PublisherError, CandidateError, OSError) as exc:
                    diagnostics.append(
                        f"run {run['id']} artifact {artifact['id']}: {exc}"
                    )
                    continue
                exact.append(candidate)
        if len(exact) != 1:
            detail = "; ".join(diagnostics) if diagnostics else "no candidate artifacts found"
            if not exact:
                raise PublisherError(
                    "no exact run-bound candidate matched; never rebuild on main—retry the original "
                    f"push run or use verified revert/re-land ({detail})"
                )
            identities = ", ".join(
                f"run {item.run['id']} artifact {item.artifact['id']}" for item in exact
            )
            raise PublisherError(
                "more than one exact run-bound candidate matched; fail closed and use manual "
                f"SHA/run/artifact selection after investigation ({identities})"
            )
        return exact[0]

    def _inspect_candidate(
        self,
        run: dict[str, Any],
        artifact: dict[str, Any],
        merge: MergeIdentity,
        pull_request: dict[str, Any],
        title_analysis: dict[str, Any],
        expected_workflow_sha: str,
        destination: Path,
    ) -> SelectedCandidate:
        archive = self.github.download_artifact(artifact["id"])
        expected_transport = artifact["digest"].removeprefix("sha256:")
        if _sha256_bytes(archive) != expected_transport:
            raise PublisherError("downloaded artifact archive digest does not match GitHub metadata")
        _extract_artifact(archive, destination)
        manifest = verify_candidate(destination)
        self._verify_candidate_identity(
            manifest,
            run,
            artifact,
            merge,
            pull_request,
            title_analysis,
            expected_workflow_sha,
        )
        return SelectedCandidate(run, artifact, destination, manifest)

    def _verify_candidate_identity(
        self,
        manifest: dict[str, Any],
        run: dict[str, Any],
        artifact: dict[str, Any],
        merge: MergeIdentity,
        pull_request: dict[str, Any],
        title_analysis: dict[str, Any],
        expected_workflow_sha: str,
    ) -> None:
        expected = {
            "repository": self.repository,
            "artifact_name": artifact["name"],
            "pull_request.number": pull_request["number"],
            "pull_request.title": pull_request["title"],
            "source.head_sha": pull_request["head_sha"],
            "source.base_sha": merge.parent_sha,
            "source.tree_sha": merge.tree_sha,
            "workflow.run_id": run["id"],
            "workflow.run_attempt": run["run_attempt"],
            "workflow.sha": expected_workflow_sha,
            "analysis.notes_sha256": title_analysis["notes"]["sha256"],
        }
        actual = {
            "repository": manifest["repository"],
            "artifact_name": manifest["artifact_name"],
            "pull_request.number": manifest["pull_request"]["number"],
            "pull_request.title": manifest["pull_request"]["title"],
            "source.head_sha": manifest["source"]["head_sha"],
            "source.base_sha": manifest["source"]["base_sha"],
            "source.tree_sha": manifest["source"]["tree_sha"],
            "workflow.run_id": manifest["workflow"]["run_id"],
            "workflow.run_attempt": manifest["workflow"]["run_attempt"],
            "workflow.sha": manifest["workflow"]["sha"],
            "analysis.notes_sha256": manifest["analysis"]["notes_sha256"],
        }
        mismatches = [
            f"{key}: expected {value!r}, got {actual[key]!r}"
            for key, value in expected.items()
            if actual[key] != value
        ]
        decision = manifest["analysis"]["decision"]
        if decision["kind"] == "recovery":
            match = RECOVERY_TITLE_RE.fullmatch(manifest["pull_request"]["title"])
            recovery_sha = manifest["analysis"]["recovery_of_sha"]
            if not match or recovery_sha != match.group(1):
                mismatches.append("verified recovery SHA does not match the exact bound title")
            if decision != {
                "release": False,
                "bump": None,
                "kind": "recovery",
                "reason": "verified-recovery-revert",
            }:
                mismatches.append("candidate recovery decision is not the exact verified form")
        else:
            if decision != title_analysis["decision"]:
                mismatches.append("candidate release decision differs from Ticket 1 title analysis")
            if manifest["analysis"]["recovery_of_sha"] is not None:
                mismatches.append("normal release unexpectedly carries recovery_of_sha")
        if mismatches:
            raise PublisherError("candidate identity mismatch: " + "; ".join(mismatches))

    def _predecessor_input(
        self,
        manifest: dict[str, Any],
        merge: MergeIdentity,
        pull_request: dict[str, Any],
    ) -> dict[str, Any]:
        analysis = manifest["analysis"]
        latest = analysis["latest_release_tag"]
        if not SEMVER_TAG_RE.fullmatch(latest):
            raise PublisherError("candidate latest_release_tag is not exact vX.Y.Z")
        if analysis["decision"]["release"]:
            release_version = analysis["release_version"]
            expected_version = {
                "previous": latest[1:],
                "next": release_version,
                "tag": f"v{release_version}",
            }
            release_tag = f"v{release_version}"
        else:
            expected_version = {"previous": latest[1:], "next": None, "tag": None}
            release_tag = None
        return {
            "schema_version": 1,
            "candidate_identity": {
                "pull_request_number": pull_request["number"],
                "pull_request_url": pull_request["url"],
                "title": manifest["pull_request"]["title"],
                "head_sha": manifest["source"]["head_sha"],
                "base_sha": manifest["source"]["base_sha"],
                "tree_sha": manifest["source"]["tree_sha"],
                "latest_release_tag": latest,
                "release_tag": release_tag,
                "notes_sha256": analysis["notes_sha256"],
                "recovery_of_sha": analysis["recovery_of_sha"],
            },
            "expected_decision": analysis["decision"],
            "expected_version": expected_version,
            "merged_commit": {
                "sha": merge.sha,
                "parent_sha": merge.parent_sha,
                "tree_sha": merge.tree_sha,
                "pull_request": {
                    "number": pull_request["number"],
                    "title": pull_request["title"],
                    "url": pull_request["url"],
                },
            },
            "predecessor": self.repo.base_state_input(merge.parent_sha, self.github),
        }

    def _rederive(
        self,
        manifest: dict[str, Any],
        merge: MergeIdentity,
        pull_request: dict[str, Any],
    ) -> dict[str, Any]:
        return _run_json(
            ["node", "tools/release-analysis/src/cli.mjs", "check-predecessor"],
            cwd=self.repo_root,
            value=self._predecessor_input(manifest, merge, pull_request),
            label="Ticket 1 predecessor/tag/decision/version/notes recheck",
        )

    def _preflight_immutable(self) -> None:
        status, value = self.github.immutable_release_capability()
        if status in {403, 404}:
            raise PublisherError(
                f"immutable-release preflight returned HTTP {status}; {IMMUTABLE_PREFLIGHT_MESSAGE}"
            )
        if status != 200:
            raise PublisherError(
                f"immutable-release preflight returned unsupported HTTP {status}; "
                f"{IMMUTABLE_PREFLIGHT_MESSAGE}"
            )
        if not isinstance(value, dict) or set(value) != {"enabled", "enforced_by_owner"}:
            raise PublisherError(
                "immutable-release preflight returned a malformed/unsupported response; "
                + IMMUTABLE_PREFLIGHT_MESSAGE
            )
        if not isinstance(value["enabled"], bool) or not isinstance(
            value["enforced_by_owner"], bool
        ):
            raise PublisherError(
                "immutable-release preflight response fields are not boolean; "
                + IMMUTABLE_PREFLIGHT_MESSAGE
            )
        if value["enabled"] is not True:
            raise PublisherError(
                "immutable releases are disabled; " + IMMUTABLE_PREFLIGHT_MESSAGE
            )

    def _release_files(self, selected: SelectedCandidate) -> dict[str, Path]:
        files = {path.name: path for path in selected.directory.iterdir() if path.is_file()}
        expected = {
            "firmware.bin",
            "release-notes.md",
            "candidate-manifest.json",
            "candidate-SHA256SUMS",
            next(record["path"] for record in selected.manifest["files"] if record["path"].endswith(".zip")),
        }
        if set(files) != expected:
            raise PublisherError("verified candidate does not expose the exact release asset set")
        return files

    def _matching_release(self, tag: str) -> dict[str, Any] | None:
        releases = self.github.releases_for_tag(tag)
        if len(releases) > 1:
            raise PublisherError(f"more than one GitHub release uses {tag}")
        return releases[0] if releases else None

    def _validate_release_identity(
        self,
        release: dict[str, Any],
        *,
        tag: str,
        target_sha: str,
        body: str,
    ) -> int:
        release_id = _positive_int(release.get("id"), "release id")
        expected = {
            "tag_name": tag,
            "target_commitish": target_sha,
            "name": tag,
            "body": body,
            "prerelease": False,
        }
        mismatches = [
            key for key, value in expected.items() if release.get(key) != value
        ]
        if mismatches:
            raise PublisherError(
                f"release {tag} conflicts in fields: {', '.join(mismatches)}"
            )
        if not isinstance(release.get("draft"), bool):
            raise PublisherError(f"release {tag} has a malformed draft state")
        return release_id

    def _verify_assets(
        self,
        release_id: int,
        expected_files: dict[str, Path],
        *,
        complete: bool,
    ) -> set[str]:
        assets = self.github.release_assets(release_id)
        names: set[str] = set()
        for asset in assets:
            name = _one_line(asset.get("name"), "release asset name")
            if name in names:
                raise PublisherError(f"release contains duplicate asset name {name}")
            names.add(name)
            if name not in expected_files:
                raise PublisherError(f"release contains unexpected asset {name}; no clobber allowed")
            asset_id = _positive_int(asset.get("id"), f"release asset {name} id")
            expected_path = expected_files[name]
            expected_size = expected_path.stat().st_size
            expected_digest = _sha256_file(expected_path)
            if asset.get("state") != "uploaded" or asset.get("size") != expected_size:
                raise PublisherError(f"release asset {name} state/size conflicts")
            if asset.get("digest") != f"sha256:{expected_digest}":
                raise PublisherError(f"release asset {name} GitHub digest conflicts")
            downloaded = self.github.download_release_asset(asset_id)
            if len(downloaded) != expected_size or _sha256_bytes(downloaded) != expected_digest:
                raise PublisherError(f"downloaded release asset {name} digest conflicts")
        if complete and names != set(expected_files):
            missing = ", ".join(sorted(set(expected_files) - names))
            raise PublisherError(f"release is missing exact assets: {missing}")
        return names

    def _publish_release(
        self,
        selected: SelectedCandidate,
        merge: MergeIdentity,
        pull_request: dict[str, Any],
        first_check: dict[str, Any],
    ) -> dict[str, Any]:
        tag = first_check.get("release_tag")
        if not isinstance(tag, str) or tag != f"v{selected.manifest['analysis']['release_version']}":
            raise PublisherError("Ticket 1 recheck did not return the candidate's exact release tag")
        notes = (selected.directory / "release-notes.md").read_text(encoding="utf-8")
        files = self._release_files(selected)
        release = self._matching_release(tag)
        tag_target = self.github.tag_target(tag)
        if release is None:
            if tag_target is not None:
                raise PublisherError(f"pre-existing tag {tag} has no matching release")
            existing_names: set[str] = set()
        else:
            release_id = self._validate_release_identity(
                release, tag=tag, target_sha=merge.sha, body=notes
            )
            if release["draft"] is False:
                if release.get("immutable") is not True:
                    raise PublisherError(f"published release {tag} is not immutable")
                if tag_target != merge.sha:
                    raise PublisherError(f"published release tag {tag} targets the wrong commit")
                self._verify_assets(release_id, files, complete=True)
                return {
                    "schema_version": 1,
                    "ok": True,
                    "outcome": "already-published",
                    "target_sha": merge.sha,
                    "tag": tag,
                    "artifact_id": selected.artifact["id"],
                    "mutations": [],
                }
            if release.get("immutable") is True:
                raise PublisherError(f"draft release {tag} unexpectedly reports immutable")
            if tag_target not in {None, merge.sha}:
                raise PublisherError(f"draft release tag {tag} targets the wrong commit")
            existing_names = self._verify_assets(release_id, files, complete=False)

        self._preflight_immutable()
        if self.github.tag_target(tag) != tag_target:
            raise PublisherError(f"tag {tag} changed during publisher preflight")
        current = self._matching_release(tag)
        created = False
        if release is None:
            if current is not None:
                raise PublisherError(f"release {tag} appeared during publisher preflight")
            # This is deliberately the final operation before draft creation.
            self._rederive(selected.manifest, merge, pull_request)
            release = self.github.create_draft(tag=tag, target_sha=merge.sha, body=notes)
            created = True
            release_id = self._validate_release_identity(
                release, tag=tag, target_sha=merge.sha, body=notes
            )
            if release.get("draft") is not True:
                raise PublisherError("new release was not created as a draft")
            if self.github.tag_target(tag) not in {None, merge.sha}:
                raise PublisherError("new draft created or exposed a conflicting tag target")
            existing_names = set()
        else:
            if current is None or current.get("id") != release.get("id"):
                raise PublisherError(f"draft release {tag} changed during publisher preflight")
            self._validate_release_identity(
                current, tag=tag, target_sha=merge.sha, body=notes
            )
            self._verify_assets(release_id, files, complete=False)
            # This is deliberately the final operation before the first
            # missing-asset upload in a resumed draft.
            self._rederive(selected.manifest, merge, pull_request)

        mutations = ["create-draft"] if created else []
        for name in sorted(set(files) - existing_names):
            value = files[name].read_bytes()
            content_type = mimetypes.guess_type(name)[0] or "application/octet-stream"
            uploaded = self.github.upload_release_asset(release, name, content_type, value)
            if uploaded.get("name") != name or uploaded.get("size") != len(value):
                raise PublisherError(f"uploaded release asset {name} identity/size conflicts")
            if uploaded.get("digest") != f"sha256:{_sha256_bytes(value)}":
                raise PublisherError(f"uploaded release asset {name} digest conflicts")
            downloaded = self.github.download_release_asset(
                _positive_int(uploaded.get("id"), f"uploaded asset {name} id")
            )
            if downloaded != value:
                raise PublisherError(f"uploaded/downloaded release asset {name} bytes differ")
            mutations.append(f"upload:{name}")

        self._verify_assets(release_id, files, complete=True)
        self._preflight_immutable()
        if self.github.tag_target(tag) not in {None, merge.sha}:
            raise PublisherError(f"tag {tag} changed before publication")
        current = self._matching_release(tag)
        if current is None or current.get("id") != release_id or current.get("draft") is not True:
            raise PublisherError(f"draft release {tag} changed before publication")
        self._validate_release_identity(
            current, tag=tag, target_sha=merge.sha, body=notes
        )
        self._verify_assets(release_id, files, complete=True)
        # Assets and release state are now exact; this is the final operation
        # immediately before publication.
        self._rederive(selected.manifest, merge, pull_request)
        published = self.github.publish_release(release_id)
        self._validate_release_identity(
            published, tag=tag, target_sha=merge.sha, body=notes
        )
        final = self.github.get_release(release_id)
        if final.get("draft") is not False or final.get("immutable") is not True:
            raise PublisherError(
                "GitHub did not return an immutable published release after a successful preflight"
            )
        if self.github.tag_target(tag) != merge.sha:
            raise PublisherError(f"published tag {tag} does not target the exact merged SHA")
        self._verify_assets(release_id, files, complete=True)
        mutations.append("publish")
        return {
            "schema_version": 1,
            "ok": True,
            "outcome": "published",
            "target_sha": merge.sha,
            "tag": tag,
            "artifact_id": selected.artifact["id"],
            "mutations": mutations,
        }

    def _publish_recovery(
        self,
        selected: SelectedCandidate,
        merge: MergeIdentity,
        pull_request: dict[str, Any],
        first_check: dict[str, Any],
    ) -> dict[str, Any]:
        failed_sha = _git_sha(
            selected.manifest["analysis"]["recovery_of_sha"], "recovery failed SHA"
        )
        if first_check.get("decision") != selected.manifest["analysis"]["decision"]:
            raise PublisherError("Ticket 1 did not rederive the exact verified recovery decision")
        if first_check.get("release_tag") is not None:
            raise PublisherError("verified recovery unexpectedly derived a release tag")
        if self.repo.expected_revert_tree(merge.parent_sha, failed_sha) != merge.tree_sha:
            raise PublisherError("recovery merge tree is not the exact isolated git revert proof")
        failed_parents = self.repo.git("rev-list", "--parents", "-n", "1", failed_sha).split()
        if len(failed_parents) != 2:
            raise PublisherError("recovery target is not a linear first-parent commit")
        unsupported = self.repo.workflow_changed_paths(failed_parents[1], failed_sha)
        if unsupported:
            raise PublisherError(
                "recovery target changed workflow files and was never a supported release-bearing merge"
            )
        tag = f"release-aborted/{failed_sha}"
        rulesets = self.github.active_tag_rulesets()
        if not any(_ruleset_protects(item, tag) for item in rulesets):
            raise PublisherError(
                f"active protective tag ruleset does not cover {tag}; Ticket 6 activation is required"
            )
        existing = self.github.tag_target(tag)
        if existing is not None:
            if existing != merge.sha:
                raise PublisherError(f"recovery marker {tag} targets a conflicting commit")
            return {
                "schema_version": 1,
                "ok": True,
                "outcome": "recovery-already-marked",
                "target_sha": merge.sha,
                "tag": tag,
                "artifact_id": selected.artifact["id"],
                "mutations": [],
            }
        self._preflight_immutable()
        rulesets = self.github.active_tag_rulesets()
        if not any(_ruleset_protects(item, tag) for item in rulesets):
            raise PublisherError("protective recovery tag ruleset changed before marker creation")
        if self.github.tag_target(tag) is not None:
            raise PublisherError("recovery marker appeared during publisher preflight")
        # Protective rules and tag absence are exact; rederive immediately
        # before the marker mutation.
        self._rederive(selected.manifest, merge, pull_request)
        self.github.create_tag(tag, merge.sha)
        if self.github.tag_target(tag) != merge.sha:
            raise PublisherError("created recovery marker does not target the recovery merge")
        return {
            "schema_version": 1,
            "ok": True,
            "outcome": "recovery-marked",
            "target_sha": merge.sha,
            "tag": tag,
            "artifact_id": selected.artifact["id"],
            "mutations": ["create-recovery-marker"],
        }


def token_from_environment() -> str:
    token = os.environ.get("GITHUB_TOKEN", "")
    if not token:
        raise PublisherError("GITHUB_TOKEN is required")
    return token
