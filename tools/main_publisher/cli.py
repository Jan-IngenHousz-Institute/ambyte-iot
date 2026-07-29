#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.firmware_candidate.candidate import CandidateError  # noqa: E402
from tools.main_publisher.publisher import (  # noqa: E402
    FirmwarePublisher,
    GitHubPublisherClient,
    PublishRequest,
    PublisherError,
    PublisherRepository,
    token_from_environment,
)
from tools.pr_gate.gate import GateError  # noqa: E402


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(
        description="Publish an exact PR-built Ambyte firmware candidate"
    )
    root.add_argument("--repository", required=True)
    root.add_argument("--repo-root", type=Path, required=True)
    root.add_argument("--runner-temp", type=Path, required=True)
    root.add_argument("--api-url", default="https://api.github.com")
    root.add_argument("--main-ref", default="refs/remotes/origin/main")
    root.add_argument("--event-name", required=True)
    root.add_argument("--event-ref", required=True)
    root.add_argument("--target-sha", required=True)
    root.add_argument("--push-sha")
    root.add_argument("--pr-workflow-run-id")
    root.add_argument("--artifact-id")
    root.add_argument("--summary", type=Path)
    return root


def append_summary(path: Path, result: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8", newline="\n") as stream:
        stream.write("## Firmware main publisher\n\n")
        stream.write(f"- Outcome: `{result['outcome']}`\n")
        stream.write(f"- Exact target: `{result['target_sha']}`\n")
        if result.get("tag"):
            stream.write(f"- Tag: `{result['tag']}`\n")
        if result.get("artifact_id"):
            stream.write(f"- GitHub artifact ID: `{result['artifact_id']}`\n")
        stream.write(
            "- Mutations: "
            + (", ".join(f"`{item}`" for item in result["mutations"]) or "none")
            + "\n"
        )


def main() -> int:
    args = parser().parse_args()
    try:
        github = GitHubPublisherClient(
            repository=args.repository,
            token=token_from_environment(),
            api_url=args.api_url,
        )
        repo = PublisherRepository(args.repo_root, args.runner_temp)
        publisher = FirmwarePublisher(
            repository=args.repository,
            repo_root=args.repo_root,
            runner_temp=args.runner_temp,
            repo=repo,
            github=github,
            main_ref=args.main_ref,
        )
        result = publisher.run(
            PublishRequest(
                event_name=args.event_name,
                event_ref=args.event_ref,
                target_sha=args.target_sha,
                push_sha=args.push_sha,
                pr_workflow_run_id=args.pr_workflow_run_id,
                artifact_id=args.artifact_id,
            )
        )
        if args.summary:
            append_summary(args.summary, result)
        print(json.dumps(result, ensure_ascii=True, sort_keys=True, separators=(",", ":")))
        return 0
    except (PublisherError, CandidateError, GateError, OSError) as exc:
        error = {
            "schema_version": 1,
            "ok": False,
            "code": "MAIN_PUBLISHER_ERROR",
            "message": str(exc),
        }
        print(json.dumps(error, ensure_ascii=True, sort_keys=True, separators=(",", ":")), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
