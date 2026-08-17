#!/usr/bin/env python3
"""Overnight SD-stress harness for PR #27 (SD power-loss hardening).

Drives the Ambyte serial console (USB-JTAG CDC, 115200) through repeated
cycles of:

  1. WRITE BURST  — `record_env` in a tight loop (each stores one event via the
                    event_log path: append + batched fsync every 8 records).
  2. CLEAN REBOOT — `reboot` (exercises app_prepare_reboot: flush/close/unmount).
  3. WRITE BURST  — again.
  4. HARD RESET   — esptool-style DTR/RTS reset fired MID-BURST with no warning,
                    bypassing the shutdown handler. This is the ungraceful-reset
                    corruption scenario the batched fsync + torn-tail repair must
                    survive. (A true power cut also drops the card's own rail —
                    that part only a bench PSU can emulate — but FAT/dir-entry
                    tearing at the filesystem layer is faithfully reproduced.)

After every reset it scans the boot log for verdict markers:
  GOOD:  "SD mounted:", "event_log ... ready: files"
  INFO:  "repaired torn tail" (expected occasionally after hard resets)
  FATAL: "FATFS unmountable", "SD mount failed" persisting past retry
and polls `status` / `evlog` / `log_status` for health between cycles.

Exits non-zero the moment corruption is detected (so the driving agent gets
woken early), otherwise runs until --until (HH:MM, local) and prints a summary.
Log: everything to --log (timestamped, raw serial included).
"""
import argparse
import datetime as dt
import re
import sys
import time

import serial  # pyserial

MARK_FATAL = [
    re.compile(rb"FATFS unmountable"),
    re.compile(rb"corrupt filesystem"),
]
MARK_MOUNT_OK = re.compile(rb"SD mounted: '")
MARK_EVLOG_OK = re.compile(rb"ready: files \d+\.\.\d+")
MARK_TORN = re.compile(rb"repaired torn tail")
MARK_MOUNT_FAIL = re.compile(rb"SD (card )?mount (retry )?failed")
MARK_RESET = re.compile(rb"rst:0x")          # ROM boot banner: proves a reset occurred
MARK_CONSOLE = re.compile(rb"CLI status:")   # `status` responded: console alive


class Bench:
    def __init__(self, port, logf):
        self.ser = serial.Serial(port, 115200, timeout=0.5)
        self.logf = logf
        self.hard_resets_enabled = True
        self.stats = {
            "events_stored": 0,
            "clean_reboots": 0,
            "hard_resets": 0,
            "torn_tail_repairs": 0,
            "mount_failures_transient": 0,
        }

    def log(self, msg):
        line = f"{dt.datetime.now().isoformat(timespec='seconds')} {msg}"
        print(line, flush=True)
        self.logf.write(line + "\n")
        self.logf.flush()

    def read_for(self, seconds):
        """Drain serial for `seconds`, logging raw output; return the bytes."""
        buf = b""
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            chunk = self.ser.read(4096)
            if chunk:
                buf += chunk
                self.logf.write(chunk.decode("utf-8", "replace"))
        self.logf.flush()
        return buf

    def cmd(self, c, wait=1.5):
        self.ser.write(c.encode() + b"\r\n")
        return self.read_for(wait)

    def hard_reset(self):
        """EN-pin reset via RTS/DTR (same dance esptool uses) — NO shutdown handler."""
        self.ser.dtr = False
        self.ser.rts = True     # EN low: chip in reset
        time.sleep(0.1)
        self.ser.rts = False    # EN high: boot
        self.stats["hard_resets"] += 1

    def check_boot(self, out, context):
        """Scan a boot log; return False on fatal corruption."""
        for pat in MARK_FATAL:
            if pat.search(out):
                self.log(f"FATAL after {context}: corrupt-FS marker seen")
                return False
        if MARK_TORN.search(out):
            self.stats["torn_tail_repairs"] += 1
            self.log(f"torn-tail repair after {context} (expected for hard resets)")
        if MARK_MOUNT_OK.search(out) and MARK_EVLOG_OK.search(out):
            self.log(f"boot OK after {context}: SD mounted, event log ready")
            return True
        if MARK_MOUNT_FAIL.search(out):
            self.stats["mount_failures_transient"] += 1
            self.log(f"WARN after {context}: mount failed in boot window — "
                     "waiting for hot-plug monitor remount")
            out2 = self.read_for(20)
            if MARK_MOUNT_OK.search(out2) or b"card detected" in out2:
                self.log("monitor remounted the card — continuing")
                return True
            self.log(f"FATAL after {context}: card did not come back within 20 s")
            return False
        # No positive marker — probe via CLI instead of failing blind.
        st = self.cmd("status", wait=3)
        if MARK_CONSOLE.search(st) and b"DB: online" in st:
            self.log(f"boot markers missed after {context}; console alive + DB online — continuing")
            return True
        if MARK_CONSOLE.search(st):
            self.log(f"FATAL after {context}: console alive but DB NOT online")
            return False
        self.log(f"FATAL after {context}: no boot markers and console unresponsive")
        return False

    def burst(self, n, period_s):
        for _ in range(n):
            self.cmd("record_env", wait=period_s)
            self.stats["events_stored"] += 1

    def health_snapshot(self):
        self.cmd("status", wait=3)
        self.cmd("evlog", wait=2)
        self.cmd("log_status", wait=2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--until", default="07:30", help="local HH:MM stop time")
    ap.add_argument("--log", default="/tmp/sd_stress.log")
    ap.add_argument("--burst", type=int, default=120, help="events per burst")
    ap.add_argument("--period", type=float, default=1.0, help="s between events")
    args = ap.parse_args()

    hh, mm = map(int, args.until.split(":"))
    now = dt.datetime.now()
    stop = now.replace(hour=hh, minute=mm, second=0, microsecond=0)
    if stop <= now:
        stop += dt.timedelta(days=1)

    with open(args.log, "a") as logf:
        b = Bench(args.port, logf)
        b.log(f"=== SD stress start; stopping at {stop} ===")
        # USB-Serial-JTAG does NOT reset on port open: expect a live console
        # mid-run, not a boot log. Probe it, then start cycle 1 with a reboot.
        b.read_for(3)
        st = b.cmd("status", wait=3)
        if not MARK_CONSOLE.search(st):
            b.log("FATAL: console unresponsive at connect")
            sys.exit(2)
        b.log("console alive at connect")
        b.health_snapshot()

        cycle = 0
        while dt.datetime.now() < stop:
            cycle += 1
            b.log(f"--- cycle {cycle}: write burst ({args.burst} events) ---")
            b.burst(args.burst, args.period)
            b.health_snapshot()

            b.log(f"--- cycle {cycle}: clean reboot ---")
            b.cmd("reboot", wait=1)
            b.stats["clean_reboots"] += 1
            boot = b.read_for(15)
            if not b.check_boot(boot, f"clean reboot (cycle {cycle})"):
                sys.exit(2)

            if dt.datetime.now() >= stop:
                break

            if b.hard_resets_enabled:
                b.log(f"--- cycle {cycle}: burst + HARD reset mid-write ---")
                b.burst(args.burst // 2, args.period)
                # Fire the reset with writes in flight: no drain, no warning.
                b.ser.write(b"record_env\r\n")
                time.sleep(0.15)
                b.hard_reset()
                boot = b.read_for(15)
                if not MARK_RESET.search(boot):
                    st = b.cmd("status", wait=3)
                    if MARK_CONSOLE.search(st):
                        b.log("WARN: DTR/RTS hard reset ineffective over USB-JTAG — "
                              "disabling hard-reset cycles (clean reboots continue)")
                        b.hard_resets_enabled = False
                        continue
                if not b.check_boot(boot, f"hard reset (cycle {cycle})"):
                    sys.exit(2)
                b.health_snapshot()

        b.log(f"=== DONE ===")
        b.log(f"summary: {b.stats}")
        # Final deep check: cursor + backlog still parse, card still mounts.
        b.health_snapshot()
        sys.exit(0)


if __name__ == "__main__":
    main()
