"""Contract scenarios for the storage harness (groups A–H, Q and the D matrix).

Each scenario builds a script for the production driver, runs it on a fresh
state directory (with simulated power losses where the row requires them),
applies the independent oracles, writes evidence and returns its reconcile
record. A failed assertion raises OracleFailure / AssertionError.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import threading
from pathlib import Path

import build
from evq_lib import (Device, HarnessError, OracleFailure, assert_e2e, check_arch_inv, check_durability, cursor_from_nvs,
                     iter_records, last_good_copy_oracle, line_sha, load_manifests, media_hashes, nvs_read, nvs_write,
                     parse_index, quarantined_ids, read_jsonl, reconcile, record_id, registered_fault_points,
                     scratch_root, sha256_file, write_evidence)

MiB = 1024 * 1024
ESP_ERR_NO_MEM = 0x101
ESP_ERR_NOT_FINISHED = 0x10C

_build_lock = threading.Lock()
_builds: dict = {}
_build_root: Path | None = None


def build_root() -> Path:
    global _build_root
    if _build_root is None:
        _build_root = scratch_root() / "build"
        _build_root.mkdir(parents=True, exist_ok=True)
    return _build_root


def exe(kind: str = "head", tuning: dict | None = None) -> Path:
    key = (kind, tuple(sorted((tuning or {}).items())))
    with _build_lock:
        if key not in _builds:
            name = "d_" + kind.replace(".", "_") + "_" + "_".join(f"{k}{v}" for k, v in sorted((tuning or {}).items()))
            name = re.sub(r"[^A-Za-z0-9_]", "", name)[:120]
            final = build_root() / name
            if not final.exists():
                # parallel workers may build the same variant: each builds under a
                # private name, then renames it into place atomically
                tmp = f"{name}_p{os.getpid()}"
                if kind == "head":
                    built = build.build_head(build_root(), tuning, name=tmp)
                elif kind.startswith("rev:"):
                    # full baseline of an indexed-store revision (red/green)
                    built, info = build.build_rev_full(kind[4:], build_root(), name=tmp, tuning=tuning)
                    (build_root() / (name + ".rewrite.json")).write_text(json.dumps(info, indent=1))
                else:
                    built, info = build.build_baseline(kind, build_root(), name=tmp)
                    (build_root() / (name + ".rewrite.json")).write_text(json.dumps(info, indent=1))
                os.replace(built, final)
            _builds[key] = final
    return _builds[key]


def baseline_rewrite_info(rev: str) -> dict:
    exe(rev)
    for p in build_root().glob("*.rewrite.json"):
        d = json.loads(p.read_text())
        if d["rev"] == rev:
            return d
    return build.baseline_sources(rev, build_root() / f"rewrite_{rev}")


def device(scn: str, seed: int, kind: str = "head", tuning: dict | None = None, flash: int = 2 * MiB,
           sd: int = 256 * MiB, env: dict | None = None, pending_check: bool = True) -> Device:
    root = scratch_root()
    e = {"EVQ_FLASH_BYTES": str(flash), "EVQ_SD_BYTES": str(sd)}
    e.update(env or {})
    d = Device(state=root / "dev", exe=exe(kind, tuning), seed=seed, scenario=scn, env=e)
    d.scratch = root  # type: ignore[attr-defined]
    if pending_check:
        d.cp_hook = pending_truth_hook
    return d


def finish(dev: Device) -> None:
    """Remove the scratch dir on success (kept on failure by the caller)."""
    shutil.rmtree(getattr(dev, "scratch", dev.state), ignore_errors=True)


def health_rows(dev: Device) -> list[dict]:
    return read_jsonl(dev.state / "out" / "health.jsonl")


def last_health(dev: Device, label: str | None = None) -> dict:
    rows = health_rows(dev)
    if label is not None:
        rows = [r for r in rows if r.get("label") == label]
    if not rows:
        raise AssertionError(f"no health row {label}")
    return rows[-1]


def pending_truth_hook(dev: Device, label: str) -> None:
    """C8: wherever pending_exact, health counts equal harness truth (crash-free runs)."""
    if dev.crashes or getattr(dev, "no_pending_check", False):
        return
    rows = [r for r in health_rows(dev) if r.get("label") == f"cp:{label}"]
    if not rows or "baseline" in rows[-1] or not rows[-1].get("pending_exact"):
        return
    h = rows[-1]
    m = load_manifests(dev.state)
    delivered = {d["id"] for d in m["delivered"]}
    truth = sum(1 for i in m["accepted"] if i not in delivered) + getattr(dev, "extra_pending", 0)
    if h["pending"] != truth:
        raise OracleFailure(f"C8: health.pending={h['pending']} but {truth} accepted records are undelivered")
    refused = read_jsonl(dev.state / "out" / "refused.jsonl")
    # refusal counters are boot-relative (STATUS carries uptime): compare this boot
    for key, code in [("refused_full", ESP_ERR_NO_MEM)]:
        n = sum(1 for r in refused if r["err"] == code and r.get("boot") == h.get("boot"))
        if h[key] != n:
            raise OracleFailure(f"C8: health.{key}={h[key]} but {n} store calls returned {code:#x}")
    if h["flash_pending"] + h["sd_pending"] + h["reimport_pending"] != h["pending"]:
        raise OracleFailure("C8: pending split does not add up")


def cps(n_total: int, chunk: int, profile: str = "default", label: str = "s") -> list[str]:
    out = []
    done = 0
    i = 0
    while done < n_total:
        k = min(chunk, n_total - done)
        out.append(f"store {k} {profile}")
        done += k
        i += 1
        out.append(f"checkpoint {label}{i}")
    return out


def run_ok(dev: Device, lines: list[str], fault: str | None = None, exe_path=None) -> None:
    dev.run(lines, fault=fault, exe=exe_path)


def c19_mode() -> str:
    """EVQ_C19=assert (default) fails a run on any C-19/C-18-cursor violation;
    EVQ_C19=record only records it in reconcile.json["c19"] (explicit opt-out
    for regression runs that must not be blocked by a known finding)."""
    m = os.environ.get("EVQ_C19", "assert")
    if m not in ("assert", "record"):
        raise HarnessError(f"EVQ_C19={m!r}: expected 'assert' or 'record'")
    return m


def _last_good_copy_oracle(dev: Device) -> dict:
    """C-19 (a)-(d) + the C-18 cursor rule over this device's ops.jsonl and
    index lines (implementation: evq_lib.last_good_copy_oracle)."""
    return last_good_copy_oracle(dev.state)


def apply_c19(dev: Device, rec: dict) -> list[str]:
    """Run the C-19 oracle, record it in `rec`, return the violations."""
    r = _last_good_copy_oracle(dev)
    r["mode"] = c19_mode()
    rec["c19"] = r
    return r["violations"] if r["n_violations"] else []


def raise_c19(rec: dict) -> None:
    c = rec.get("c19") or {}
    if c.get("n_violations") and c.get("mode", "assert") == "assert":
        raise OracleFailure(f"C-19: {c['n_violations']} violation(s): " + " | ".join(c["violations"][:6]))


def e2e(dev: Device, extra: dict | None = None, media_before=None, allow_missing=None, dup_bound=None) -> dict:
    dev.run(["drain_all", "service 2", "health final", "checkpoint final"])
    rec = reconcile(dev)
    if extra:
        rec.update(extra)
    apply_c19(dev, rec)
    try:
        rec["arch_inv"] = check_arch_inv(dev)
    except OracleFailure as e:
        rec["arch_inv"] = {"error": str(e)}
        write_evidence(dev, rec, media_before=media_before)
        raise
    write_evidence(dev, rec, media_before=media_before)
    assert_e2e(rec, allow_missing)
    if dup_bound is not None and rec["duplicate_count"] > dup_bound:
        raise OracleFailure(f"duplicates {rec['duplicate_count']} exceed bound {dup_bound}")
    raise_c19(rec)
    return rec


def sd_files(dev: Device, sub: str, card: str = "sdcard") -> list[Path]:
    d = dev.state / card / sub
    return sorted(d.glob("*.log")) if d.exists() else []


def ops(dev: Device) -> list[dict]:
    return read_jsonl(dev.state / ".shim" / "ops.jsonl")


def index_segs(dev: Device):
    p = dev.state / "evstore" / "evq.idx"
    return parse_index(p.read_bytes() if p.exists() else b"")


# ═══ A. reproduction and headline fix ═══════════════════════════════════════
def A1(seed: int) -> dict:
    """Baseline b3f9b8a loses measurements on a full small flash while SD has room."""
    dev = device("A1", seed, kind="b3f9b8a")
    dev.cp_hook = None
    dev.durability_mode = "record"     # the baseline also acks before fsync (<=8 records): recorded, not the A1 claim
    lines = ["keeper auto"] + cps(1400, 100) + ["health end"]
    dev.run(lines)
    m = load_manifests(dev.state)
    refused = read_jsonl(dev.state / "out" / "refused.jsonl")
    nomem = [r for r in refused if r["err"] == ESP_ERR_NO_MEM]
    h = last_health(dev, "end")
    unsent_on_sd = 0
    for p in sd_files(dev, "events") + sd_files(dev, "archive"):
        for _, line in iter_records(p.read_bytes()):
            unsent_on_sd += 1
    sd_free = (dev.state / "sdcard").exists()
    rec = {"scenario": "A1", "seed": seed, "baseline": "b3f9b8a", "generated": len(m["accepted"]) + len(refused),
           "accepted": len(m["accepted"]), "refused_no_mem": len(nomem), "health_dropped": h["dropped"],
           "unsent_records_on_sd": unsent_on_sd, "sd_present": sd_free, "rewrite": baseline_rewrite_info("b3f9b8a"),
           "baseline_accepted_not_durable_findings": dev.durability_findings}
    write_evidence(dev, rec)
    assert len(nomem) > 0, "baseline did not refuse - loss not reproduced"
    assert rec["accepted"] < rec["generated"]
    assert unsent_on_sd == 0, "baseline wrote unsent records to SD?"
    assert h["dropped"] == len(refused), (h["dropped"], len(refused))
    finish(dev)
    return rec


def A2(seed: int) -> dict:
    dev = device("A2", seed)
    lines = ["keeper auto"] + cps(1400, 100) + ["health end"]
    dev.run(lines)
    refused = read_jsonl(dev.state / "out" / "refused.jsonl")
    h = last_health(dev, "end")
    assert not refused, f"HEAD refused {len(refused)} records with SD room: {refused[:3]}"
    assert h["sd_pending"] > 0, "nothing SD-only - flash overflow not exercised"
    flash_max = max(r["flash_free"] for r in health_rows(dev))
    _ = flash_max
    rec = e2e(dev, {"refused_during_outage": len(refused), "sd_pending_at_end": h["sd_pending"],
                    "reclaimed": h["reclaimed"], "spooled": h["spool_files"]})
    assert rec["first_delivery_in_id_order"], "first deliveries not in id order"
    assert rec["duplicate_count"] == 0
    finish(dev)
    return rec


def _reclaim_ordering(dev: Device) -> dict:
    """ops.jsonl: before every flash remove of ev-N, both SD copies of N were
    renamed into place and the index was synced after them."""
    events = ops(dev)
    checked = 0
    last_idx_sync = -1
    renames: dict[str, int] = {}
    mirror_ok: dict[int, int] = {}
    for i, e in enumerate(events):
        if e["op"] == "sync" and e.get("path", "").endswith("evq.idx"):
            last_idx_sync = i
        if e["op"] == "rename" and e.get("to", "").startswith("sdcard/"):
            renames[e["to"]] = i
            mm = re.search(r"sdcard/evq/m-(\d+)(-\d+)?\.log$", e["to"])
            if mm:
                mirror_ok[int(mm.group(1))] = i
        if e["op"] == "remove" and re.search(r"evstore/events/ev-(\d+)\.log$", e.get("path", "")):
            seq = int(re.search(r"ev-(\d+)\.log$", e["path"]).group(1))
            # only reclaim removes are relevant (archive/evict removes are of delivered files)
            prev_marks = [x for x in events[max(0, i - 400):i] if x["op"] == "mark"]
            if seq in mirror_ok:
                if not (last_idx_sync > mirror_ok[seq]):
                    raise OracleFailure(f"A3: flash ev-{seq} removed before the index sync that follows its mirror rename")
                checked += 1
            _ = prev_marks
    return {"reclaims_checked": checked}


def A3(seed: int) -> dict:
    """Full scale at production constants: 9 MiB partition, 256 KiB rotation."""
    tuning = {"EVLOG_ROTATE_BYTES": 262144, "EVQ_SD_RESERVE_BYTES": 64 * MiB}
    dev = device("A3", seed, tuning=tuning, flash=9437184, sd=512 * MiB)
    lines = ["keeper auto"] + cps(3900, 500) + ["health end"]
    dev.run(lines)
    refused = read_jsonl(dev.state / "out" / "refused.jsonl")
    h = last_health(dev, "end")
    acc_bytes = sum(1 for _ in read_jsonl(dev.state / "out" / "accepted.jsonl"))
    total_bytes = sum(p.stat().st_size for p in (dev.state / "evstore" / "events").glob("ev-*.log")) + \
        sum(p.stat().st_size for p in sd_files(dev, "events"))
    assert not refused, f"refused {len(refused)}"
    assert h["reclaimed"] >= 1
    assert total_bytes >= int(2.5 * 9 * MiB), f"only {total_bytes} B generated"
    order = _reclaim_ordering(dev)
    assert order["reclaims_checked"] >= 1
    rec = e2e(dev, {"generated_bytes": total_bytes, "records": acc_bytes, "reclaimed": h["reclaimed"], **order})
    assert rec["first_delivery_in_id_order"]
    finish(dev)
    return rec


# ═══ B. batching and transfer ═══════════════════════════════════════════════
def B1(seed: int) -> dict:
    dev = device("B1", seed)
    (dev.state / "sdcard" / "archive").mkdir(parents=True)
    foreign = dev.state / "sdcard" / "archive" / "arc-1.log"
    foreign.write_bytes(b"pre-existing archive, not ours\n")
    fh = sha256_file(foreign)
    lines = ["keeper auto"]
    for i in range(100):
        lines += ["store 50 small", "deliver all"]
        if i % 10 == 9:
            lines.append(f"checkpoint b{i}")
    lines += ["health end"]
    dev.run(lines)
    h = last_health(dev, "end")
    assert h["pressure_notifies"] == 0, f"pressure notifications {h['pressure_notifies']}"
    assert 4 <= h["sd_bursts"] <= 6, f"sd bursts {h['sd_bursts']} not ⌈5000/1000⌉±1"
    assert sha256_file(foreign) == fh, "pre-existing arc-1.log changed"
    acc = {r["id"]: r for r in read_jsonl(dev.state / "out" / "accepted.jsonl")}
    checked = 0
    for p in sd_files(dev, "archive"):
        if p == foreign:
            continue
        ids = []
        for _, line in iter_records(p.read_bytes()):
            rid = record_id(line)
            assert rid in acc and line_sha(line) == acc[rid]["line_sha"], f"{p.name}: line for {rid} differs"
            ids.append(rid)
        assert ids == list(range(ids[0], ids[0] + len(ids))), f"{p.name} not a contiguous run"
        checked += 1
    assert checked > 0
    rec = e2e(dev, {"sd_bursts": h["sd_bursts"], "pressure_notifies": h["pressure_notifies"], "archives_verified": checked,
                    "foreign_archive_sha": fh})
    finish(dev)
    return rec


def B2(seed: int) -> dict:
    dev = device("B2", seed, flash=16 * MiB)
    lines = ["keeper auto"]
    for i in range(5):
        lines += ["store 1000 small", f"health batch{i}", f"checkpoint batch{i}"]
    lines += ["deliver all", "health delivered"]
    dev.run(lines)
    for i in range(5):
        h = last_health(dev, f"batch{i}")
        assert h["pressure_notifies"] == 0, f"pressure before batch {i}"
        assert h["spool_files"] > 0 and h["reclaimed"] == 0
    assert len(sd_files(dev, "events")) > 0 and len(sd_files(dev, "evq")) > 0
    ev = ops(dev)
    begin = max(i for i, e in enumerate(ev) if e.get("what") == "deliver_begin")
    end = max(i for i, e in enumerate(ev) if e.get("what") == "deliver_end")
    sd_reads = [e for e in ev[begin:end] if e["op"] == "open_r" and e.get("path", "").startswith("sdcard")]
    assert not sd_reads, f"drain read SD: {sd_reads[:3]}"
    rec = e2e(dev, {"spool_files": last_health(dev, "batch4")["spool_files"], "sd_reads_during_drain": 0})
    finish(dev)
    return rec


def B3(seed: int) -> dict:
    dev = device("B3", seed)
    # manual keeper: observe the state just before and just after the
    # pressure-triggered pass (auto mode runs it inside the store loop)
    dev.run(["keeper manual"])
    crossed = None
    for i in range(80):                      # store until the pressure watermark is crossed
        dev.run(["keeper manual", "store 10", f"health fill{i}"])
        if last_health(dev, f"fill{i}")["pressure_notifies"] >= 1:
            crossed = last_health(dev, f"fill{i}")
            break
    assert crossed is not None, "pressure never reached"
    dev.run(["keeper manual", "health pre", "checkpoint pre", "service", "health post", "checkpoint post",
             "keeper auto"] + cps(300, 50) + ["health end"])
    pre, post, h = last_health(dev, "pre"), last_health(dev, "post"), last_health(dev, "end")
    stores = len(read_jsonl(dev.state / "out" / "attempts.jsonl"))
    assert crossed["pressure_notifies"] >= 1 and pre["flash_free"] * 100 < 2 * MiB * 25 and stores < 1000, \
        "pressure not reached before store #1000"
    assert post["spool_files"] > 0 and post["reclaimed"] >= 1, "no early transfer/reclaim under pressure"
    assert post["flash_free"] * 100 >= 2 * MiB * 40, f"free {post['flash_free']} below the 40% reclaim target"
    removed = []
    for e in ops(dev):
        mm = re.search(r"evstore/events/ev-(\d+)\.log$", e.get("path", "")) if e["op"] == "remove" else None
        if mm:
            removed.append(int(mm.group(1)))
    assert removed == sorted(removed), f"reclaim not oldest-first: {removed}"
    assert max(removed) < h["tail_seq"], "tail removed"
    rec = e2e(dev, {"free_before": pre["flash_free"], "free_after": post["flash_free"], "reclaimed": h["reclaimed"],
                    "pressure_notifies": h["pressure_notifies"], "removed_seqs": removed})
    finish(dev)
    return rec


def B4(seed: int) -> dict:
    """Rename durability at every rename site: crash inside the call (outcomes
    not_applied/applied/both) and right after its return."""
    sites = ["spool.rename.inside_call", "mirror.rename.inside_call", "archive.rename.inside_call",
             "compact.inside_rename", "spool.rename.after_return", "mirror.rename.after_return",
             "archive.after_rename_before_verify", "compact.after_rename"]
    outcomes: dict[str, set] = {}
    runs = []
    tuning = {"EVQ_INDEX_COMPACT_BYTES": 1024}
    for site in sites:
        want = ({"not_applied", "applied", "both"} if not site.startswith("compact") else {"not_applied", "applied"}) \
            if "inside" in site else set()
        for variant in range(40):
            if variant >= 3 and want <= outcomes.get(site, set()):
                break           # every crash outcome for this site produced and repaired
            dev = device(f"B4_{site}", seed * 100 + variant, tuning=tuning)
            dev.no_pending_check = True
            workload = ["keeper auto"] + cps(300, 60) + ["deliver 200", "store 1000 small", "checkpoint post",
                                                         "store 200", "checkpoint post2"]
            dev.run(workload, fault=f"{site}:1:crash")
            both = False
            for e in ops(dev):
                if e["op"] == "rename_inside":
                    outcomes.setdefault(site, set()).add(e["outcome"])
                    both = both or e["outcome"] == "both"
            rec = e2e(dev)
            # Amendment A1: a "both" outcome on an SD rename retires the .tmp
            # name (never unlinks it) and the count on the card is reported.
            evq_dir = dev.state / "sdcard" / "evq"
            xlk = sorted(evq_dir.glob("xlk-*.junk")) if evq_dir.exists() else []
            if both and site.startswith(("spool.", "mirror.")):
                assert xlk, f"{site}: 'both' outcome left no retired xlk name on the card"
            if both and site.startswith("archive."):
                # The archive rename moves the primary itself (no .tmp): both
                # names are archive-grade, the retry takes the next free
                # arc-* name and no archive name is ever unlinked.
                gone = [e["path"] for e in ops(dev) if e["op"] == "remove" and "sdcard/archive/" in e.get("path", "")]
                assert not gone, f"{site}: archive name(s) unlinked after a 'both' rename: {gone[:5]}"
            assert last_health(dev, "final")["sd_retired_names"] == len(xlk), \
                f"{site}: sd_retired_names={last_health(dev, 'final')['sd_retired_names']} but {len(xlk)} xlk on card"
            runs.append({"site": site, "variant": variant, "crashes": dev.crashes, "dups": rec["duplicate_count"]})
            finish(dev)
    for site in sites:
        if "inside" in site:
            want = {"not_applied", "applied", "both"} if not site.startswith("compact") else {"not_applied", "applied"}
            got = outcomes.get(site, set())
            assert want <= got, f"{site}: outcomes {got} missing {want - got}"
    rec = {"scenario": "B4", "seed": seed, "outcomes": {k: sorted(v) for k, v in outcomes.items()}, "runs": runs}
    d = os.environ.get("EVQ_OUT")
    if d:
        p = Path(d) / "B4" / str(seed)
        p.mkdir(parents=True, exist_ok=True)
        (p / "reconcile.json").write_text(json.dumps(rec, indent=1, sort_keys=True))
    return rec


def B5(seed: int) -> dict:
    dev = device("B5", seed)
    dev.run(["keeper auto"] + cps(300, 100))
    last_sync: dict[str, int] = {}
    checked = 0
    for e in ops(dev):
        if e["op"] == "sync":
            last_sync[e["path"]] = e["size"]
        elif e["op"] == "mark" and e["what"].startswith("store_ok "):
            _, rid, path, size = e["what"].split(" ")
            assert last_sync.get(path) == int(size), f"id {rid}: ESP_OK before an fsync covering {size} B of {path}"
            checked += 1
    assert checked == 300
    rec = e2e(dev, {"store_ok_checked": checked})
    finish(dev)
    return rec


# ═══ C. capacity and accounting ═════════════════════════════════════════════
def _fill_to(dev: Device, free_above_reserve: int) -> int:
    """Pre-fill the SD so only `free_above_reserve` bytes remain above the 8 MiB reserve."""
    cap = int(dev.env["EVQ_SD_BYTES"])
    filler = cap - 8 * MiB - free_above_reserve - 64 * 1024
    return max(filler, 0)


def C1(seed: int, keep: bool = False):
    dev = device("C1", seed, sd=64 * MiB)
    fill = _fill_to(dev, 3 * MiB)
    lines = ["keeper auto", f"sd fill {fill}"]
    for i in range(40):
        lines += ["store 60", f"health c{i}", f"checkpoint c{i}"]
    dev.run(lines)
    refused = read_jsonl(dev.state / "out" / "refused.jsonl")
    assert refused, "never refused - SD never ran full"
    first_ref_call = refused[0]["call"]
    rows = [r for r in health_rows(dev) if r["label"].startswith("c")]
    # before the first refusal the SD must have been full
    full_seen = any(r["sd_state"] == "full" for r in rows)
    assert full_seen, "refusals without sd_state=full"
    blocked = [r for r in rows if r["storage_blocked"]]
    assert blocked and all(r["blocked_reason"] == "flash_full_sd_full" for r in blocked), \
        {r["blocked_reason"] for r in blocked}
    nomem = [r for r in refused if r["err"] == ESP_ERR_NO_MEM]
    assert len(nomem) == len(refused)
    assert rows[-1]["refused_full"] == len([r for r in nomem if r.get("boot") == rows[-1].get("boot")])
    info = {"first_refused_call": first_ref_call, "refused": len(refused), "fill": fill}
    if keep:
        return dev, info
    rec = reconcile(dev)
    rec.update(info)
    apply_c19(dev, rec)
    write_evidence(dev, rec)
    assert_e2e(rec, allow_missing=set(rec["missing_ids"]))  # delivery never ran: only integrity here
    raise_c19(rec)
    finish(dev)
    return rec


def C2(seed: int) -> dict:
    dev, info = C1(seed, keep=True)
    dev.scenario = "C2"
    dev.run(["sd unfill", "service", "health after", "store 5", "health after2", "checkpoint c2"])
    h = last_health(dev, "after")
    h2 = last_health(dev, "after2")
    refused_after = [r for r in read_jsonl(dev.state / "out" / "refused.jsonl") if r["call"] > info["first_refused_call"] + 10**9]
    assert not h["storage_blocked"] or not h2["storage_blocked"], "still blocked after SD space returned"
    acc = read_jsonl(dev.state / "out" / "accepted.jsonl")
    assert acc[-1]["call"] == max(r["call"] for r in read_jsonl(dev.state / "out" / "attempts.jsonl")), "store after recovery not ESP_OK"
    rec = e2e(dev, {**info, "refused_after_recovery": len(refused_after)})
    finish(dev)
    return rec


def C3(seed: int) -> dict:
    dev = device("C3", seed)
    lines = ["keeper auto", "sd remove"] + cps(600, 100) + ["health blocked", "deliver 150", "store 30",
                                                          "health after_deliver", "checkpoint d",
                                                          "sd insert", "service", "health inserted", "store 200", "checkpoint i"]
    dev.run(lines)
    hb = last_health(dev, "blocked")
    assert hb["storage_blocked"] and hb["blocked_reason"] == "flash_full_sd_unavailable", hb["blocked_reason"]
    ha = last_health(dev, "after_deliver")
    assert ha["spool_files"] == 0
    hi = last_health(dev, "inserted")
    assert hi["spool_files"] > 0, "spooling did not resume after insert"
    rec = e2e(dev, {"blocked_reason": hb["blocked_reason"], "refused_full": hb["refused_full"]})
    finish(dev)
    return rec


def C4(seed: int) -> dict:
    dev = device("C4", seed)
    dev.no_pending_check = True
    lines = ["keeper auto"] + cps(400, 50) + ["health mid", "sd insert", "service", "health reinserted"] + cps(200, 50, label="t")
    dev.run(lines, fault="mirror.mid_copy:2:remove")
    h = last_health(dev, "mid")
    assert h["spool_errors"] >= 1, "removal mid-spool not counted"
    rec = e2e(dev, {"spool_errors": h["spool_errors"]})
    finish(dev)
    return rec


def C5(seed: int) -> dict:
    out = {}
    for point in ["spool.mid_copy", "spool.before_tmp_fsync", "spool.rename.inside_call", "spool.verify_read_error"]:
        dev = device(f"C5_{point}", seed)
        dev.no_pending_check = True
        lines = ["keeper auto"] + cps(350, 50) + ["health err", "service", "health retry_early", "tick 61000",
                                                 "service", "health retry_late"] + cps(100, 50, label="t")
        dev.run(lines, fault=f"{point}:1:eio")
        he = last_health(dev, "err")
        early = last_health(dev, "retry_early")
        late = last_health(dev, "retry_late")
        assert he["spool_errors"] >= 1, f"{point}: EIO not counted"
        assert early["sd_bursts"] == he["sd_bursts"], f"{point}: retried inside the backoff window"
        assert late["spool_files"] >= he["spool_files"], f"{point}: no retry after backoff"
        out[point] = e2e(dev, {"spool_errors": he["spool_errors"]})
        finish(dev)
    return {"scenario": "C5", "seed": seed, "runs": {k: {"dups": v["duplicate_count"]} for k, v in out.items()}}


def C6(seed: int) -> dict:
    dev = device("C6", seed, sd=64 * MiB)
    fill = _fill_to(dev, 1 * MiB)
    lines = ["keeper auto", f"sd fill {fill}"] + cps(700, 100) + ["health blocked", "deliver all", "service",
                                                                 "store 50", "health resumed", "checkpoint r"]
    dev.run(lines)
    hb = last_health(dev, "blocked")
    assert hb["storage_blocked"]
    refused_before = len(read_jsonl(dev.state / "out" / "refused.jsonl"))
    atts = read_jsonl(dev.state / "out" / "attempts.jsonl")[-50:]
    acc_ids = {r["id"] for r in read_jsonl(dev.state / "out" / "accepted.jsonl")}
    assert all(a["id"] in acc_ids for a in atts), "acquisition did not resume after delivery freed space"
    rec = e2e(dev, {"refused": refused_before})
    finish(dev)
    return rec


def C7(seed: int) -> dict:
    dev = device("C7", seed, tuning={"EVQ_INDEX_CAP": 6})
    dev.run(["keeper auto"] + cps(600, 100) + ["health end"])
    h = last_health(dev, "end")
    assert h["storage_blocked"] and h["blocked_reason"] == "flash_full_index_cap", (h["storage_blocked"], h["blocked_reason"])
    rec = e2e(dev, {"blocked_reason": h["blocked_reason"]})
    finish(dev)
    return rec


def C9(seed: int) -> dict:
    """>20,000 pending records in UNINDEXED (pre-upgrade) files."""
    dev = device("C9", seed, kind="b3f9b8a", flash=16 * MiB)
    dev.cp_hook = None
    dev.run(["keeper manual", "store 21000 tiny", "health pre"])
    dev.exe = exe("head")
    dev.run(["keeper manual", "health boot"])
    hb = last_health(dev, "boot")
    truth = 21000
    assert not hb["pending_exact"] and hb["pending"] <= truth, (hb["pending_exact"], hb["pending"])
    dev.run(["service 30", "health indexed"])
    hi = last_health(dev, "indexed")
    assert hi["pending_exact"] and hi["pending"] == truth, (hi["pending_exact"], hi["pending"])
    rec = e2e(dev, {"pending_at_boot": hb["pending"], "pending_exact_at_boot": hb["pending_exact"],
                    "pending_after_index": hi["pending"]})
    finish(dev)
    return rec


# ═══ E. delivery and ACK ordering ═══════════════════════════════════════════
def _overflowed(dev: Device, n: int = 900) -> None:
    dev.run(["keeper auto"] + cps(n, 150) + ["health overflowed"])
    h = last_health(dev, "overflowed")
    assert h["sd_pending"] > 0 and h["reclaimed"] > 0, "no SD_ONLY backlog built"


def E1(seed: int) -> dict:
    dev = device("E1", seed)
    _overflowed(dev)
    n0 = len(ops(dev))
    dev.run(["keeper manual", "deliver all", "health d"])
    ev = ops(dev)
    removed = set()
    for e in ev[:n0]:
        mm = re.search(r"evstore/events/ev-(\d+)\.log$", e.get("path", "")) if e["op"] == "remove" else None
        if mm:
            removed.add(int(mm.group(1)))
    bad = []
    for e in ev[n0:]:
        mm = re.search(r"evstore/events/ev-(\d+)\.log$", e.get("path", "")) if e["op"] == "remove" else None
        if mm:
            removed.add(int(mm.group(1)))
        if e["op"] == "open_r" and e.get("path", "").startswith("sdcard/evq/"):
            mm = re.search(r"m-(\d+)", e["path"])
            if mm and int(mm.group(1)) not in removed:
                bad.append(e["path"])
    assert not bad, f"SD read of a segment that still had its flash copy: {bad[:3]}"
    rec = e2e(dev)
    assert rec["first_delivery_in_id_order"] and rec["duplicate_count"] == 0
    finish(dev)
    return rec


def E2(seed: int) -> dict:
    dev = device("E2", seed)
    _overflowed(dev, 600)
    dev.run(["deliver all refuse=0.1 shuffle=1", "checkpoint e2"])
    rec = e2e(dev)
    assert rec["first_delivery_in_id_order"] is not None
    finish(dev)
    return rec


def E3(seed: int) -> dict:
    dev = device("E3", seed)
    _overflowed(dev, 600)
    dev.run(["deliver all disc=3", "checkpoint e3"])
    rec = e2e(dev)
    finish(dev)
    return rec


def E4(seed: int) -> dict:
    dev = device("E4", seed)
    dev.no_pending_check = True
    _overflowed(dev, 600)
    dev.run(["deliver all shuffle=1"], fault="ack.after_ram_prefix_before_nvs:5:crash")
    assert dev.crashes == 1
    rec = e2e(dev, dup_bound=16 + 64)
    finish(dev)
    return rec


def E5(seed: int) -> dict:
    dev = device("E5", seed)
    _overflowed(dev, 300)
    dev.run(["claimhold 3", "health before"])
    idx0 = (dev.state / "evstore" / "evq.idx").read_bytes()
    nvs0 = (dev.state / "nvs.txt").read_bytes() if (dev.state / "nvs.txt").exists() else b""
    acc = read_jsonl(dev.state / "out" / "accepted.jsonl")
    held = [r["id"] for r in read_jsonl(dev.state / "out" / "claims.jsonl") if r.get("label") == "claimhold"]
    stale = [999999999, acc[-1]["id"] + 1]
    dev.run([f"ackstale {s}" for s in stale] + ["health after"])
    idx1 = (dev.state / "evstore" / "evq.idx").read_bytes()
    nvs1 = (dev.state / "nvs.txt").read_bytes() if (dev.state / "nvs.txt").exists() else b""
    rows = [r for r in read_jsonl(dev.state / "out" / "claims.jsonl") if r.get("label") == "ackstale"]
    assert all(r["synced"] == 0x103 and r["pending"] == 0x103 for r in rows), rows
    assert idx0 == idx1 and nvs0 == nvs1, "stale ACK changed index/NVS"
    hb, ha = last_health(dev, "before"), last_health(dev, "after")
    for k in ["pending", "rd_seq", "rd_off", "last_acked_id", "skipped"]:
        assert hb[k] == ha[k], (k, hb[k], ha[k])
    rec = e2e(dev, {"held": held})
    finish(dev)
    return rec


def E6(seed: int) -> dict:
    dev = device("E6", seed)
    dev.no_pending_check = True
    _overflowed(dev, 300)
    dev.run(["claimhold 5", "crash"])
    held = [r["id"] for r in read_jsonl(dev.state / "out" / "claims.jsonl") if r.get("label") == "claimhold"]
    rec = e2e(dev)
    delivered = {d["id"] for d in read_jsonl(dev.state / "out" / "delivered.jsonl")}
    assert set(held) <= delivered
    finish(dev)
    return rec


def E7(seed: int) -> dict:
    dev = device("E7", seed)
    dev.no_pending_check = True
    _overflowed(dev, 300)
    dev.run(["deliver all"], fault="ack.after_ram_prefix_before_nvs:3:crash")
    rec = e2e(dev, dup_bound=16)
    finish(dev)
    return rec


def E8(seed: int) -> dict:
    dev = device("E8", seed)
    dev.no_pending_check = True
    _overflowed(dev, 600)
    dev.run(["deliver all", "store 1000 small"], fault="cursor.after_nvs_before_index_delivered:2:crash")
    rec = e2e(dev, dup_bound=16 + 64)
    finish(dev)
    return rec


def E9(seed: int) -> dict:
    dev = device("E9", seed)
    _overflowed(dev, 300)
    dev.run(["claimhold 8", "revert", "claimhold 8", "revert", "deliver 20", "health mid"])
    holds = [r["id"] for r in read_jsonl(dev.state / "out" / "claims.jsonl") if r.get("label") == "claimhold"]
    assert holds[:8] == holds[8:16], "reverted slots not re-claimed first, in order"
    rec = e2e(dev)
    finish(dev)
    return rec


def E10(seed: int) -> dict:
    dev = device("E10", seed)
    lines = ["keeper auto"]
    for i in range(12):
        lines += ["store 120", "deliver 70", "service", f"checkpoint x{i}"]
    dev.run(lines)
    rec = e2e(dev)
    assert rec["duplicate_count"] == 0 and rec["first_delivery_in_id_order"]
    finish(dev)
    return rec


def E11(seed: int) -> dict:
    """Archive of a delivered spooled segment whose renamed copy fails
    verification (EIO on the read-back): nothing is retired, the mirror and
    the primary survive, a later pass archives a VERIFIED copy."""
    dev = device("E11", seed, flash=16 * MiB)
    dev.run(["keeper auto", "store 1100 small", "deliver all", "health d"])
    segs0, _, _ = index_segs(dev)
    spooled = sorted(q for q, s in segs0.items() if s.primary and s.state in ("SPOOLED", "DELIVERED"))
    assert spooled, "nothing spooled to archive"
    victim = segs0[spooled[0]]
    mirror = dev.state / "sdcard" / "evq" / victim.mirror
    primary = dev.state / "sdcard" / "events" / victim.primary
    assert mirror.exists() and primary.exists()
    # arm: the first archive rename's read-back fails
    dev.run(["keeper manual", "store 1000 small", "service", "health a1"], fault="archive.after_rename_before_verify:1:eio")
    assert dev.fault_fired(), "fault never fired"
    segs1, _, _ = index_segs(dev)
    first = next(q for q in spooled if True)
    still = segs1.get(first)
    assert still is not None, "segment retired although its archive failed verification"
    assert (dev.state / "sdcard" / "evq" / still.mirror).exists(), "mirror removed although archive unverified"
    assert (dev.state / "sdcard" / "events" / still.primary).exists(), "primary not restored after failed verification"
    dev.armed = None
    dev.run(["keeper manual", "tick 61000", "service", "store 1000 small", "service", "health a2"])
    segs2, _, _ = index_segs(dev)
    assert first not in segs2, "segment never archived after the failure cleared"
    rec = e2e(dev)
    assert rec["arch_inv"]["checked"] >= 1
    finish(dev)
    return rec


def E12(seed: int) -> dict:
    """Eval R2-F1: a delivered segment whose primary is PERMANENTLY damaged
    (readable, wrong bytes) next to a byte-exact mirror. The keeper must not
    stall on it: the damaged bytes are kept aside (A1 bad-*), the archive is
    made from the verified mirror, the entry retires, later segments keep
    spooling, and flash never refuses while the card has room."""
    dev = device("E12", seed)
    dev.run(["keeper auto", "store 1100 small", "deliver all", "health d"])
    segs0, _, _ = index_segs(dev)
    spooled = sorted(q for q, s in segs0.items() if s.primary and s.mirror and s.state in ("SPOOLED", "DELIVERED"))
    assert spooled, "nothing spooled to corrupt"
    victim = segs0[spooled[0]]
    primary = dev.state / "sdcard" / "events" / victim.primary
    mirror = dev.state / "sdcard" / "evq" / victim.mirror
    good = mirror.read_bytes()
    assert primary.read_bytes() == good, "precondition: primary and mirror byte-identical"
    raw = bytearray(good)
    mid = len(raw) // 2
    raw[mid] = ord("x") if raw[mid] != ord("x") else ord("y")   # same length, reads back fine, wrong CRC
    damaged = bytes(raw)
    primary.write_bytes(damaged)
    h0 = last_health(dev, "d")
    # six keeper periods with work due (the batch trigger reopens the transfer burst)
    lines = ["keeper manual"]
    for i in range(6):
        lines += ["store 200 small", "tick 61000", "service", f"health k{i}"]
    dev.run(lines)
    segs1, _, _ = index_segs(dev)
    assert victim.seq not in segs1, f"damaged-primary segment {victim.seq} never retired: {segs1.get(victim.seq)}"
    bad = sorted((dev.state / "sdcard" / "evq").glob(f"bad-{victim.seq:06d}-*.log"))
    assert bad and any(p.read_bytes() == damaged for p in bad), "damaged primary bytes not preserved in evq/bad-*"
    assert not primary.exists(), "damaged primary left in the rollback-import directory"
    assert not mirror.exists(), "mirror kept although a verified archive exists"
    arcs = [p for p in (dev.state / "sdcard" / "archive").glob("arc-*.log") if p.read_bytes() == good]
    assert arcs, "no archive copy byte-identical to the verified mirror"
    hk = last_health(dev, "k5")
    assert hk["sd_bad_copies"] >= 1, "sd_bad_copies not reported"
    # capacity: undelivered stores well past the flash size keep overflowing to SD
    refused0 = len(read_jsonl(dev.state / "out" / "refused.jsonl"))
    spool0 = hk["spool_files"]
    cap = ["keeper auto"]
    for i in range(8):
        cap += ["store 500", "tick 61000", "service", f"health c{i}"]
    dev.run(cap)
    hc = last_health(dev, "c7")
    refused = read_jsonl(dev.state / "out" / "refused.jsonl")[refused0:]
    assert not refused, f"{len(refused)} store(s) refused while the SD had room: {refused[:3]}"
    assert not hc["storage_blocked"], f"storage blocked: {hc['blocked_reason']}"
    assert hc["spool_files"] > spool0 or hc["sd_pending"] > h0["sd_pending"], "no spooling after the damaged primary"
    assert hc["sd_pending"] > 0, "undelivered overflow never reached SD"
    rec = e2e(dev, {"victim": victim.seq, "bad_copies": [p.name for p in bad], "archive_copies": [p.name for p in arcs]})
    assert rec["arch_inv"]["checked"] >= 1
    finish(dev)
    return rec


def E13(seed: int) -> dict:
    """Eval R2-F1, unreadable variant: the delivered primary's data clusters
    fail every read (persistent EIO; rename still works). Retried with
    backoff a bounded number of passes, then treated as damaged: kept aside
    byte-for-byte, archived from the verified mirror, retired; spooling and
    stores continue."""
    dev = device("E13", seed)
    dev.run(["keeper auto", "store 1100 small", "deliver all", "health d"])
    segs0, _, _ = index_segs(dev)
    spooled = sorted(q for q, s in segs0.items() if s.primary and s.mirror and s.state in ("SPOOLED", "DELIVERED"))
    assert spooled, "nothing spooled"
    victim = segs0[spooled[0]]
    primary = dev.state / "sdcard" / "events" / victim.primary
    mirror = dev.state / "sdcard" / "evq" / victim.mirror
    orig = primary.read_bytes()
    dev.env["EVQ_SHIM_BADREAD"] = f"sdcard/events/{victim.primary}"
    lines = ["keeper manual"]
    for i in range(8):
        lines += ["store 200 small", "tick 300000", "service", f"health k{i}"]
    dev.run(lines)
    fails = [e for e in ops(dev) if e.get("fail") == "badread"]
    assert len(fails) >= 2, f"primary read retried {len(fails)}x: the bounded retry never ran"
    segs1, _, _ = index_segs(dev)
    assert victim.seq not in segs1, f"unreadable-primary segment {victim.seq} never retired: {segs1.get(victim.seq)}"
    bad = sorted((dev.state / "sdcard" / "evq").glob(f"bad-{victim.seq:06d}-*.log"))
    assert bad and any(p.read_bytes() == orig for p in bad), "unreadable primary not preserved in evq/bad-*"
    assert not primary.exists() and not mirror.exists(), "primary/mirror left behind after the verified archive"
    assert any(p.read_bytes() == orig for p in (dev.state / "sdcard" / "archive").glob("arc-*.log")), \
        "no verified archive copy"
    h = last_health(dev, "k7")
    assert h["sd_bad_copies"] >= 1 and not h["storage_blocked"], f"health: {h}"
    refused = [r for r in read_jsonl(dev.state / "out" / "refused.jsonl")]
    assert not refused, f"{len(refused)} store(s) refused"
    del dev.env["EVQ_SHIM_BADREAD"]
    rec = e2e(dev, {"victim": victim.seq, "badread_fails": len(fails)})
    finish(dev)
    return rec


def HOLD1(seed: int) -> dict:
    """Sprint 02 HOLD-1 (docs/evq-sd-overflow-hil-contract.md §2.1): the
    verification build's SD boot hold, installed before event_log_init, keeps
    event_log off the card entirely. Pre-existing sentinels of every record
    kind (indexed primary + mirror, archive, legacy import candidate, orphan
    mirror, .tmp siblings, retired xlk, bad copy) stay byte-identical and the
    trace shows ZERO SD operations of any kind (not even opens or stats)
    through init, index rebuild, keeper passes, pressure, a claim at an
    SD_ONLY head, a remount notify and a power-guard park/un-park. After
    `sd_release` normal processing resumes (the legacy sentinel is imported)."""
    import hashlib
    dev = device("HOLD1", seed)
    dev.no_pending_check = True
    _overflowed(dev, 900)                       # SPOOLED/SD_ONLY segments with primary + mirror
    dev.run(["keeper auto", "deliver 150", "store 1000 small", "service 2", "health pre"])
    sd = dev.state / "sdcard"
    _write_legacy(dev, 880000, 12, name="ev-880000.log")
    (sd / "evq").mkdir(parents=True, exist_ok=True)
    (sd / "archive").mkdir(parents=True, exist_ok=True)
    (sd / "evq" / "m-999999.log").write_bytes(b"orphan mirror sentinel\n")
    (sd / "events" / "ev-777777.tmp").write_bytes(b"tmp sentinel\n")
    (sd / "evq" / "xlk-3.junk").write_bytes(b"retired name sentinel\n")
    (sd / "evq" / "bad-000004-0.log").write_bytes(b"bad copy sentinel\n")
    (sd / "archive" / "arc-424242.log").write_bytes(b"archive sentinel\n")
    segs, _, _ = index_segs(dev)
    assert any(g.state == "SD_ONLY" for g in segs.values()), "precondition: an SD_ONLY segment"
    assert list(sd.glob("archive/arc-*.log")), "precondition: archive files"

    def snap() -> dict:
        return {str(p.relative_to(sd)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in sorted(sd.rglob("*")) if p.is_file()}

    before = snap()
    n0 = len(ops(dev))
    dev.exe = exe("head", {"EVQ_HIL_HOST": 1})
    dev.run(["keeper auto", "health held0", "hil_state", "store 400", "service 10", "tick 61000", "service 3",
             "claimcode c1", "claimcode c2", "sd park", "sd unpark", "service 3", "deliver 20", "health held1"])
    held = ops(dev)[n0:]
    sd_ops = [e for e in held if e.get("path", "").startswith("sdcard") or e.get("to", "").startswith("sdcard")]
    assert not sd_ops, f"{len(sd_ops)} SD op(s) while held, first: {sd_ops[:3]}"
    after = snap()
    assert after == before, f"SD changed while held: {sorted(set(after.items()) ^ set(before.items()))[:6]}"
    h1 = last_health(dev, "held1")
    assert h1["sd_state"] == "parked", f"sd_state {h1['sd_state']} while held"
    codes = [c for c in read_jsonl(dev.state / "out" / "claims.jsonl") if c.get("label") in ("c1", "c2")]
    assert codes and all(c["name"] != "ESP_ERR_NOT_FOUND" for c in codes), f"held claim codes {codes}"
    # release: the next boot keeps the RTC "released" state (EVQ_HIL_RELEASED)
    dev.env["EVQ_HIL_RELEASED"] = "1"
    n1 = len(ops(dev))
    dev.run(["keeper auto", "service 3", "health rel"])
    rel_ops = [e for e in ops(dev)[n1:] if e.get("path", "").startswith("sdcard")]
    assert rel_ops, "no SD processing after release"
    assert not (sd / "events" / "ev-880000.log").exists() or last_health(dev, "rel")["reimported"] >= 0
    rec = e2e(dev, {"held_sd_ops": 0, "sentinels": len(before)})
    finish(dev)
    return rec


# ═══ F. SD missing / mismatched / unreadable at an SD_ONLY head ══════════════
def _sd_only_head(dev: Device) -> None:
    _overflowed(dev, 900)
    segs, _, _ = index_segs(dev)
    cseq, _, _ = cursor_from_nvs(dev.state)
    head = segs.get(cseq) if cseq else segs.get(min(segs))
    assert head is not None and head.state == "SD_ONLY", f"head not SD_ONLY: {head}"


def _claim_codes(dev: Device) -> list[dict]:
    return read_jsonl(dev.state / "out" / "claims.jsonl")


def F1(seed: int, keep: bool = False):
    dev = device("F1", seed)
    _sd_only_head(dev)
    nvs0 = (dev.state / "nvs.txt").read_bytes()
    idx0 = (dev.state / "evstore" / "evq.idx").read_bytes()
    lines = ["sd remove", "claimcode r0", "health removed"]
    for i in range(6):
        lines += ["tick 43200000", f"claimcode r{i+1}", "deliver all", "service"]
    lines += ["health days"]
    dev.run(lines)
    codes = [c for c in _claim_codes(dev) if str(c.get("label", "")).startswith("r")]
    assert codes and all(c["code"] == ESP_ERR_NOT_FINISHED for c in codes), codes
    assert (dev.state / "nvs.txt").read_bytes() == nvs0, "NVS cursor moved while waiting"
    assert (dev.state / "evstore" / "evq.idx").read_bytes() == idx0, "index changed while waiting"
    h = last_health(dev, "days")
    assert h["sd_pending"] > 0 and h["pending"] >= h["sd_pending"] and h["deliverable_pending"] == 0, h
    assert h["sd_state"] in ("absent", "lost"), h["sd_state"]
    if keep:
        return dev
    dev.run(["sd insert"])
    rec = e2e(dev, {"waiting_codes": len(codes)})
    finish(dev)
    return rec


def F2(seed: int, keep: bool = False):
    dev = F1(seed, keep=True)
    dev.scenario = "F2"
    dev.run(cps(400, 100, label="f") + ["health full"])
    h = last_health(dev, "full")
    refused = read_jsonl(dev.state / "out" / "refused.jsonl")
    assert refused and h["storage_blocked"] and h["blocked_reason"] == "flash_full_backlog_waiting", h["blocked_reason"]
    assert h["refused_full"] == len([r for r in refused if r["err"] == ESP_ERR_NO_MEM and r.get("boot") == h.get("boot")])
    if keep:
        return dev
    dev.run(["sd insert"])
    rec = e2e(dev)
    finish(dev)
    return rec


def F3(seed: int) -> dict:
    dev = F2(seed, keep=True)
    dev.scenario = "F3"
    dev.run(["sd insert", "service", "health back"])
    rec = e2e(dev)
    assert rec["first_delivery_in_id_order"] and rec["duplicate_count"] == 0
    assert not last_health(dev, "final")["storage_blocked"]
    finish(dev)
    return rec


def F4(seed: int) -> dict:
    dev = device("F4", seed)
    _sd_only_head(dev)
    dev.run(["sd swap B deadbeef", "health swapped", "store 100", "deliver all", "service", "service",
             "claimcode m0", "health swapped2"])
    h1, h2 = last_health(dev, "swapped"), last_health(dev, "swapped2")
    assert h1["sd_state"] == "mismatch" and h2["sd_ops"] == h1["sd_ops"], (h1["sd_state"], h1["sd_ops"], h2["sd_ops"])
    newcard = list((dev.state / "sdcard").rglob("*"))
    assert not newcard, f"mismatched card was written: {newcard[:3]}"
    dev.run(cps(500, 100, label="m") + ["health full"])
    hf = last_health(dev, "full")
    if hf["storage_blocked"]:
        assert hf["blocked_reason"] in ("flash_full_sd_mismatch", "flash_full_backlog_waiting"), hf["blocked_reason"]
    assert hf["sd_ops"] == h1["sd_ops"], "operations on the mismatched card"
    dev.run(["sd swap c1d00001 c1d00001", "service"])
    rec = e2e(dev, {"sd_ops_while_mismatched": hf["sd_ops"] - h1["sd_ops"]})
    assert rec["first_delivery_in_id_order"]
    finish(dev)
    return rec


def F5(seed: int) -> dict:
    dev = device("F5", seed, flash=16 * MiB)
    dev.run(["keeper auto", "store 1100 small", "health spooled"])
    h = last_health(dev, "spooled")
    assert h["spool_files"] > 0 and h["reclaimed"] == 0
    dev.run(["sd swap B 0badcafe", "store 1000 small", "service", "health adopted"])
    segs, _, _ = index_segs(dev)
    cids = {s.cid for s in segs.values() if s.primary}
    assert cids <= {0x0BADCAFE}, f"old CID still referenced: {cids}"
    rec = e2e(dev)
    finish(dev)
    return rec


def _damage(p: Path, how: str) -> None:
    if how == "missing":
        p.unlink()
    elif how == "truncate":
        with open(p, "r+b") as f:
            f.truncate(p.stat().st_size // 2)
    else:
        b = bytearray(p.read_bytes())
        b[len(b) // 2] ^= 0x01
        p.write_bytes(bytes(b))


def F6(seed: int) -> dict:
    out = {}
    for how in ["missing", "truncate", "bitflip"]:
        dev = device(f"F6_{how}", seed)
        _sd_only_head(dev)
        segs, _, _ = index_segs(dev)
        cseq, _, _ = cursor_from_nvs(dev.state)
        head = segs.get(cseq) or segs[min(segs)]
        prim = dev.state / "sdcard" / "events" / head.primary
        _damage(prim, how)
        dmg_hash = sha256_file(prim) if prim.exists() else None
        rec = e2e(dev)
        h = last_health(dev, "final")
        assert h["mirror_used"] >= 1, "mirror not used"
        assert rec["first_delivery_in_id_order"]
        kept = [p for p in (dev.state / "sdcard" / "evq").glob("bad-*.log")]
        if dmg_hash:
            # the damaged primary is preserved byte-for-byte for inspection, out of
            # the import directory, and never delivered (R-E2E hashes above)
            assert any(sha256_file(p) == dmg_hash for p in kept), f"damaged primary not preserved: {kept}"
            assert all(sha256_file(p) != dmg_hash for p in (dev.state / "sdcard" / "events").glob("*")), \
                "damaged primary left in the rollback-import directory"
        else:
            assert not kept
        out[how] = {"mirror_used": h["mirror_used"], "kept_damaged": [p.name for p in kept]}
        finish(dev)
    return {"scenario": "F6", "seed": seed, "runs": out}


def F7(seed: int) -> dict:
    dev = device("F7", seed)
    _sd_only_head(dev)
    segs, _, _ = index_segs(dev)
    cseq, _, _ = cursor_from_nvs(dev.state)
    head = segs.get(cseq) or segs[min(segs)]
    prim = dev.state / "sdcard" / "events" / head.primary
    mir = dev.state / "sdcard" / "evq" / head.mirror
    saved = prim.read_bytes()
    _damage(prim, "bitflip")
    _damage(mir, "truncate")
    h_prim, h_mir = sha256_file(prim), sha256_file(mir)
    n_before = len(read_jsonl(dev.state / "out" / "delivered.jsonl"))
    dev.run(["claimcode k0", "deliver all", "service", "claimcode k1", "health corrupt"])
    h = last_health(dev, "corrupt")
    codes = [c for c in _claim_codes(dev) if str(c.get("label", "")).startswith("k")]
    assert all(c["code"] == ESP_ERR_NOT_FINISHED for c in codes), codes
    assert h["sd_state"] == "backlog_corrupt", h["sd_state"]
    assert len(read_jsonl(dev.state / "out" / "delivered.jsonl")) == n_before, "records delivered past a corrupt head"
    assert sha256_file(prim) == h_prim and sha256_file(mir) == h_mir, "corrupt copies modified"
    prim.write_bytes(saved)            # restore an intact copy (operator action)
    dev.run(["sd remove", "sd insert"])
    rec = e2e(dev)
    assert rec["first_delivery_in_id_order"]
    finish(dev)
    return rec


def F8(seed: int) -> dict:
    dev = device("F8", seed)
    _sd_only_head(dev)
    dev.run(["sd park", "health p0", "claimcode p", "deliver all", "service", "store 50", "health p1"])
    h0, h1 = last_health(dev, "p0"), last_health(dev, "p1")
    assert h1["sd_state"] == "parked" and h1["sd_ops"] == h0["sd_ops"], (h1["sd_state"], h0["sd_ops"], h1["sd_ops"])
    codes = [c for c in _claim_codes(dev) if c.get("label") == "p"]
    assert codes and codes[0]["code"] == ESP_ERR_NOT_FINISHED
    dev.run(["sd unpark"])
    rec = e2e(dev)
    finish(dev)
    return rec


# ═══ G. corruption and torn state ═══════════════════════════════════════════
def G1(seed: int) -> dict:
    dev = device("G1", seed)
    dev.no_pending_check = True
    dev.run(["keeper auto"] + cps(200, 50), fault="store.after_write_before_fsync:120:crash")
    assert dev.crashes == 1
    tail = sorted((dev.state / "evstore" / "events").glob("ev-*.log"))[-1]
    with open(tail, "ab") as f:
        f.write(b"123456\tuart_0\ttorn")     # a torn fragment after the last sync
    dev.run(["health after"])
    rec = e2e(dev)
    finish(dev)
    return rec


def G2(seed: int) -> dict:
    dev = device("G2", seed, kind="b3f9b8a")
    dev.cp_hook = None
    dev.run(["keeper manual", "store 5"])
    tail = sorted((dev.state / "evstore" / "events").glob("ev-*.log"))[-1]
    with open(tail, "ab") as f:
        f.write(b"999999999\tmalformed pre-upgrade line without fields\n")
    dev.run(["store 5"])
    dev.exe = exe("head")
    dev.cp_hook = pending_truth_hook
    dev.extra_pending = 1
    dev.no_pending_check = True
    dev.run(["keeper auto", "store 20", "health h"])
    rec = e2e(dev)
    h = last_health(dev, "final")
    assert h["quarantined_malformed"] == 1, h["quarantined_malformed"]
    assert 999999999 in quarantined_ids(dev.state) and 999999999 not in {d["id"] for d in read_jsonl(dev.state / "out" / "delivered.jsonl")}
    finish(dev)
    return rec


def G3(seed: int) -> dict:
    dev = device("G3", seed)
    _overflowed(dev, 600)
    p = dev.state / "evstore" / "evq.idx"
    lines = p.read_bytes().split(b"\n")
    mid = len(lines) // 2
    lines[mid] = re.sub(rb"\*[0-9a-f]{8}$", b"*00000000", lines[mid])
    data = b"\n".join(lines) + b"S 999 1 2 3 4 torn-line-without-crc"
    p.write_bytes(data)
    dev.no_pending_check = True
    rec = e2e(dev)
    finish(dev)
    return rec


def G4(seed: int) -> dict:
    out = {}
    for how in ["deleted", "zeroed"]:
        dev = device(f"G4_{how}", seed)
        dev.no_pending_check = True
        _overflowed(dev, 600)
        p = dev.state / "evstore" / "evq.idx"
        if how == "deleted":
            p.unlink()
        else:
            p.write_bytes(b"\0" * p.stat().st_size)
        rec = e2e(dev)
        out[how] = {"dups": rec["duplicate_count"]}
        finish(dev)
    return {"scenario": "G4", "seed": seed, "runs": out}


def G5(seed: int) -> dict:
    out = {}
    for how in ["legacy_behind", "legacy_ahead", "misaligned"]:
        dev = device(f"G5_{how}", seed)
        dev.no_pending_check = True
        _overflowed(dev, 600)
        dev.run(["deliver 150"])
        kv = nvs_read(dev.state)
        seq = int.from_bytes(kv[("evlog", "rd_seq")], "little")
        off = int.from_bytes(kv[("evlog", "rd_off")], "little")
        if how == "legacy_behind":
            kv[("evlog", "rd_off")] = (0).to_bytes(4, "little")
        elif how == "legacy_ahead":
            kv[("evlog", "rd_seq")] = (seq + 2).to_bytes(4, "little")
            kv[("evlog", "rd_off")] = (0).to_bytes(4, "little")
        else:
            del kv[("evlog", "cur")]
            kv[("evlog", "rd_off")] = (off + 7 if off > 0 else 7).to_bytes(4, "little")
        nvs_write(dev.state, kv)
        rec = e2e(dev)
        out[how] = {"dups": rec["duplicate_count"], "reimported": last_health(dev, "final")["reimported"]}
        finish(dev)
    return {"scenario": "G5", "seed": seed, "runs": out}


def G6(seed: int) -> dict:
    dev = device("G6", seed)
    dev.no_pending_check = True
    dev.run(["keeper auto", "store 1100 small", "health spooled"])
    segs, _, _ = index_segs(dev)
    sp = [s for s in segs.values() if s.state == "SPOOLED"]
    assert sp
    victim = dev.state / "sdcard" / "events" / sp[0].primary
    with open(victim, "r+b") as f:
        f.truncate(victim.stat().st_size - 10)
    dev.run(cps(300, 50, label="g") + ["health after"])
    h = last_health(dev, "after")
    assert h["spool_errors"] >= 1, "reclaim did not refuse a damaged copy"
    rec = e2e(dev)
    finish(dev)
    return rec


def G7(seed: int) -> dict:
    dev = device("G7", seed, flash=16 * MiB)
    dev.run(["keeper auto", "store 400"])
    segs, _, _ = index_segs(dev)
    cseq, _, _ = cursor_from_nvs(dev.state)
    first = min(s for s in segs if segs[s].state == "FLASH")
    (dev.state / "evstore" / "events" / f"ev-{first:06d}.log").unlink()     # the ONLY copy
    nvs0 = (dev.state / "nvs.txt").read_bytes() if (dev.state / "nvs.txt").exists() else b""
    dev.run(["claimcode w0", "deliver all", "claimcode w1", "health w"])
    codes = [c for c in _claim_codes(dev) if str(c.get("label", "")).startswith("w")]
    h = last_health(dev, "w")
    assert all(c["code"] == ESP_ERR_NOT_FINISHED for c in codes), codes
    assert h["sd_state"] == "backlog_missing" and h["head_block"] == "backlog_missing", (h["sd_state"], h["head_block"])
    assert h["skipped"] == 0 and not read_jsonl(dev.state / "out" / "delivered.jsonl")
    nvs1 = (dev.state / "nvs.txt").read_bytes() if (dev.state / "nvs.txt").exists() else b""
    assert nvs0 == nvs1
    rec = {"scenario": "G7", "seed": seed, "codes": codes, "sd_state": h["sd_state"]}
    full = {**reconcile(dev), **rec}
    apply_c19(dev, full)
    write_evidence(dev, full)
    raise_c19(full)
    finish(dev)
    return rec


def G8(seed: int) -> dict:
    out = {}
    # (a) rotated flash file with a verified SD copy
    dev = device("G8a", seed, flash=16 * MiB)
    dev.run(["keeper auto", "store 1100 small"])
    segs, _, _ = index_segs(dev)
    sp = min(s.seq for s in segs.values() if s.state == "SPOOLED")
    _damage(dev.state / "evstore" / "events" / f"ev-{sp:06d}.log", "bitflip")
    dev.run(["health x"])
    out["a"] = e2e(dev)
    ha = last_health(dev, "final")
    assert ha["corrupt_detected"] >= 1
    finish(dev)
    # (b) rotated flash file with no SD copy
    dev = device("G8b", seed, flash=16 * MiB)
    dev.run(["keeper auto", "store 300"])
    segs, _, _ = index_segs(dev)
    fl = min(s.seq for s in segs.values() if s.state == "FLASH")
    _damage(dev.state / "evstore" / "events" / f"ev-{fl:06d}.log", "bitflip")
    dev.run(["sd remove", "claimcode b0", "deliver all", "health b"])
    hb = last_health(dev, "b")
    assert hb["sd_state"] == "backlog_corrupt" and hb["corrupt_medium"] == "flash", (hb["sd_state"], hb["corrupt_medium"])
    assert not read_jsonl(dev.state / "out" / "delivered.jsonl")
    out["b"] = {"sd_state": hb["sd_state"]}
    finish(dev)
    # (c) tail file
    dev = device("G8c", seed, flash=16 * MiB)
    dev.no_pending_check = True
    dev.run(["keeper auto", "store 30"])
    tail = sorted((dev.state / "evstore" / "events").glob("ev-*.log"))[-1]
    data = bytearray(tail.read_bytes())
    recs = list(iter_records(bytes(data)))
    off, line = recs[len(recs) // 2]
    victim = record_id(line)
    tab = data.index(b"\t", off)
    data[tab] = ord("X")                      # break the field structure
    data[tab + 1: tab + 1] = b""
    fields_end = off + len(line)
    seg = bytes(data[off:fields_end]).replace(b"\t", b"#")
    corrupted = seg[:-1] + b"\n"                # no tab: no parseable id, no fields
    data[off:fields_end] = corrupted
    tail.write_bytes(bytes(data))
    dev.allow_lost = {victim}
    rec = e2e(dev, allow_missing={victim})
    hc = [r for r in health_rows(dev) if r.get("corrupt_detected")]
    assert rec["missing_ids"] == [victim], rec["missing_ids"]
    assert hc and hc[-1]["corrupt_detected"] == 1, hc[-1:] if hc else "never reported"
    qbytes = (dev.state / "evstore" / "events" / "quarantine.log").read_bytes()
    assert corrupted in qbytes, "the corrupted bytes were not preserved exactly in quarantine.log"
    assert record_id(corrupted) is None
    out["c"] = {"missing": rec["missing_ids"], "corrupt_detected": hc[-1]["corrupt_detected"],
                "quarantined_bytes": len(corrupted)}
    finish(dev)
    return {"scenario": "G8", "seed": seed, "runs": {"a": {"dups": out["a"]["duplicate_count"]}, "b": out["b"], "c": out["c"]}}


# ═══ H. discovery, migration, rollback ══════════════════════════════════════
def _write_legacy(dev: Device, first_id: int, n: int, name: str | None = None, extra: bytes = b"") -> Path:
    d = dev.state / "sdcard" / "events"
    d.mkdir(parents=True, exist_ok=True)
    lines = []
    acc = dev.state / "out"
    acc.mkdir(exist_ok=True)
    with open(acc / "accepted.jsonl", "a") as f:
        import hashlib
        for i in range(n):
            rid = first_id + i
            payload = '{"synthetic_test":true,"legacy_fixture":true,"k":%d}' % rid
            line = f"{rid}\tuart_1\t28:37:2F:FF:E7:04\tMEASUREMENT\tarrun legacy\t{1757000000000 + rid}\t{1757000001000 + rid}\t\t{payload}\n"
            lines.append(line.encode())
            h = lambda s: hashlib.sha256(s.encode()).hexdigest()
            f.write(json.dumps({"kind": "fixture", "id": rid, "channel": "uart_1", "device": "28:37:2F:FF:E7:04",
                                "tag": "MEASUREMENT", "cmd_sha": h("arrun legacy"), "start_ms": 1757000000000 + rid,
                                "end_ms": 1757000001000 + rid, "meta_sha": h(""), "payload_sha": h(payload),
                                "line_sha": hashlib.sha256(line.encode()).hexdigest()}) + "\n")
    p = d / (name or f"ev-{first_id:06d}.log")
    p.write_bytes(b"".join(lines) + extra)
    return p


def _baseline_fixture(dev: Device, rev: str = "b3f9b8a") -> dict:
    dev.exe = exe(rev)
    dev.cp_hook = None
    dev.durability_mode = "record"
    dev.run(["keeper auto", "store 1300 small", "deliver 1100", "store 200 small", "deliver 40", "health base"])
    (dev.state / "sdcard" / "archive").mkdir(parents=True, exist_ok=True)
    (dev.state / "sdcard" / "archive" / "arc-5-77.log").write_bytes(b"suffixed archive fixture\n")
    (dev.state / "sdcard" / "logs").mkdir(parents=True, exist_ok=True)
    (dev.state / "sdcard" / "logs" / "sd_logger.txt").write_bytes(b"unrelated sd_logger output\n" * 10)
    (dev.state / "sdcard" / "ambit").mkdir(parents=True, exist_ok=True)
    (dev.state / "sdcard" / "ambit" / "ambit_fw_1.4.0.bin").write_bytes(os.urandom(4096))
    (dev.state / "evstore" / "events" / "quarantine.log").write_bytes(b"4242\tq\tfixture quarantine line\n")
    _write_legacy(dev, 800000, 40)
    tail = sorted((dev.state / "evstore" / "events").glob("ev-*.log"))[-1]
    with open(tail, "ab") as f:
        f.write(b"555555\tuart_0\ttorn-fixture")
    kv = nvs_read(dev.state)
    return {"nid": int.from_bytes(kv.get(("evlog", "nid"), b"\0" * 8), "little"),
            "legacy_cursor": (int.from_bytes(kv[("evlog", "rd_seq")], "little"), int.from_bytes(kv[("evlog", "rd_off")], "little"))}


def H1(seed: int, crash_point: str | None = None, sd_absent: bool = False, scn: str = "H1") -> dict:
    dev = device(scn, seed)
    dev.no_pending_check = True
    fx = _baseline_fixture(dev)
    before = media_hashes(dev.state)
    unrelated = {k: v for k, v in before.items() if "/logs/" in k or "/ambit/" in k or k.endswith("arc-5-77.log")}
    dev.exe = exe("head")
    boot = ["keeper manual", "health boot"]
    if sd_absent:
        boot = ["sd remove"] + boot + ["service 3", "health nosd"]
    elif crash_point:
        boot = boot + ["service 3", "deliver all", "service 3"]
    dev.run(boot, fault=crash_point)
    if crash_point:
        assert dev.crashes == 1, f"{crash_point} never fired"
    hb = [r for r in health_rows(dev) if r["label"] == "boot"][-1] if any(r["label"] == "boot" for r in health_rows(dev)) else None
    kv = nvs_read(dev.state)
    cur = (int.from_bytes(kv[("evlog", "rd_seq")], "little"), int.from_bytes(kv[("evlog", "rd_off")], "little"))
    if crash_point is None and not sd_absent:
        assert cur == fx["legacy_cursor"], f"cursor changed at upgrade: {fx['legacy_cursor']} -> {cur}"
    if hb is not None:
        assert hb["next_id"] >= fx["nid"], (hb["next_id"], fx["nid"])
    if sd_absent:
        segs, _, _ = index_segs(dev)
        assert not any(s.primary for s in segs.values()), "SD entries created while SD absent"
        dev.run(["sd insert"])
    rec = e2e(dev)
    after = media_hashes(dev.state)
    for k, v in unrelated.items():
        assert after.get(k) == v, f"unrelated SD file changed: {k}"
    changed = sorted(k for k in before if before[k] != after.get(k))
    allowed = re.compile(r"^(evstore/events/ev-\d+\.log|sdcard/events/ev-\d+\.log|evstore/events/quarantine\.log|evstore/evq\.idx)$")
    bad = [k for k in changed if not allowed.match(k)]
    assert not bad, f"pre-existing files changed outside documented transitions: {bad}"
    q = (dev.state / "evstore" / "events" / "quarantine.log").read_bytes()
    assert q.startswith(b"4242\tq\tfixture quarantine line\n"), "pre-existing quarantine.log rewritten"
    rec.update({"fixture": fx, "changed_preexisting": changed, "crash_point": crash_point, "sd_absent_at_boot": sd_absent})
    write_evidence(dev, rec, media_before=before)
    finish(dev)
    return rec


def _h2_adopt(seed: int) -> dict:
    """boot.adopt_mid: an interrupted spool (copies committed, SPOOLED line
    never written) is ADOPTED on the next pass; crash in the middle of that."""
    dev = device("H2_boot.adopt_mid", seed)
    dev.no_pending_check = True
    dev.run(["keeper auto", "store 1000 small"], fault="spool.after_verify_before_index:1:crash")
    assert dev.crashes == 1
    dev.run(["keeper manual", "store 1000 small", "service 2"], fault="boot.adopt_mid:1:crash")
    assert dev.crashes == 2, "boot.adopt_mid never fired"
    rec = e2e(dev, dup_bound=16 + 64)
    names = [s.primary for s in index_segs(dev)[0].values() if s.primary]
    assert len(names) == len(set(names)), "a copy was adopted twice"
    finish(dev)
    return rec


def H2(seed: int) -> dict:
    runs = {}
    base = None
    for point in ["boot.index_rebuild_mid", "import.mid_append", "import.after_fsync_before_sd_remove"]:
        r = H1(seed, crash_point=f"{point}:1:crash", scn=f"H2_{point}")
        runs[point] = {"dups": r["duplicate_count"], "crashes": r["crashes"]}
    r = _h2_adopt(seed)
    runs["boot.adopt_mid"] = {"dups": r["duplicate_count"], "crashes": r["crashes"]}
    base = H1(seed, scn="H2_uninterrupted")
    return {"scenario": "H2", "seed": seed, "runs": runs, "uninterrupted_delivered": base["delivered_distinct"]}


def H3(seed: int) -> dict:
    return H1(seed, sd_absent=True, scn="H3")


def H4(seed: int) -> dict:
    out = {}
    for rev in ["b3f9b8a", "v2.2.3"]:
        dev = device(f"H4_{rev}", seed)
        dev.no_pending_check = True
        _overflowed(dev, 900)
        segs, _, _ = index_segs(dev)
        sd_only = [s for s in segs.values() if s.state == "SD_ONLY"]
        assert sd_only
        # rollback: baseline keeper + delivery on the SAME flash/SD/NVS
        dev.exe = exe(rev)
        dev.run(["keeper auto"] + ["service", "deliver all"] * 60 + ["drain_all 200", "health base"])
        tmp_imported = [e for e in ops(dev) if e["op"] == "open_r" and (".tmp" in e.get("path", "") or "sdcard/evq/" in e.get("path", ""))
                        and False]
        base_deliv = {d["id"] for d in read_jsonl(dev.state / "out" / "delivered.jsonl")}
        acc = {r["id"] for r in read_jsonl(dev.state / "out" / "accepted.jsonl")}
        missing_after_baseline = sorted(acc - base_deliv)
        # back to HEAD
        dev.exe = exe("head")
        dev.run(["keeper auto", "service 3", "health back"])
        rec = e2e(dev)
        h = last_health(dev, "final")
        all_ids = set(acc)
        for p in list((dev.state / "sdcard").rglob("ev-*.log")):
            for _, line in iter_records(p.read_bytes()):
                rid = record_id(line)
                if rid:
                    all_ids.add(rid)
        dev.run(["store 1000 small"])
        new = [r["id"] for r in read_jsonl(dev.state / "out" / "accepted.jsonl")][-1000:]
        assert min(new) > max(all_ids - set(new)), "id reuse after rollback round trip"
        assert h["next_id"] > max(all_ids - set(new))
        untracked = []
        segs, _, _ = index_segs(dev)
        known = {s.primary for s in segs.values()} | {s.mirror for s in segs.values()}
        for sub in ["events", "evq"]:
            for p in sd_files(dev, sub):
                if p.name not in known:
                    untracked.append(str(p.relative_to(dev.state)))
        assert not untracked, f"untracked files left: {untracked}"
        out[rev] = {"sd_only_before_rollback": len(sd_only), "missing_after_baseline": len(missing_after_baseline),
                    "dups": rec["duplicate_count"], "reimported": h["reimported"]}
        write_evidence(dev, {**rec, **out[rev]})
        finish(dev)
    return {"scenario": "H4", "seed": seed, "variants": out}


def H5(seed: int) -> dict:
    dev = device("H5", seed)
    dev.no_pending_check = True
    ev = dev.state / "sdcard" / "events"
    mir = dev.state / "sdcard" / "evq"
    arc = dev.state / "sdcard" / "archive"
    for d in (ev, mir, arc):
        d.mkdir(parents=True, exist_ok=True)
    # ids start at 1 → first segment first_id is 1; its seq is 1
    # primary name of seq 1 (first_id 1) and its first fallback: foreign legacy-format files
    _write_legacy(dev, 1_000_001, 3, name="ev-000001.log")
    _write_legacy(dev, 1_100_001, 3, name="ev-3000000001.log")
    (mir / "m-000001.log").write_bytes(b"foreign mirror collision\n")
    (mir / "m-000001-1.log").write_bytes(b"foreign mirror collision 2\n")
    (arc / "arc-1.log").write_bytes(b"foreign archive collision\n")
    before = media_hashes(dev.state)
    dev.run(["keeper auto"] + cps(700, 100) + ["deliver 300", "store 1000 small", "health h"])
    after = media_hashes(dev.state)
    legacy_names = {"sdcard/events/ev-000001.log", "sdcard/events/ev-3000000001.log"}
    for k, v in before.items():
        if k in legacy_names and k not in after:
            continue           # left via the documented legacy import (records delivered below)
        assert after.get(k) == v, f"pre-existing {k} changed in place"
    segs, _, _ = index_segs(dev)
    names = [s.primary for s in segs.values() if s.primary] + [s.mirror for s in segs.values() if s.mirror]
    assert len(names) == len(set(names))
    assert not ({"ev-000001.log", "ev-3000000001.log", "m-000001.log", "m-000001-1.log"} & set(names))
    rec = e2e(dev)
    after2 = media_hashes(dev.state)
    for k in ["sdcard/evq/m-000001.log", "sdcard/evq/m-000001-1.log", "sdcard/archive/arc-1.log"]:
        assert after2.get(k) == before[k], f"{k} changed"
    for k in legacy_names:
        assert k not in after2 or after2[k] == before[k], f"{k} rewritten"
    rec["names_used"] = sorted(names)[:20]
    write_evidence(dev, rec, media_before=before)
    finish(dev)
    return rec


def H6(seed: int) -> dict:
    dev = device("H6", seed)
    dev.no_pending_check = True
    _overflowed(dev, 900)
    dev.run(["deliver 400", "rewind 0", "rewind 99999", "health rw"])
    rows = [r for r in read_jsonl(dev.state / "out" / "claims.jsonl") if r.get("label") == "rewind"]
    segs, _, _ = index_segs(dev)
    flash = [int(re.search(r"ev-(\d+)", p.name).group(1)) for p in (dev.state / "evstore" / "events").glob("ev-*.log")]
    qmin = min([s.seq for s in segs.values() if s.state in ("SPOOLED", "SD_ONLY")] + flash)
    assert rows[0]["seq"] == qmin, (rows[0], qmin)
    assert rows[1]["seq"] <= rows[0]["seq"], "rewind moved forward"
    rec = e2e(dev)
    finish(dev)
    return rec


# ═══ Q. quarantine classes (outside the accepted set) ═══════════════════════
def Q1(seed: int) -> dict:
    dev = device("Q1", seed)
    dev.no_pending_check = True
    dev.run(["keeper auto", "store 10", "store_poison", "store 10"])
    poison = read_jsonl(dev.state / "out" / "poison.jsonl")
    assert poison and poison[0]["rc"] == 0
    rec = e2e(dev)
    h = last_health(dev, "final")
    pid = poison[0]["id"]
    assert h["quarantined_poison"] == 1, h["quarantined_poison"]
    assert pid in quarantined_ids(dev.state)
    assert pid not in {d["id"] for d in read_jsonl(dev.state / "out" / "delivered.jsonl")}
    # intact copy before the cursor moved: the quarantine sync precedes the cursor commit
    finish(dev)
    return rec


def Q2(seed: int) -> dict:
    dev = device("Q2", seed)
    dev.no_pending_check = True
    big = b"7777777\t" + b"x" * 70000 + b"\n"
    _write_legacy(dev, 900000, 5, name="ev-000050.log", extra=b"bad legacy line\n" + big)
    dev.run(["keeper auto", "store 10", "service", "health q"])
    h = last_health(dev, "q")
    assert h["quarantined_malformed"] == 2, h["quarantined_malformed"]
    q = (dev.state / "evstore" / "events" / "quarantine.log").read_bytes()
    assert b"bad legacy line" in q and b"7777777\t" in q, "malformed legacy bytes not preserved"
    rec = e2e(dev)
    finish(dev)
    return rec


def Q3(seed: int) -> dict:
    dev = device("Q3", seed, kind="b3f9b8a")
    dev.cp_hook = None
    dev.run(["keeper manual", "store 3"])
    tail = sorted((dev.state / "evstore" / "events").glob("ev-*.log"))[-1]
    with open(tail, "ab") as f:
        f.write(b"888888888\tmalformed\n")
    dev.run(["store 3"])
    dev.exe = exe("head")
    dev.no_pending_check = True
    dev.run(["keeper auto", "deliver all", "health q3", "claimcode q"], fault="quarantine.before_copy:1:eio")
    h = last_health(dev, "q3")
    assert h["quarantined_malformed"] == 0, "quarantine claimed although its write failed"
    deliv = [d["id"] for d in read_jsonl(dev.state / "out" / "delivered.jsonl")]
    acc = [r["id"] for r in read_jsonl(dev.state / "out" / "accepted.jsonl")]
    assert deliv == acc[:3], "cursor moved past the frontier despite the failed quarantine write"
    rec = e2e(dev)
    assert 888888888 in quarantined_ids(dev.state), "malformed frontier line never quarantined after the write recovered"
    finish(dev)
    return rec


# ═══ X. SD write integrity (Sprint 1: contract §5.3 C-13..C-20, §5.8 C-50..C-52) ═
# Every X scenario takes `kind` ("head" = working tree, "rev:<sha>" = the full
# baseline build of an indexed-store revision) and `strict`. strict (default
# for head) raises on any oracle or GREEN-signature failure; non-strict
# returns the record with its signature, oracle errors and GREEN errors so
# red_green.py can judge a baseline as RED. Each run: fault phase → signature
# snapshot → simulated reboot (C-20) → recovery + E2E → C-18/C-19 + diag.
ESP_FAIL = -1
ESP_ERR_INVALID_SIZE = 0x104
ESP_ERR_NOT_SUPPORTED = 0x106
ESP_ERR_TIMEOUT = 0x107
REFUSAL_REASON = {ESP_ERR_NO_MEM: "full", ESP_FAIL: "media", ESP_ERR_INVALID_SIZE: "too_large",
                  ESP_ERR_NOT_SUPPORTED: "unavailable", ESP_ERR_TIMEOUT: "unavailable"}
SIMULATED_REBOOT = ("SIMULATED power loss: harness restart + host media durability transform "
                    "(not an electrical interruption, not an SD power cut)")
X_SEEDS = [1, 7, 1337]


def _x_device(name: str, seed: int, kind: str, flash: int = 16 * MiB, env: dict | None = None) -> Device:
    dev = device(name, seed, kind=kind, flash=flash, env=env)
    dev.x_kind = kind  # type: ignore[attr-defined]
    if kind != "head":
        # a baseline is expected to break oracles: record, never abort mid-run
        dev.cp_hook = None
        dev.durability_mode = "record"
    return dev


def _all_zero(p: Path) -> bool:
    b = p.read_bytes() if p.exists() else b""
    return len(b) > 0 and b.count(0) == len(b)


def _pending_truth_durable(dev: Device) -> int:
    """Accepted records the DURABLE cursor has not passed: exactly what a
    rebooted store owes (un-persisted ACKs are re-delivered, never skipped)."""
    cseq, coff, _ = cursor_from_nvs(dev.state)
    n = 0
    for r in read_jsonl(dev.state / "out" / "accepted.jsonl"):
        m = re.search(r"ev-(\d+)\.log$", r.get("tail", ""))
        if not m or r.get("tail_size", -1) < 0:
            continue
        q, end = int(m.group(1)), r["tail_size"]
        if not (q < cseq or (q == cseq and end <= coff)):
            n += 1
    return n


def _x_render_check(dev: Device) -> list[str]:
    """C-32/C-52: pending renders as exact ("pending=N") only with
    pending_exact=1, otherwise as a floor ("pending>=N")."""
    bad = []
    for r in health_rows(dev):
        if "pending_text" not in r:
            continue
        want = ("pending=" if r["pending_exact"] else "pending>=") + str(r["pending"])
        if r["pending_text"] != want:
            bad.append(f"C-52: health {r['label']} renders {r['pending_text']!r}, expected {want!r}")
    return bad


def _x_diag(dev: Device, label: str) -> dict | None:
    rows = [r for r in read_jsonl(dev.state / "out" / "sddiag.jsonl") if r.get("label") == label]
    return rows[-1]["diag"] if rows else None


def _x_diag_checks(dev: Device, diag: dict | None, faults: dict[str, int]) -> list[str]:
    """C-29/C-30 for event_log: evlog fault counters equal exactly the injected
    SD faults; refusal counts/first/last id exact and disjoint from accepted."""
    if diag is None:
        return ["C-29: no sd_diag block rendered"]
    errs = []
    if diag.get("faults") != faults:
        errs.append(f"C-29: sd_diag faults {diag.get('faults')} != injected {faults}")
    if faults:
        last = diag.get("last") or {}
        ops_set = {k.split(".", 1)[1] for k in faults}
        if last.get("w") != "evlog" or last.get("op") not in ops_set or last.get("errno") != 5:
            errs.append(f"C-29: sd_diag last fault {last} is not an evlog {sorted(ops_set)} EIO")
    refused = read_jsonl(dev.state / "out" / "refused.jsonl")
    want = {"full": 0, "media": 0, "too_large": 0, "unavailable": 0}
    for r in refused:
        want[REFUSAL_REASON.get(r["err"], "unavailable")] += 1
    got = diag.get("refused", {})
    for k, n in want.items():
        if got.get(k) != n:
            errs.append(f"C-30: sd_diag refused.{k}={got.get(k)} but {n} store call(s) were refused {k}")
    ids = [r["id"] for r in refused if r["id"] > 0]
    first, last = (ids[0], ids[-1]) if ids else (0, 0)
    if (got.get("first_id"), got.get("last_id")) != (first, last):
        errs.append(f"C-30: sd_diag refused first/last id {got.get('first_id')}/{got.get('last_id')} != {first}/{last}")
    acc = {r["id"] for r in read_jsonl(dev.state / "out" / "accepted.jsonl")}
    if set(ids) & acc or {got.get("first_id"), got.get("last_id")} & acc:
        errs.append("C-30: an accepted id is reported refused")
    return errs


def _x_reboot(dev: Device, sig: dict) -> list[str]:
    """C-20: simulated reboot mid-scenario; pending must be exact afterwards."""
    n0 = dev.crashes
    dev.run(["crash", "keeper manual", "health boot", "tick 61000", "service 3", "health xb", "checkpoint xb"])
    if dev.crashes != n0 + 1:
        raise HarnessError("simulated reboot did not happen")
    hb = last_health(dev, "xb")
    truth = _pending_truth_durable(dev)
    sig["reboot"] = {"kind": SIMULATED_REBOOT, "pending": hb["pending"], "pending_exact": hb["pending_exact"],
                     "pending_truth_durable": truth, "transforms": len(dev.transforms)}
    if not hb["pending_exact"] or hb["pending"] != truth:
        return [f"C-20: after the simulated reboot pending={hb['pending']} exact={hb['pending_exact']}, "
                f"durable truth {truth}"]
    return []


def _x_lifecycle(dev: Device) -> None:
    """After the reboot: a transfer burst (retry/adopt the failed copy), full
    delivery, then another burst so delivered segments ARCHIVE (the primary is
    renamed into the archive, the mirror dropped): every later owner of a name
    touched by the fault gets exercised before the E2E."""
    dev.run(["keeper auto", "tick 61000", "store 1000 small", "health xc", "checkpoint xc", "deliver all",
             "tick 61000", "store 1000 small", "health xd", "checkpoint xd"])


def _x_e2e(dev: Device, extra: dict) -> tuple[dict, list[str]]:
    """Recovery + C-18 (accepted-record oracle) + C-19; returns (rec, errors)."""
    errors: list[str] = []
    dev.run(["drain_all", "service 2", "health final", "checkpoint final", "sddiag final"])
    rec = reconcile(dev)
    rec.update(extra)
    c = _last_good_copy_oracle(dev)
    c["mode"] = "assert"
    rec["c19"] = c
    errors += c["violations"] if c["n_violations"] else []
    errors += c["c18_cursor_violations"]
    try:
        rec["arch_inv"] = check_arch_inv(dev)
    except OracleFailure as e:
        rec["arch_inv"] = {"error": str(e)}
        errors.append(str(e))
    try:
        assert_e2e(rec)
    except OracleFailure as e:
        errors.append(f"C-18 {e}")
    m = load_manifests(dev.state)
    att = {r["id"] for r in read_jsonl(dev.state / "out" / "attempts.jsonl")}
    delivered = {d["id"] for d in m["delivered"]}
    # "no id gap": every accepted id is delivered (ids themselves may jump
    # across a reboot: next_id is reserved in NVS blocks — never reused)
    gap = sorted(set(m["accepted"]) - delivered)
    both = sorted(set(m["accepted"]) & set(m["refused"]))
    unclassified = sorted(att - set(m["accepted"]) - set(m["refused"]) - set(m["indeterminate"]))
    rec["c18"] = {"attempted": len(att), "accepted": len(m["accepted"]), "delivered_distinct": len(delivered),
                  "id_gap": gap[:10], "accepted_and_refused": both[:10], "unclassified": unclassified[:10],
                  "duplicates_counted": rec["duplicate_count"], "cursor_rule_violations": c["n_c18_cursor"],
                  "delivered_equals_accepted": delivered - set(m["indeterminate"]) == set(m["accepted"])}
    if gap:
        errors.append(f"C-18: accepted ids never delivered (gap): {gap[:10]}")
    if not rec["c18"]["delivered_equals_accepted"]:
        errors.append("C-18: delivered set != accepted set")
    if unclassified:
        errors.append(f"C-18: store calls not classified: {unclassified[:10]}")
    if both:
        errors.append(f"C-18: ids both accepted and refused: {both[:10]}")
    errors += _x_render_check(dev)
    for f in dev.durability_findings:
        errors.append(f"R-DUR at {f['checkpoint']}: {f['finding']}")
    return rec, errors


def _x_post(dev: Device, strict: bool, extra: dict, sig: dict, phases) -> tuple[dict, list[str]]:
    """Run the post-fault phases (`phases(dev) -> errors`, then recovery/E2E).
    Strict (head in run.py): any harness/oracle abort propagates. Non-strict
    (red_green): an abort is recorded — the fault-phase signature (the RED
    evidence) was already captured — and the record is still produced."""
    try:
        errors = phases(dev)
        rec, e2 = _x_e2e(dev, extra)
        return rec, errors + e2
    except (HarnessError, OracleFailure) as e:
        if strict:
            raise
        sig["post_fault_abort"] = f"{type(e).__name__}: {str(e)[:1500]}"
        rec = reconcile(dev)
        rec.update(extra)
        return rec, [f"post-fault phase aborted: {sig['post_fault_abort']}"]


def _x_rev_info(kind: str) -> dict:
    rev = kind[4:]
    for p in build_root().glob("*.rewrite.json"):
        d = json.loads(p.read_text())
        if d.get("mode") == "full" and d.get("rev") == rev:
            return d
    return build.rev_full_sources(rev, build_root() / f"rewrite_full_{re.sub(r'[^A-Za-z0-9]', '_', rev)}")


def _x_conclude(dev: Device, rec: dict, sig: dict, errors: list[str], green: list[str], red: dict | None,
                strict: bool) -> dict:
    rec["signature"] = sig
    rec["oracle_errors"] = errors[:40]
    rec["green_errors"] = green
    rec["red"] = red
    rec["variant"] = dev.x_kind  # type: ignore[attr-defined]
    if dev.x_kind != "head":  # type: ignore[attr-defined]
        k = dev.x_kind  # type: ignore[attr-defined]
        rec["baseline_sources"] = _x_rev_info(k) if k.startswith("rev:") else baseline_rewrite_info(k)
    write_evidence(dev, rec)
    if strict and (errors or green):
        raise OracleFailure("; ".join((errors + green)[:8]))
    finish(dev)
    return rec


def _x_ops_since(dev: Device, i0: int) -> list[dict]:
    return ops(dev)[i0:]


def _x_lifecycle_delivered_first(dev: Device) -> None:
    """Variant: the segment whose commit failed is DELIVERED before any retry
    re-spools it, so the archive takes the never-spooled (flash) path and
    meets the interrupted spool's leftover SD copies."""
    dev.run(["keeper auto", "deliver all", "tick 61000", "store 1000 small", "health xc", "checkpoint xc",
             "deliver all", "tick 61000", "store 1000 small", "health xd", "checkpoint xd"])


def _x_commit_both_run(name: str, fp: str, seed: int, kind: str, prelude: list[str] | None,
                       phase: list[str] | None, delivered_first: bool, strict: bool):
    dev = _x_device(name, seed, kind)
    if prelude:
        dev.run(prelude)
    fault = f"{fp}.rename.inside_call:1:both_eio"
    n0 = len(ops(dev))
    dev.run((phase or ["keeper auto", "store 1100 small"]) + ["health xa", "checkpoint xa", "sddiag xa"], fault=fault)
    if not dev.fault_fired():
        raise HarnessError(f"{name}: {fault} never fired")
    ev = _x_ops_since(dev, n0)
    be = [e for e in ev if e["op"] == "rename_both_eio"]
    if not be:
        raise HarnessError(f"{name}: armed both_eio did not reach a rename")
    tmp, dst = be[0]["from"], be[0]["to"]
    dstp = dev.state / dst
    xl_a = [e for e in ev if e["op"] == "xlink_unlink"]
    xlk = sorted((dev.state / "sdcard" / "evq").glob("xlk-*.junk")) if (dev.state / "sdcard" / "evq").exists() else []
    # S1 policy: neither name of the cross-linked pair stays in service; both are
    # retired to evq/xlk-* (two junk names on the one chain), never unlinked.
    inos = [x.stat().st_ino for x in xlk]
    pair_retired = any(inos.count(i) >= 2 for i in inos)
    twin = (not dstp.exists()) and (not (dev.state / tmp).exists()) and pair_retired
    ha = last_health(dev, "xa")
    sig = {"fault": fault, "lifecycle": "delivered_first" if delivered_first else "retry_first", "tmp": tmp,
           "dst": dst, "xlink_unlink_after_fault": len(xl_a),
           "dst_exists_after_fault": dstp.exists(), "dst_zero_filled_after_fault": _all_zero(dstp),
           "tmp_exists_after_fault": (dev.state / tmp).exists(), "xlk_after_fault": [x.name for x in xlk],
           "tmp_and_dst_retired_as_pair": twin, "sd_rename_ambiguous_after_fault": ha["sd_rename_ambiguous"]}
    life = _x_lifecycle_delivered_first if delivered_first else _x_lifecycle

    def phases(d: Device) -> list[str]:
        errs = _x_reboot(d, sig)
        life(d)
        return errs
    rec, errors = _x_post(dev, strict, {"fault": fault, "lifecycle": sig["lifecycle"]}, sig, phases)
    ev = ops(dev)
    xl = [e for e in ev if e["op"] == "xlink_unlink"]
    sig["xlink_unlink_total"] = len(xl)
    sig["xlink_unlinked"] = [{"path": e["path"], "twins": e.get("twins"), "why": e.get("why")} for e in xl][:10]
    arc_rm = [e["path"] for e in ev if e["op"] == "remove" and re.search(r"sdcard/archive/arc-[^/]*\.log$", e.get("path", ""))]
    sig["arc_unlinked"] = arc_rm[:10]
    diag = _x_diag(dev, "final")
    sig["diag"] = diag
    red = {"expect": "RED", "observed": sig["xlink_unlink_after_fault"] >= 1 and sig["dst_zero_filled_after_fault"],
           "rule": ">=1 xlink_unlink at the failed commit AND the committed SD copy (dst) reads zero"}
    green = []
    if sig["xlink_unlink_total"] != 0:
        green.append(f"GREEN[{sig['lifecycle']}]: {sig['xlink_unlink_total']} xlink_unlink (expected 0): "
                     f"{sig['xlink_unlinked'][:3]}")
    if not twin:
        green.append(f"GREEN[{sig['lifecycle']}]: tmp (and its committed twin) not both retired to evq/xlk-*")
    if sig["sd_rename_ambiguous_after_fault"] != 1:
        green.append(f"GREEN[{sig['lifecycle']}]: sd_rename_ambiguous={sig['sd_rename_ambiguous_after_fault']} (expected 1)")
    if arc_rm:
        green.append(f"GREEN[{sig['lifecycle']}]: archive name(s) unlinked: {arc_rm[:3]}")
    green += [f"[{sig['lifecycle']}] {g}" for g in _x_diag_checks(dev, diag, {"evlog.rename": 1})]
    return dev, rec, sig, errors, green, red


def _x_commit_both(name: str, fp: str, seed: int, kind: str, strict: bool, prelude: list[str] | None = None,
                   phase: list[str] | None = None) -> dict:
    """C-13/C-14/C-15: the commit rename of `fp` fails with BOTH names left
    on one chain (hard link + EIO). Base unlinks tmp → frees dst's clusters
    (RED: xlink_unlink + zero-filled committed copy). S1 retires tmp to
    evq/xlk-* and counts sd_rename_ambiguous. For spool/mirror the committed
    twin is followed through BOTH lifecycles (retry-before-delivery, and
    delivered-before-retry: evidence under <name>_dfirst/<seed>); an unlink of
    the twin in either is a C-19(b) violation."""
    dev, rec, sig, errors, green, red = _x_commit_both_run(name, fp, seed, kind, prelude, phase, False, strict)
    if fp in ("spool", "mirror"):
        d2, rec2, sig2, err2, green2, _ = _x_commit_both_run(f"{name}_dfirst", fp, seed, kind, prelude, phase, True,
                                                             strict)
        _x_conclude(d2, rec2, sig2, err2, green2, None, False)
        sig["delivered_first"] = {k: sig2.get(k) for k in ("xlink_unlink_total", "xlink_unlinked", "arc_unlinked",
                                                           "tmp_retired_as_dst_twin", "reboot", "post_fault_abort")}
        errors += [f"[delivered_first] {e}" for e in err2]
        green += green2
    return _x_conclude(dev, rec, sig, errors, green, red, strict)


def X1(seed: int, kind: str = "head", strict: bool | None = None) -> dict:
    """C-13 spool-primary commit rename both_eio: tmp retired (xlk-*), never unlinked."""
    return _x_commit_both("X1", "spool", seed, kind, kind == "head" if strict is None else strict)


def X2(seed: int, kind: str = "head", strict: bool | None = None) -> dict:
    """C-14 mirror commit rename both_eio: as C-13."""
    return _x_commit_both("X2", "mirror", seed, kind, kind == "head" if strict is None else strict)


def X3(seed: int, kind: str = "head", strict: bool | None = None) -> dict:
    """C-15 archive copy-commit rename both_eio (never-spooled delivered segment copied from flash)."""
    return _x_commit_both("X3", "archive", seed, kind, kind == "head" if strict is None else strict,
                          prelude=["keeper manual", "store 300 small", "deliver all", "health d0", "checkpoint d0"],
                          phase=["keeper auto", "store 1000 small"])


def X4(seed: int, kind: str = "head", strict: bool | None = None) -> dict:
    """C-16 read-back mismatch after a successful commit rename: dst kept under evq/bad-*, flash copy retained."""
    strict = kind == "head" if strict is None else strict
    dev = _x_device("X4", seed, kind)
    fault = "spool.verify_read_error:1:flip"
    dev.run(["keeper auto", "store 1100 small", "health xa", "checkpoint xa", "sddiag xa"], fault=fault)
    if not dev.fault_fired():
        raise HarnessError(f"X4: {fault} never fired")
    ev = ops(dev)
    fi = next((i for i, e in enumerate(ev) if e["op"] == "flip"), None)
    if fi is None:
        raise HarnessError("X4: armed flip did not reach a read-back")
    dst, off = ev[fi]["path"], ev[fi]["offset"]
    first = int(re.search(r"ev-(\d+)\.log$", dst).group(1))
    segs, _, _ = index_segs(dev)
    seq = next((q for q, sg in segs.items() if sg.first == first), None)
    flash = dev.state / "evstore" / "events" / f"ev-{seq:06d}.log" if seq is not None else None
    unlinked = any(e["op"] == "remove" and e.get("path") == dst for e in ev[fi:])
    bad = sorted((dev.state / "sdcard" / "evq").glob("bad-*.log")) if (dev.state / "sdcard" / "evq").exists() else []
    expect = None
    if flash is not None and flash.exists():
        b = bytearray(flash.read_bytes())
        b[off] ^= 0x01
        expect = bytes(b)
    ha = last_health(dev, "xa")
    sig = {"fault": fault, "dst": dst, "flip_offset": off, "seq": seq, "dst_unlinked": unlinked,
           "bad_copies": [p.name for p in bad], "bad_holds_written_bytes": expect is not None and any(p.read_bytes() == expect for p in bad),
           "flash_copy_retained": bool(flash is not None and flash.exists()),
           "sd_verify_fail_after_fault": ha["sd_verify_fail"], "sd_bad_copies_after_fault": ha["sd_bad_copies"]}
    def phases(d: Device) -> list[str]:
        errs = _x_reboot(d, sig)
        _x_lifecycle(d)
        return errs
    rec, errors = _x_post(dev, strict, {"fault": fault}, sig, phases)
    diag = _x_diag(dev, "final")
    sig["diag"] = diag
    red = {"expect": "RED", "observed": unlinked and not bad,
           "rule": "the committed copy that failed read-back is unlinked and no evq/bad-* copy exists"}
    green = []
    if unlinked:
        green.append("GREEN: dst unlinked after the failed read-back")
    if not sig["bad_holds_written_bytes"]:
        green.append(f"GREEN: no evq/bad-* holds the written (mismatching) bytes: {sig['bad_copies']}")
    if not sig["flash_copy_retained"]:
        green.append("GREEN: flash copy not retained after the failed verification")
    if sig["sd_verify_fail_after_fault"] != 1:
        green.append(f"GREEN: sd_verify_fail={sig['sd_verify_fail_after_fault']} (expected 1)")
    green += _x_diag_checks(dev, diag, {"evlog.verify": 1})
    return _x_conclude(dev, rec, sig, errors, green, red, strict)


def X5(seed: int, kind: str = "head", strict: bool | None = None) -> dict:
    """C-17 commit/archive rename applied_eio (applied, reported failed): no loss, dups counted not skipped."""
    strict = kind == "head" if strict is None else strict
    dev = _x_device("X5", seed, kind)
    phases = [("spool.rename.inside_call:1:applied_eio", ["keeper auto", "store 1100 small", "health x1", "checkpoint x1"]),
              ("mirror.rename.inside_call:1:applied_eio", ["tick 61000", "store 1000 small", "health x2", "checkpoint x2"]),
              ("archive.rename.inside_call:1:applied_eio", ["deliver all", "tick 61000", "store 1000 small", "health x3",
                                                            "checkpoint x3"])]
    fired = []
    for fault, lines in phases:
        dev.armed = None
        dev.run(lines, fault=fault)
        if not dev.fault_fired():
            raise HarnessError(f"X5: {fault} never fired")
        fired.append(fault)
    ev = ops(dev)
    applied = [{"from": e["from"], "to": e["to"]} for e in ev if e["op"] == "rename" and e.get("applied_eio")]
    sig = {"faults": fired, "applied_renames": applied,
           "sd_rename_ambiguous": [last_health(dev, x)["sd_rename_ambiguous"] for x in ("x1", "x2", "x3")]}
    def phases(d: Device) -> list[str]:
        errs = _x_reboot(d, sig)
        _x_lifecycle(d)
        return errs
    rec, errors = _x_post(dev, strict, {"faults": fired}, sig, phases)
    diag = _x_diag(dev, "final")
    sig["diag"] = diag
    sig["duplicate_count"] = rec.get("duplicate_count")
    sig["missing"] = len(rec.get("missing_ids", []))
    green = []
    if len(applied) != 3:
        green.append(f"GREEN: {len(applied)} applied-then-EIO renames observed (expected 3)")
    if sig["sd_rename_ambiguous"][0] != 1:
        green.append(f"GREEN: sd_rename_ambiguous after the spool fault = {sig['sd_rename_ambiguous'][0]} (expected 1)")
    if not isinstance(rec.get("duplicate_count"), int):
        green.append("GREEN: duplicates not counted")
    green += _x_diag_checks(dev, diag, {"evlog.rename": len(applied)})
    return _x_conclude(dev, rec, sig, errors, green, {"expect": "any"}, strict)


def _x6_hook(dev: Device, label: str) -> None:
    """C-51, checked at EVERY refusal (driver checkpoint `refused_<id>`)."""
    if dev.x_kind == "head" and not label.startswith("refused_"):  # type: ignore[attr-defined]
        pending_truth_hook(dev, label)
    if not label.startswith("refused_"):
        return
    rid = int(label.split("_", 1)[1])
    st = dev.state
    cseq, coff, _ = cursor_from_nvs(st)
    segs, _, _ = index_segs(dev)
    p = st / ".shim" / "ops.jsonl"
    with open(p, "rb") as f:
        f.seek(dev.x6_off)  # type: ignore[attr-defined]
        chunk = f.read()
    dev.x6_off += len(chunk)  # type: ignore[attr-defined]
    dev.x6_ops.extend(json.loads(x) for x in chunk.decode().splitlines() if x.strip())  # type: ignore[attr-defined]
    evs = dev.x6_ops  # type: ignore[attr-defined]
    b = max((i for i, e in enumerate(evs) if e.get("op") == "mark" and e.get("what") == f"store_begin {rid}"), default=None)
    r = max((i for i, e in enumerate(evs) if e.get("op") == "mark" and e.get("what", "").startswith(f"store_refused {rid} ")), default=None)
    evicted_after = []
    if b is not None and r is not None:
        span = evs[b:r]
        fail = next((k for k, e in enumerate(span) if "fail" in e and e.get("path", "").startswith("evstore/")), None)
        if fail is not None:
            evicted_after = [e["path"] for e in span[fail:] if e.get("op") == "remove" and
                             re.match(r"evstore/events/ev-\d+\.log$", e.get("path", ""))]
    remaining = []
    for fp in sorted((st / "evstore" / "events").glob("ev-*.log")):
        q = int(re.search(r"ev-(\d+)", fp.name).group(1))
        sg = segs.get(q)
        if q < cseq and not (sg is not None and sg.state == "REIMPORT"):
            remaining.append(str(fp.relative_to(st)))      # delivered, unpinned (no keeper pass in a store), evictable
    dev.x6_refusals += 1  # type: ignore[attr-defined]
    if evicted_after or remaining:
        dev.x6_c51.append({"refused_id": rid, "durable_cursor": [cseq, coff],  # type: ignore[attr-defined]
                           "evictable_when_refused": (evicted_after + remaining)[:8],
                           "evicted_only_after_the_refusal": evicted_after[:8], "still_present": remaining[:8]})


def X6(seed: int, kind: str = "head", strict: bool | None = None) -> dict:
    """C-50..C-52 (AMBYTE194): full internal store, SD unmounted, concurrent drain; remount resumes."""
    strict = kind == "head" if strict is None else strict
    dev = _x_device("X6", seed, kind, flash=1 * MiB, env={"EVQ_CP_ON_REFUSE": "1"})
    dev.x6_off, dev.x6_ops, dev.x6_c51, dev.x6_refusals = 0, [], [], 0  # type: ignore[attr-defined]
    dev.cp_hook = _x6_hook
    lines = ["keeper auto", "sd remove"]
    for i in range(60):
        lines += ["store 12", "deliver 7"]
        if i % 6 == 5:
            lines.append(f"checkpoint f{i}")
    lines += ["health full", "checkpoint full", "sddiag full"]
    dev.run(lines)
    hf = last_health(dev, "full")
    refused = read_jsonl(dev.state / "out" / "refused.jsonl")
    m = load_manifests(dev.state)
    att = {r["id"] for r in read_jsonl(dev.state / "out" / "attempts.jsonl")}
    n_full = sum(1 for r in refused if r["err"] == ESP_ERR_NO_MEM)
    sig = {"refused": len(refused), "refused_full": n_full, "refusals_checked_c51": dev.x6_refusals,  # type: ignore[attr-defined]
           "health_refused_full": hf["refused_full"], "blocked_reason": hf["blocked_reason"],
           "classified": {"attempted": len(att), "accepted": len(m["accepted"]), "refused": len(m["refused"]),
                          "indeterminate": len(m["indeterminate"])}}
    errors: list[str] = []
    if not refused:
        raise HarnessError("X6: the internal store never refused (fill not reached)")
    if m["indeterminate"] or set(m["accepted"]) | set(m["refused"]) != att or set(m["accepted"]) & set(m["refused"]):
        errors.append(f"C-50: store calls not classified exactly: {sig['classified']}")
    if hf["refused_full"] != n_full:
        errors.append(f"C-50: health refused_full={hf['refused_full']} but {n_full} store calls returned NO_MEM")

    def phases(d: Device) -> list[str]:
        # remount: spool/reclaim and stores resume (C-52)
        errs = []
        d.run(["sd insert", "service 3", "health remount", "store 200", "service 2", "health resumed", "checkpoint r"])
        hr, hs = last_health(d, "remount"), last_health(d, "resumed")
        tail = read_jsonl(d.state / "out" / "attempts.jsonl")[-200:]
        acc_now = {r["id"] for r in read_jsonl(d.state / "out" / "accepted.jsonl")}
        sig["after_remount"] = {"spool_files": hr["spool_files"], "reclaimed": hs["reclaimed"],
                                "stores_accepted": sum(1 for a in tail if a["id"] in acc_now),
                                "storage_blocked": hs["storage_blocked"]}
        # Spooling is demand-driven (flash pressure / every N stores): once the
        # C-51 eviction has freed space there may be no pressure right at the
        # remount, so C-52 is judged after the post-remount stores.
        if max(hr["spool_files"], hs["spool_files"]) < 1:
            errs.append("C-52: spooling did not resume after the remount")
        if sig["after_remount"]["stores_accepted"] != 200 or hs["storage_blocked"]:
            errs.append(f"C-52: stores did not resume after the remount: {sig['after_remount']}")
        return errs + _x_reboot(d, sig)
    rec, e2 = _x_post(dev, strict, {}, sig, phases)
    errors += e2
    diag = _x_diag(dev, "final")
    sig["diag"] = diag
    green = []
    # ── C-51 (non-negotiable; NOT weakened): no store may be refused while a
    #    delivered, unpinned, evictable flash file exists. A finding here is a
    #    firmware defect in the refusal/eviction order, not a harness issue.
    sig["c51_violations"] = dev.x6_c51[:10]  # type: ignore[attr-defined]
    sig["c51_violation_count"] = len(dev.x6_c51)  # type: ignore[attr-defined]
    if dev.x6_c51:  # type: ignore[attr-defined]
        green.append(f"C-51 VIOLATED: {len(dev.x6_c51)} refusal(s) while a delivered evictable flash file existed, "  # type: ignore[attr-defined]
                     f"first: {dev.x6_c51[0]}")  # type: ignore[attr-defined]
    green += _x_diag_checks(dev, diag, {})
    return _x_conclude(dev, rec, sig, errors, green, {"expect": "any"}, strict)


# ── X7..X10: an ambiguous rename pair is retired as a UNIT or left intact ──
# (eval round 1: a half-retired pair — one name in evq/xlk-*, the other still a
# normal .log — was later imported and unlinked, freeing the chain its junk
# twin still referenced). Every run follows the pair's shared inode from the
# fault through a simulated reboot and the complete import/archive lifecycle,
# in BOTH lifecycles (retry-first; delivered-first under <name>_dfirst), and
# requires: zero xlink_unlink, neither original name ever removed, the chain
# still held by exactly two names at the end, pre-existing xlk sentinels
# byte-identical, plus C-18/C-19/C-20 and the exact sd_diag rename count.
X_PAIR_NO_BASE = ("the retire.pair.* fault points do not exist on the baseline (e1ca6ee): "
                  "recorded, no base expectation")


def _x_prefill_xlk(dev: Device, n: int) -> dict[str, str]:
    """n pre-existing junk names evq/xlk-0..n-1 (slots already used on this card)."""
    import hashlib
    d = dev.state / "sdcard" / "evq"
    d.mkdir(parents=True, exist_ok=True)
    out = {}
    for k in range(n):
        b = f"pre-existing retired name sentinel {k}\n".encode()
        (d / f"xlk-{k}.junk").write_bytes(b)
        out[f"sdcard/evq/xlk-{k}.junk"] = hashlib.sha256(b).hexdigest()
    return out


def _x_names_with_ino(dev: Device, ino: int | None) -> list[str]:
    if ino is None:
        return []
    return sorted(str(p.relative_to(dev.state)) for p in (dev.state / "sdcard").rglob("*")
                  if p.is_file() and p.stat().st_ino == ino)


def _x_pair_run(name: str, seed: int, kind: str, strict: bool, faults: str, delivered_first: bool,
                prefill: int, after_fault, at_end, diag_renames: int | None) -> tuple:
    """One device: arm `faults` during the first spool burst, snapshot the pair,
    reboot, run the lifecycle, E2E. `after_fault(sig) -> [errors]` and
    `at_end(sig) -> [errors]` state the scenario's GREEN signature."""
    import hashlib
    dev = _x_device(name, seed, kind)
    sentinels = _x_prefill_xlk(dev, prefill)
    dev.run(["keeper auto", "store 1100 small", "health xa", "checkpoint xa", "sddiag xa"], fault=faults)
    specs = [f.split(":")[0] for f in faults.split(",")]
    unfired = [sp for sp in specs if not dev.fault_fired(sp)]
    ev = ops(dev)
    pair = next(((e["from"], e["to"]) for e in ev if e["op"] == "rename_both_eio" or
                 (e["op"] == "rename_inside" and e.get("outcome") == "both")), None)
    sig: dict = {"faults": faults, "lifecycle": "delivered_first" if delivered_first else "retry_first",
                 "prefilled_xlk": prefill, "unfired": unfired}
    errors: list[str] = []
    green: list[str] = []
    if unfired and (strict or kind == "head"):
        green.append(f"fault point(s) never fired: {unfired}")
    ino = None
    if pair is not None:
        tmp, dst = pair
        # follow the pair's names through later renames (e.g. both retired to xlk-*)
        cur = {tmp, dst}
        for e in ev:
            if e["op"] == "rename" and "fail" not in e and e.get("from") in cur:
                cur.add(e["to"])
        for pth in [dst, tmp] + sorted(cur - {tmp, dst}):
            if (dev.state / pth).exists():
                ino = (dev.state / pth).stat().st_ino
                break
        sig.update(tmp=tmp, dst=dst)
    xlk_dir = dev.state / "sdcard" / "evq"
    junk = sorted(xlk_dir.glob("xlk-*.junk")) if xlk_dir.exists() else []
    ha = last_health(dev, "xa")
    sig["after_fault"] = {"tmp_exists": pair is not None and (dev.state / pair[0]).exists(),
                          "dst_exists": pair is not None and (dev.state / pair[1]).exists(),
                          "pair_inode_names": _x_names_with_ino(dev, ino),
                          "new_xlk": len(junk) - prefill,
                          "xlink_unlink": sum(1 for e in ev if e["op"] == "xlink_unlink"),
                          "sd_rename_ambiguous": ha["sd_rename_ambiguous"]}
    if pair is None:
        (errors if kind == "head" else green).append("no ambiguous (both-names) rename happened")

    def phases(d: Device) -> list[str]:
        errs = _x_reboot(d, sig)
        (_x_lifecycle_delivered_first if delivered_first else _x_lifecycle)(d)
        return errs
    rec, e2 = _x_post(dev, strict, {"faults": faults, "lifecycle": sig["lifecycle"]}, sig, phases)
    errors += e2
    ev = ops(dev)
    xl = [e for e in ev if e["op"] == "xlink_unlink"]
    removed = sorted({e["path"] for e in ev if e["op"] == "remove" and pair is not None and e.get("path") in pair})
    changed = sorted(k for k, h in sentinels.items() if not (dev.state / k).exists() or
                     hashlib.sha256((dev.state / k).read_bytes()).hexdigest() != h)
    sig["at_end"] = {"tmp_exists": pair is not None and (dev.state / pair[0]).exists(),
                     "dst_exists": pair is not None and (dev.state / pair[1]).exists(),
                     "pair_inode_names": _x_names_with_ino(dev, ino), "pair_names_removed": removed,
                     "sentinels_changed": changed[:10]}
    sig["xlink_unlink_total"] = len(xl)
    sig["xlink_unlinked"] = [{"path": e["path"], "twins": e.get("twins"), "why": e.get("why")} for e in xl][:10]
    diag = _x_diag(dev, "final")
    sig["diag"] = diag
    lc = sig["lifecycle"]
    if pair is not None and kind == "head" or (pair is not None and strict):
        green += [f"GREEN[{lc}] after fault: {g}" for g in after_fault(sig)]
        green += [f"GREEN[{lc}] at end: {g}" for g in at_end(sig)]
    if xl:
        green.append(f"GREEN[{lc}]: {len(xl)} xlink_unlink (expected 0): {sig['xlink_unlinked'][:3]}")
    if removed:
        green.append(f"GREEN[{lc}]: pair name(s) unlinked: {removed}")
    if pair is not None and len(sig["at_end"]["pair_inode_names"]) != 2:
        green.append(f"GREEN[{lc}]: the pair's chain is held by {sig['at_end']['pair_inode_names']} at the end "
                     f"(expected exactly two names: never freed, never half-unlinked)")
    if changed:
        green.append(f"GREEN[{lc}]: pre-existing xlk sentinel(s) changed/removed: {changed[:5]}")
    if diag_renames is not None:
        want = {"evlog.rename": diag_renames} if diag_renames else {}
        green += [f"[{lc}] {g}" for g in _x_diag_checks(dev, diag, want)]
    return dev, rec, sig, errors, green


def _x_pair(name: str, seed: int, kind: str, strict: bool | None, variants: list[dict], red_rule: dict) -> dict:
    """Run every variant of one X7..X10 scenario in both lifecycles; the first
    (retry-first) device is the scenario's own evidence dir, the others are
    <name>_<variant>[_dfirst]. Errors of every device fail the scenario."""
    strict = kind == "head" if strict is None else strict
    main = None
    agg_err: list[str] = []
    agg_green: list[str] = []
    subs = {}
    for v in variants:
        for dfirst in (False, True):
            dname = name + (f"_{v['tag']}" if v["tag"] else "") + ("_dfirst" if dfirst else "")
            dev, rec, sig, err, green = _x_pair_run(dname, seed, kind, strict, v["faults"], dfirst, v["prefill"],
                                                    v["after_fault"], v["at_end"], v["diag_renames"])
            label = dname[len(name):].lstrip("_") or "main"
            agg_err += [f"[{label}] {e}" for e in err]
            agg_green += [f"[{label}] {g}" for g in green]
            if main is None:
                main = (dev, rec, sig)
                continue
            subs[label] = {k: sig.get(k) for k in ("faults", "unfired", "after_fault", "at_end", "xlink_unlink_total",
                                                   "reboot", "post_fault_abort")}
            _x_conclude(dev, rec, sig, err, green, None, False)
    dev, rec, sig = main
    sig["variants"] = subs
    red = dict(red_rule)
    if red.get("expect") == "RED":
        dst_rm = any(e["op"] == "remove" and e.get("path") == sig.get("dst") for e in ops(dev))
        red["observed"] = sig["xlink_unlink_total"] >= 1 or dst_rm
    return _x_conclude(dev, rec, sig, agg_err, agg_green, red, strict)


def _pair_intact(stage: str):
    def f(sig: dict) -> list[str]:
        s = sig[stage]
        e = []
        if not (s["tmp_exists"] and s["dst_exists"]):
            e.append(f"pair not intact under its own names (tmp={s['tmp_exists']} dst={s['dst_exists']})")
        if len(s["pair_inode_names"]) != 2:
            e.append(f"chain held by {s['pair_inode_names']}")
        return e
    return f


def _no_new_xlk(sig: dict) -> list[str]:
    s = sig["after_fault"]
    return [] if s["new_xlk"] == 0 else [f"{s['new_xlk']} new xlk name(s) (expected none: the pair must not be split)"]


def _both(*fs):
    return lambda sig: [x for f in fs for x in f(sig)]


def _ambiguous_one(sig: dict) -> list[str]:
    v = sig["after_fault"]["sd_rename_ambiguous"]
    return [] if v == 1 else [f"sd_rename_ambiguous={v} (expected 1)"]


def _chain_held_by_two(sig: dict) -> list[str]:
    return []   # checked for every run in _x_pair_run


def X7(seed: int, kind: str = "head", strict: bool | None = None) -> dict:
    """Eval-R1 reproduction: xlk-0..998 used (ONE slot left) + both_eio at the spool commit rename — the pair
    cannot be retired as a unit, so it stays intact and guarded; also the pair appearing at BOOT (crash inside
    the rename, 'both' outcome) with one slot left (epoch-repair path)."""
    return _x_pair(
        "X7", seed, kind, strict,
        [{"tag": "", "faults": "spool.rename.inside_call:1:both_eio", "prefill": 999,
          "after_fault": _both(_pair_intact("after_fault"), _no_new_xlk, _ambiguous_one),
          "at_end": _pair_intact("at_end"), "diag_renames": 1},
         {"tag": "boot", "faults": "spool.rename.inside_call:1:crash_both", "prefill": 999,
          "after_fault": _both(_pair_intact("after_fault"), _no_new_xlk),
          "at_end": _pair_intact("at_end"), "diag_renames": 0}],
        {"expect": "RED", "rule": "base e1ca6ee (xlk-0..998 pre-filled, spool both_eio): >=1 xlink_unlink over the "
                                  "run OR the committed dst unlinked"})


def X8(seed: int, kind: str = "head", strict: bool | None = None) -> dict:
    """Pair retirement whose FIRST rename (the .log) fails: nothing moves, the pair stays intact and guarded."""
    return _x_pair(
        "X8", seed, kind, strict,
        [{"tag": "", "faults": "spool.rename.inside_call:1:both_eio,retire.pair.log:1:eio", "prefill": 0,
          "after_fault": _both(_pair_intact("after_fault"), _no_new_xlk, _ambiguous_one),
          "at_end": _chain_held_by_two, "diag_renames": 2}],
        {"expect": "any", "rule": X_PAIR_NO_BASE})


def X9(seed: int, kind: str = "head", strict: bool | None = None) -> dict:
    """Pair retirement whose SECOND rename (the .tmp) fails: the .log is renamed back, the pair stays intact."""
    return _x_pair(
        "X9", seed, kind, strict,
        [{"tag": "", "faults": "spool.rename.inside_call:1:both_eio,retire.pair.tmp:1:eio", "prefill": 0,
          "after_fault": _both(_pair_intact("after_fault"), _no_new_xlk, _ambiguous_one),
          "at_end": _chain_held_by_two, "diag_renames": 2}],
        {"expect": "any", "rule": X_PAIR_NO_BASE})


def _log_junk_tmp_alone(sig: dict) -> list[str]:
    s = sig["after_fault"]
    e = []
    if s["dst_exists"] or not s["tmp_exists"]:
        e.append(f"expected the .log retired to xlk and the .tmp left alone (tmp={s['tmp_exists']} dst={s['dst_exists']})")
    junk = [n for n in s["pair_inode_names"] if re.search(r"sdcard/evq/xlk-\d+\.junk$", n)]
    if len(junk) != 1 or len(s["pair_inode_names"]) != 2:
        e.append(f"chain names after the fault {s['pair_inode_names']} (expected the .tmp + one xlk junk)")
    return e


def _tmp_retired(sig: dict) -> list[str]:
    s = sig["at_end"]
    junk = [n for n in s["pair_inode_names"] if re.search(r"sdcard/evq/xlk-\d+\.junk$", n)]
    if s["tmp_exists"] or len(junk) != 2:
        return [f"lone .tmp not retired by rename (tmp exists={s['tmp_exists']}, chain names {s['pair_inode_names']})"]
    return []


def X10(seed: int, kind: str = "head", strict: bool | None = None) -> dict:
    """Pair retirement: the .tmp rename AND the .log restore both fail — the .log stays as xlk junk, the lone .tmp
    is later retired (never unlinked); plus a crash at retire.pair.tmp (reboot → lone .tmp retired)."""
    return _x_pair(
        "X10", seed, kind, strict,
        [{"tag": "", "faults": "spool.rename.inside_call:1:both_eio,retire.pair.tmp:1:eio2", "prefill": 0,
          "after_fault": _both(_log_junk_tmp_alone, _ambiguous_one), "at_end": _tmp_retired, "diag_renames": 3},
         {"tag": "crash", "faults": "spool.rename.inside_call:1:both_eio,retire.pair.tmp:1:crash", "prefill": 0,
          "after_fault": lambda sig: [], "at_end": _tmp_retired, "diag_renames": 1}],
        {"expect": "any", "rule": X_PAIR_NO_BASE})


# ═══ D. fault matrix ════════════════════════════════════════════════════════
MATRIX_TUNING = {"EVLOG_ROTATE_BYTES": 16384, "EVQ_INDEX_COMPACT_BYTES": 2048}


def matrix_workload() -> list[str]:
    # the reboot makes the first deliveries re-verify SD_ONLY copies (claim.* points)
    return (["keeper auto"] + cps(300, 60, "small", "a") + ["store 25 default", "checkpoint big"] +
            ["deliver 200", "checkpoint d1", "store 1000 small", "checkpoint b1", "reboot", "deliver all", "checkpoint d2",
             "store 1000 small", "checkpoint b2", "service", "checkpoint s"])


def matrix_prepare(dev: Device) -> None:
    """Pre-upgrade residue so boot/import/quarantine points are reachable:
    a baseline-era rotated file + tail containing a malformed line, and a
    legacy /sdcard/events file."""
    evd = dev.state / "evstore" / "events"
    evd.mkdir(parents=True, exist_ok=True)
    import hashlib
    rows = []
    for seq, ids in [(1, range(900001, 900031)), (2, range(900031, 900041))]:
        buf = b""
        for rid in ids:
            payload = '{"synthetic_test":true,"preupgrade":true,"k":%d}' % rid
            line = f"{rid}\tuart_2\t28:37:2F:FF:E7:04\tMEASUREMENT\tarrun pre\t{1756000000000 + rid}\t{1756000001000 + rid}\t\t{payload}\n".encode()
            buf += line
            h = lambda s: hashlib.sha256(s.encode()).hexdigest()
            rows.append({"kind": "fixture", "id": rid, "channel": "uart_2", "device": "28:37:2F:FF:E7:04", "tag": "MEASUREMENT",
                         "cmd_sha": h("arrun pre"), "start_ms": 1756000000000 + rid, "end_ms": 1756000001000 + rid,
                         "meta_sha": h(""), "payload_sha": h(payload), "line_sha": hashlib.sha256(line).hexdigest()})
            if seq == 2 and rid == 900035:
                buf += b"999999999\tmalformed pre-upgrade line\n"
        (evd / f"ev-{seq:06d}.log").write_bytes(buf)
    (dev.state / "out").mkdir(exist_ok=True)
    with open(dev.state / "out" / "accepted.jsonl", "a") as f:
        for r in rows:
            f.write(json.dumps(r) + "\n")
    _write_legacy(dev, 950000, 20)
    # An unindexed ambiguous-rename PAIR (both names on one chain: a hard link
    # here) so the boot epoch repair always runs evq_sd_retire_pair and the
    # retire.pair.* points are reached/fired on every matrix run. The bytes are
    # not records (no accepted/delivered obligation): only the chain matters,
    # and C-19(b) catches any unlink of either name while the other exists.
    evq = dev.state / "sdcard" / "evq"
    evq.mkdir(parents=True, exist_ok=True)
    (evq / "m-000999.log").write_bytes(b"matrix residue: an ambiguous rename pair (not records)\n" * 8)
    os.link(evq / "m-000999.log", evq / "m-000999.tmp")


def matrix_foreign_cursor(dev: Device) -> None:
    """Second phase for the reimport.* points: another firmware advanced the
    legacy cursor keys past SD-only segments (a rollback)."""
    kv = nvs_read(dev.state)
    segs, _, _ = index_segs(dev)
    sd_only = sorted(s.seq for s in segs.values() if s.state == "SD_ONLY")
    if not sd_only or ("evlog", "rd_seq") not in kv:
        return
    target = max(sd_only) + 1
    kv[("evlog", "rd_seq")] = target.to_bytes(4, "little")
    kv[("evlog", "rd_off")] = (0).to_bytes(4, "little")
    nvs_write(dev.state, kv)


def matrix_run(point: str, mode: str, nth: int, seed: int, env_extra: dict | None = None) -> dict:
    """One D-matrix run: prepare residue, run the workload with the fault armed,
    then (for foreign-cursor coverage) a rollback-style cursor jump, recovery and E2E."""
    scn = f"D_{point}_{mode}_{nth}"
    dev = device(scn, seed, tuning=MATRIX_TUNING, flash=1 * MiB, env=env_extra)
    dev.no_pending_check = True
    matrix_prepare(dev)
    phase2 = point.startswith("reimport.")
    if point == "boot.adopt_mid":
        # precondition: a spool interrupted after its copies verified
        dev.run(["keeper auto", "store 1000 small"], fault="spool.after_verify_before_index:1:crash")
        assert dev.crashes == 1, "adopt precondition (interrupted spool) did not happen"
        dev.armed = None
    dev.run(matrix_workload(), fault=None if phase2 else f"{point}:{nth}:{mode}")
    # D duplicate bound for the fault phase: <= 16 + one segment's records per crash
    # (the rollback phase below only reports, as the contract allows)
    ids = [d["id"] for d in read_jsonl(dev.state / "out" / "delivered.jsonl")]
    dups_phase1 = len(ids) - len(set(ids))
    seg_count_max = max([x.count for x in index_segs(dev)[0].values()] + [1])
    crashes_phase1 = dev.crashes
    # an injected I/O error is one fault event too (e.g. EIO on the SD remove
    # after a legacy import re-imports that file: at-least-once, bounded)
    fired_now = (dev.state / ".shim" / "fault_fired").exists()
    events = crashes_phase1 + (1 if (mode != "crash" and fired_now and not phase2) else 0)
    bound = events * (16 + max(seg_count_max, 80))
    if dups_phase1 > bound:
        raise OracleFailure(f"D: {dups_phase1} duplicates after {events} fault event(s) exceed the bound {bound}")
    # Phase 2 for every run: another firmware advanced the legacy cursor past
    # SD-only segments (rollback) → re-import; reimport.* points are armed here.
    matrix_foreign_cursor(dev)
    dev.run(["keeper auto", "service 3", "checkpoint fc"], fault=f"{point}:{nth}:{mode}" if phase2 else None)
    seg_max = max([x.count for x in index_segs(dev)[0].values()] + [80])
    rec = e2e(dev)
    fired = (dev.state / ".shim" / "fault_fired").read_text().split() if (dev.state / ".shim" / "fault_fired").exists() else []
    counts = {}
    for p in [dev.state / ".shim" / "fault_counts"]:
        if p.exists():
            for line in p.read_text().splitlines():
                n, c = line.rsplit(" ", 1)
                counts[n] = counts.get(n, 0) + int(c)
    reached = (dev.state / ".shim" / "faults_reached").read_text().split() if (dev.state / ".shim" / "faults_reached").exists() else []
    out = {"point": point, "mode": mode, "nth": nth, "seed": seed, "fired": fired, "crashes": dev.crashes,
           "dups": rec["duplicate_count"], "reached": sorted(set(reached)), "counts": counts,
           "accepted": rec["accepted"], "refused": rec["refused"], "indeterminate": rec["indeterminate"],
           "dups_fault_phase": dups_phase1, "dup_bound": bound, "crashes_fault_phase": crashes_phase1}
    finish(dev)
    return out


def matrix_dry(seed: int) -> dict:
    """Uninstrumented run: which points are reached and how often (sizes nth=last)."""
    return matrix_run("none.never", "crash", 1, seed)
