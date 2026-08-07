from pathlib import Path
import re
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[1]
WORKFLOW_PATH = ".github/workflows/fleet-deploy-ambit.yml"
WORKFLOW = (ROOT / WORKFLOW_PATH).read_text(
    encoding="utf-8"
)


class AmbitCandidateWorkflowTest(unittest.TestCase):
    def candidate_job(self) -> str:
        return WORKFLOW.split("  candidate-canary:\n", 1)[1]

    def release_job(self) -> str:
        return WORKFLOW.split("  deploy:\n", 1)[1].split(
            "\n  # This path is intentionally", 1
        )[0]

    def test_existing_workflow_path_is_present_on_origin_main(self) -> None:
        result = subprocess.run(
            ["git", "cat-file", "-e", f"origin/main:{WORKFLOW_PATH}"],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_dispatch_surface_has_explicit_candidate_contract(self) -> None:
        input_names = set(re.findall(r"(?m)^      ([a-z][a-z0-9_]*):$", WORKFLOW))
        self.assertTrue(
            {
                "candidate_device_id",
                "candidate_version",
                "candidate_image_url",
                "candidate_image_size",
                "candidate_image_sha256",
                "dry_run",
            }.issubset(input_names)
        )
        self.assertIn("Candidate mode only: exactly one canonical", WORKFLOW)
        self.assertIn("--device-id \"${DEVICE_ID}\"", WORKFLOW)
        self.assertIn("default: true", WORKFLOW)
        self.assertIn("options: [release, candidate]", WORKFLOW)
        self.assertIn("default: release", WORKFLOW)

    def test_is_hard_bound_to_protected_dev_and_global_mutation_lock(self) -> None:
        candidate_job = self.candidate_job()
        self.assertIn("environment: fleet-deploy-dev", candidate_job)
        self.assertIn("inputs.environment", candidate_job)
        self.assertIn('[[ "${TARGET_ENVIRONMENT}" == "dev" ]]', candidate_job)
        self.assertIn(
            '[[ "${DISPATCH_REF}" == '
            '"refs/heads/ops/ambit-v1.1.4-candidate-canary" ]]',
            candidate_job,
        )
        self.assertIn("--environment dev", candidate_job)
        self.assertIn('--git-ref "${DISPATCH_REF}"', candidate_job)
        self.assertIn("id-token: write", WORKFLOW)
        self.assertIn("group: fleet-deploy", WORKFLOW)
        self.assertIn("cancel-in-progress: false", WORKFLOW)

    def test_every_visible_release_control_is_guarded_at_its_safe_default(self) -> None:
        candidate_job = self.candidate_job()
        expected_guards = (
            '[[ "${TARGET_ENVIRONMENT}" == "dev" ]]',
            '[[ "${RELEASE_TAG}" == "latest" ]]',
            '[[ "${VERSION_OP}" == "any" ]]',
            '[[ -z "${VERSION_FILTER}" ]]',
            '[[ "${PERCENTAGE}" == "100" ]]',
            '[[ -z "${RELEASE_DEVICES}" ]]',
            '[[ "${DISCOVERY_WINDOW}" == "1440" ]]',
            '[[ "${ALLOW_DOWNGRADE}" == "false" ]]',
            '[[ "${ALLOW_PRERELEASE}" == "false" ]]',
            '[[ "${FORCE_REFLASH}" == "false" ]]',
        )
        for guard in expected_guards:
            with self.subTest(guard=guard):
                self.assertIn(guard, candidate_job)

    def test_download_and_proof_happen_before_aws(self) -> None:
        candidate_job = self.candidate_job()
        prepare = candidate_job.index("Download and verify exact candidate before AWS")
        configure = candidate_job.index("Configure DEV AWS credentials")
        deploy = candidate_job.index("Run isolated single-gateway candidate canary")
        self.assertLess(prepare, configure)
        self.assertLess(configure, deploy)
        self.assertIn("candidate-supply-chain-proof.json", candidate_job)
        self.assertIn("results.json", candidate_job)

    def test_candidate_job_has_no_release_or_broad_cohort_forwarding(self) -> None:
        candidate_job = self.candidate_job()
        for forbidden in (
            "release-tag",
            "allow-downgrade",
            "force-reflash",
            "allow-prerelease",
            "discovery-window",
            "percentage:",
            "devices:",
            "kind: ambit",
            "uses: ./.github/actions/fleet-deploy",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, candidate_job)
        execution = candidate_job.split(
            "      - name: Download and verify exact candidate before AWS\n", 1
        )[1]
        self.assertNotIn("inputs.devices", execution)
        self.assertNotIn("inputs.release_tag", execution)

    def test_normal_immutable_release_job_remains_default_and_separate(self) -> None:
        release_job = self.release_job()
        self.assertIn("if: inputs.mode == 'release'", WORKFLOW)
        self.assertIn("if: inputs.mode == 'candidate'", WORKFLOW)
        self.assertIn("Deploy immutable public AMBIT release", release_job)
        self.assertIn("uses: ./.github/actions/fleet-deploy", release_job)
        self.assertNotIn("ambit_candidate_deploy.py", release_job)

    def test_release_job_matches_origin_main_except_default_mode_gate(self) -> None:
        origin = subprocess.check_output(
            ["git", "show", f"origin/main:{WORKFLOW_PATH}"],
            cwd=ROOT,
            text=True,
        )
        origin_job = origin.split("jobs:\n  deploy:\n", 1)[1].rstrip()
        branch_job = self.release_job().replace(
            "    if: inputs.mode == 'release'\n", "", 1
        ).rstrip()
        self.assertEqual(branch_job, origin_job)


if __name__ == "__main__":
    unittest.main()
