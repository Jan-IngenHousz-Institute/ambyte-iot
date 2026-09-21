# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Factory PCBA flash + test runner:  python -m flash_gui.factory_test

One invocation per board, minimum operator interaction: flash the firmware
(latest published release by default; `--fw <build dir>` for a local build;
`--no-flash` to only test), then drive the firmware's `selftest` console
command (components/CLI/CLI.c) and archive the result. Flashing writes ONLY
the release images — bootloader, partition table, otadata, app. NVS is never
touched, so a board's identity/provisioning survives a retest and a fresh
PCBA simply boots unprovisioned (the selftest needs no network). Onboarding
stays the flash GUI's job.

The test intelligence lives in the firmware — this side only flashes,
connects, runs, parses, asks the operator about the LED, and writes the log.
Wire contract (hold in sync with CLI.c):

    SELFTEST BEGIN fw=<ver> mac=<mac>
    TEST <name> <PASS|FAIL> key=value ... t=<ms>ms      (one line per test)
    SELFTEST <PASS|FAIL> passed=<n> failed=<n> fw=<ver> mac=<mac>

Per run this writes one JSON file (full detail: parsed tests, raw values, the
verbatim serial transcript — the thing you want when a field failure comes
back months later) and appends one row to results.csv (the thing you want
open in a spreadsheet while a production batch is running). Files are never
overwritten: a retest is a new row, and "failed twice, passed the third time"
is itself a signal worth keeping.

Exit codes: 0 board PASSED, 1 board FAILED, 2 the test could not run.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

import serial

from .ambyte_serial import (AmbyteConsole, ConsoleError, connect_after_boot,
                            esp_jtag_ports)
from .esptool_ops import EsptoolError, flash_images, nudge_reset
from .release_fetch import (MANIFEST, ReleaseError, ReleaseImages,
                            fetch_latest)

# Anchored to line start: ESP_LOG lines from other tasks may interleave whole
# lines into the reply, but each printf/log call is one atomic line, so the
# TEST/SELFTEST lines themselves stay intact.
_TEST_RE = re.compile(r"^TEST (\S+) (PASS|FAIL)\b(.*)$", re.MULTILINE)
_VERDICT_RE = re.compile(
    r"^SELFTEST (PASS|FAIL) passed=(\d+) failed=(\d+) fw=(\S+) mac=(\S+)",
    re.MULTILINE)
_BEGIN_RE = re.compile(r"^SELFTEST BEGIN fw=(\S+) mac=(\S+)", re.MULTILINE)
# `rtc set <epoch>` answers "RTC set; RTC: <YYYY-MM-DD HH:MM:SS> (<epoch>)".
_RTC_SET_OK_RE = re.compile(r"^RTC set;\s*RTC:\s*\S+ \S+\s*\((\d+)\)",
                            re.MULTILINE)
_KV_RE = re.compile(r"(\w+)=(\S+)")

# The RTC tick test alone holds the console ~1.1 s and the I2C sweep probes
# 112 addresses; 30 s is an order of magnitude of headroom, not a guess at
# the happy path.
SELFTEST_TIMEOUT_S = 30.0

# Park detection. A booting board streams boot/app logs from the first
# seconds; a chip parked in the S3's ROM download mode is bit-for-bit silent.
# So a port that is OPEN and totally silent for this long is parked — nudge
# another reset immediately instead of waiting out a console deadline.
PARK_SILENCE_S = 10.0
# A board that IS talking gets this long to reach the prompt (CLI is up
# ~13-15 s into a good boot; margin for a first-boot littlefs format).
CONSOLE_WAIT_SLICE_S = 45.0

CSV_FIELDS = ["timestamp_utc", "mac", "fw", "overall", "firmware_verdict",
              "led_operator", "failed_tests", "operator", "station", "json_file"]


class FactoryTestError(RuntimeError):
    """The test could not be run to a verdict (distinct from a board FAIL)."""


@dataclass
class TestLine:
    name: str
    passed: bool
    values: dict = field(default_factory=dict)
    raw: str = ""


@dataclass
class SelftestResult:
    fw: str
    mac: str
    overall_pass: bool          # firmware's own verdict (LED excluded)
    passed: int
    failed: int
    tests: list = field(default_factory=list)
    transcript: str = ""

    @property
    def failed_names(self) -> list[str]:
        return [t.name for t in self.tests if not t.passed]


def parse_selftest(reply: str) -> SelftestResult:
    """Parse one `selftest` reply. Raises FactoryTestError when the contract
    lines are absent — which is how a firmware too old to carry the command
    shows up (the REPL answers 'Unrecognized command')."""
    verdict = _VERDICT_RE.search(reply)
    if verdict is None:
        if "Unrecognized command" in reply:
            raise FactoryTestError(
                "this firmware has no `selftest` command — flash a release "
                "that includes it before running the factory test")
        raise FactoryTestError(
            "selftest produced no SELFTEST verdict line; raw reply tail:\n"
            + reply[-500:])

    tests = []
    for m in _TEST_RE.finditer(reply):
        values = dict(_KV_RE.findall(m.group(3)))
        tests.append(TestLine(name=m.group(1), passed=m.group(2) == "PASS",
                              values=values, raw=m.group(0).strip()))

    return SelftestResult(
        fw=verdict.group(4),
        mac=verdict.group(5),
        overall_pass=verdict.group(1) == "PASS",
        passed=int(verdict.group(2)),
        failed=int(verdict.group(3)),
        tests=tests,
        transcript=reply,
    )


def _schedule_stop_best_effort(con: AmbyteConsole, log) -> None:
    """A running measurement script could hold the I2C bus mid-selftest. On a
    factory-fresh board nothing is running and this is a no-op; on a re-test
    of a provisioned board it keeps the suite deterministic. Never fatal."""
    try:
        con.command("schedule stop", timeout=10.0)
        log("Schedule runner stopped for the test window.")
    except ConsoleError:
        pass


def set_rtc_from_host(con: AmbyteConsole, log) -> dict:
    """Set the board's clock from the station clock BEFORE running the selftest.

    Why this has to happen first: a factory-fresh PCF2131 powers up with its
    oscillator-stop flag (OSF) set, and the driver deliberately REFUSES to
    return a time while that flag is set (PCF2131_base::rtc_time returns
    ESP_ERR_INVALID_STATE) so that a never-set clock can never be mistaken for
    a real one. The selftest's tick test is built on that same read path, so on
    an out-of-factory board both of its reads fail, the two timestamps keep
    their zero initialiser, and a perfectly good board reports
    `rtc FAIL ready=1 osf=1 tick=fail epoch=0`. Writing the time clears OSF,
    which is the only thing that makes the oscillator observable at all.

    Setting from the station clock rather than a synthetic stamp is deliberate:
    the board then leaves the station with a correct time, which is what it
    needs in the field anyway, and no bogus epoch can ever reach the event log.

    This does NOT weaken the test. A dead 32 kHz crystal still fails, because
    the time will not advance between the selftest's two reads even after a
    successful set — which is exactly the fault the tick test exists to catch.

    Best-effort by design: a station-side failure here is logged and recorded
    but never fatal. The firmware's own rtc verdict stays the gate.
    """
    stamp = datetime.now(timezone.utc).replace(microsecond=0)
    epoch = int(stamp.timestamp())
    record = {
        "requested_epoch": epoch,
        "requested_utc": stamp.isoformat(),
        "ok": False,
        "reply": "",
    }
    try:
        reply = con.command(f"rtc set {epoch}", timeout=10.0)
    except ConsoleError as exc:
        record["reply"] = str(exc)
        log(f"WARNING: could not set the RTC ({exc}); a factory-fresh board "
            "will fail the rtc test for lack of a set clock.")
        return record

    record["reply"] = reply.strip()
    record["ok"] = _RTC_SET_OK_RE.search(reply) is not None
    if record["ok"]:
        log(f"RTC set from the station clock: {stamp.isoformat()}.")
    else:
        log("WARNING: the board did not confirm the RTC set; a factory-fresh "
            "board will fail the rtc test for lack of a set clock.")
    return record


def _led_off_best_effort(con: AmbyteConsole) -> None:
    try:
        con.command("red 0", timeout=5.0)
    except ConsoleError:
        pass


def overall_verdict(fw_pass: bool, led_operator: bool | None) -> bool:
    """The station verdict: firmware verdict AND the operator's LED answer.
    A skipped prompt (--no-led, None) leaves the firmware verdict alone — an
    unattended run must not fail boards for a question nobody was asked."""
    return fw_pass and led_operator is not False


def ask_operator_led() -> bool:
    """Blocking y/n prompt. The LED is the one thing the board cannot verify
    about itself, so this answer is recorded as operator-attested."""
    while True:
        answer = input("Is the red LED lit or blinking? [y/n] ").strip().lower()
        if answer in ("y", "yes"):
            return True
        if answer in ("n", "no"):
            return False
        print("Please answer y or n.")


def write_logs(logdir: Path, result: SelftestResult, *, port: str,
               operator: str, station: str, led_operator: bool | None,
               overall: bool, duration_s: float,
               flashed: str | None = None,
               rtc_set: dict | None = None) -> Path:
    """One JSON per run + one CSV row per run. The JSON is the archive; the
    CSV is the batch overview. Neither is ever overwritten."""
    logdir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc)
    mac_flat = result.mac.replace(":", "") or "NOMAC"
    json_path = logdir / f"{stamp.strftime('%Y%m%dT%H%M%SZ')}_{mac_flat}.json"
    # A same-second retest of the same board must not clobber the first run.
    seq = 1
    while json_path.exists():
        json_path = logdir / (
            f"{stamp.strftime('%Y%m%dT%H%M%SZ')}_{mac_flat}_{seq}.json")
        seq += 1

    record = {
        "schema": 1,
        "timestamp_utc": stamp.isoformat(),
        "mac": result.mac,
        "fw": result.fw,
        "port": port,
        "operator": operator,
        "station": station,
        "flashed": flashed,          # release tag written this run; None = --no-flash
        "duration_s": round(duration_s, 2),
        "firmware_verdict": "PASS" if result.overall_pass else "FAIL",
        "passed": result.passed,
        "failed": result.failed,
        "failed_tests": result.failed_names,
        "led_operator_pass": led_operator,     # None = prompt skipped
        # What the station wrote to the RTC before testing; None = --no-rtc-set.
        # Archived because the rtc verdict is only interpretable alongside it.
        "rtc_set": rtc_set,
        "overall": "PASS" if overall else "FAIL",
        "tests": [
            {"name": t.name, "verdict": "PASS" if t.passed else "FAIL",
             "values": t.values, "raw": t.raw}
            for t in result.tests
        ],
        "transcript": result.transcript,
    }
    json_path.write_text(json.dumps(record, indent=2), encoding="utf-8")

    csv_path = logdir / "results.csv"
    is_new = not csv_path.exists()
    with csv_path.open("a", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=CSV_FIELDS)
        if is_new:
            writer.writeheader()
        writer.writerow({
            "timestamp_utc": record["timestamp_utc"],
            "mac": result.mac,
            "fw": result.fw,
            "overall": record["overall"],
            "firmware_verdict": record["firmware_verdict"],
            "led_operator": {True: "yes", False: "no", None: "skipped"}[led_operator],
            "failed_tests": ";".join(result.failed_names),
            "operator": operator,
            "station": station,
            "json_file": json_path.name,
        })
    return json_path


def _remap_flattened_build(root: Path, manifest: dict) -> dict:
    """PlatformIO flattens ESP-IDF's build layout (bootloader/bootloader.bin →
    bootloader.bin, partition_table/partition-table.bin → partitions.bin, the
    app <project>.bin → firmware.bin) while shipping IDF's flasher_args.json
    verbatim — so re-point each flash_files entry at whichever candidate
    actually exists. A pristine IDF layout matches on the first candidate and
    passes through unchanged; a truly missing file keeps its original name so
    ReleaseImages raises its normal "file is missing" error."""
    app_rel = (manifest.get("app") or {}).get("file", "")
    fixed: dict[str, str] = {}
    for off, rel in (manifest.get("flash_files") or {}).items():
        candidates = [rel, Path(rel).name]
        if rel == app_rel:
            candidates.append("firmware.bin")
        if Path(rel).name == "partition-table.bin":
            candidates.append("partitions.bin")
        fixed[off] = next((c for c in candidates if (root / c).is_file()), rel)
    out = dict(manifest)
    out["flash_files"] = fixed
    return out


def resolve_images(fw: str, log) -> ReleaseImages:
    """Resolve what to flash. 'latest' = newest published firmware release
    (cached); anything else is a local build directory containing
    flasher_args.json (e.g. .pio/build/esp32-s3-devkitm-1) — the dev/bring-up
    path before a selftest-capable release exists."""
    if fw == "latest":
        return fetch_latest(log=log)

    root = Path(fw)
    manifest_path = root / MANIFEST if root.is_dir() else root
    if manifest_path.name == MANIFEST and manifest_path.is_file():
        root = manifest_path.parent
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        return ReleaseImages("local", "local", root,
                             _remap_flattened_build(root, manifest))
    raise FactoryTestError(
        f"--fw must be 'latest' or a build directory containing {MANIFEST} "
        f"(got: {fw})")


def flash_board(port: str, images: ReleaseImages, log) -> None:
    log(f"Flashing firmware {images.tag} on {port}...")
    # flash_images redirects sys.stdout into its own tee and forwards each
    # esptool line to this callback — so the callback must write to the REAL
    # stdout, never through print()/sys.stdout, or every forwarded line
    # re-enters the tee (observed: RecursionError on the first line).
    def esptool_log(line: str) -> None:
        sys.__stdout__.write(line + "\n")
        sys.__stdout__.flush()

    flash_images(port, images.flash_files, images.flash_settings,
                 log=esptool_log)
    log("Flash complete; board resetting into the app.")


def connect_console(port: str | None, deadline_s: float, log) -> AmbyteConsole:
    """Console session that survives a re-parked chip, fast.

    Open the board's port and CLASSIFY it instead of blindly waiting: a
    booting board streams log traffic within seconds (prompt follows ~13-15 s
    into boot), while a chip parked in the ROM downloader is bit-for-bit
    silent — so ~10 s of true silence means parked, and another reset goes
    out immediately rather than after a long console deadline. A board that
    arrives already parked is rescued by the same loop."""
    end = time.time() + deadline_s
    attempt = 0
    said_wait = False
    while time.time() < end:
        candidates = ([port] if port else []) + \
            [p for p in esp_jtag_ports() if p != port]
        con = None
        for cand in candidates:
            try:
                con = AmbyteConsole(cand)
                break
            except (OSError, serial.SerialException):
                continue        # ghost / mid-re-enumeration / busy
        if con is None:
            if not said_wait:
                log("Waiting for the board's USB port to enumerate...")
                said_wait = True
            time.sleep(1.0)
            continue

        target = con.port
        saw_traffic = False
        try:
            prompt, seen = con.listen(
                min(PARK_SILENCE_S, max(0.5, end - time.time())))
            saw_traffic = seen > 0
            if not prompt and saw_traffic:
                # Booting: logs are flowing, the prompt just isn't up yet.
                prompt = con.wait_prompt(
                    timeout=min(CONSOLE_WAIT_SLICE_S, max(0.5, end - time.time())))
            if prompt:
                log(f"Console up on {target}.")
                return con
        except ConsoleError:
            # Port died under us — the chip reset mid-listen. Rescan.
            con.close()
            continue
        con.close()

        if time.time() >= end:
            break
        if not saw_traffic and target not in esp_jtag_ports():
            # The "silence" came from a ghost handle: the board re-enumerated
            # under us (Windows keeps the dead name readable for a moment).
            # It may be booting fine on the new name — rescan, don't reset.
            continue
        attempt += 1
        log(("Board is silent — parked in the S3 ROM downloader; "
             if not saw_traffic else
             "Board is talking but shows no console prompt; ")
            + f"firing reset attempt {attempt}.")
        if attempt == 2:
            # The automated escape is flaky; a VBUS power cycle is not. The
            # loop keeps rescanning ports, so a replug needs no restart.
            log(">> If this keeps failing: unplug the board's USB cable and "
                "plug it back in — the test continues automatically.")
        # Re-resolve the port at nudge time and ride out re-enumeration: the
        # name the dead session used can be gone by now (observed: 'reset
        # nudge failed: port is busy or doesn't exist' mid-re-enumeration).
        nudge_end = time.time() + 10.0
        while True:
            live = esp_jtag_ports()
            try:
                nudge_reset(live[0] if live else target)
                break
            except EsptoolError as exc:
                if time.time() >= nudge_end:
                    log(f"Reset nudge failed: {exc}")
                    break
                time.sleep(2.0)
        # Leave the strap-sensitive early-boot window untouched before the
        # next round of port-open attempts.
        time.sleep(5.0)

    raise ConsoleError(
        f"no ambyte console within {deadline_s:.0f}s despite {attempt} reset "
        "attempt(s). Unplug the board's USB cable and plug it back in — a "
        "physical power cycle always clears the S3's download-mode latch — "
        "then run the test again.")


def resolve_port(requested: str | None, log) -> str | None:
    """Pick the board's port. None is fine — connect_after_boot rescans every
    Espressif JTAG port anyway (the S3 re-enumerates on each reset)."""
    if requested:
        return requested
    ports = esp_jtag_ports()
    if len(ports) > 1:
        raise FactoryTestError(
            "multiple Espressif USB ports found (" + ", ".join(ports) +
            ") — connect exactly one board or pass --port")
    if ports:
        log(f"Using {ports[0]}.")
        return ports[0]
    log("No board port yet; waiting for one to enumerate...")
    return None


def await_port(existing: str | None, deadline_s: float, log) -> str:
    """Flashing (unlike console-hunting) needs a concrete port name up front,
    so wait for exactly one Espressif port when none is known yet — e.g. the
    operator starts the tool first and plugs the board in second."""
    if existing:
        return existing
    end = time.time() + deadline_s
    while time.time() < end:
        ports = esp_jtag_ports()
        if len(ports) == 1:
            log(f"Using {ports[0]}.")
            return ports[0]
        if len(ports) > 1:
            raise FactoryTestError(
                "multiple Espressif USB ports found (" + ", ".join(ports) +
                ") — connect exactly one board or pass --port")
        time.sleep(1.0)
    raise FactoryTestError(
        f"no Espressif USB port appeared within {deadline_s:.0f}s — is the "
        "board plugged in via USB-C?")


def run(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m flash_gui.factory_test",
        description="Flash a fresh ambyte PCBA, run the firmware selftest, "
                    "and log the result.")
    parser.add_argument("--port", help="serial port (default: auto-detect the "
                        "single Espressif USB-JTAG port)")
    parser.add_argument("--fw", default="latest",
                        help="firmware to flash: 'latest' (newest published "
                        "release, default) or a local build directory "
                        "containing flasher_args.json "
                        "(e.g. .pio/build/esp32-s3-devkitm-1)")
    parser.add_argument("--no-flash", action="store_true",
                        help="skip flashing and only test the firmware "
                        "already on the board (fast retest)")
    parser.add_argument("--logdir", type=Path, default=Path("factory_logs"),
                        help="directory for JSON logs + results.csv "
                        "(default: ./factory_logs)")
    parser.add_argument("--operator", default="",
                        help="operator name/id recorded in the log")
    parser.add_argument("--station", default="",
                        help="test-station id recorded in the log")
    parser.add_argument("--connect-timeout", type=float, default=200.0,
                        help="seconds to wait for the console (the CLI comes "
                        "up ~20-35 s after boot; default 200)")
    parser.add_argument("--no-rtc-set", action="store_true",
                        help="do not set the RTC from the station clock before "
                             "testing. A factory-fresh board will then fail the "
                             "rtc step: its oscillator-stop flag blocks the "
                             "read until the clock is written once")
    parser.add_argument("--no-led", action="store_true",
                        help="skip the operator LED prompt (unattended runs); "
                        "the verdict is then the firmware's alone")
    args = parser.parse_args(argv)

    def log(msg: str) -> None:
        print(msg, flush=True)

    started = time.time()
    flashed_tag: str | None = None
    try:
        port = resolve_port(args.port, log)
        if not args.no_flash:
            # Resolve/download BEFORE waiting on the port so a network or
            # cache problem surfaces immediately, not after the operator
            # already plugged a board in.
            images = resolve_images(args.fw, log)
            port = await_port(port, args.connect_timeout, log)
            flash_board(port, images, log)
            flashed_tag = images.tag
        con = connect_console(port, args.connect_timeout, log)
    except (FactoryTestError, ConsoleError, EsptoolError, ReleaseError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    led_operator: bool | None = None
    rtc_set: dict | None = None
    try:
        _schedule_stop_best_effort(con, log)
        # Must precede the selftest: the rtc step cannot read a clock that has
        # never been set (see set_rtc_from_host).
        if not args.no_rtc_set:
            rtc_set = set_rtc_from_host(con, log)
        log("Running selftest...")
        try:
            reply = con.command("selftest", timeout=SELFTEST_TIMEOUT_S)
            result = parse_selftest(reply)
        except (ConsoleError, FactoryTestError) as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
            return 2

        for t in result.tests:
            log("  " + t.raw)

        if args.no_led:
            _led_off_best_effort(con)
        else:
            led_operator = ask_operator_led()
            _led_off_best_effort(con)
    finally:
        con.close()

    overall = overall_verdict(result.overall_pass, led_operator)
    json_path = write_logs(
        args.logdir, result, port=con.port, operator=args.operator,
        station=args.station, led_operator=led_operator, overall=overall,
        rtc_set=rtc_set,
        duration_s=time.time() - started, flashed=flashed_tag)

    verdict = "PASS" if overall else "FAIL"
    banner = "=" * 46
    log(banner)
    log(f"  BOARD {result.mac}  fw {result.fw}:  {verdict}")
    if not overall:
        reasons = list(result.failed_names)
        if led_operator is False:
            reasons.append("led(operator)")
        log("  failed: " + (", ".join(reasons) or "?"))
    log(f"  log: {json_path}")
    log(banner)
    return 0 if overall else 1


def main() -> None:
    sys.exit(run())


if __name__ == "__main__":
    main()
