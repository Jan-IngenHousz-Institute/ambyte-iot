#!/usr/bin/env python3
"""Send a device command to a set of Ambytes and collect the replies.

Companion to fleet_deploy.py for the read-mostly operations an OTA campaign
needs around it: `ping` (firmware check), `evlog_inventory` (what the SD
archive holds) and `evlog_replay` (count / status / run / cancel of an archive
re-send). Same transport (SigV4 websocket MQTT), same discovery, same exact
client-id spelling rules: `ambyte_` and `AMBYTE_` are different MQTT routes.

    fleet_command.py ping --devices ambyte_28:37:2F:FF:FC:80 --expect-fw 2.3.1
    fleet_command.py inventory --devices ... --from-id 966 --to-id 19559
    fleet_command.py replay --mode status --devices ... --token sep16-fffc80
    fleet_command.py replay --mode count --devices ... --token t --from-id A --to-id B
    fleet_command.py replay --mode run   --devices ... --token t --from-id A --to-id B --chunk 64

Replies are written to --results-json (per device) and summarised on stdout.
Exit status: 0 when every targeted device replied (and, for ping with
--expect-fw, every reply carries that firmware); 1 otherwise; 2 on bad input.
Never publishes retained messages. `replay --mode run` and `cancel` change
device state; everything else is read-only.
"""
import argparse
import json
import queue
import sys
import time

try:
    import fleet_deploy
except ImportError:
    from . import fleet_deploy

READ_ONLY_MODES = ("count", "status")
WINDOW_KEYS = ("from_id", "to_id", "from_ms", "to_ms")


def parse_devices(spec):
    out = []
    for tok in spec.replace(",", " ").split():
        dev = fleet_deploy.normalize_device(tok)
        if dev is None:
            raise SystemExit(f"invalid device id: {tok!r}")
        out.append(dev)
    return fleet_deploy.unique_devices(out)


def build_message(args, req_id):
    """Command payload for one device. Pure, so it can be unit-tested."""
    if args.cmd == "ping":
        return {"type": "ping", "id": req_id}
    if args.cmd == "inventory":
        msg = {"type": "evlog_inventory", "id": req_id, "list": True}
        if args.from_id or args.to_id:
            msg.update({"from_id": args.from_id, "to_id": args.to_id})
        return msg
    if args.cmd == "replay":
        if not args.token:
            raise SystemExit("replay needs --token")
        msg = {"type": "evlog_replay", "id": req_id, "mode": args.mode, "token": args.token}
        if args.mode in ("count", "run"):
            window = {k: getattr(args, k) for k in WINDOW_KEYS if getattr(args, k)}
            if not window:
                raise SystemExit("replay count/run needs a window (--from-id/--to-id or --from-ms/--to-ms)")
            if args.mode == "run" and not (args.to_id or args.to_ms):
                raise SystemExit("replay run refuses an unbounded window; give --to-id or --to-ms")
            msg.update(window)
            if args.mode == "run":
                msg["chunk"] = args.chunk
        return msg
    raise SystemExit(f"unknown command {args.cmd}")


def version_tuple(s):
    try:
        return tuple(int(x) for x in str(s).split("."))
    except (TypeError, ValueError):
        return ()


def summarise(dev, reply):
    t = reply.get("type")
    if t == "pong":
        return f"pong fw={reply.get('fw')} uptime_ms={reply.get('uptime_ms')}"
    if t == "evlog_inventory":
        ar = reply.get("archive", {}) or {}
        st = reply.get("store", {}) or {}
        return (f"inventory ok={reply.get('ok')} sd={reply.get('sd_mounted')} files={ar.get('files')} "
                f"records={ar.get('records')} in_window={ar.get('in_window')} ids={ar.get('min_id')}..{ar.get('max_id')} "
                f"free={st.get('free_bytes')} pending={st.get('pending')} detail={reply.get('detail', '')}")
    if t == "evlog_replay_result":
        return (f"replay mode={reply.get('mode')} ok={reply.get('ok')} state={reply.get('state')} "
                f"detail={reply.get('detail')} matched={reply.get('matched')} appended={reply.get('appended')} "
                f"next_id={reply.get('next_id')}")
    return f"{t} {json.dumps(reply)[:160]}"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["ping", "inventory", "replay", "ota"])
    ap.add_argument("--url", default="", help="dev-bench OTA only; HTTPS URL (never printed)")
    ap.add_argument("--devices", default="", help="exact client ids or MACs, comma/space separated; empty = discover")
    ap.add_argument("--window-minutes", type=int, default=1440, help="discovery lookback when --devices is empty")
    ap.add_argument("--mode", default="status", choices=["count", "run", "cancel", "status"])
    ap.add_argument("--token", default="")
    ap.add_argument("--from-id", dest="from_id", type=int, default=0)
    ap.add_argument("--to-id", dest="to_id", type=int, default=0)
    ap.add_argument("--from-ms", dest="from_ms", type=int, default=0)
    ap.add_argument("--to-ms", dest="to_ms", type=int, default=0)
    ap.add_argument("--chunk", type=int, default=64)
    ap.add_argument("--expect-fw", default="", help="ping only: fail unless every reply reports this firmware")
    ap.add_argument("--wait", type=int, default=60, help="seconds to wait for replies")
    ap.add_argument("--profile", default=None)
    ap.add_argument("--region", default="eu-central-1")
    ap.add_argument("--results-json", default=None)
    ap.add_argument("--dry-run", action="store_true", help="print the plan and the payload, publish nothing")
    args = ap.parse_args(argv)

    if args.cmd == "ota":
        # No discovery and no profile-name trust: resolve STS before publishing.
        devices = parse_devices(args.devices)
        if len(devices) != 1 or not args.devices.strip():
            ap.error("dev-bench OTA requires exactly one explicit --devices target")
        if not args.url.startswith("https://") or len(args.url) >= 2048:
            ap.error("OTA URL must be HTTPS and shorter than 2048 characters")
        if not args.token or len(args.token) >= 72:
            ap.error("dev-bench OTA needs a unique --token shorter than 72 characters")
        session = fleet_deploy.boto_session(args.profile, args.region)
        if session.client("sts").get_caller_identity()["Account"] != "084375565727":
            ap.error("OTA transport is dev-bench only; production accounts are refused")
        print(f"dev-bench OTA: target={devices[0]} id={args.token} url_length={len(args.url)}")
        if args.dry_run:
            return 0
        results, error, _ = fleet_deploy.fleet_ota(session, devices, args.token, args.url,
                                                90, args.wait, 1, 0)
        report = {"results": results, "error": error}
        if args.results_json:
            # No URL or credentials in durable diagnostics.
            with open(args.results_json, "w") as output:
                json.dump(report, output, indent=2)
        return 0 if not error and all(r.get("state") == "success" for r in results.values()) else 1

    session = fleet_deploy.boto_session(args.profile, args.region)
    if args.devices.strip():
        devices = parse_devices(args.devices)
    else:
        devices = fleet_deploy.unique_devices(fleet_deploy.discover_active_devices(session, args.window_minutes))
    if not devices:
        raise SystemExit("no target devices")
    req_id = f"fc-{args.cmd}-{args.mode if args.cmd == 'replay' else 'x'}-{int(time.time())}"
    payload = build_message(args, req_id)
    mutating = args.cmd == "replay" and args.mode not in READ_ONLY_MODES

    print(f"{args.cmd}{' ' + args.mode if args.cmd == 'replay' else ''}: {len(devices)} device(s), "
          f"request id {req_id}{' (MUTATING)' if mutating else ''}")
    print("payload:", json.dumps(payload))
    if args.dry_run:
        for d in devices:
            print("  ", d)
        return 0

    # Payload building, bench admission checks and mocked transports need no
    # AWS SDK. Match fleet_deploy's lazy dependency boundary for live MQTT.
    from awscrt import mqtt

    by_identity = fleet_deploy.device_index(devices)
    q = queue.Queue()
    # The environment roles may only connect as client/fleet-deploy-*; the
    # helper appends pid and time for uniqueness.
    conn = fleet_deploy.mqtt_connection(session, fleet_deploy.STATUS_TOPIC,
                                        lambda topic, payload, **kw: q.put((time.time(), topic, payload)),
                                        client_id="fleet-deploy-command")
    replies = {}
    try:
        for dev in devices:
            fut, _ = conn.publish(topic=fleet_deploy.COMMAND_TOPIC_FMT.format(device=dev),
                                  payload=json.dumps(payload), qos=mqtt.QoS.AT_LEAST_ONCE)
            fut.result(timeout=10)
            time.sleep(0.15)
        deadline = time.monotonic() + args.wait
        while time.monotonic() < deadline:
            try:
                ts, topic, raw = q.get(timeout=0.5)
            except queue.Empty:
                continue
            try:
                data = json.loads(raw)
            except ValueError:
                continue
            if data.get("id") != req_id:
                continue
            dev = fleet_deploy.requested_device_from_status_topic(topic, by_identity)
            if dev is None:
                continue
            data["_received"] = ts
            # keep the first terminal-looking reply; progress replies of a run keep coming
            if dev not in replies or data.get("detail") not in ("progress",):
                replies[dev] = data
            print(f"  {dev}: {summarise(dev, data)}", flush=True)
            if len(replies) >= len(devices) and not (args.cmd == "replay" and args.mode == "run"):
                break
    finally:
        conn.disconnect().result(timeout=10)

    missing = [d for d in devices if d not in replies]
    bad_fw = []
    if args.cmd == "ping" and args.expect_fw:
        want = version_tuple(args.expect_fw)
        bad_fw = [d for d, r in replies.items() if version_tuple(r.get("fw")) != want]
    fw_hist = {}
    for r in replies.values():
        if r.get("type") == "pong":
            fw_hist[r.get("fw")] = fw_hist.get(r.get("fw"), 0) + 1
    states = {}
    for r in replies.values():
        if r.get("type") == "evlog_replay_result":
            states[r.get("state")] = states.get(r.get("state"), 0) + 1

    print(f"replies {len(replies)}/{len(devices)}; no reply: {missing}")
    if fw_hist:
        print("firmware:", json.dumps(fw_hist, sort_keys=True))
    if states:
        print("replay states:", json.dumps(states, sort_keys=True))
    if bad_fw:
        print(f"not on {args.expect_fw}: {bad_fw}")

    if args.results_json:
        with open(args.results_json, "w") as fh:
            json.dump({"command": args.cmd, "mode": args.mode if args.cmd == "replay" else None,
                       "request_id": req_id, "payload": payload, "devices": devices,
                       "replies": replies, "no_reply": missing, "not_expected_fw": bad_fw,
                       "firmware": fw_hist, "replay_states": states}, fh, indent=1, sort_keys=True)
    return 1 if (missing or bad_fw) else 0


if __name__ == "__main__":
    sys.exit(main())
