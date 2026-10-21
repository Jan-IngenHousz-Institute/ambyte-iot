#!/usr/bin/env python3
"""A4 build evidence; runtime timing/heap/stack margins require the Evaluator bench."""
import argparse
import json
from pathlib import Path

from qualify_svc_resources import qualify


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = qualify(args.build)
    report["sprint"] = "A4"
    report["static_ram_delta_from_a3"] = report["static_ram"] - 108212
    if report["static_ram_delta_from_a3"] > 8192:
        raise ValueError("A4 static RAM delta exceeds F601")
    frames = report["target_function_frames"]
    for component in ("CLI", "sensor_inventory"):
        for source in sorted((args.build / "components" / component).glob("*.su")):
            for line in source.read_text().splitlines():
                location, size, kind = line.split("\t")
                frames.append(dict(location=location, bytes=int(size), kind=kind))
    for function in ("cli_cmd_sensors", "sensor_snapshot_json", "sensor_inventory_refresh"):
        if not any(row["location"].endswith(":" + function) for row in frames):
            raise ValueError(f"Missing target stack usage: {function}")
    report["runtime_admission_wait_bound_us"] = 1000000
    report["runtime_maintenance_wait_bound_us"] = 1000000
    report["runtime_stdout_hold_bound_us"] = 250000
    report["runtime_sensor_pin_bound_bytes"] = 4096
    report["runtime"] = "NOT YET EVALUATED: internal heap, largest block, task margins, pin allocation, admission/job waits and stdout hold require same-image bench measurements"
    encoded = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(encoded)
        print(f"Sensor build evidence: {args.output}")
    else:
        print(encoded, end="")


if __name__ == "__main__":
    main()
