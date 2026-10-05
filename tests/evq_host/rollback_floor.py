#!/usr/bin/env python3
"""Rollback-floor evidence: which released firmware tags would re-import
SD-only spool files after a rollback, proven by EXECUTED variants.

For every release tag v1.10.0..v2.4.2 this hashes the pieces of the event
store that decide what an older firmware does with a HEAD-written card:

  import      event_log_import_sd_backlog  (walks /sdcard/events, rebuilds
              "%s/ev-%06u.log", re-appends verbatim, deletes after fsync)
  append      evlog_append_verbatim_locked (the line gate; absent in older tags,
              whose import inlines it)
  parse_name  parse_ev_name
  clamp       the open-time cursor clamp in evlog_open_locked
  claim_gap   the missing-file-at-cursor claim path
  keeper      the app_main keeper call site

A tag is "proven" only when EVERY hash equals one of the two variants the
harness EXECUTES in H4 (v2.2.3 and b3f9b8a). Anything else is listed as not
proven - the claim in docs/evq-sd-overflow.md is limited to proven tags.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXECUTED = {"v2.2.3": "v2.2.3", "b3f9b8a": "b3f9b8a"}


def git_show(rev: str, path: str) -> str | None:
    r = subprocess.run(["git", "show", f"{rev}:{path}"], cwd=ROOT, capture_output=True, text=True)
    return r.stdout if r.returncode == 0 else None


def func_body(src: str, signature_regex: str) -> str | None:
    m = re.search(signature_regex, src, re.M)
    if not m:
        return None
    end = src.find("\n}\n", m.start())
    return src[m.start(): end + 3] if end >= 0 else None


def norm(s: str | None) -> str:
    if s is None:
        return "absent"
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    s = re.sub(r"//[^\n]*", "", s)
    s = re.sub(r"\s+", " ", s).strip()
    return hashlib.sha256(s.encode()).hexdigest()[:16]


def pieces(rev: str) -> dict[str, str]:
    ev = git_show(rev, "components/event_log/event_log.c") or ""
    app = git_show(rev, "main/app_main.c") or ""
    out = {
        "import": norm(func_body(ev, r"^size_t event_log_import_sd_backlog\(")),
        "append": norm(func_body(ev, r"^static esp_err_t evlog_append_verbatim_locked\(")),
        "parse_name": norm(func_body(ev, r"^static bool parse_ev_name\(")),
    }
    open_fn = func_body(ev, r"^static esp_err_t evlog_open_locked\(") or ""
    clamp = "\n".join(l for l in open_fn.splitlines() if "cseq" in l or "coff" in l)
    out["clamp"] = norm(clamp or None)
    claim = func_body(ev, r"^static esp_err_t event_log_claim_impl\(") or func_body(ev, r"^esp_err_t event_log_claim_next_event\(") or ""
    i = claim.find("missing at cursor")
    out["claim_gap"] = norm(claim[max(0, i - 400): i + 300] if i >= 0 else None)
    keeper = func_body(app, r"^static void app_sd_keeper_task\(")
    out["keeper"] = norm(keeper)
    # the one property the rollback import depends on, stated plainly
    imp = func_body(ev, r"^size_t event_log_import_sd_backlog\(") or ""
    out["_rebuilds_ev_%06u"] = "yes" if 'ev-%06u.log' in imp else "no"
    out["_imports_dir"] = "/sdcard/events" if '"/sdcard/events"' in ev else "?"
    return out


def tags() -> list[str]:
    r = subprocess.run(["git", "tag", "-l", "v*", "--sort=v:refname"], cwd=ROOT, capture_output=True, text=True, check=True)
    all_tags = [t for t in r.stdout.split() if t]

    def key(t: str):
        m = re.match(r"v(\d+)\.(\d+)\.(\d+)", t)
        return tuple(int(x) for x in m.groups()) if m else (0, 0, 0)

    return [t for t in all_tags if (1, 10, 0) <= key(t) <= (2, 4, 2)]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=False)
    a = ap.parse_args()
    variants = {name: pieces(rev) for name, rev in EXECUTED.items()}
    rows = []
    for t in tags():
        p = pieces(t)
        match = None
        for name, v in variants.items():
            if all(p[k] == v[k] for k in p if not k.startswith("_")):
                match = name
                break
        rows.append({"tag": t, "pieces": p, "matches_executed_variant": match, "proven": match is not None})
    doc = {"executed_variants": variants, "tags": rows,
           "proven": [r["tag"] for r in rows if r["proven"]],
           "not_proven": [r["tag"] for r in rows if not r["proven"]]}
    text = json.dumps(doc, indent=1, sort_keys=True)
    if a.out:
        Path(a.out).parent.mkdir(parents=True, exist_ok=True)
        Path(a.out).write_text(text)
    print(f"proven: {', '.join(doc['proven']) or '-'}")
    print(f"not proven: {', '.join(doc['not_proven']) or '-'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
