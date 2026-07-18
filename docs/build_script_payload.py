#!/usr/bin/env python3
"""
Build a `script_update` MQTT command from a Lua file — stdlib only, any OS.

Two delivery shapes, both dispatched by the firmware's command_router on the
device command topic (see docs/push-main-lua-via-mqtt.md):

  URL   (default, recommended)  — a tiny command carrying a raw URL; the device
        streams the script over HTTPS in 4 KB chunks. No large contiguous TLS
        buffer is ever needed, so it works even mid-measurement on a fragmented
        heap. The raw URL is auto-derived from the git remote+branch+path.
          {"type":"script_update","id":"<unique>","url":"<raw-url>","checksum":"<sha256>"}

  INLINE  (--inline, fallback)  — the whole script is embedded in the message.
        Simple, but the ~8 KB message needs a contiguous heap buffer the device
        often can't allocate while measuring, so it silently drops. Use only for
        small scripts, or when you can't host the file. Capped at 16 KB.
          {"type":"script_update","id":"<unique>","script":"<lua>","checksum":"<sha256>"}

The SHA-256 is computed over the CRLF->LF-normalized file bytes. For the URL
shape this must equal the bytes the host serves, so **commit and push the file
first** — this tool warns if the file has uncommitted changes. To verify against
what is actually hosted: `curl -sL <raw-url> | sha256sum`.

No dependencies: Python 3.8+ standard library only. Writes payload.json (paste
into the AWS IoT MQTT test client, or `mosquitto_pub -f payload.json`).

Examples:
  # URL command, raw URL auto-derived from git (default):
  python3 docs/build_script_payload.py docs/example_SDfolders/main.lua
  # URL command with an explicit host (non-GitHub, gist, S3, …):
  python3 docs/build_script_payload.py my.lua --url https://example.com/main.lua
  # inline fallback (small scripts only):
  python3 docs/build_script_payload.py my.lua --inline
  python3 docs/build_script_payload.py my.lua --inline --no-reboot   # swap in place, no reboot
  # copy to clipboard: | pbcopy (macOS) | clip (Windows) | xclip -sel c (Linux)
  python3 docs/build_script_payload.py my.lua -o - | clip
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path

# Kept in lockstep with the firmware:
MSG_CAP = 16384   # INBOUND_MSG_LARGE_MAX  — components/mqtt_client/mqtt_client.c
ID_MAX = 63       # SCRIPT_ID_MAX - 1      — components/script_update/script_update.c


def git(top, *args):
    """Run `git -C <top> <args>`; return stripped stdout, or None on any failure."""
    try:
        out = subprocess.run(("git", "-C", str(top), *args),
                             capture_output=True, text=True, check=True)
        return out.stdout.strip()
    except Exception:
        return None


def normalized_bytes(src: Path):
    """Read `src`, normalize CRLF->LF, return (text, utf-8 bytes)."""
    try:
        text = src.read_bytes().decode("utf-8")
    except UnicodeDecodeError as e:
        sys.exit(f"error: {src} is not valid UTF-8: {e}")
    lua = text.replace("\r\n", "\n")
    return lua, lua.encode("utf-8")


def derive_raw_url(src: Path):
    """Build a raw.githubusercontent.com URL for `src` from the git remote+branch.

    Returns (url, warnings[]). Exits with a clear message if `src` is not in a
    git repo or the origin/branch/path can't be resolved — pass --url instead.
    """
    top = git(src.parent, "rev-parse", "--show-toplevel")
    if not top:
        sys.exit(f"error: {src} is not inside a git repo (or git is unavailable).\n"
                 f"       Pass --url <raw-url> explicitly, or --inline to embed the script.")
    remote = git(top, "remote", "get-url", "origin")
    branch = git(top, "rev-parse", "--abbrev-ref", "HEAD")
    if not (remote and branch):
        sys.exit("error: could not resolve origin remote / current branch.\n"
                 "       Pass --url <raw-url> explicitly.")
    rel = os.path.relpath(str(src.resolve()), top).replace("\\", "/")

    # Normalize the origin to owner/repo:
    #   git@github.com:Owner/Repo.git  |  https://github.com/Owner/Repo.git
    r = remote
    if r.startswith("git@"):
        r = r.split(":", 1)[1]
    else:
        for pre in ("https://", "http://", "ssh://git@", "ssh://", "git://"):
            if r.startswith(pre):
                r = r[len(pre):]
                break
        r = r.split("/", 1)[1] if "/" in r else r   # drop host
    if r.endswith(".git"):
        r = r[:-4]

    host = remote.split("/")[2] if "//" in remote else remote.split("@")[-1].split(":")[0]
    warnings = []
    if "github.com" not in host:
        warnings.append(f"origin host is '{host}', not github.com — the raw.githubusercontent.com "
                        f"URL below is a guess; pass --url if your host differs.")

    # Warn if the file the URL will serve differs from what's on disk right now.
    dirty = git(top, "status", "--porcelain", "--", rel)
    if dirty:
        warnings.append(f"{rel} has UNCOMMITTED changes — the raw URL serves the pushed "
                        f"version, not your working copy. Commit AND push before publishing, "
                        f"or the device will download stale bytes / fail the checksum.")
    else:
        ahead = git(top, "rev-list", "--count", f"origin/{branch}..HEAD")
        if ahead and ahead != "0":
            warnings.append(f"local branch is {ahead} commit(s) ahead of origin/{branch} — "
                            f"push before publishing so the raw URL serves this version.")

    url = f"https://raw.githubusercontent.com/{r}/{branch}/{rel}"
    return url, warnings


def build_inline(lua: str, checksum: str, script_id: str, ascii_only: bool, reboot: bool):
    obj = {"type": "script_update", "id": script_id, "script": lua}
    if checksum:
        obj["checksum"] = checksum
    if not reboot:
        obj["reboot"] = False
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=ascii_only)


def build_url(url: str, checksum: str, script_id: str, reboot: bool):
    obj = {"type": "script_update", "id": script_id, "url": url}
    if checksum:
        obj["checksum"] = checksum
    if not reboot:
        obj["reboot"] = False
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("lua", help="path to the Lua script to push")
    ap.add_argument("--url", metavar="RAW_URL", nargs="?", const="",
                    help="URL command with an explicit raw URL. Omit the value (just --url) "
                         "or the whole flag to auto-derive it from the git remote (the default).")
    ap.add_argument("--inline", action="store_true",
                    help="embed the script in the message instead of using a URL (fallback for "
                         "small scripts / unhostable files; often dropped on a busy device)")
    ap.add_argument("--id",
                    help="update id (default: main-lua[-url]-<UTC timestamp>). Reusing an "
                         "already-applied id is silently ignored by the device.")
    ap.add_argument("--no-checksum", action="store_true",
                    help="omit the checksum field (checksum is optional)")
    ap.add_argument("--ascii", action="store_true",
                    help="inline only: escape non-ASCII to \\uXXXX (paste-safe, larger message)")
    ap.add_argument("--no-reboot", action="store_true",
                    help='add "reboot": false (device default is to reboot into the new script)')
    ap.add_argument("-o", "--out", default="payload.json",
                    help="output file (default: payload.json; '-' = stdout)")
    args = ap.parse_args()

    if args.inline and args.url is not None:
        sys.exit("error: choose one delivery shape — --inline OR --url (not both).")

    src = Path(args.lua)
    if not src.is_file():
        sys.exit(f"error: no such file: {src}")

    use_url = not args.inline
    prefix = "main-lua-url-" if use_url else "main-lua-"
    script_id = args.id or prefix + time.strftime("%Y%m%d-%H%M%S", time.gmtime())
    if len(script_id) > ID_MAX:
        sys.exit(f"error: id is {len(script_id)} chars; the device truncates at {ID_MAX} "
                 f"(dedupe + reply would then use the truncated id). Shorten --id.")

    lua, script_bytes = normalized_bytes(src)
    checksum = None if args.no_checksum else hashlib.sha256(script_bytes).hexdigest()

    # When emitting JSON to stdout, keep it the only thing on stdout — summary to stderr.
    to_stdout = args.out == "-"
    log = (lambda *a: print(*a, file=sys.stderr)) if to_stdout else print

    warnings = []
    if use_url:
        url = args.url if args.url else None
        if url:
            log(f"mode          = URL (explicit)")
        else:
            url, warnings = derive_raw_url(src)
            log(f"mode          = URL (auto-derived from git)")
        payload = build_url(url, checksum, script_id, not args.no_reboot)
        log(f"url           = {url}")
    else:
        log(f"mode          = INLINE")
        payload = build_inline(lua, checksum, script_id, args.ascii, not args.no_reboot)

    msg_bytes = len(payload.encode("utf-8"))
    log(f"id            = {script_id}")
    log(f"script bytes  = {len(script_bytes)}   (CRLF->LF normalized)")
    if checksum:
        log(f"sha256        = {checksum}")
    log(f"reboot        = {'no (in-place swap)' if args.no_reboot else 'yes (device default)'}")
    log(f"message bytes = {msg_bytes} / {MSG_CAP}   "
        f"{'OK, within cap' if msg_bytes <= MSG_CAP else 'OVER CAP'}")

    # Inline is the only shape bounded by the device message cap; a URL command is tiny.
    if not use_url and msg_bytes > MSG_CAP:
        sys.exit(f"error: inline message is {msg_bytes} B, over the {MSG_CAP} B device cap "
                 f"(INBOUND_MSG_LARGE_MAX) — the device would drop it. Use the default URL "
                 f"delivery (drop --inline) or trim the script.")

    for w in warnings:
        log(f"!! WARNING: {w}")
    if use_url and checksum:
        log("   verify the hosted bytes match after pushing: "
            f"curl -sL '{url}' | sha256sum")

    if to_stdout:
        sys.stdout.buffer.write(payload.encode("utf-8") + b"\n")
    else:
        Path(args.out).write_bytes(payload.encode("utf-8"))
        log(f"-> wrote {args.out}   (paste into the AWS IoT MQTT test client, "
            f"or: mosquitto_pub ... -f {args.out})")


if __name__ == "__main__":
    main()
