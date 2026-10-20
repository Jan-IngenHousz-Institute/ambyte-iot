#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Evaluator-only USB service measurement runner; never flashes a device.

transport: 1 MiB sink ingress and generator egress at 144/702 raw bytes.
stage: production 16 KiB fsync path, explicitly aborting before install.
project: combine recorded reports into labelled linear projections.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from flash_gui import ambyte_serial, config  # noqa: E402
from flash_gui.service_benchmark import ServiceBenchmarkClient, measure_stage, projections  # noqa: E402
from flash_gui.service_client import OperationJournal, ServiceClient, ServiceClientError  # noqa: E402


def padded_yaml(blob):
    if not blob or not blob.endswith(b"\n") or b"\0" in blob or len(blob) > 16384:
        raise ValueError("Supply a valid UTF-8 YAML fixture ending in LF, at most 16,384 bytes.")
    blob.decode("utf-8", "strict")
    left = 16384 - len(blob)
    return blob + (b"#" + b"x" * (left - 2) + b"\n" if left >= 2 else b"\n" * left)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    sub = parser.add_subparsers(dest="mode", required=True)
    for name in ("transport", "stage"):
        p = sub.add_parser(name)
        p.add_argument("--port", required=True)
        p.add_argument("--expect-mac", required=True)
        p.add_argument("--image", type=Path, required=True, help="exact flashed firmware.bin for digest attribution")
        p.add_argument("--output", type=Path, required=True)
        if name == "stage":
            p.add_argument("--yaml", type=Path, required=True, help="valid log-only fixture; comments pad to 16 KiB")
            p.add_argument("--version", required=True)
            p.add_argument("--built-fw", required=True)
            p.add_argument("--workbook")
            p.add_argument("--new-request", action="store_true", help="explicit new measurement after outcome readback")
    p = sub.add_parser("project")
    p.add_argument("reports", type=Path, nargs="+")
    p.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.mode == "project":
            reports = [r for path in args.reports for r in json.loads(path.read_text())["reports"]]
            result = dict(projections=projections(reports), reports=reports)
        else:
            image_sha = hashlib.sha256(args.image.read_bytes()).hexdigest()
            staged_blob = padded_yaml(args.yaml.read_bytes()) if args.mode == "stage" else None
            # Same production console layer as GUI, with DTR/RTS deasserted
            # before open and read-only recovery if an old session is active.
            with ambyte_serial.connect_after_boot(args.port, log=lambda m: print(m, file=sys.stderr),
                                                   recover_service=True) as con:
                identity = con.identity()
                if identity.mac != args.expect_mac.upper() or "svc" not in identity.capabilities:
                    raise ValueError("Gateway MAC/service capability does not match the selected bench.")
                if args.mode == "transport":
                    client = ServiceBenchmarkClient(con._ser, None)
                    reports = [client.transfer(direction, chunk) for direction, chunk in (("rx", 144), ("tx", 144), ("tx", 702))]
                else:
                    journal = OperationJournal(config.CONFIG_DIR / "service_bench_stage", identity.mac)
                    client = ServiceClient(con._ser, journal)
                    reports = [measure_stage(client, staged_blob, args.version,
                                             args.built_fw, args.workbook, new_request=args.new_request)]
                result = dict(at_utc=datetime.now(timezone.utc).isoformat(), mac=identity.mac,
                              fw=identity.fw, port=con.port, reports=reports,
                              image_sha256=image_sha,
                              projections=projections(reports), acquisition_jitter="not measured")
        args.output.write_text(json.dumps(result, indent=2) + "\n")
        print(f"Recorded measurement evidence: {args.output}")
        return 0
    except (OSError, ValueError, KeyError, TypeError, ServiceClientError, ambyte_serial.ConsoleError) as exc:
        print(f"Benchmark did not complete: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
