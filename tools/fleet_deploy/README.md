# Fleet deploy (OTA)

Targeted, staged rollouts of a published firmware release to the Ambyte
fleet, from GitHub Actions (**Actions -> Fleet deploy (OTA) -> Run
workflow**) or locally.

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

## GitHub Actions setup (one-time)

1. Create the `fleet-deploy` GitHub **environment** (Settings -> Environments)
   and add **required reviewers**: the job deploys to production hardware,
   and `workflow_dispatch` runs whichever ref's copy of the workflow and
   script was selected, so an approval gate is the only thing between a
   modified feature-branch script and the fleet. Store the secret below as
   an environment secret. Dispatch from `main` unless deliberately testing.
2. Create the IAM role the workflow assumes via OIDC and set the
   environment secret `AWS_FLEET_DEPLOY_ROLE_ARN` to its ARN. The role
   needs:

- trust policy for `token.actions.githubusercontent.com` pinned to this
  environment:
  `sub = repo:Jan-IngenHousz-Institute/ambyte-iot:environment:fleet-deploy`
- permissions:

```json
{
  "Version": "2012-10-17",
  "Statement": [
    { "Effect": "Allow",
      "Action": ["logs:StartQuery", "logs:GetQueryResults"],
      "Resource": "arn:aws:logs:eu-central-1:*:log-group:AWSIotLogsV2:*" },
    { "Effect": "Allow",
      "Action": ["iot:DescribeEndpoint"],
      "Resource": "*" },
    { "Effect": "Allow",
      "Action": ["iot:Connect"],
      "Resource": "arn:aws:iot:eu-central-1:*:client/fleet-deploy-*" },
    { "Effect": "Allow",
      "Action": ["iot:Publish"],
      "Resource": "arn:aws:iot:eu-central-1:*:topic/device/scripts/v1/Ambyte/2/*" },
    { "Effect": "Allow",
      "Action": ["iot:Subscribe"],
      "Resource": "arn:aws:iot:eu-central-1:*:topicfilter/experiment/data_ingest/v1/*" },
    { "Effect": "Allow",
      "Action": ["iot:Receive"],
      "Resource": "arn:aws:iot:eu-central-1:*:topic/experiment/data_ingest/v1/*" }
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
