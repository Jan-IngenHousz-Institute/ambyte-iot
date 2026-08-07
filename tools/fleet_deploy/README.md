# Fleet deploy (Ambyte OTA, Lua, and AMBIT OTA)

Targeted, staged rollouts to the Ambyte fleet, from the three manual GitHub
Actions workflows (**Fleet deploy (OTA)**, **Fleet deploy (Lua)**, and **Fleet
deploy (AMBIT via Ambyte)**) or locally.

This is the productionized successor of the manual `utility-fleetOTA`
notebook flow: same mechanism (one `ota_update` MQTT publish per device, no
broadcast topic, no IoT Jobs), plus version-aware targeting, deterministic
percentage cohorts, and a written outcome per device.

## How a rollout works

1. **Discover** the device universe: clientIds seen publishing to IoT Core
   within the lookback window (CloudWatch Logs Insights on `AWSIotLogsV2`),
   or an explicit device list.
2. **Ping** the fleet once. Every device answers `pong` with a `fw` string.
   ⚠️ On every image fielded before this PR's firmware fix, that string is
   the NVS provisioning value from USB flash time (the fleet reports `"1"`
   and `"1.0.2"`), NOT the running version; OTA never updates it. The fix
   shipping with this PR makes `pong` report the compiled running version,
   so version targeting becomes fully trustworthy as devices move onto
   post-fix releases. Until then: `lt`-style filters against old
   provisioning strings still correctly select never-updated devices, but
   `eq`/`gt` filters and the already-up-to-date skip only see flash-time
   truth.
3. **Filter** by the version predicate, **slice** the percentage cohort,
   **skip** units already on the target release (their applied-id latch
   would silently ignore the command) and units reporting a newer version
   (the firmware has no downgrade protection; pass `allow_downgrade` to
   override deliberately).
4. **Publish** `{"type":"ota_update","id":"fw-<tag>","url":<release asset>}`
   to each device's command topic and **track** every device to a terminal
   state (`success` / `failed` / `dropped`), writing a per-device table to
   the job summary and a `results.json` artifact.

A live run succeeds only when every commanded gateway reports terminal
`success` and the exact target firmware version. Missing replies,
accepted-without-final reports, dropped/failed terminals, absent version fields,
and version mismatches fail the job while preserving the partial artifact.

Safety properties, all firmware-side: dual-slot OTA with ~300 s
auto-rollback (a broken image cannot brick a device), NVS untouched
(provisioning + event cursor survive), applied-id latch (re-running a
campaign is idempotent), and on release builds v1.2.0+ a MAC-derived
0-899 s download stagger so a fleet-wide push does not stampede the site
uplink. The tracking window (`--final-seconds`, default 2100) is sized for
that worst case: jitter + download on a degraded link + reboot + confirm.

## Targeting recipes

| Goal | version_op | version | percentage |
|---|---|---|---|
| All devices | `any` | | 100 |
| All devices on 1.0.5 | `eq` | 1.0.5 | 100 |
| 30% of all devices | `any` | | 30 |
| 30% of devices below 1.2.0 | `lt` | 1.2.0 | 30 |
| Everything at or above 1.1.0 | `gte` | 1.1.0 | 100 |

**Cohorts are deterministic over a fixed population.** Devices are ordered
by `sha256(clientId)` and the first `ceil(N * pct/100)` are taken, so a
higher percentage over the same population is a superset of the earlier
cohort. The population itself can drift between runs (devices going
online/offline, version filters thinning out as updates land), which
shifts the slice. For a strictly frozen canary, re-target the exact cohort
with the `devices` input, pasting the cohort list from the previous run's
`results.json` artifact. Version comparison is semver (prerelease sorts
before its release: `1.0.6-rc1 < 1.0.6`).

**Staged-rollout flow:** run at 30% live -> save the cohort from
`results.json` -> watch it on the platform for a day -> promote with a 100%
run of the same tag. Sweep stragglers of a specific wave by re-running with
`devices` = that saved cohort (idempotent: updated devices no-op).

`dry_run` defaults to **true**; every campaign's first run should be the
preview. The live run refuses to publish unless the release asset actually
serves an ESP32 app image (magic byte `0xE9`), which catches the
HTML-instead-of-binary URL mistake before the fleet downloads it.

## Local use

```sh
pip install -r tools/fleet_deploy/requirements.txt
python tools/fleet_deploy/fleet_deploy.py \
    --profile <sso-profile> --tag v1.2.0 \
    --version-op lt --version 1.2.0 --percentage 30 --dry-run
```

Drop `--dry-run` to deploy. `--devices "E8:F6:0A:B1:1D:D4"` targets the
bench unit directly.

## GitHub Actions setup (provisioned 2026-08-01)

Dev vs prod follows the org's account split (jii-infra Control Tower;
open-jii's per-env `iam-oidc` pattern). The `environment` workflow input
selects the target; everything below already exists:

| | dev | prod |
|---|---|---|
| AWS account | OpenJII-DEV `084375565727` | OpenJII-PROD `494249241400` |
| GitHub environment | `fleet-deploy-dev` | `fleet-deploy-prod` |
| IAM role (via OIDC) | `GithubActionsAmbyteFleetDeploy` | `GithubActionsAmbyteFleetDeploy` |
| Secret `AWS_FLEET_DEPLOY_ROLE_ARN` | set (env-scoped) | set (env-scoped) |

Both GitHub environments have **required reviewers**: the job deploys to
real hardware, and `workflow_dispatch` runs whichever ref's copy of the
workflow and script was selected, so the approval gate is the only thing
between a modified feature-branch script and the fleet. Dispatch from
`main` unless deliberately testing. The whole fleet connects to the dev
IoT Core today; prod is pre-provisioned for the platform migration.

Each account's role reuses that account's existing GitHub OIDC provider
(created by open-jii's `iam-oidc` module) and trusts exactly its own
environment
(`sub = repo:Jan-IngenHousz-Institute/ambyte-iot:environment:fleet-deploy-<env>`,
2 h max session to cover long campaigns). Reference permissions policy
(account-scoped; attached as inline policy `fleet-deploy`):

```json
{
  "Version": "2012-10-17",
  "Statement": [
    { "Sid": "DiscoverDevices", "Effect": "Allow",
      "Action": ["logs:StartQuery"],
      "Resource": "arn:aws:logs:eu-central-1:<account>:log-group:AWSIotLogsV2:*" },
    { "Sid": "QueryResults", "Effect": "Allow",
      "Action": ["logs:GetQueryResults"],
      "Resource": "*" },
    { "Sid": "IotEndpoint", "Effect": "Allow",
      "Action": "iot:DescribeEndpoint",
      "Resource": "*" },
    { "Sid": "IotConnect", "Effect": "Allow",
      "Action": "iot:Connect",
      "Resource": "arn:aws:iot:eu-central-1:<account>:client/fleet-deploy-*" },
    { "Sid": "CommandPublish", "Effect": "Allow",
      "Action": "iot:Publish",
      "Resource": "arn:aws:iot:eu-central-1:<account>:topic/device/scripts/v1/Ambyte/2/*" },
    { "Sid": "StatusSubscribe", "Effect": "Allow",
      "Action": "iot:Subscribe",
      "Resource": "arn:aws:iot:eu-central-1:<account>:topicfilter/experiment/data_ingest/v1/*" },
    { "Sid": "StatusReceive", "Effect": "Allow",
      "Action": "iot:Receive",
      "Resource": "arn:aws:iot:eu-central-1:<account>:topic/experiment/data_ingest/v1/*" }
  ]
}
```

IoT logging (`AWSIotLogsV2`, Publish-In events) must stay enabled; device
discovery reads it.

## Reading the outcome

| Outcome | Meaning | Action |
|---|---|---|
| `succeeded` | New image booted and self-confirmed (`now fw=` shows it) | none |
| `already up to date` | Pong already reported the target version | none |
| `skipped (newer fw)` | Device reports a newer version; downgrade blocked | use `allow_downgrade` if intended |
| `dropped` | Device rejected: busy with maintenance / OTA / OOM | re-run later |
| `failed` | Download or apply failed (detail says why); device rolled back | fix cause, re-run |
| `accepted_no_final` | Accepted but no terminal report in the wait window (slow link, long jitter slot, or the misrouted-status-topic units) | verify via telemetry `fw`, or re-run: it no-ops if it landed |
| `no_reply` | No ack received. Usually offline at publish time; rarely the device applied anyway but its best-effort ack was lost | sweep re-run later (no-ops if it landed) |

A run exits non-zero when at least one device reports `failed`, or when the
campaign hit an error mid-tracking (partial results are still written to
`results.json` and the summary).

## Deploying AMBIT firmware through Ambytes

Use **Actions -> Fleet deploy (AMBIT via Ambyte) -> Run workflow**. The release
source is fixed to the public `Jan-IngenHousz-Institute/ambit` repository.
`latest` selects the highest stable `vX.Y.Z` release; it never selects a
prerelease. An exact `vX.Y.Z-suffix` tag is accepted only when
`allow_prerelease` is explicitly enabled. Dry-run, the default, performs the
read-only gateway ping and correlated `ambit_versions` preflight but never
publishes `ambit_ota`.

Before AWS access or MQTT publication, the runner anonymously fetches the
GitHub REST release representation and fails closed unless it is public,
published, non-draft, immutable, and allowed by the prerelease policy. It then:

1. Downloads `manifest.json` and requires `manifest.version` to equal the tag
   without `v`.
2. Selects the application named by `manifest.ota.file` and requires exactly
   one matching `flash` entry at offset `0x10000`, with integer size and a
   lowercase SHA-256.
3. Requires the GitHub asset's REST size and `sha256:<digest>` to match the
   manifest, downloads its anonymous `browser_download_url`, and verifies its
   byte count, SHA-256, and ESP application magic byte `0xE9`.

Gateway discovery, the optional Ambyte firmware predicate, explicit-device
targeting, and deterministic percentage slicing are shared with the other fleet
workflows. After selecting that cohort, the runner sends a unique correlated
`ambit_versions` query to each gateway and records every channel's presence and
numeric `major.minor.patch` version. A prerelease such as
`v1.1.2-recovery.1` therefore expects the device-reported numeric identity
`1.1.2`. Current gateway firmware stops the Lua schedule and waits for any
already-triggered SS/MPF run to finish before reading versions, so a scheduled
measurement cannot masquerade as missing hardware. The runner additionally
retries incomplete, busy, absent, or unversioned inventory after a bounded idle
delay and reconciles compatible positive version evidence. Both correlated
attempts are retained. Absence requires two complete observations; a missing,
incomplete, unversioned, or conflicting retry becomes `ambiguous_preflight` and
blocks the whole run instead of silently narrowing a live cohort.

The execution unit is a gateway, not an individual sensor channel. Every
eligible gateway receives exactly one command:

```json
{"type":"ambit_ota","id":"<unique-run-id>","channel":"all","url":"<verified-public-app-asset>"}
```

This matters for mixed gateways: if one channel is old and another is already
current, the current channel can be reflashed because the firmware accepts only
the all-channel fleet sweep. If any present channel is newer, the entire gateway
is skipped unless `allow_downgrade` is explicit. An all-current cohort is a
clean success. For recovery, `force_reflash` may reapply the same numeric
version, but defaults false and is rejected unless `devices` names the exact
gateways and `percentage` is 100. It does not bypass newer-version protection;
`allow_downgrade` is still required when any selected channel is newer.

The result artifact preserves the exact release proof, cohort and gateway
firmware map, every correlated preflight attempt and its effective versions,
per-gateway decision/skip reason, unique command ID, acceptance, all four
per-channel outcomes, and the overall terminal. The host deliberately expects
the firmware's `channel=all` status path to report channels 0 through 3,
including `absent`, followed by the overall terminal after MQTT recovery.

The terminal observation budget is 3600 seconds: it covers 0..899 seconds of
firmware jitter, degraded HTTPS/SD transfer, four sequential channel streams,
and MQTT recovery. The workflow job allows 90 minutes and its OIDC credentials
last 7200 seconds. After terminal tracking finishes or times out, the runner
always sends a new correlated `ambit_versions` query to every gateway that
received `ambit_ota`. Results record the expected numeric target and actual
version of every present channel. A matching version change can confirm success
when best-effort terminal reports were lost, but it never masks an explicitly
reported present-channel failure/absence. A forced same-version reflash cannot
be proven by version effect, so it requires the complete successful terminal
set. Otherwise the gateway is `indeterminate` and the live run fails.

Empty universes/cohorts, any unresolved preflight ambiguity, live zero
reachable/zero present, post-OTA mismatches or missing verification, and every
present-channel failure/timeout fail closed. These checks also make an empty
dry-run cohort fail instead of presenting a misleading successful preview.
Partial evidence survives publish, tracking, and verification errors.

For a local exact-gateway preview:

```sh
python tools/fleet_deploy/ambit_deploy.py \
    --profile <sso-profile> --tag v1.1.1 \
    --devices "E8:F6:0A:B1:1D:D4" --dry-run
```

Do not use this workflow for bare, bricked, or pre-cooperative-OTA AMBITs; they
require the ROM-flasher recovery path.

## Deploying a Lua release

Use **Actions -> Fleet deploy (Lua) -> Run workflow**. Its targeting form
matches Fleet deploy (OTA): environment, release tag or `latest`, firmware
predicate, deterministic percentage, optional exact devices, and discovery
window. It also controls whether the device reboots after the swap. Dry-run is
the default; it publishes correlated ping commands to discover firmware
versions but publishes no `script_update` command.

`latest` considers only published `lua-v*` releases. Before connecting to IoT,
the deploy tool verifies the manifest schema, tag and version identity,
immutable asset URL, byte count, and SHA-256. `built_against_fw` is reported as
provenance and is not an automatic compatibility constraint.

For all workflows, `latest` means the highest stable semantic version inside
that repository/release family: `v*` for Ambyte OTA and AMBIT OTA, and
`lua-v*` for Lua. It does not depend on which release happened to be published
most recently.

For a local exact-device preview:

```sh
python tools/fleet_deploy/lua_deploy.py \
    --profile <sso-profile> --tag lua-v1.0.0 \
    --devices "E8:F6:0A:B1:1D:D4" --dry-run
```

Drop `--dry-run` to publish. The result artifact and job summary classify each
target as `accepted`, `applied`, `failed`, `busy`, or `no_reply`.

| Lua outcome | Meaning | Action |
|---|---|---|
| `applied` | Device reported the manifest's expected active SHA-256 | none |
| `applied (sha mismatch)` | Device said applied but reported different or unavailable active bytes | inspect the SD card and re-run; the workflow fails |
| `failed` | Download, verification, syntax check, or swap failed | fix the reported cause and re-run |
| `busy` | Device explicitly refused because another maintenance operation was active | re-run later |
| `accepted` | Device accepted but no terminal result arrived in the wait window | inspect telemetry, then sweep again |
| `no_reply` | No correlated acknowledgement arrived | confirm connectivity and sweep again |

A live run fails when any device reports `failed`/a SHA mismatch, or when no
target confirms the expected SHA as applied. This prevents an all-busy or
all-silent campaign from appearing successful.
