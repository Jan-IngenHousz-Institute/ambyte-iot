#!/usr/bin/env python3
"""
Build a `script_update` MQTT payload from a Lua file — stdlib only, any OS.

Turns a multi-line .lua file into the single-line JSON message the firmware's
command_router expects on the device command topic: it normalizes line endings,
computes the SHA-256 the firmware verifies, and enforces the 16 KB message cap.
Write the result to payload.json and paste it into the AWS IoT MQTT test client
(see docs/push-main-lua-via-mqtt.md), or pipe it to `mosquitto_pub -f payload.json`.

No dependencies: Python 3.8+ standard library only (json, hashlib, argparse).

Message shape (docs/device-script-delivery.md):
  {"type":"script_update","id":"<unique>","script":"<lua>","checksum":"<sha256 hex>"}

Examples:
  python3 docs/build_script_payload.py docs/example_Sdfolders/main.lua
  python3 docs/build_script_payload.py my.lua --id my-2026-07-15 -o -
  python3 docs/build_script_payload.py my.lua --no-checksum
  python3 docs/build_script_payload.py my.lua --no-reboot   # in-place swap, no device reboot
  # copy to clipboard: | pbcopy (macOS) | clip (Windows) | xclip -sel c (Linux)
  python3 docs/build_script_payload.py my.lua -o - | clip
"""
import argparse
import hashlib
import json
import sys
import time
from pathlib import Path

# Kept in lockstep with the firmware:
MSG_CAP = 16384   # INBOUND_MSG_LARGE_MAX  — components/mqtt_client/mqtt_client.c
ID_MAX = 63       # SCRIPT_ID_MAX - 1      — components/script_update/script_update.c


def build(src: Path, script_id: str, with_checksum: bool, ascii_only: bool, reboot: bool):
    """Return (script_bytes, obj, payload_str) for the given Lua file."""
    try:
        text = src.read_bytes().decode("utf-8")
    except UnicodeDecodeError as e:
        sys.exit(f"error: {src} is not valid UTF-8: {e}")

    # Normalize CRLF -> LF: a stable checksum, no stray \r in the running script,
    # and a smaller message. The firmware hashes exactly the decoded `script`
    # string it receives, so hashing the same bytes we emit guarantees they agree.
    lua = text.replace("\r\n", "\n")
    script_bytes = lua.encode("utf-8")

    obj = {"type": "script_update", "id": script_id, "script": lua}
    if with_checksum:
        # lowercase hex; the firmware compares case-insensitively and needs 64 chars
        obj["checksum"] = hashlib.sha256(script_bytes).hexdigest()
    # The firmware reboots into the new script by default; only emit the field to
    # opt OUT (reboot=false → in-place swap + Lua-runner restart, no reboot).
    if not reboot:
        obj["reboot"] = False

    # Compact, standard JSON. ensure_ascii=False keeps non-ASCII (µ, ≈, box-drawing
    # comments) as raw UTF-8 → smallest message; --ascii escapes them to \uXXXX for
    # channels that might mangle UTF-8 on the way to the device (checksum unchanged
    # either way — cJSON decodes both back to the same bytes).
    payload = json.dumps(obj, separators=(",", ":"), ensure_ascii=ascii_only)
    return script_bytes, obj, payload


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("lua", help="path to the Lua script to push")
    ap.add_argument("--id",
                    help="update id (default: main-lua-<UTC timestamp>). Reusing an "
                         "already-applied id is silently ignored by the device.")
    ap.add_argument("--no-checksum", action="store_true",
                    help="omit the checksum field (checksum is optional)")
    ap.add_argument("--ascii", action="store_true",
                    help="escape non-ASCII to \\uXXXX (paste-safe, larger message)")
    ap.add_argument("--no-reboot", action="store_true",
                    help='add "reboot": false (device default is to reboot into the new script)')
    ap.add_argument("-o", "--out", default="payload.json",
                    help="output file (default: payload.json; '-' = stdout)")
    args = ap.parse_args()

    src = Path(args.lua)
    if not src.is_file():
        sys.exit(f"error: no such file: {src}")

    script_id = args.id or "main-lua-" + time.strftime("%Y%m%d-%H%M%S", time.gmtime())
    if len(script_id) > ID_MAX:
        sys.exit(f"error: id is {len(script_id)} chars; the device truncates at {ID_MAX} "
                 f"(dedupe + reply would then use the truncated id). Shorten --id.")

    script_bytes, obj, payload = build(src, script_id, not args.no_checksum, args.ascii,
                                       not args.no_reboot)
    msg_bytes = len(payload.encode("utf-8"))

    # When emitting JSON to stdout, keep it the only thing on stdout — summary to stderr.
    to_stdout = args.out == "-"
    log = (lambda *a: print(*a, file=sys.stderr)) if to_stdout else print

    log(f"id            = {script_id}")
    log(f"script bytes  = {len(script_bytes)}   (CRLF->LF normalized)")
    if not args.no_checksum:
        log(f"sha256        = {obj['checksum']}")
    log(f"reboot        = {'no (in-place swap)' if args.no_reboot else 'yes (device default)'}")
    log(f"message bytes = {msg_bytes} / {MSG_CAP}   "
        f"{'OK, within cap' if msg_bytes <= MSG_CAP else 'OVER CAP'}")

    if msg_bytes > MSG_CAP:
        sys.exit(f"error: message is {msg_bytes} B, over the {MSG_CAP} B device cap "
                 f"(INBOUND_MSG_LARGE_MAX) — the device would drop it. Trim the script; "
                 f"there is no chunked/URL delivery path.")

    if to_stdout:
        sys.stdout.buffer.write(payload.encode("utf-8") + b"\n")
    else:
        Path(args.out).write_bytes(payload.encode("utf-8"))
        log(f"-> wrote {args.out}   (paste into the AWS IoT MQTT test client, "
            f"or: mosquitto_pub ... -f {args.out})")


if __name__ == "__main__":
    main()
