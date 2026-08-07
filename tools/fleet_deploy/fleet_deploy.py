#!/usr/bin/env python3
"""Fleet OTA deploy: targeted, staged firmware rollouts over AWS IoT.

One MQTT publish per device (there is no broadcast topic and IoT Jobs is not
wired into the firmware): {"type":"ota_update","id":"fw-<tag>","url":...} to
device/scripts/v1/Ambyte/2/AMBYTE_<MAC>. The device downloads over HTTPS into
its spare OTA slot, reboots, and self-confirms or auto-rolls-back (~300 s),
so a broken image cannot brick the fleet. NVS (provisioning, cursors) is
never touched by OTA.

Targeting model (the four rollout shapes this tool exists for):
  --version-op any                                      -> all active devices
  --version-op eq  --version 1.0.5                      -> all ON that version
  --version-op any --percentage 30                      -> 30% of all devices
  --version-op lt  --version 1.2.0 --percentage 30      -> 30% BELOW 1.2.0
  (gt / gte / lte complete the comparator set)

Device universe: clientIds seen publishing to IoT Core within the discovery
window (CloudWatch Logs Insights on AWSIotLogsV2), or an explicit --devices
list. Versions come from a fleet ping: every device answers pong with a `fw`
string.

FLEET REALITY (2026-08): on every image fielded so far, pong.fw echoes the
NVS `firmware_ver` PROVISIONING string (written at USB flash time, never by
OTA), so it reflects what was flashed, not what is running. The firmware fix
shipping alongside this tool makes pong report the compiled running version.
Until a device runs a release containing that fix, treat its reported
version as flash-time truth: good enough to select never-OTA'd devices
(lt/lte against old strings like "1"/"1.0.2"), but the already-up-to-date
skip and eq/gt filters only become reliable on post-fix firmware.

Percentage selection is DETERMINISTIC over a fixed population: candidates
are ordered by sha256(clientId) and the first ceil(N * pct/100) are taken,
so re-running with a higher percentage yields a superset of the earlier
cohort. The population itself can drift between runs (devices going
online/offline, versions changing), which shifts the slice; for a strictly
frozen canary, re-target the exact cohort via --devices with the list from
the previous run's results.json.

Timing: `accepted` is reported before the firmware's fleet-jitter wait, so
acks are prompt; the download itself is spread across 900 one-second
MAC-derived slots (release builds v1.2.0+), so terminals can take ~15 min +
download + reboot + a <=300 s self-confirm. Hence the generous default
--final-seconds.
"""

import argparse
import hashlib
import json
import math
import os
import queue
import re
import sys
import time
import urllib.request

COMMAND_TOPIC_FMT = "device/scripts/v1/Ambyte/2/{device}"
STATUS_TOPIC = "experiment/data_ingest/v1/+/multispeq/v1.0/+/status"
LOG_GROUP = "AWSIotLogsV2"
CLIENT_ID_RE = re.compile(r"^AMBYTE_[0-9A-F]{2}(:[0-9A-F]{2}){5}$")

# Terminal OTA states (components/ota_update/ota_update.c ota_report calls).
# "dropped" = rejected before acceptance (busy / OOM / maintenance), a retry
# candidate rather than a failure. An already-applied id is silently ignored
# by the device, which is why already-on-target units must be skipped up
# front.
TERMINAL_STATES = {"success", "failed", "dropped"}


# -- version handling --------------------------------------------------------
# The fleet reports semver-ish strings: "1", "1.0.5", "1.0.6-rc1", "1.2.0".
# Prerelease sorts BEFORE its release (1.0.6-rc1 < 1.0.6), per semver.

def parse_version(s):
    """-> ((major, minor, patch), prerelease_ids) or None if unparseable."""
    if not s:
        return None
    core, _, pre = s.strip().lstrip("vV").partition("-")
    try:
        nums = [int(x) for x in core.split(".")]
    except ValueError:
        return None
    if not 1 <= len(nums) <= 3:
        return None
    nums = tuple(nums + [0] * (3 - len(nums)))
    return (nums, tuple(pre.split(".")) if pre else ())


def cmp_version(a, b):
    """semver compare of two parse_version() results -> -1 / 0 / 1."""
    if a[0] != b[0]:
        return -1 if a[0] < b[0] else 1
    ap, bp = a[1], b[1]
    if ap == bp:
        return 0
    if not ap:
        return 1          # release > its prerelease
    if not bp:
        return -1
    for x, y in zip(ap, bp):
        if x == y:
            continue
        # numeric identifiers compare numerically and rank below alphanumeric
        xn, yn = x.isdigit(), y.isdigit()
        if xn and yn:
            return -1 if int(x) < int(y) else 1
        if xn != yn:
            return -1 if xn else 1
        return -1 if x < y else 1
    return -1 if len(ap) < len(bp) else 1


VERSION_OPS = {
    "eq":  lambda c: c == 0,
    "lt":  lambda c: c < 0,
    "lte": lambda c: c <= 0,
    "gt":  lambda c: c > 0,
    "gte": lambda c: c >= 0,
}


def normalize_device(s):
    """Accept 'AMBYTE_<MAC>' or a bare MAC; -> canonical clientId or None."""
    s = s.strip().upper()
    if not s:
        return None
    if not s.startswith("AMBYTE_"):
        s = "AMBYTE_" + s
    return s if CLIENT_ID_RE.match(s) else None


def selection_key(client_id):
    """Stable pseudo-random order for percentage slicing. No salt, on
    purpose: a later, larger percentage must be a superset of the earlier
    cohort."""
    return hashlib.sha256(client_id.encode()).hexdigest()


# -- AWS plumbing -------------------------------------------------------------

def boto_session(profile, region):
    import boto3
    return boto3.Session(profile_name=profile, region_name=region)


def discover_active_devices(session, window_minutes):
    """clientIds that published to IoT Core within the window, via the
    CloudWatch query Ludovico's fleet tooling established (AWSIotLogsV2 must
    have Publish-In logging enabled; it does on this account)."""
    logs = session.client("logs")
    now = int(time.time())
    start = logs.start_query(
        logGroupName=LOG_GROUP,
        startTime=now - window_minutes * 60,
        endTime=now,
        queryString=(
            "fields @timestamp, clientId"
            ' | filter eventType = "Publish-In"'
            " | stats count() as publishCount by clientId"
            " | sort clientId asc"
        ),
    )
    while True:
        resp = logs.get_query_results(queryId=start["queryId"])
        if resp["status"] in ("Complete", "Failed", "Cancelled", "Timeout"):
            break
        time.sleep(1)
    if resp["status"] != "Complete":
        raise RuntimeError(f"CloudWatch Insights query {resp['status']}")
    devices = []
    for row in resp["results"]:
        fields = {c["field"]: c["value"] for c in row}
        dev = normalize_device(fields.get("clientId") or "")
        if dev:
            devices.append(dev)
    return sorted(set(devices))


def mqtt_connection(session, sub_topic, on_message, client_id="fleet-deploy"):
    """MQTT-over-WebSocket to IoT Core with SigV4; no device certs needed.

    Subscribes to `sub_topic` (blocking on SUBACK) before returning, and
    RE-subscribes after any reconnect: with clean_session=True the broker
    forgets the subscription on disconnect, and awscrt does not restore it,
    so without this hook a mid-campaign network blip would silently drop
    every later status report while the run kept looking healthy."""
    from awscrt import auth, mqtt
    from awsiot import mqtt_connection_builder

    def on_interrupted(connection, error, **kwargs):
        print(f"  (mqtt interrupted: {error}; auto-reconnecting)")

    def on_resumed(connection, return_code, session_present, **kwargs):
        if not session_present:
            connection.subscribe(topic=sub_topic,
                                 qos=mqtt.QoS.AT_LEAST_ONCE,
                                 callback=on_message)
            print("  (mqtt resumed; re-subscribed)")

    frozen = session.get_credentials().get_frozen_credentials()
    creds = auth.AwsCredentialsProvider.new_static(
        access_key_id=frozen.access_key,
        secret_access_key=frozen.secret_key,
        session_token=frozen.token,
    )
    endpoint = session.client("iot").describe_endpoint(
        endpointType="iot:Data-ATS")["endpointAddress"]
    conn = mqtt_connection_builder.websockets_with_default_aws_signing(
        endpoint=endpoint,
        region=session.region_name,
        credentials_provider=creds,
        client_id=f"{client_id}-{os.getpid()}-{int(time.time())}",
        clean_session=True,
        keep_alive_secs=30,
        on_connection_interrupted=on_interrupted,
        on_connection_resumed=on_resumed,
    )
    conn.connect().result()
    # SUBACK before the caller publishes: a fast reply must not slip past.
    sub, _ = conn.subscribe(topic=sub_topic, qos=mqtt.QoS.AT_LEAST_ONCE,
                            callback=on_message)
    sub.result()
    return conn


def device_from_status_topic(topic):
    """experiment/data_ingest/v1/<uuid>/multispeq/v1.0/<clientId>/status:
    the topic's clientId is the reliable per-device key; the payload's
    device_id is NOT unique across the fleet."""
    parts = topic.split("/")
    return parts[-2] if len(parts) >= 2 else None


# -- fleet operations (single connection, subscribe-before-publish) ----------

def fleet_ping(session, devices, wait_seconds):
    """-> {clientId: fw_string_or_None}; devices absent = silent."""
    from awscrt import mqtt

    ping_id = f"deploy-ping-{int(time.time())}"
    payload = json.dumps({"type": "ping", "id": ping_id})
    received = queue.Queue()
    alive = {}

    conn = mqtt_connection(
        session, STATUS_TOPIC,
        lambda topic, payload, dup, qos, retain, **kw:
            received.put((topic, payload.decode("utf-8", "replace"))))
    try:
        pubs = [conn.publish(topic=COMMAND_TOPIC_FMT.format(device=d),
                             payload=payload, qos=mqtt.QoS.AT_LEAST_ONCE)[0]
                for d in devices]
        for p in pubs:
            p.result()
        deadline = time.time() + wait_seconds
        while len(alive) < len(devices) and time.time() < deadline:
            try:
                topic, msg = received.get(timeout=0.2)
            except queue.Empty:
                continue
            try:
                data = json.loads(msg)
            except json.JSONDecodeError:
                continue
            if not isinstance(data, dict) or data.get("type") != "pong":
                continue
            if data.get("id") != ping_id:
                continue
            dev = device_from_status_topic(topic)
            if dev in devices and dev not in alive:
                alive[dev] = data.get("fw")
                print(f"  pong  {dev}  fw={data.get('fw')}")
    finally:
        conn.disconnect().result()
    return alive


def fleet_ota(session, devices, campaign_id, url,
              ack_seconds, final_seconds, batch_size, stagger_seconds):
    """Fan one ota_update out and track every device to a terminal state.
    -> ({clientId: {"ack": bool, "state": str|None, "detail": str|None,
                    "fw": str|None}}, error_string_or_None)

    Never raises after fan-out has begun: once commands may have reached
    live devices, partial results MUST survive to the report, so errors are
    captured and returned instead."""
    from awscrt import mqtt

    payload = json.dumps({"type": "ota_update", "id": campaign_id, "url": url})
    received = queue.Queue()
    acked = {d: False for d in devices}
    final = {d: None for d in devices}
    error = None

    conn = mqtt_connection(
        session, STATUS_TOPIC,
        lambda topic, payload, dup, qos, retain, **kw:
            received.put((topic, payload.decode("utf-8", "replace"))))
    try:
        # Fan out in waves. Release builds v1.2.0+ self-stagger their
        # download by a MAC slot, but pre-jitter firmware starts downloading
        # immediately; batching protects a choked shared uplink from N
        # simultaneous ~2 MB downloads.
        pubs = []
        for i, dev in enumerate(devices):
            pub, _ = conn.publish(topic=COMMAND_TOPIC_FMT.format(device=dev),
                                  payload=payload, qos=mqtt.QoS.AT_LEAST_ONCE)
            pubs.append(pub)
            if batch_size and (i + 1) % batch_size == 0 and (i + 1) < len(devices):
                for p in pubs:
                    p.result()
                pubs = []
                print(f"  ...fanned out {i + 1}/{len(devices)}, "
                      f"pausing {stagger_seconds}s")
                time.sleep(stagger_seconds)
        for p in pubs:
            p.result()
        print(f"Fanned out campaign '{campaign_id}' to {len(devices)} device(s)")

        start = time.time()
        ack_deadline = start + ack_seconds
        final_deadline = start + final_seconds

        def pending():
            # Inside the ack window, or once anything has been accepted,
            # wait for every missing terminal (the device-side `accepted`
            # send is best-effort, so a late success can arrive from a
            # device we never saw ack). Only when NOTHING acked by the ack
            # deadline do we declare the cohort unreachable and stop early.
            if any(final[d] is None for d in devices):
                return time.time() < ack_deadline or any(acked.values())
            return False

        while time.time() < final_deadline and pending():
            try:
                topic, msg = received.get(timeout=0.2)
            except queue.Empty:
                continue
            try:
                data = json.loads(msg)
            except json.JSONDecodeError:
                continue
            if not isinstance(data, dict) or data.get("type") != "ota_status":
                continue
            if data.get("id") != campaign_id:
                continue
            dev = device_from_status_topic(topic)
            if dev not in acked:
                continue
            state = data.get("state")
            if state == "accepted":
                if not acked[dev]:
                    acked[dev] = True
                    print(f"  ack      {dev}")
            elif state in TERMINAL_STATES:
                if state != "dropped":
                    acked[dev] = True  # success/failed imply acceptance
                if final[dev] is None:
                    final[dev] = data
                    detail = f" ({data['detail']})" if data.get("detail") else ""
                    print(f"  {state:<8} {dev}"
                          f"{' fw=' + data['fw'] if data.get('fw') else ''}{detail}")
    except Exception as e:                     # capture, never lose partials
        error = f"{type(e).__name__}: {e}"
        print(f"  ERROR mid-campaign: {error}")
    finally:
        try:
            conn.disconnect().result()
        except Exception:
            pass

    results = {d: {"ack": acked[d],
                   "state": (final[d] or {}).get("state"),
                   "detail": (final[d] or {}).get("detail"),
                   "fw": (final[d] or {}).get("fw")}
               for d in devices}
    return results, error


# -- release asset ------------------------------------------------------------

def verify_firmware_url(url):
    """Fetch the first bytes and require the ESP32 app-image magic (0xE9).
    Catches the classic /blob/-URL-serves-HTML failure before the fleet does."""
    req = urllib.request.Request(url, headers={"Range": "bytes=0-15",
                                               "User-Agent": "ambyte-fleet-deploy"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        head = resp.read(16)
    if not head or head[0] != 0xE9:
        raise RuntimeError(
            f"{url} does not serve an ESP32 app image (first byte "
            f"{head[:1].hex() or 'none'}, expected e9); use the "
            "releases/download/<tag>/firmware.bin asset link")


# -- reporting ----------------------------------------------------------------

def classify(rec):
    if rec["state"] == "success":
        return "succeeded"
    if rec["state"] == "failed":
        return "failed"
    if rec["state"] == "dropped":
        return "dropped"
    if rec["ack"]:
        return "accepted_no_final"
    return "no_reply"


def deployment_failure_reason(rec, expected_version):
    """Return why a commanded device lacks exact terminal proof, else None."""
    outcome = classify(rec)
    if outcome != "succeeded":
        return outcome
    reported = parse_version(rec.get("fw") or "")
    if expected_version is not None and (
        reported is None or cmp_version(reported, expected_version) != 0
    ):
        return "target_version_not_confirmed"
    return None


def write_summary(path, lines):
    if not path:
        return
    with open(path, "a", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", default="Jan-IngenHousz-Institute/ambyte-iot",
                    help="owner/name whose release asset to deploy")
    ap.add_argument("--tag", required=True,
                    help="release tag (e.g. v1.2.0); firmware URL and campaign "
                         "id derive from it")
    ap.add_argument("--url", default=None,
                    help="override the firmware.bin URL (default: the --tag "
                         "release asset)")
    ap.add_argument("--version-op", choices=["any"] + sorted(VERSION_OPS),
                    default="any",
                    help="restrict to devices whose reported fw compares to "
                         "--version like this")
    ap.add_argument("--version", default=None,
                    help="reference version for --version-op (e.g. 1.0.5)")
    ap.add_argument("--percentage", type=int, default=100,
                    help="deterministic slice of the matching devices (1-100)")
    ap.add_argument("--devices", default=None,
                    help="comma/space-separated clientIds or MACs; skips "
                         "CloudWatch discovery")
    ap.add_argument("--window-minutes", type=int, default=1440,
                    help="discovery lookback for active devices")
    ap.add_argument("--profile", default=None, help="AWS profile (local runs)")
    ap.add_argument("--region", default="eu-central-1")
    ap.add_argument("--ping-wait", type=int, default=25)
    ap.add_argument("--ack-seconds", type=int, default=90)
    ap.add_argument("--final-seconds", type=int, default=2100,
                    help="covers the firmware's 0-899 s MAC jitter + download "
                         "on a degraded link + reboot + <=300 s self-confirm")
    ap.add_argument("--batch", type=int, default=10)
    ap.add_argument("--stagger", type=int, default=30)
    ap.add_argument("--allow-downgrade", action="store_true",
                    help="also command devices whose reported fw is NEWER "
                         "than the release (skipped by default: the firmware "
                         "applies any image it is given)")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the plan, publish nothing")
    ap.add_argument("--results-json", default=None)
    args = ap.parse_args()

    if not 1 <= args.percentage <= 100:
        ap.error("--percentage must be 1-100")
    ref_version = None
    if args.version_op != "any":
        ref_version = parse_version(args.version or "")
        if ref_version is None:
            ap.error(f"--version-op {args.version_op} needs a parseable "
                     "--version (got {!r})".format(args.version))

    release_version = parse_version(args.tag)
    url = args.url or (f"https://github.com/{args.repo}/releases/download/"
                       f"{args.tag}/firmware.bin")
    campaign_id = f"fw-{args.tag}"
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")

    session = boto_session(args.profile, args.region)

    # 1) device universe
    if args.devices:
        universe, bad = [], []
        for tok in re.split(r"[\s,]+", args.devices):
            if not tok:
                continue
            dev = normalize_device(tok)
            (universe if dev else bad).append(dev or tok)
        if bad:
            ap.error(f"unrecognized device tokens: {bad}")
        universe = sorted(set(universe))
        print(f"Explicit device list: {len(universe)} device(s)")
    else:
        print(f"Discovering devices active in the last "
              f"{args.window_minutes} min ...")
        universe = discover_active_devices(session, args.window_minutes)
        print(f"  {len(universe)} active device(s)")
    if not universe:
        print("No devices to target.")
        return 0

    # 2) versions via fleet ping (also a liveness check)
    print(f"Pinging {len(universe)} device(s) "
          f"(up to {args.ping_wait}s) ...")
    fw_map = fleet_ping(session, universe, args.ping_wait)
    silent = [d for d in universe if d not in fw_map]

    # 3) version predicate. With 'any', silent devices stay in: the publish
    #    is cheap and harmless, and they may still hear the OTA.
    if args.version_op == "any":
        matching = list(universe)
    else:
        op = VERSION_OPS[args.version_op]
        matching, unproven = [], []
        for d in universe:
            v = parse_version(fw_map.get(d) or "")
            if v is None:
                unproven.append(d)   # silent or unparseable fw: can't prove
            elif op(cmp_version(v, ref_version)):
                matching.append(d)
        if unproven:
            print(f"  {len(unproven)} device(s) excluded (no provable "
                  f"version): {', '.join(unproven)}")

    # 4) deterministic percentage slice, over the FULL matching population,
    #    so re-runs keep the cohort frozen (see module docstring)
    matching.sort(key=selection_key)
    take = math.ceil(len(matching) * args.percentage / 100)
    cohort = sorted(matching[:take])

    # 5) skip units already on the release version (their OTA latch would
    #    silently ignore the command and they'd read as timeouts) and, unless
    #    explicitly allowed, units reporting a NEWER version: the firmware
    #    has no version check and would happily apply the downgrade.
    #    Both skips only see what pong reports; on pre-fix firmware that is
    #    the flash-time provisioning string (see module docstring).
    to_deploy, up_to_date, newer = [], [], []
    for d in cohort:
        v = parse_version(fw_map.get(d) or "")
        if release_version and v and cmp_version(v, release_version) == 0:
            up_to_date.append(d)
        elif (release_version and v and cmp_version(v, release_version) > 0
              and not args.allow_downgrade):
            newer.append(d)
        else:
            to_deploy.append(d)
    if newer:
        print(f"  {len(newer)} device(s) report a NEWER fw than {args.tag}; "
              f"skipped (pass --allow-downgrade to override): "
              + ", ".join(newer))

    print(f"\nPlan: release {args.tag}  campaign '{campaign_id}'")
    print(f"  universe {len(universe)} | matching {len(matching)} | "
          f"cohort {args.percentage}% -> {len(cohort)} | "
          f"already up to date {len(up_to_date)} | "
          f"newer-skipped {len(newer)} | deploying {len(to_deploy)}")
    for d in to_deploy:
        print(f"    {d}  fw={fw_map.get(d) or 'silent'}")

    plan = {
        "tag": args.tag, "campaign_id": campaign_id, "url": url,
        "version_op": args.version_op, "version": args.version,
        "percentage": args.percentage, "dry_run": args.dry_run,
        "universe": len(universe),
        "matching": len(matching),
        "cohort": cohort,
        "up_to_date": up_to_date,
        "newer_skipped": newer,
        "silent_on_ping": silent,
        "fw_map": fw_map,
        "results": {},
        "error": None,
    }

    failed = []
    if args.dry_run:
        print("\nDRY RUN: nothing published.")
    elif to_deploy:
        verify_firmware_url(url)
        print(f"\nDeploying to {len(to_deploy)} device(s) ...")
        results, error = fleet_ota(session, to_deploy, campaign_id, url,
                                   args.ack_seconds, args.final_seconds,
                                   args.batch, args.stagger)
        plan["results"] = results
        plan["error"] = error
        failed = [
            d
            for d, record in results.items()
            if deployment_failure_reason(record, release_version) is not None
        ]
    elif not matching:
        print("\nNo devices match the version filter; nothing to deploy.")
    elif up_to_date and not newer:
        print("\nNothing to deploy (cohort already up to date).")
    else:
        print("\nNothing to deploy (cohort is up to date or newer-skipped).")

    # reporting
    if args.results_json:
        with open(args.results_json, "w", encoding="utf-8") as f:
            json.dump(plan, f, indent=2, sort_keys=True)
    lines = [
        f"## Fleet deploy: {args.tag} "
        f"({'dry run' if args.dry_run else 'live'})",
        "",
        f"- Targeting: `{args.version_op}"
        + (f" {args.version}" if args.version_op != "any" else "")
        + f"` at **{args.percentage}%**",
        f"- Universe {len(universe)} -> matching {len(matching)} -> "
        f"cohort {len(cohort)} (up-to-date {len(up_to_date)}, "
        f"deploying {len(to_deploy)})",
        "",
        "| device | fw before | outcome | detail |",
        "|---|---|---|---|",
    ]
    for d in cohort:
        if d in up_to_date:
            outcome, detail = "already up to date", ""
        elif d in newer:
            outcome, detail = "skipped (newer fw)", ""
        elif args.dry_run:
            outcome, detail = "would deploy", ""
        else:
            rec = plan["results"].get(d, {})
            outcome = classify(rec) if rec else "not attempted"
            detail = rec.get("detail") or ""
            if rec.get("fw"):
                detail = (detail + f" now fw={rec['fw']}").strip()
        lines.append(f"| {d} | {fw_map.get(d) or 'silent'} | {outcome} | {detail} |")
    if silent:
        lines += ["", f"Silent on ping ({len(silent)}): " + ", ".join(silent)]
    if plan["error"]:
        lines += ["", f"**Campaign error (tracking incomplete): "
                      f"{plan['error']}**"]
    write_summary(summary_path, lines)

    if plan["error"]:
        print(f"\nCampaign error: {plan['error']}")
        return 1
    if failed:
        details = ", ".join(
            f"{device} ({deployment_failure_reason(plan['results'][device], release_version)})"
            for device in failed
        )
        print(f"\n{len(failed)} device(s) lack exact terminal proof: {details}")
        return 1
    print("\nDone.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
