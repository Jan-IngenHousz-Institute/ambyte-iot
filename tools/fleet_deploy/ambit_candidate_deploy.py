#!/usr/bin/env python3
"""DEV-only, single-gateway AMBIT candidate canary deployment.

This deliberately is not a release deployment mode.  ``prepare`` downloads one
operator-specified public HTTPS image before AWS credentials are configured and
pins its exact bytes in a supply-chain proof.  ``deploy`` re-verifies that local
image, targets the one proof-bound Ambyte gateway, and reuses the normal AMBIT
correlation/tracking primitives without exposing discovery, cohort, downgrade,
or force-reflash controls.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import ipaddress
import json
import os
import re
import socket
import sys
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any, Callable

try:  # Package import in tests; sibling import for direct CLI invocation.
    from . import ambit_deploy
    from . import fleet_deploy as fleet
except ImportError:  # pragma: no cover
    import ambit_deploy
    import fleet_deploy as fleet


KIND = "ambit_candidate"
ENVIRONMENT = "dev"
PROTECTED_ENVIRONMENT = "fleet-deploy-dev"
REQUIRED_GIT_REF = "refs/heads/ops/ambit-v1.1.4-candidate-canary"
MAX_IMAGE_BYTES = 16 * 1024 * 1024
CANDIDATE_VERSION_RE = re.compile(
    r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$"
)
IMAGE_NAME = "candidate.bin"
PROOF_SCHEMA = 1


class CandidateError(ValueError):
    """A candidate request or its prepared bytes violate the canary contract."""


def _utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def _write_json(path: str | Path | None, value: dict[str, Any]) -> None:
    if path is None:
        return
    destination = Path(path)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + ".tmp")
    temporary.write_text(
        json.dumps(ambit_deploy._redact(value), indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    os.replace(temporary, destination)


def _require_environment(value: str) -> None:
    if value != ENVIRONMENT:
        raise CandidateError(
            f"candidate canary environment must be exactly {ENVIRONMENT!r} "
            f"(protected by {PROTECTED_ENVIRONMENT})"
        )


def _require_git_ref(value: str) -> None:
    if value != REQUIRED_GIT_REF:
        raise CandidateError(
            f"candidate canary git ref must be exactly {REQUIRED_GIT_REF!r}"
        )


def validate_device_id(value: str) -> str:
    if fleet.CLIENT_ID_RE.fullmatch(value) is None:
        raise CandidateError(
            "--device-id must be exactly one canonical AMBYTE_AA:BB:CC:DD:EE:FF ID"
        )
    return value


def validate_candidate_version(value: str) -> str:
    if CANDIDATE_VERSION_RE.fullmatch(value) is None:
        raise CandidateError("candidate version must be an exact numeric X.Y.Z")
    if fleet.parse_version(value) is None:  # defensive parity with fleet comparison
        raise CandidateError("candidate version is not parseable")
    return value


def validate_candidate_url(value: str) -> str:
    if "\\" in value or any(ord(character) <= 32 or ord(character) == 127 for character in value):
        raise CandidateError("candidate URL must not contain whitespace, controls, or backslashes")
    try:
        parsed = urllib.parse.urlsplit(value)
        port = parsed.port
    except ValueError as exc:
        raise CandidateError("candidate URL is malformed") from exc
    if parsed.scheme != "https" or not parsed.hostname:
        raise CandidateError("candidate URL must use https with a hostname")
    if parsed.username is not None or parsed.password is not None:
        raise CandidateError("candidate URL must not contain credentials")
    if parsed.query:
        raise CandidateError("candidate URL must not contain a query string")
    if parsed.fragment:
        raise CandidateError("candidate URL must not contain a fragment")
    if port not in (None, 443):
        raise CandidateError("candidate URL must use the default HTTPS port")
    hostname = parsed.hostname.rstrip(".").lower()
    if hostname == "localhost" or hostname.endswith(".localhost") or hostname.endswith(".local"):
        raise CandidateError("candidate URL hostname must be public")
    try:
        address = ipaddress.ip_address(hostname)
    except ValueError:
        pass
    else:
        if not address.is_global:
            raise CandidateError("candidate URL IP address must be globally routable")
    if not parsed.path or parsed.path.endswith("/"):
        raise CandidateError("candidate URL must identify an image file")
    return value


def validate_size(value: int) -> int:
    if (
        isinstance(value, bool)
        or not isinstance(value, int)
        or not 1 <= value <= MAX_IMAGE_BYTES
    ):
        raise CandidateError(
            f"candidate byte size must be in the range 1..{MAX_IMAGE_BYTES}"
        )
    return value


def validate_sha256(value: str) -> str:
    if ambit_deploy.SHA256_RE.fullmatch(value) is None:
        raise CandidateError("candidate SHA-256 must be exact lowercase 64-digit hex")
    return value


def _download_candidate(url: str, expected_size: int) -> tuple[bytes, str]:
    hostname = urllib.parse.urlsplit(url).hostname
    assert hostname is not None  # validate_candidate_url already ran
    try:
        addresses = {
            info[4][0]
            for info in socket.getaddrinfo(hostname, 443, type=socket.SOCK_STREAM)
        }
    except socket.gaierror as exc:
        raise CandidateError("candidate hostname did not resolve") from exc
    if not addresses or any(not ipaddress.ip_address(address).is_global for address in addresses):
        raise CandidateError("candidate hostname must resolve only to public addresses")

    request = urllib.request.Request(
        url,
        headers={
            "Accept": "application/octet-stream",
            "Accept-Encoding": "identity",
            "User-Agent": "ambyte-ambit-candidate-canary",
        },
    )

    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(
            self,
            req: Any,
            fp: Any,
            code: int,
            msg: str,
            headers: Any,
            newurl: str,
        ) -> None:
            raise CandidateError("candidate URL must not redirect")

    opener = urllib.request.build_opener(NoRedirect)
    with opener.open(request, timeout=60) as response:
        final_url = response.geturl()
        if final_url != url:
            raise CandidateError("candidate URL must not redirect")
        encoding = response.headers.get("Content-Encoding")
        if encoding not in (None, "", "identity"):
            raise CandidateError("candidate response must not use content encoding")
        length = response.headers.get("Content-Length")
        if length is not None:
            try:
                reported_size = int(length)
            except ValueError as exc:
                raise CandidateError("candidate Content-Length is not an integer") from exc
            if reported_size != expected_size:
                raise CandidateError(
                    f"candidate Content-Length {reported_size} does not match "
                    f"expected {expected_size}"
                )
        image = response.read(expected_size + 1)
    return image, final_url


def verify_image(image: bytes, expected_size: int, expected_sha256: str) -> None:
    if len(image) != expected_size:
        raise CandidateError(
            f"candidate byte count mismatch: got {len(image)}, expected {expected_size}"
        )
    actual = hashlib.sha256(image).hexdigest()
    if actual != expected_sha256:
        raise CandidateError(
            f"candidate SHA-256 mismatch: got {actual}, expected {expected_sha256}"
        )
    if image[0] != 0xE9:
        raise CandidateError(
            f"candidate is not an ESP application image (first byte {image[0]:02x}, expected e9)"
        )


def _base_result(
    *,
    device_id: str | None,
    candidate_version: str | None,
    dry_run: bool,
) -> dict[str, Any]:
    return {
        "kind": KIND,
        "candidate_only": True,
        "environment": ENVIRONMENT,
        "protected_environment": PROTECTED_ENVIRONMENT,
        "device_id": device_id,
        "candidate_version": candidate_version,
        "percentage": 100,
        "discovery": False,
        "allow_downgrade": False,
        "force_reflash": False,
        "dry_run": dry_run,
        "results": {},
    }


def prepare_candidate(
    *,
    environment: str,
    git_ref: str,
    device_id: str,
    candidate_version: str,
    image_url: str,
    image_size: int,
    image_sha256: str,
    dry_run: bool,
    prepared_dir: Path,
    proof_json: Path,
    results_json: Path | None,
    downloader: Callable[[str, int], tuple[bytes, str]] = _download_candidate,
) -> dict[str, Any]:
    """Validate/download once and persist exact provenance before AWS is available."""
    _require_environment(environment)
    _require_git_ref(git_ref)
    device_id = validate_device_id(device_id)
    candidate_version = validate_candidate_version(candidate_version)
    image_url = validate_candidate_url(image_url)
    image_size = validate_size(image_size)
    image_sha256 = validate_sha256(image_sha256)

    initial = _base_result(
        device_id=device_id,
        candidate_version=candidate_version,
        dry_run=dry_run,
    )
    _write_json(
        results_json,
        {
            **initial,
            "phase": "preparing",
            "error": "candidate verification did not finish",
        },
    )

    image, final_url = downloader(image_url, image_size)
    if final_url != image_url:
        raise CandidateError("candidate URL must not redirect")
    verify_image(image, image_size, image_sha256)

    prepared_dir.mkdir(parents=True, exist_ok=True)
    image_path = prepared_dir / IMAGE_NAME
    image_path.write_bytes(image)
    proof = {
        "schema": PROOF_SCHEMA,
        "kind": KIND,
        "candidate_only": True,
        "environment": ENVIRONMENT,
        "protected_environment": PROTECTED_ENVIRONMENT,
        "git_ref": REQUIRED_GIT_REF,
        "device_id": device_id,
        "candidate_version": candidate_version,
        "dry_run": dry_run,
        "source": {
            "url": image_url,
            "final_url": final_url,
            "redirected": False,
            "anonymous_https": True,
            "credentials": False,
            "query": False,
            "fragment": False,
        },
        "application": {
            "local_name": IMAGE_NAME,
            "size": image_size,
            "sha256": image_sha256,
            "esp_app_magic": "0xe9",
            "download_count": 1,
            "verified_before_aws": True,
        },
        "targeting": {
            "explicit_device_count": 1,
            "percentage": 100,
            "discovery": False,
            "allow_downgrade": False,
            "force_reflash": False,
        },
        "prepared_at_utc": _utc_now(),
    }
    _write_json(proof_json, proof)
    prepared = {
        **initial,
        "phase": "prepared_before_aws",
        "supply_chain_proof": proof,
        "error": None,
    }
    _write_json(results_json, prepared)
    return proof


def _load_prepared(
    *,
    environment: str,
    git_ref: str,
    prepared_dir: Path,
    proof_json: Path,
) -> tuple[dict[str, Any], bytes]:
    _require_environment(environment)
    _require_git_ref(git_ref)
    try:
        proof = json.loads(proof_json.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise CandidateError("prepared supply-chain proof is unreadable") from exc
    if not isinstance(proof, dict) or proof.get("schema") != PROOF_SCHEMA:
        raise CandidateError("prepared supply-chain proof schema is invalid")
    invariant_pairs = {
        "kind": KIND,
        "candidate_only": True,
        "environment": ENVIRONMENT,
        "protected_environment": PROTECTED_ENVIRONMENT,
        "git_ref": REQUIRED_GIT_REF,
    }
    for field, expected in invariant_pairs.items():
        if proof.get(field) != expected:
            raise CandidateError(f"prepared proof invariant {field!r} is invalid")
    validate_device_id(proof.get("device_id") if isinstance(proof.get("device_id"), str) else "")
    validate_candidate_version(
        proof.get("candidate_version")
        if isinstance(proof.get("candidate_version"), str)
        else ""
    )
    if not isinstance(proof.get("dry_run"), bool):
        raise CandidateError("prepared proof dry_run must be boolean")

    source = proof.get("source")
    application = proof.get("application")
    targeting = proof.get("targeting")
    if (
        not isinstance(source, dict)
        or not isinstance(application, dict)
        or not isinstance(targeting, dict)
    ):
        raise CandidateError("prepared proof sections are invalid")
    image_url = source.get("url")
    if not isinstance(image_url, str) or source.get("final_url") != image_url:
        raise CandidateError("prepared source URL identity is invalid")
    validate_candidate_url(image_url)
    if source.get("redirected") is not False or source.get("anonymous_https") is not True:
        raise CandidateError("prepared source anonymity/redirect proof is invalid")
    if any(source.get(field) is not False for field in ("credentials", "query", "fragment")):
        raise CandidateError("prepared source contains forbidden URL features")

    size = application.get("size")
    digest = application.get("sha256")
    validate_size(size)
    validate_sha256(digest if isinstance(digest, str) else "")
    if (
        application.get("local_name") != IMAGE_NAME
        or application.get("esp_app_magic") != "0xe9"
        or application.get("download_count") != 1
        or application.get("verified_before_aws") is not True
    ):
        raise CandidateError("prepared application proof is invalid")
    expected_targeting = {
        "explicit_device_count": 1,
        "percentage": 100,
        "discovery": False,
        "allow_downgrade": False,
        "force_reflash": False,
    }
    if targeting != expected_targeting:
        raise CandidateError("prepared targeting proof violates candidate isolation")

    try:
        image = (prepared_dir / IMAGE_NAME).read_bytes()
    except OSError as exc:
        raise CandidateError("prepared candidate image is unreadable") from exc
    verify_image(image, size, digest)
    return proof, image


def _write_failure(
    results_json: Path | None,
    base: dict[str, Any],
    phase: str,
    error: str,
) -> None:
    _write_json(results_json, {**base, "phase": phase, "error": ambit_deploy._redact_text(error)})


def deploy_candidate(
    *,
    environment: str,
    git_ref: str,
    prepared_dir: Path,
    proof_json: Path,
    results_json: Path | None,
    profile: str | None,
    region: str,
    ping_wait: int,
    preflight_seconds: int,
    verify_seconds: int,
    ack_seconds: int,
    final_seconds: int,
) -> int:
    proof, _image = _load_prepared(
        environment=environment,
        git_ref=git_ref,
        prepared_dir=prepared_dir,
        proof_json=proof_json,
    )
    device = proof["device_id"]
    version = proof["candidate_version"]
    dry_run = proof["dry_run"]
    base = {
        **_base_result(
            device_id=device,
            candidate_version=version,
            dry_run=dry_run,
        ),
        "supply_chain_proof": proof,
        "phase": "targeting",
        "error": "targeting did not finish",
    }
    _write_json(results_json, base)

    try:
        session = fleet.boto_session(profile, region)
        firmware = fleet.fleet_ping(session, [device], ping_wait)
        preflight_id = ambit_deploy._new_id("ambit-candidate-preflight", version)
        preflight, preflight_error = ambit_deploy.fleet_ambit_versions(
            session, [device], preflight_id, preflight_seconds
        )
    except Exception as exc:
        error = f"{type(exc).__name__}: {exc}"
        _write_failure(results_json, base, "targeting_failed", error)
        print(f"Candidate targeting failed: {ambit_deploy._redact_text(error)}", file=sys.stderr)
        return 1

    decisions, to_deploy = ambit_deploy.decide_gateways(
        [device], preflight, version, allow_downgrade=False, force_reflash=False
    )
    campaign_id = ambit_deploy._new_id("ambit-candidate-ota", version)
    plan: dict[str, Any] = {
        **base,
        "phase": "preflight_complete",
        "gateway_firmware": firmware,
        "silent_on_ping": [] if device in firmware else [device],
        "preflight_id": preflight_id,
        "preflight_versions": preflight,
        "preflight_error": preflight_error,
        "gateway_decisions": decisions,
        "deployed_gateways": to_deploy,
        "campaign_id": campaign_id,
        "command": {
            "type": "ambit_ota",
            "id": campaign_id,
            "channel": "all",
            "url": proof["source"]["url"],
        },
        "post_verify_id": None,
        "post_verify_versions": {},
        "post_verify_error": None,
        "verification": {},
        "results": {},
        "error": None,
    }

    decision = decisions.get(device, {})
    if device not in firmware:
        plan["error"] = "explicit gateway did not answer correlated ping"
    elif preflight_error:
        plan["error"] = f"AMBIT preflight failed: {preflight_error}"
    elif decision.get("blocking"):
        plan["error"] = f"AMBIT preflight is unproven: {decision.get('skip_reason')}"
    elif decision.get("skip_reason") == "newer_ambit_present":
        plan["error"] = "candidate downgrade is forbidden"
    elif not preflight:
        plan["error"] = "explicit gateway did not answer correlated ambit_versions"
    elif not to_deploy and decision.get("skip_reason") != "all_ambits_up_to_date":
        plan["error"] = (
            "single-gateway canary has no proven-present eligible AMBIT: "
            f"{decision.get('skip_reason') or 'unknown_policy_state'}"
        )

    if plan["error"]:
        plan["phase"] = "preflight_failed"
        _write_json(results_json, plan)
        print(f"Candidate canary failed closed: {plan['error']}", file=sys.stderr)
        return 1

    if dry_run:
        plan["phase"] = "dry_run_complete"
        plan["error"] = None
        _write_json(results_json, plan)
        print("DRY RUN: ping and ambit_versions were published; no ambit_ota was published.")
        return 0

    if not to_deploy:
        if decision.get("skip_reason") == "all_ambits_up_to_date":
            plan["phase"] = "already_current"
            _write_json(results_json, plan)
            print("All preflight-present AMBIT channels already report the candidate version.")
            return 0
        plan["phase"] = "nothing_eligible"
        plan["error"] = f"nothing eligible to deploy: {decision.get('skip_reason')}"
        _write_json(results_json, plan)
        return 1

    # Future atomic calibration snapshot hook 1: immediately here, after the
    # exact gateway is eligible and before any ambit_ota publication. The
    # deployed 192-byte lua_exec result cap currently makes raw-hex proof unsafe.
    try:
        results, tracking_error = ambit_deploy.fleet_ambit_ota(
            session,
            [device],
            campaign_id,
            proof["source"]["url"],
            ack_seconds,
            final_seconds,
            1,
            0,
        )
        plan["phase"] = "ota_published"
        plan["results"] = results
        plan["tracking_error"] = tracking_error
        _write_json(results_json, plan)

        post_verify_id = ambit_deploy._new_id("ambit-candidate-verify", version)
        post_reports, post_error = ambit_deploy.fleet_ambit_versions(
            session, [device], post_verify_id, verify_seconds
        )
        verification, failures = ambit_deploy.assess_post_verification(
            [device], decisions, results, post_reports, version
        )
    except Exception as exc:
        error = ambit_deploy._redact_text(f"{type(exc).__name__}: {exc}")
        plan["phase"] = "execution_failed"
        plan["error"] = error
        _write_json(results_json, plan)
        return 1
    plan["post_verify_id"] = post_verify_id
    plan["post_verify_versions"] = post_reports
    plan["post_verify_error"] = post_error
    plan["verification"] = verification
    if post_error:
        failures = [*failures, f"post_verify_transport:{post_error}"]
    plan["live_failures"] = failures
    if failures:
        plan["phase"] = "verification_failed"
        plan["error"] = (
            "all preflight-present channels did not verify at the target "
            "numeric version"
        )
        _write_json(results_json, plan)
        return 1

    # Future atomic calibration snapshot hook 2: immediately here, after
    # assess_post_verification confirms the target and before final success.
    plan["phase"] = "verified"
    plan["error"] = None
    _write_json(results_json, plan)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    prepare = subparsers.add_parser("prepare", help="download and verify before AWS")
    prepare.add_argument("--environment", required=True)
    prepare.add_argument("--git-ref", required=True)
    prepare.add_argument("--device-id", required=True)
    prepare.add_argument("--candidate-version", required=True)
    prepare.add_argument("--image-url", required=True)
    prepare.add_argument("--image-size", required=True, type=int)
    prepare.add_argument("--image-sha256", required=True)
    prepare.add_argument("--dry-run", action="store_true")
    prepare.add_argument("--prepared-dir", required=True, type=Path)
    prepare.add_argument("--proof-json", required=True, type=Path)
    prepare.add_argument("--results-json", type=Path)

    deploy = subparsers.add_parser("deploy", help="use one already verified image/proof")
    deploy.add_argument("--environment", required=True)
    deploy.add_argument("--git-ref", required=True)
    deploy.add_argument("--prepared-dir", required=True, type=Path)
    deploy.add_argument("--proof-json", required=True, type=Path)
    deploy.add_argument("--results-json", type=Path)
    deploy.add_argument("--profile")
    deploy.add_argument("--region", default="eu-central-1")
    deploy.add_argument("--ping-wait", type=int, default=25)
    deploy.add_argument("--preflight-seconds", type=int, default=90)
    deploy.add_argument("--verify-seconds", type=int, default=120)
    deploy.add_argument("--ack-seconds", type=int, default=90)
    deploy.add_argument("--final-seconds", type=int, default=3600)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.command == "prepare":
        try:
            prepare_candidate(
                environment=args.environment,
                git_ref=args.git_ref,
                device_id=args.device_id,
                candidate_version=args.candidate_version,
                image_url=args.image_url,
                image_size=args.image_size,
                image_sha256=args.image_sha256,
                dry_run=args.dry_run,
                prepared_dir=args.prepared_dir,
                proof_json=args.proof_json,
                results_json=args.results_json,
            )
        except (CandidateError, OSError, RuntimeError) as exc:
            error = ambit_deploy._redact_text(str(exc))
            base = _base_result(
                device_id=getattr(args, "device_id", None),
                candidate_version=getattr(args, "candidate_version", None),
                dry_run=getattr(args, "dry_run", False),
            )
            _write_failure(args.results_json, base, "preparation_failed", error)
            print(f"Candidate preparation failed: {error}", file=sys.stderr)
            return 2
        print("Candidate image downloaded once and verified before AWS configuration.")
        return 0

    if any(
        value <= 0
        for value in (
            args.ping_wait,
            args.preflight_seconds,
            args.verify_seconds,
            args.ack_seconds,
            args.final_seconds,
        )
    ):
        parser.error("all wait/observation durations must be positive")
    try:
        return deploy_candidate(
            environment=args.environment,
            git_ref=args.git_ref,
            prepared_dir=args.prepared_dir,
            proof_json=args.proof_json,
            results_json=args.results_json,
            profile=args.profile,
            region=args.region,
            ping_wait=args.ping_wait,
            preflight_seconds=args.preflight_seconds,
            verify_seconds=args.verify_seconds,
            ack_seconds=args.ack_seconds,
            final_seconds=args.final_seconds,
        )
    except (CandidateError, OSError, RuntimeError) as exc:
        error = ambit_deploy._redact_text(str(exc))
        _write_failure(
            args.results_json,
            _base_result(device_id=None, candidate_version=None, dry_run=False),
            "prepared_proof_failed",
            error,
        )
        print(f"Prepared candidate validation failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
