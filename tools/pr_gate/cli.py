#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.pr_gate.gate import (  # noqa: E402
    GateError,
    GitHubClient,
    GitRepository,
    append_github_outputs,
    collect_candidate_input,
    load_json,
    prepare_candidate_files,
    token_from_environment,
    write_canonical_json,
)


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description="Collect and prepare firmware PR-gate facts")
    commands = root.add_subparsers(dest="command", required=True)

    collect = commands.add_parser("collect", help="collect exact Git/GitHub candidate facts")
    collect.add_argument("--event", type=Path, required=True)
    collect.add_argument("--repository", required=True)
    collect.add_argument("--repo-root", type=Path, required=True)
    collect.add_argument("--runner-temp", type=Path, required=True)
    collect.add_argument("--api-url", default="https://api.github.com")
    collect.add_argument("--output", type=Path, required=True)

    prepare = commands.add_parser("prepare", help="prepare notes and manifest metadata")
    prepare.add_argument("--event", type=Path, required=True)
    prepare.add_argument("--analysis", type=Path, required=True)
    prepare.add_argument("--repository", required=True)
    prepare.add_argument("--repo-root", type=Path, required=True)
    prepare.add_argument("--workflow-sha", required=True)
    prepare.add_argument("--run-id", type=int, required=True)
    prepare.add_argument("--run-attempt", type=int, required=True)
    prepare.add_argument("--output-dir", type=Path, required=True)
    prepare.add_argument("--github-output", type=Path)
    return root


def main() -> int:
    args = parser().parse_args()
    try:
        event = load_json(args.event)
        if args.command == "collect":
            github = GitHubClient(
                repository=args.repository,
                token=token_from_environment(),
                api_url=args.api_url,
            )
            repo = GitRepository(args.repo_root, args.runner_temp)
            result = collect_candidate_input(
                event_raw=event,
                repository=args.repository,
                repo=repo,
                github=github,
            )
            write_canonical_json(args.output, result)
            return 0

        analysis = load_json(args.analysis)
        parsed_event = event.get("pull_request") if isinstance(event, dict) else None
        if not isinstance(parsed_event, dict):
            raise GateError("event must contain a pull_request object")
        head = parsed_event.get("head")
        base = parsed_event.get("base")
        if not isinstance(head, dict) or not isinstance(base, dict):
            raise GateError("pull_request must contain head and base objects")
        repo = GitRepository(args.repo_root, args.output_dir.parent)
        workflow_changed_paths = repo.workflow_changed_paths(
            base.get("sha"), head.get("sha")
        )
        prepared = prepare_candidate_files(
            event_raw=event,
            analysis=analysis,
            repository=args.repository,
            workflow_sha=args.workflow_sha,
            run_id=args.run_id,
            run_attempt=args.run_attempt,
            output_dir=args.output_dir,
            workflow_changed_paths=workflow_changed_paths,
        )
        if args.github_output:
            append_github_outputs(args.github_output, prepared)
        return 0
    except GateError as exc:
        print(
            json.dumps(
                {"schema_version": 1, "ok": False, "code": "PR_GATE_ERROR", "message": str(exc)},
                ensure_ascii=True,
                sort_keys=True,
                separators=(",", ":"),
            ),
            file=sys.stderr,
        )
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
