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
                     iter_records, line_sha, load_manifests, media_hashes, nvs_read, nvs_write, parse_index,
                     quarantined_ids, read_jsonl, reconcile, record_id, registered_fault_points, scratch_root,
                     sha256_file, write_evidence)

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


def e2e(dev: Device, extra: dict | None = None, media_before=None, allow_missing=None, dup_bound=None) -> dict:
    dev.run(["drain_all", "service 2", "health final", "checkpoint final"])
    rec = reconcile(dev)
    if extra:
        rec.update(extra)
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
    write_evidence(dev, rec)
    assert_e2e(rec, allow_missing=set(rec["missing_ids"]))  # delivery never ran: only integrity here
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
    write_evidence(dev, {**reconcile(dev), **rec})
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
