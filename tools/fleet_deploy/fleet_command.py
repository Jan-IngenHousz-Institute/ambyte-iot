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
    fleet_command.py ambit-versions --devices ambyte_10:00:3B:72:22:38
    fleet_command.py ambit-flash --devices ... --version 1.4.0 --channel 0 \
        --manifest-url https://github.com/Jan-IngenHousz-Institute/ambit/releases/download/v1.4.0/manifest.json
    fleet_command.py log-list --devices ambyte_28:37:2F:FF:FC:80
    fleet_command.py log-read --devices ... --name ambyte.log --tail 65536 --out ambyte.log
    fleet_command.py log-read --devices ... --name ambyte.1.log --offset 0 --length 8192
    fleet_command.py power-protect --action status --devices ambyte_28:37:2F:FF:FC:80
    fleet_command.py power-protect --action force --state hold --devices ...
    fleet_command.py power-protect --action clear --devices ...
    fleet_command.py vin-limit --devices ambyte_28:37:2F:FF:FC:80
    fleet_command.py vin-limit --vin-action set --mv 4000 --devices ...
    fleet_command.py iin-limit --devices ambyte_28:37:2F:FF:FC:80
    fleet_command.py iin-limit --iin-action set --ma 1000 --devices ...

log-list / log-read (firmware with remote diagnostics) read the SD log
directory (/sdcard/logs) without changing anything on the device. One
log-read request returns at most 8192 bytes (base64 in `data`); --out stitches
consecutive chunks of one device's file into a local file until EOF.

power-protect (firmware with brownout protection, docs/power-protect.md) reads
(`status`) or changes (`clear`, `force --state no_flash|hold`) the device's
brownout protection state. clear and force are MUTATING and need an explicit
--devices list: a forced state never auto-exits, and clear wipes the brownout
history the next escalation would have used.

vin-limit (firmware >= 2.21.0, docs/charger-vin-limit.md) reads (`get`) or
changes (`set --mv N`) the MP2731 input voltage regulation threshold. set is
MUTATING and needs an explicit --devices list. It writes the live charger
register only: nothing is stored on the gateway, so the charger keeps the value
until it is set again or loses all power. A set is confirmed only when the
reply carries the requested millivolts and the matching REG01 readback; a lost
reply does not prove the write failed, so read the limit again before retrying.

iin-limit (firmware >= 2.23.0, docs/charger-iin-limit.md) reads (`get`) or
changes (`set --ma N`) the MP2731 input current limit REGISTER (REG00[5:0]).
Same rules as vin-limit: set is MUTATING, explicit --devices only, live
register only, confirmed only by a reply whose `ma` matches the request and
whose REG00 readback encodes it. The reply's `en_ilim` says whether the
board's ILIM-pin resistor also limits: then the lower of the two applies, so
the register value is a ceiling, not the input current. 1 A doubles the
charger's input power over the 500 mA default: mind the board's heat.

Replies are written to --results-json (per device) and summarised on stdout.
Exit status: 0 when every targeted device replied (and, for ping with
--expect-fw, every reply carries that firmware); 1 otherwise; 2 on bad input.
Never publishes retained messages. `replay --mode run` and `cancel` change
device state, and `ambit-flash` rewrites AMBIT firmware (the ROM-bootloader path,
the only one a pre-1.0 AMBIT has; with --manifest-url the Ambyte first downloads
and SHA-256-verifies that ambit-iot release onto its SD card). Everything else is
read-only. An ambit-flash pauses measurement and MQTT on the device for the
download plus ~10-60 s per channel; replies wait for its terminal report.
"""
import argparse
import base64
import json
import queue
import re
import sys
import time

try:
    import fleet_deploy
except ImportError:
    from . import fleet_deploy

READ_ONLY_MODES = ("count", "status")
WINDOW_KEYS = ("from_id", "to_id", "from_ms", "to_ms")
# log_read limits mirror components/command_router/include/log_command.h.
LOG_READ_DEFAULT = 4096
LOG_READ_MAX = 8192
LOG_NAME_RE = re.compile(r"[A-Za-z0-9_-][A-Za-z0-9._-]{0,63}")
# A busy/queue refusal can be a QoS 1 duplicate of a request whose real answer
# is still coming: never terminal on its own (same rule as the timezone commands).
NON_TERMINAL_DETAILS = ("busy", "queue_unavailable")
REPLY_TYPES = {"get-timezone": "timezone_result", "set-timezone": "set_timezone_result",
               "log-list": "log_list_result", "log-read": "log_read_result",
               "power-protect": "power_protect_result"}
# power_protect (components/command_router/include/power_protect_command.h).
POWER_PROTECT_ACTIONS = ("status", "clear", "force")
POWER_PROTECT_FORCE_STATES = ("no_flash", "hold")
# vin_limit: bounds mirror components/domain/include/sensing_port.h
# (POWER_VIN_LIMIT_MIN_MV / MAX_MV / STEP_MV); the firmware rejects anything
# else, checking here keeps a typo from costing a field round trip.
VIN_LIMIT_ACTIONS = ("get", "set")
VIN_LIMIT_MIN_MV = 3700
VIN_LIMIT_MAX_MV = 15200
VIN_LIMIT_STEP_MV = 100
VIN_LIMIT_REPLY = {"get": "vin_limit_result", "set": "set_vin_limit_result"}
# iin_limit: bounds mirror POWER_IIN_LIMIT_MIN_MA / MAX_MA / STEP_MA in the
# same header. REG00 bits 7:6 are EN_HIZ / EN_ILIM, which the firmware keeps.
IIN_LIMIT_ACTIONS = ("get", "set")
IIN_LIMIT_MIN_MA = 100
IIN_LIMIT_MAX_MA = 3250
IIN_LIMIT_STEP_MA = 50
IIN_LIMIT_REPLY = {"get": "iin_limit_result", "set": "set_iin_limit_result"}


def valid_vin_limit_mv(mv):
    return (isinstance(mv, int) and not isinstance(mv, bool)
            and VIN_LIMIT_MIN_MV <= mv <= VIN_LIMIT_MAX_MV
            and (mv - VIN_LIMIT_MIN_MV) % VIN_LIMIT_STEP_MV == 0)


def valid_iin_limit_ma(ma):
    return (isinstance(ma, int) and not isinstance(ma, bool)
            and IIN_LIMIT_MIN_MA <= ma <= IIN_LIMIT_MAX_MA
            and (ma - IIN_LIMIT_MIN_MA) % IIN_LIMIT_STEP_MA == 0)


def expected_reply_type(args):
    """Reply type that answers this command, or None when any reply does. Pure."""
    if args.cmd == "vin-limit":
        return VIN_LIMIT_REPLY[getattr(args, "vin_action", "get")]
    if args.cmd == "iin-limit":
        return IIN_LIMIT_REPLY[getattr(args, "iin_action", "get")]
    return REPLY_TYPES.get(args.cmd)


def vin_limit_problem(args, reply):
    """Why a VIN-limit reply does not show the requested outcome, or None. Pure."""
    action = getattr(args, "vin_action", "get")
    if reply.get("type") != VIN_LIMIT_REPLY[action] or reply.get("ok") is not True:
        return f"refused: {reply.get('detail')}"
    mv, reg01 = reply.get("mv"), reply.get("reg01")
    if not valid_vin_limit_mv(mv) or not isinstance(reg01, int) or isinstance(reg01, bool):
        return f"reply without a confirmed readback: mv={mv!r} reg01={reg01!r}"
    # REG01 bit 7 is REGISTER RESET; the limit lives in bits 6:0.
    if reg01 & 0x80 or VIN_LIMIT_MIN_MV + (reg01 & 0x7F) * VIN_LIMIT_STEP_MV != mv:
        return f"reg01={reg01:#04x} does not encode {mv} mV"
    if action == "set" and mv != args.mv:
        return f"reads {mv} mV, asked {args.mv} mV"
    return None


def iin_limit_problem(args, reply):
    """Why an IIN-limit reply does not show the requested outcome, or None. Pure."""
    action = getattr(args, "iin_action", "get")
    if reply.get("type") != IIN_LIMIT_REPLY[action] or reply.get("ok") is not True:
        return f"refused: {reply.get('detail')}"
    ma, reg00 = reply.get("ma"), reply.get("reg00")
    if (not valid_iin_limit_ma(ma) or not isinstance(reg00, int) or isinstance(reg00, bool)
            or not 0 <= reg00 <= 0xFF):
        return f"reply without a confirmed readback: ma={ma!r} reg00={reg00!r}"
    # REG00[5:0] holds the limit; bits 7:6 (HIZ, EN_ILIM) are reported state.
    if IIN_LIMIT_MIN_MA + (reg00 & 0x3F) * IIN_LIMIT_STEP_MA != ma:
        return f"reg00={reg00:#04x} does not encode {ma} mA"
    if action == "set" and ma != args.ma:
        return f"reads {ma} mA, asked {args.ma} mA"
    return None


def power_protect_problem(args, reply):
    """Why a power_protect_result does not show the requested outcome, or None. Pure."""
    if reply.get("type") != "power_protect_result" or reply.get("ok") is not True:
        return f"refused: {reply.get('detail')}"
    action = getattr(args, "action", "status")
    if reply.get("action") != action:
        return f"answered {reply.get('action')!r}, asked {action!r}"
    if action == "force" and (reply.get("state") != args.state or reply.get("forced") is not True):
        return f"state {reply.get('state')} forced={reply.get('forced')}, asked forced {args.state}"
    if action == "clear" and (reply.get("state") != "normal" or reply.get("forced") is not False):
        return f"state {reply.get('state')} forced={reply.get('forced')} after clear"
    return None


def valid_log_name(name):
    """Same allow-list as the firmware: a plain basename, no '/', no '..'."""
    return isinstance(name, str) and bool(LOG_NAME_RE.fullmatch(name)) and ".." not in name


def log_read_window(args):
    """(offset, length) of the first log_read request. Pure."""
    tail = getattr(args, "tail", 0) or 0
    offset = getattr(args, "offset", 0) or 0
    if tail < 0:
        raise SystemExit("--tail must be a positive byte count")
    if tail and offset:
        raise SystemExit("--tail and --offset are mutually exclusive")
    length = getattr(args, "length", 0) or (LOG_READ_MAX if getattr(args, "out", None) else LOG_READ_DEFAULT)
    if not 1 <= length <= LOG_READ_MAX:
        raise SystemExit(f"--length must be 1..{LOG_READ_MAX} bytes per request")
    return (-tail if tail else offset), length


def _is_count(v):
    return isinstance(v, int) and not isinstance(v, bool) and v >= 0


def log_reply_problem(reply):
    """Why an ok:true log_list/log_read reply cannot be trusted, or None. Pure.

    A device that ran out of memory mid-reply must never be taken for a
    complete answer (review finding 4): every listed file needs a valid name
    and a size, and read data must decode to exactly `length` bytes inside
    the reported file size.
    """
    if reply.get("ok") is not True:
        return None
    t = reply.get("type")
    if not isinstance(reply.get("gen"), str) or not reply.get("gen"):
        return "missing gen"
    if t == "log_list_result":
        files = reply.get("files")
        if not isinstance(files, list):
            return "files missing"
        for f in files:
            if not isinstance(f, dict) or not valid_log_name(f.get("name")) or not _is_count(f.get("size")):
                return f"incomplete file entry {f!r}"
        if not _is_count(reply.get("omitted")):
            return "omitted missing"
        return None
    if t == "log_read_result":
        if not valid_log_name(reply.get("name")):
            return "name missing"
        if not all(_is_count(reply.get(k)) for k in ("size", "offset", "length")):
            return "size/offset/length missing"
        if not isinstance(reply.get("eof"), bool) or reply.get("encoding") != "base64":
            return "eof/encoding missing"
        try:
            raw = base64.b64decode(reply.get("data"), validate=True)
        except (TypeError, ValueError) as exc:
            return f"data is not base64: {exc}"
        if len(raw) != reply["length"]:
            return f"data decodes to {len(raw)} bytes, reply says {reply['length']}"
        if reply["offset"] + reply["length"] > reply["size"]:
            return "chunk extends past the reported file size"
        if reply["eof"] != (reply["offset"] + reply["length"] >= reply["size"]):
            return "eof inconsistent with offset/length/size"
        if reply["length"] == 0 and not reply["eof"]:
            # Review round 2, finding 5: an empty chunk before EOF makes no
            # progress and must never complete a stitched read.
            return "empty chunk before EOF (no progress)"
        return None
    return f"unexpected type {t!r}"


class LogStitcher:
    """Joins consecutive log_read_result chunks of ONE file into bytes.

    Each reply is checked against the request it answers: the right name, the
    requested absolute offset (a negative/tail first request is resolved by the
    device), base64 that decodes to exactly `length` bytes, and a file that did
    not shrink between chunks.

    Rotation (review finding 5): sd_logger renames ambyte.log -> ambyte.1.log
    -> ... on its own schedule, so a basename can name different bytes between
    two chunks, even of the same size. The device reports `gen` (boot, SD
    mount, rotation sequence); the first chunk pins it and every later request
    carries it as `expect_gen`. A "file_rotated" refusal restarts the read once
    from the original offset; a second one fails the command. A reply whose
    gen differs from the pinned one is never accepted.
    """

    MIN_CHUNK = 1024
    RETRY_DETAILS = NON_TERMINAL_DETAILS + ("rotation_in_progress",)

    def __init__(self, name, offset, length, max_chunks=400, expect_gen=None):
        self.name, self.first_offset, self.next_offset, self.length = name, offset, offset, length
        self.max_chunks = max_chunks
        self.data = bytearray()
        self.start = self.size = self.error = None
        # An operator-supplied --expect-gen pins the identity from the very
        # first request and is never traded for another generation: a
        # mismatch fails, it does not restart (review round 2, finding 3).
        self.gen = expect_gen or None
        self.user_pinned = self.gen is not None
        self.chunks = 0
        self.restarts = 0
        self.done = False

    def request(self, req_id):
        msg = {"type": "log_read", "id": req_id, "name": self.name,
               "offset": self.next_offset, "length": self.length}
        if self.gen is not None:
            msg["expect_gen"] = self.gen
        return msg

    def _restart(self):
        self.data = bytearray()
        self.start = self.size = self.gen = None
        self.next_offset = self.first_offset
        self.chunks = 0
        self.restarts += 1

    def feed(self, reply):
        """Returns 'continue', 'retry' (same offset again), 'restart', 'done' or 'error'."""
        if reply.get("ok") is not True:
            detail = reply.get("detail")
            if detail == "no_mem" and self.length > self.MIN_CHUNK:
                self.length = max(self.MIN_CHUNK, self.length // 2)   # fragmented heap: smaller reply
                return "retry"
            if detail in self.RETRY_DETAILS:
                return "retry"
            if detail == "file_rotated" and self.restarts == 0 and not self.user_pinned:
                self._restart()
                return "restart"
            self.error = f"device refused: {detail}"
            return "error"
        if reply.get("type") != "log_read_result":
            self.error = f"unexpected reply type {reply.get('type')!r}"
            return "error"
        problem = log_reply_problem(reply)
        if problem:
            self.error = f"malformed reply: {problem}"
            return "error"
        offset, length, size = reply["offset"], reply["length"], reply["size"]
        raw = base64.b64decode(reply["data"], validate=True)
        if reply.get("name") != self.name:
            self.error = f"reply for {reply.get('name')!r}, asked for {self.name!r}"
        elif self.next_offset >= 0 and offset != self.next_offset:
            self.error = f"reply at offset {offset}, asked for {self.next_offset}"
        elif self.gen is not None and reply["gen"] != self.gen:
            self.error = f"{self.name} changed identity (gen {self.gen} -> {reply['gen']}) during the read"
        elif self.size is not None and size < self.size:
            self.error = f"{self.name} shrank from {self.size} to {size} bytes (rotated during the read)"
        if self.error:
            return "error"
        if self.start is None:
            self.start = offset
            self.gen = reply["gen"]
        self.size = size
        self.data += raw
        self.next_offset = offset + length
        self.chunks += 1
        if reply["eof"]:                          # only EOF completes a stitched read
            self.done = True
            return "done"
        if self.chunks >= self.max_chunks:
            self.error = f"gave up after {self.chunks} chunks"
            return "error"
        return "continue"


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
    if args.cmd == "get-timezone":
        return {"type": "get_timezone", "id": req_id}
    if args.cmd == "set-timezone":
        zone = args.timezone
        if not isinstance(zone, str) or len(zone) > 47 or not re.fullmatch(r"(?:[A-Za-z0-9_+-]+/)+[A-Za-z0-9_+-]+|UTC", zone):
            raise SystemExit("set-timezone needs --timezone with a supported IANA name, e.g. Australia/Darwin")
        return {"type": "set_timezone", "id": req_id, "timezone": zone}
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
    if args.cmd == "log-list":
        return {"type": "log_list", "id": req_id}
    if args.cmd == "log-read":
        if not valid_log_name(getattr(args, "name", "")):
            raise SystemExit("log-read needs --name: a plain file name from log-list (e.g. ambyte.log)")
        offset, length = log_read_window(args)
        msg = {"type": "log_read", "id": req_id, "name": args.name, "offset": offset, "length": length}
        if getattr(args, "expect_gen", ""):
            msg["expect_gen"] = args.expect_gen
        return msg
    if args.cmd == "power-protect":
        action = getattr(args, "action", "status")
        state = getattr(args, "state", "") or ""
        if action not in POWER_PROTECT_ACTIONS:
            raise SystemExit("power-protect --action must be status, clear or force")
        msg = {"type": "power_protect", "id": req_id, "action": action}
        if action == "force":
            if state not in POWER_PROTECT_FORCE_STATES:
                raise SystemExit("power-protect --action force needs --state no_flash or hold")
            msg["state"] = state
        elif state:
            raise SystemExit("power-protect --state is only valid with --action force")
        return msg
    if args.cmd == "vin-limit":
        action = getattr(args, "vin_action", "get")
        mv = getattr(args, "mv", None)
        if action not in VIN_LIMIT_ACTIONS:
            raise SystemExit("vin-limit --vin-action must be get or set")
        if action == "get":
            if mv is not None:
                raise SystemExit("vin-limit --mv is only valid with --vin-action set")
            return {"type": "get_vin_limit", "id": req_id}
        if not valid_vin_limit_mv(mv):
            raise SystemExit(f"vin-limit set needs --mv: whole millivolts {VIN_LIMIT_MIN_MV}..{VIN_LIMIT_MAX_MV} "
                             f"in {VIN_LIMIT_STEP_MV} mV steps (e.g. 4000)")
        return {"type": "set_vin_limit", "id": req_id, "mv": mv}
    if args.cmd == "iin-limit":
        action = getattr(args, "iin_action", "get")
        ma = getattr(args, "ma", None)
        if action not in IIN_LIMIT_ACTIONS:
            raise SystemExit("iin-limit --iin-action must be get or set")
        if action == "get":
            if ma is not None:
                raise SystemExit("iin-limit --ma is only valid with --iin-action set")
            return {"type": "get_iin_limit", "id": req_id}
        if not valid_iin_limit_ma(ma):
            raise SystemExit(f"iin-limit set needs --ma: whole milliamps {IIN_LIMIT_MIN_MA}..{IIN_LIMIT_MAX_MA} "
                             f"in {IIN_LIMIT_STEP_MA} mA steps (e.g. 1000)")
        return {"type": "set_iin_limit", "id": req_id, "ma": ma}
    if args.cmd == "ambit-versions":
        return {"type": "ambit_versions", "id": req_id}
    if args.cmd == "ambit-flash":
        if not re.fullmatch(r"\d{1,3}\.\d{1,3}\.\d{1,3}", args.version or ""):
            raise SystemExit("ambit-flash needs --version M.m.b")
        msg = {"type": "ambit_flash", "id": req_id, "version": args.version}
        if args.channel != "all":
            if not args.channel.isdigit() or int(args.channel) > 3:
                raise SystemExit("--channel must be 0-3 or all")
            msg["channel"] = int(args.channel)
        else:
            msg["channel"] = "all"
        if args.manifest_url:
            if not args.manifest_url.startswith("https://") or len(args.manifest_url) >= 256:
                raise SystemExit("--manifest-url must be https:// and shorter than 256 characters")
            msg["manifest_url"] = args.manifest_url
        return msg
    raise SystemExit(f"unknown command {args.cmd}")


# ambit_flash_status states that end a request (accepted/fetched/flash_ok/... are progress).
AMBIT_FLASH_TERMINAL = ("success", "failed", "busy")


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
    if t in ("timezone_result", "set_timezone_result"):
        return (f"timezone={reply.get('timezone')} active={reply.get('active_timezone')} "
                f"offset_s={reply.get('utc_offset_seconds')} applied={reply.get('applied')} "
                f"ok={reply.get('ok')} detail={reply.get('detail')}")
    if t == "log_list_result":
        files = " ".join(f"{f.get('name')}={f.get('size')}" for f in reply.get("files") or [])
        return (f"log_list ok={reply.get('ok')} detail={reply.get('detail')} dir={reply.get('dir')} "
                f"files: {files or '-'}" + (f" omitted={reply.get('omitted')}" if reply.get("omitted") else ""))
    if t == "log_read_result":
        return (f"log_read ok={reply.get('ok')} detail={reply.get('detail')} name={reply.get('name')} "
                f"gen={reply.get('gen')} offset={reply.get('offset')} length={reply.get('length')} "
                f"size={reply.get('size')} eof={reply.get('eof')}")
    if t == "power_protect_result":
        sk = reply.get("skipped") or {}
        return (f"power_protect ok={reply.get('ok')} action={reply.get('action')} detail={reply.get('detail')} "
                f"state={reply.get('state')} reason={reply.get('reason')} forced={reply.get('forced')} "
                f"since_utc={reply.get('since_utc')} brownouts_6h={reply.get('brownouts_6h')} "
                f"exit_in_s={reply.get('exit_in_s')} skipped(flash={sk.get('flash_runs')} "
                f"ambit={sk.get('ambit_runs')} ota={sk.get('ota')})")
    if t in ("vin_limit_result", "set_vin_limit_result"):
        reg01 = reply.get("reg01")
        return (f"{t} ok={reply.get('ok')} mv={reply.get('mv')} "
                f"reg01={f'{reg01:#04x}' if isinstance(reg01, int) else reg01} detail={reply.get('detail')}")
    if t in ("iin_limit_result", "set_iin_limit_result"):
        reg00 = reply.get("reg00")
        return (f"{t} ok={reply.get('ok')} register_ma={reply.get('ma')} "
                f"reg00={f'{reg00:#04x}' if isinstance(reg00, int) else reg00} "
                f"en_ilim={reply.get('en_ilim')} hiz={reply.get('hiz')} detail={reply.get('detail')}")
    if t == "ambit_flash_status":
        return (f"ambit_flash ch={reply.get('channel')} state={reply.get('state')}"
                + (f" detail={reply.get('detail')}" if reply.get("detail") else ""))
    if t == "ambit_versions":
        chans = " ".join(f"ch{c.get('ch')}={c.get('version', 'present' if c.get('present') else 'absent')}"
                         for c in reply.get("channels", []))
        return f"ambit_versions {chans}"
    return f"{t} {json.dumps(reply)[:160]}"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["ping", "inventory", "replay", "ota", "ambit-versions", "ambit-flash",
                                    "get-timezone", "set-timezone", "log-list", "log-read", "power-protect",
                                    "vin-limit", "iin-limit"])
    ap.add_argument("--vin-action", dest="vin_action", default="get", choices=VIN_LIMIT_ACTIONS,
                    help="vin-limit: get (read-only) or set (MUTATING)")
    ap.add_argument("--mv", type=int, default=None,
                    help=f"vin-limit --vin-action set: millivolts, {VIN_LIMIT_MIN_MV}..{VIN_LIMIT_MAX_MV} "
                         f"in {VIN_LIMIT_STEP_MV} mV steps")
    ap.add_argument("--iin-action", dest="iin_action", default="get", choices=IIN_LIMIT_ACTIONS,
                    help="iin-limit: get (read-only) or set (MUTATING)")
    ap.add_argument("--ma", type=int, default=None,
                    help=f"iin-limit --iin-action set: input current limit register in mA, "
                         f"{IIN_LIMIT_MIN_MA}..{IIN_LIMIT_MAX_MA} in {IIN_LIMIT_STEP_MA} mA steps")
    ap.add_argument("--action", default="status", choices=POWER_PROTECT_ACTIONS,
                    help="power-protect: status (read-only), clear or force (MUTATING)")
    ap.add_argument("--state", default="", choices=("",) + POWER_PROTECT_FORCE_STATES,
                    help="power-protect --action force: no_flash or hold")
    ap.add_argument("--name", default="", help="log-read: file name from log-list (e.g. ambyte.log)")
    ap.add_argument("--offset", type=int, default=0, help="log-read: byte offset; negative = from the end")
    ap.add_argument("--length", type=int, default=0,
                    help=f"log-read: bytes per request, 1..{LOG_READ_MAX} (default {LOG_READ_DEFAULT}; {LOG_READ_MAX} with --out)")
    ap.add_argument("--tail", type=int, default=0, help="log-read: start this many bytes before the end")
    ap.add_argument("--out", default=None, help="log-read: stitch chunks until EOF into this local file (one device)")
    ap.add_argument("--expect-gen", dest="expect_gen", default="",
                    help="log-read: refuse unless the file still has this gen (from an earlier reply)")
    ap.add_argument("--expect-offset", type=int, default=None, help="timezone commands: require this UTC offset in seconds at verification")
    ap.add_argument("--timezone", default="", help="set-timezone: canonical IANA name (e.g. Australia/Darwin)")
    ap.add_argument("--url", default="", help="dev-bench OTA only; HTTPS URL (never printed)")
    ap.add_argument("--version", default="", help="ambit-flash: AMBIT release version M.m.b")
    ap.add_argument("--channel", default="all", help="ambit-flash: 0-3 or all (default)")
    ap.add_argument("--manifest-url", dest="manifest_url", default="",
                    help="ambit-flash: https:// ambit-iot release manifest.json to download first")
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
    ap.add_argument("--wait", type=int, default=0,
                    help="seconds to wait for replies (default 60; ambit-versions 150, ambit-flash 900)")
    ap.add_argument("--profile", default=None)
    ap.add_argument("--region", default="eu-central-1")
    ap.add_argument("--results-json", default=None)
    ap.add_argument("--dry-run", action="store_true", help="print the plan and the payload, publish nothing")
    args = ap.parse_args(argv)
    if args.cmd == "set-timezone" and (not args.devices.strip() or not parse_devices(args.devices)):
        ap.error("set-timezone requires an explicit --devices list; fleet-wide discovery is refused")
    if args.cmd == "power-protect":
        # Validate before any AWS access: a bad action/state must cost nothing.
        try:
            build_message(args, "validate")
        except SystemExit as exc:
            ap.error(str(exc))
        if args.action != "status" and (not args.devices.strip() or not parse_devices(args.devices)):
            ap.error(f"power-protect {args.action} requires an explicit --devices list; "
                     "fleet-wide discovery is refused")
    if args.cmd == "vin-limit":
        # Validate before any AWS access: a bad value must cost nothing.
        try:
            build_message(args, "validate")
        except SystemExit as exc:
            ap.error(str(exc))
        if args.vin_action == "set" and (not args.devices.strip() or not parse_devices(args.devices)):
            ap.error("vin-limit set requires an explicit --devices list; fleet-wide discovery is refused")
    if args.cmd == "iin-limit":
        # Validate before any AWS access: a bad value must cost nothing.
        try:
            build_message(args, "validate")
        except SystemExit as exc:
            ap.error(str(exc))
        if args.iin_action == "set" and (not args.devices.strip() or not parse_devices(args.devices)):
            ap.error("iin-limit set requires an explicit --devices list; fleet-wide discovery is refused")
    if args.cmd == "log-read":
        # Validate before any AWS access: a bad name or window must cost nothing.
        try:
            log_read_window(args)
        except SystemExit as exc:
            ap.error(str(exc))
        if not valid_log_name(args.name):
            ap.error("log-read needs --name: a plain file name from log-list (e.g. ambyte.log)")
        if args.expect_gen and (len(args.expect_gen) >= 48 or not re.fullmatch(r"[0-9a-f.]+", args.expect_gen)):
            ap.error("--expect-gen must be a gen string from an earlier log reply (e.g. 412.9c41e07a.3.18)")
        if args.out and (not args.devices.strip() or len(parse_devices(args.devices)) != 1):
            ap.error("log-read --out stitches one file from exactly one explicit --devices target")
        if args.out and args.dry_run:
            # Before ANY session or transport: the stitcher publishes for real
            # (review finding 1 — the workflow always passes --out).
            dev = parse_devices(args.devices)[0]
            offset, length = log_read_window(args)
            first = LogStitcher(args.name, offset, length, expect_gen=args.expect_gen or None).request(
                f"fc-log-read-x-{int(time.time())}-0")
            print(f"log-read (dry run): target={dev}")
            print("first request:", json.dumps(first))
            pin = (f"require gen {args.expect_gen} on every chunk and fail on file_rotated"
                   if args.expect_gen else
                   "pin the device's gen from the first reply, send it as expect_gen, restart once on file_rotated")
            print(f"plan: read {args.name} from offset {offset} in chunks of up to {length} bytes until EOF, "
                  f"{pin}, write the bytes to {args.out}")
            print("dry run: nothing published, nothing written")
            return 0
    if args.wait <= 0:
        # ambit-versions quiesces 65 s before sweeping; ambit-flash adds the
        # release download, ~10-60 s per channel and the MQTT reconnect.
        args.wait = {"ambit-versions": 150, "ambit-flash": 900}.get(args.cmd, 60)

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
    if args.cmd == "log-read" and args.out:
        return stitch_log(args, session, devices[0], req_id)
    mutating = ((args.cmd == "replay" and args.mode not in READ_ONLY_MODES)
                or args.cmd in ("ambit-flash", "set-timezone")
                or (args.cmd == "power-protect" and args.action != "status")
                or (args.cmd == "vin-limit" and args.vin_action == "set")
                or (args.cmd == "iin-limit" and args.iin_action == "set"))
    reply_type = expected_reply_type(args)

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
    progress = {}
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
            print(f"  {dev}: {summarise(dev, data)}", flush=True)
            if args.cmd == "ambit-flash":
                # accepted -> (fetched|fetch_failed) -> per-channel -> success|failed
                progress.setdefault(dev, []).append(data)
                if data.get("state") in AMBIT_FLASH_TERMINAL:
                    replies[dev] = data
                    if len(replies) >= len(devices):
                        # AWS IoT does not preserve order: per-channel reports
                        # published just before the terminal one can land after it.
                        deadline = min(deadline, time.monotonic() + 5)
                continue
            if reply_type:
                if data.get("type") != reply_type:
                    continue
                if data.get("detail") in NON_TERMINAL_DETAILS:
                    # A QoS 1 duplicate can be refused while the original
                    # request is still queued. Keep that evidence, but wait for
                    # the actual terminal rather than exiting on a false fail.
                    progress.setdefault(dev, []).append(data)
                    replies.setdefault(dev, data)
                    continue
                replies[dev] = data
                if len(replies) >= len(devices) and all(r.get("detail") not in NON_TERMINAL_DETAILS for r in replies.values()):
                    break
                continue
            # keep the first terminal-looking reply; progress replies of a run keep coming
            if dev not in replies or data.get("detail") not in ("progress",):
                replies[dev] = data
            if len(replies) >= len(devices) and not (args.cmd == "replay" and args.mode == "run"):
                break
    finally:
        conn.disconnect().result(timeout=10)

    missing = [d for d in devices if d not in replies]
    bad_fw = []
    timezone_failed = []
    if args.cmd in ("get-timezone", "set-timezone"):
        expected_type = "timezone_result" if args.cmd == "get-timezone" else "set_timezone_result"
        timezone_failed = [d for d, r in replies.items()
            if r.get("type") != expected_type or r.get("ok") is not True or r.get("applied") is not True
            or not r.get("timezone") or r.get("timezone") != r.get("active_timezone")
            or (args.expect_offset is not None and r.get("utc_offset_seconds") != args.expect_offset)
            or (args.cmd == "set-timezone" and (r.get("saved") is not True or r.get("timezone") != args.timezone))]
        if timezone_failed:
            print(f"timezone not verified on: {timezone_failed}")
    # log-list / log-read: a refusal (sd_parked, sd_absent, busy, ...) is a
    # failed command, with the device's detail in the results.
    log_failed = [d for d, r in replies.items()
                  if args.cmd in ("log-list", "log-read") and (r.get("type") != REPLY_TYPES[args.cmd] or r.get("ok") is not True)]
    if log_failed:
        print(f"{args.cmd} refused on: {log_failed}")
    log_malformed = {d: log_reply_problem(r) for d, r in replies.items()
                     if args.cmd in ("log-list", "log-read") and d not in log_failed and log_reply_problem(r)}
    if log_malformed:
        print(f"{args.cmd} incomplete reply from: {json.dumps(log_malformed)}")
        log_failed += sorted(log_malformed)
    # power-protect: a refusal, or a reply that does not show the requested
    # state, is a failed command.
    pp_failed = {d: power_protect_problem(args, r) for d, r in replies.items()
                 if args.cmd == "power-protect" and power_protect_problem(args, r)}
    if pp_failed:
        print(f"power-protect not confirmed on: {json.dumps(pp_failed)}")
    # vin-limit: a refusal, or a set whose readback is not the requested value,
    # is a failed command.
    vin_failed = {d: vin_limit_problem(args, r) for d, r in replies.items()
                  if args.cmd == "vin-limit" and vin_limit_problem(args, r)}
    if vin_failed:
        print(f"vin-limit not confirmed on: {json.dumps(vin_failed)}")
    iin_failed = {d: iin_limit_problem(args, r) for d, r in replies.items()
                  if args.cmd == "iin-limit" and iin_limit_problem(args, r)}
    if iin_failed:
        print(f"iin-limit not confirmed on: {json.dumps(iin_failed)}")
    flash_failed = [d for d, r in replies.items()
                    if r.get("type") == "ambit_flash_status" and r.get("state") != "success"]
    if flash_failed:
        print(f"ambit_flash did not succeed on: {flash_failed}")
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
                       "firmware": fw_hist, "replay_states": states, "progress": progress,
                       "timezone_failures": timezone_failed, "log_failures": log_failed,
                       "power_protect_failures": pp_failed, "vin_limit_failures": vin_failed,
                       "iin_limit_failures": iin_failed},
                      fh, indent=1, sort_keys=True)
    return 1 if (missing or bad_fw or flash_failed or timezone_failed or log_failed or pp_failed
                 or vin_failed or iin_failed) else 0


def stitch_log(args, session, dev, req_id):
    """log-read --out: sequential chunk requests to ONE device until EOF.

    Every chunk has its own correlated id (`<req_id>-<n>`); a reply for any
    other id is ignored. A chunk with no reply within --wait fails the command
    (after a bounded retry when the only answers were busy/queue refusals).
    The bytes read so far are always written, and the results JSON records
    whether the file is complete.
    """
    from awscrt import mqtt

    offset, length = log_read_window(args)
    st = LogStitcher(args.name, offset, length, expect_gen=args.expect_gen or None)
    by_identity = fleet_deploy.device_index([dev])
    q = queue.Queue()
    conn = fleet_deploy.mqtt_connection(session, fleet_deploy.STATUS_TOPIC,
                                        lambda topic, payload, **kw: q.put((time.time(), topic, payload)),
                                        client_id="fleet-deploy-command")
    chunk_log = []
    first_payload = None
    n = 0
    retries = 0
    print(f"log-read: {dev} {args.name} from offset {offset} in chunks of {length} -> {args.out} (request id {req_id})")
    try:
        while not st.done and st.error is None:
            chunk_id = f"{req_id}-{n}"
            payload = st.request(chunk_id)
            first_payload = first_payload or payload
            fut, _ = conn.publish(topic=fleet_deploy.COMMAND_TOPIC_FMT.format(device=dev),
                                  payload=json.dumps(payload), qos=mqtt.QoS.AT_LEAST_ONCE)
            fut.result(timeout=10)
            n += 1
            terminal, refused = None, None
            deadline = time.monotonic() + args.wait
            while time.monotonic() < deadline and terminal is None:
                try:
                    _, topic, raw = q.get(timeout=0.5)
                except queue.Empty:
                    continue
                try:
                    data = json.loads(raw)
                except ValueError:
                    continue
                if data.get("id") != chunk_id or data.get("type") != "log_read_result":
                    continue
                if fleet_deploy.requested_device_from_status_topic(topic, by_identity) != dev:
                    continue
                if data.get("ok") is not True and data.get("detail") in NON_TERMINAL_DETAILS:
                    refused = data              # maybe a QoS 1 duplicate: keep waiting
                    continue
                terminal = data
            reply = terminal or refused
            if reply is None:
                st.error = f"no reply to chunk {chunk_id} within {args.wait} s"
                break
            outcome = st.feed(reply)
            chunk_log.append({k: reply.get(k) for k in ("id", "ok", "detail", "gen", "offset", "length", "size", "eof")})
            print(f"  {dev}: {summarise(dev, reply)}", flush=True)
            if outcome == "restart":
                print(f"  {dev}: {args.name} was rotated during the read; restarting once from offset {offset}")
                retries = 0
                continue
            if outcome != "retry":
                retries = 0
                continue
            retries += 1                        # consecutive refusals of the same offset
            if retries > 5:
                st.error = f"gave up after {retries} retries (last: {reply.get('detail')})"
                break
            time.sleep(2)
    finally:
        conn.disconnect().result(timeout=10)

    with open(args.out, "wb") as fh:
        fh.write(st.data)
    complete = st.done and st.error is None
    print(f"{'complete' if complete else 'INCOMPLETE'}: {len(st.data)} bytes of {args.name} "
          f"(offset {st.start}, file size {st.size}) in {st.chunks} chunk(s) -> {args.out}"
          + (f"; {st.error}" if st.error else ""))
    if args.results_json:
        with open(args.results_json, "w") as fh:
            # No file data in the results: the bytes are in --out.
            json.dump({"command": "log-read", "request_id": req_id, "payload": first_payload, "devices": [dev],
                       "log": {"name": args.name, "start": st.start, "bytes": len(st.data), "size": st.size,
                               "gen": st.gen, "restarts": st.restarts,
                               "chunks": st.chunks, "complete": complete, "error": st.error, "out": args.out},
                       "replies": chunk_log, "no_reply": [] if chunk_log else [dev]},
                      fh, indent=1, sort_keys=True)
    return 0 if complete else 1


if __name__ == "__main__":
    sys.exit(main())
