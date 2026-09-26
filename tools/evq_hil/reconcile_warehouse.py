"""Authoritative delivery reconciliation (contract P7 D-2/D-3) against
open_jii_dev.centrum.raw_data, read-only, Databricks profile `sandbox`.

    python reconcile_warehouse.py fetch --since '2026-09-26 02:00:00' --until '...' --out rows.json
    python reconcile_warehouse.py check --rows rows.json --capture CAP [...] [--pre pre.json] \
        [--real real.json] --out wh.json

`fetch` pulls every row of the bench client ingested in the window (10-minute
slices, JSON). `check` validates each expected record: synthetic rows field by
field against the EVQ_ACC tuple (payload recomputed from run/k/pad), real and
pre-existing rows by JSON-semantic equality of the sample object with the
stored line's payload. Output `passed_ids` feeds reconcile.py (HW-INV).
Transport (PUBACK) is never consulted here.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hilpay  # noqa: E402
import manifest  # noqa: E402

CLIENT = "ambyte_28:37:2f:ff:e7:04"
EXPERIMENT = "b3c8d242-dbb3-4520-9a5c-664e364ae6bd"
MAC = "28:37:2F:FF:E7:04"
PROFILE = "sandbox"


def _query(sql: str) -> list[dict]:
    r = subprocess.run(["databricks", "experimental", "aitools", "tools", "query", sql, "--profile", PROFILE,
                        "--output", "json"], capture_output=True, text=True, timeout=600)
    if r.returncode != 0:
        raise SystemExit(f"query failed: {r.stderr[-1500:]}")
    return json.loads(r.stdout or "[]")


def fetch(since: str, until: str, slice_min: int = 10) -> list[dict]:
    t0 = dt.datetime.fromisoformat(since)
    t1 = dt.datetime.fromisoformat(until)
    rows = []
    while t0 < t1:
        t = min(t0 + dt.timedelta(minutes=slice_min), t1)
        sql = ("SELECT experiment_id, workbook_version_id, CAST(ingestion_timestamp AS STRING) ingestion_timestamp, "
               "CAST(kinesis_arrival_time AS STRING) kinesis_arrival_time, kinesis_sequence_number, "
               "to_json(parsed_data) pd FROM open_jii_dev.centrum.raw_data "
               f"WHERE lower(client_id)='{CLIENT}' AND ingest_date >= DATE'{t0.date()}' "
               f"AND ingestion_timestamp >= timestamp'{t0:%Y-%m-%d %H:%M:%S}' "
               f"AND ingestion_timestamp < timestamp'{t:%Y-%m-%d %H:%M:%S}'")
        rows.extend(_query(sql))
        t0 = t
    return rows


def parse_rows(rows: list[dict]) -> dict[int, list[dict]]:
    by_id: dict[int, list[dict]] = {}
    for r in rows:
        pd = json.loads(r["pd"])
        sample = pd.get("sample")
        if isinstance(sample, str):
            sample = json.loads(sample)
        if not isinstance(sample, list) or not sample:
            continue
        obj = sample[0]
        mid = obj.get("measure_id")
        if mid is None:
            continue
        by_id.setdefault(int(mid), []).append({"sample": obj, "env": {k: v for k, v in pd.items() if k != "sample"},
                                               "experiment_id": r["experiment_id"],
                                               "workbook_version_id": r.get("workbook_version_id"),
                                               "arrival": r["kinesis_arrival_time"],
                                               "ingestion": r["ingestion_timestamp"]})
    return by_id


def _iso(ms: int) -> str:
    return dt.datetime.fromtimestamp(ms / 1000, dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def check_synthetic(row: dict, exp: dict, pad: int, workbook: str | None, cfg: dict) -> list[str]:
    s, e, bad = row["sample"], row["env"], []
    pl = hilpay.payload(exp["run"], exp["k"], pad)
    want = {"v": 2, "measure_id": exp["id"], "startTicks_UTC": exp["start_ms"], "endTicks_UTC": exp["end_ms"],
            "channel": None, "device": "evq_hil", "cmd_raw": f"evq_hil {exp['run']}", "tag": "MEASUREMENT",
            "metadata": None}
    for k, v in want.items():
        if s.get(k) != v:
            bad.append(f"sample.{k}={s.get(k)!r}!={v!r}")
    data = s.get("data")
    if hilpay.sha(json.dumps(data, separators=(",", ":"))) != hilpay.sha(pl):
        bad.append("data hash")
    if row["experiment_id"] != EXPERIMENT:
        bad.append(f"experiment {row['experiment_id']}")
    if e.get("timestamp") != _iso(exp["start_ms"]):
        bad.append(f"timestamp {e.get('timestamp')}")
    if e.get("device_id") != MAC:
        bad.append(f"device_id {e.get('device_id')}")
    for k in ("device_name", "device_version", "device_firmware"):
        if k in cfg and e.get(k) != cfg[k]:
            bad.append(f"{k} {e.get(k)!r}")
    if workbook is not None and row.get("workbook_version_id") != workbook:
        bad.append(f"workbook_version_id {row.get('workbook_version_id')}")
    if "macros" in e:
        bad.append("macros present")
    return bad


def check_real(row: dict, stored_line: bytes) -> list[str]:
    fields = stored_line.rstrip(b"\n").split(b"\t", 8)
    if len(fields) < 9:
        return ["stored line unparseable"]
    payload = json.loads(fields[8])
    start_ms = int(fields[5])
    bad = []
    s = row["sample"]
    if "schema" in payload:                     # v3 object: spliced verbatim
        if s != payload:
            bad.append("v3 sample != stored payload")
    elif s.get("data") != payload:
        bad.append("v2 data != stored payload")
    if row["env"].get("timestamp") != _iso(start_ms):
        bad.append(f"timestamp {row['env'].get('timestamp')} != {_iso(start_ms)}")
    return bad


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch")
    f.add_argument("--since", required=True)
    f.add_argument("--until", required=True)
    f.add_argument("--out", required=True)
    c = sub.add_parser("check")
    c.add_argument("--rows", required=True)
    c.add_argument("--capture", nargs="+", required=True)
    c.add_argument("--real", help="json {id: base64 stored line} of REAL/PRE records (private)")
    c.add_argument("--indeterminate", help="reconcile.py output (indeterminate list)")
    c.add_argument("--workbook")
    c.add_argument("--cfg", help='json {"device_name":..,"device_version":..,"device_firmware":..}')
    c.add_argument("--out", required=True)
    a = ap.parse_args()
    if a.cmd == "fetch":
        rows = fetch(a.since, a.until)
        Path(a.out).write_text(json.dumps(rows))
        print(json.dumps({"rows": len(rows)}))
        return 0
    import base64
    rows = parse_rows(json.loads(Path(a.rows).read_text()))
    texts = [Path(p).read_text(errors="replace") for p in a.capture]
    m = manifest.parse(texts)
    summ = manifest.verify(m)
    cfg = json.loads(Path(a.cfg).read_text()) if a.cfg else {}
    exp = {x["id"]: x for x in m["accepted"]}
    if a.indeterminate:
        for x in json.loads(Path(a.indeterminate).read_text()).get("indeterminate", []):
            exp.setdefault(x["id"], {**x, "end_ms": x["start_ms"]})
    passed, failed, missing, dups = [], {}, [], {}
    for i, x in exp.items():
        rs = rows.get(i, [])
        if not rs:
            missing.append(i)
            continue
        dups[i] = len(rs)
        errs = [check_synthetic(r, x, summ["pads"][x["run"]], a.workbook, cfg) for r in rs]
        if any(not e for e in errs):
            passed.append(i)
        else:
            failed[i] = errs[0]
    real = json.loads(Path(a.real).read_text()) if a.real else {}
    rpassed, rfailed, rmissing = [], {}, []
    for i_s, b64 in real.items():
        i = int(i_s)
        rs = rows.get(i, [])
        if not rs:
            rmissing.append(i)
            continue
        line = base64.b64decode(b64)
        errs = [check_real(r, line) for r in rs]
        if any(not e for e in errs):
            rpassed.append(i)
        else:
            rfailed[i] = errs[0]
    ref_delivered = sorted(r["id"] for r in m["refused"] if r["id"] is not None and r["id"] in rows)
    out = {"passed_ids": sorted(passed + rpassed), "synthetic": {"expected": len(exp), "passed": len(passed),
           "failed": failed, "missing": sorted(missing), "rows_per_id_max": max(dups.values()) if dups else 0,
           "duplicate_rows": sum(v - 1 for v in dups.values())},
           "real": {"expected": len(real), "passed": len(rpassed), "failed": rfailed, "missing": sorted(rmissing)},
           "refused_delivered": ref_delivered}
    Path(a.out).write_text(json.dumps(out, indent=1))
    print(json.dumps({k: (v if k != "passed_ids" else len(v)) for k, v in out.items()}, default=str)[:3000])
    ok = not missing and not failed and not rmissing and not rfailed and not ref_delivered
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
