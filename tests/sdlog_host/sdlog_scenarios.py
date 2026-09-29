"""sd_logger scenarios SL1..SL21 (contract §5.2 + §5.4a C-53/C-54).

Each scenario drives one Dev and returns (checks, base_red): `checks` is a list
of (name, ok, detail) that must all hold on HEAD; `base_red` is the RED
signature evaluated on the baseline build (True = the defect reproduced), or
None when the contract claims no baseline RED for that scenario."""
from __future__ import annotations

import sdlog_lib as L

MB = 1024 * 1024
IDLE = 250          # ticks per idle writer step
SYNC_STEPS = 10     # > SD_LOGGER_FSYNC_MS / IDLE: a periodic commit happens


def _ops(dev, op, sub=None):
    return [o for o in dev.ops() if o["op"] == op and (sub is None or sub in o["path"])]


def _common(dev, a, rep):
    head = a.get("acct") is not None
    c = [("C-10 record oracle", rep["ok"], rep)]
    if head:
        c.append(("C-12 refs held at every FS call / none at delay",
                  L.refs_ok(a), {k: a[k] for k in ("violations", "refs_at_delay", "refs")}))
    return c


def _start(dev, arms=()):
    dev.start()
    for arm in arms:                  # armed before init: the very first open is covered
        dev.arm(*arm)
    dev.init()
    dev.step(1)


def sl1(dev):
    """periodic fsync EIO → rollback to the committed boundary, attributed"""
    _start(dev)
    for i in range(5):
        dev.log(i)
    dev.step(2)
    dev.arm("fsync", "eio")
    dev.step(SYNC_STEPS)
    for i in range(5, 10):
        dev.log(i)
    dev.step(SYNC_STEPS)
    a = dev.acct()
    dev.finish()
    rep = L.oracle(dev, a)
    injected = any(o["inj"] == "eio" for o in _ops(dev, "fsync"))
    base_red = injected and a["io_errors"] == 0
    c = _common(dev, a, rep)
    if a.get("acct"):
        x = a["acct"]
        first5 = sum(len(L.expected_record(f, t)) for f, t in dev.calls[:5])
        c += [("C-01 fsync_err=1", x["fsync_err"] == 1, x["fsync_err"]),
              ("C-01 rolled_back = bytes since commit", x["rolled_back_bytes"] == first5, (x["rolled_back_bytes"], first5)),
              ("C-01 io error reported", a["io_errors"] >= 1, a["io_errors"]),
              ("C-29 diag sdlog.fsync counted", x and a["diag"]["faults"].get("sdlog.fsync") == 1, a.get("diag"))]
    return c, base_red


def sl2(dev):
    """fflush EIO → same handling as a failed fsync"""
    _start(dev)
    for i in range(4):
        dev.log(i)
    dev.step(2)
    dev.arm("fflush", "eio")
    dev.step(SYNC_STEPS)
    for i in range(4, 8):
        dev.log(i)
    dev.step(SYNC_STEPS)
    a = dev.acct()
    dev.finish()
    rep = L.oracle(dev, a)
    injected = any(o["inj"] == "eio" for o in _ops(dev, "fflush"))
    base_red = injected and a["io_errors"] == 0
    c = _common(dev, a, rep)
    if a.get("acct"):
        x = a["acct"]
        c += [("C-02 flush_err=1", x["flush_err"] == 1, x["flush_err"]),
              ("C-02 rolled back", x["rolled_back_bytes"] > 0, x["rolled_back_bytes"]),
              ("C-02 io error reported", a["io_errors"] >= 1, a["io_errors"])]
    return c, base_red


def sl3(dev):
    """short fwrite → truncated back; next record starts on a boundary"""
    _start(dev)
    dev.log(0)
    dev.step(SYNC_STEPS)
    dev.arm("fwrite", "short")
    dev.log(1, 120)
    dev.log(2, 70)
    dev.step(2)
    for i in range(3, 6):
        dev.log(i)
    dev.step(SYNC_STEPS)
    a = dev.acct()
    dev.finish()
    rep = L.oracle(dev, a)
    base_red = not rep["ok"]
    c = _common(dev, a, rep)
    if a.get("acct"):
        x = a["acct"]
        c += [("C-03 write_err=1", x["write_err"] == 1, x["write_err"]),
              ("C-03 lost_unwritten > 0", x["lost_unwritten_bytes"] > 0, x["lost_unwritten_bytes"]),
              ("C-03 not quarantined (rollback proven)", x["quarantined"] == 0 and x["indeterminate_bytes"] == 0, x)]
    return c, base_red


def sl4(dev):
    """short fwrite with the rollback truncate failing → quarantine + rotation;
    torn tail only at the EOF of the retired file, length == indeterminate"""
    _start(dev)
    dev.log(0)
    dev.step(SYNC_STEPS)
    dev.arm("fwrite", "short")
    dev.arm("ftruncate", "eio")
    dev.log(1, 150)
    dev.step(1)
    for i in range(2, 5):
        dev.log(i)
    dev.step(SYNC_STEPS)
    a = dev.acct()
    dev.finish()
    rep = L.oracle(dev, a)
    base_red = not rep["ok"]
    c = _common(dev, a, rep)
    if a.get("acct"):
        x = a["acct"]
        rot = [o for o in _ops(dev, "rename") if o["ret"] == 0]
        c += [("C-04 indeterminate == torn tail length",
               len(rep["tails"]) == 1 and rep["tails"][0][1] == x["indeterminate_bytes"] > 0, (rep["tails"], x)),
              ("C-04 tail sits in the retired file", rep["tails"] and rep["tails"][0][0] == "ambyte.1.log", rep["tails"]),
              ("C-04 next append only after a rotation", len(rot) >= 1, len(rot)),
              ("C-04 truncate_err counted", x["truncate_err"] == 1, x["truncate_err"])]
    return c, base_red


def _storm_setup(dev):
    # steady state after five rotations: every slot 1..5 exists, so when the
    # oldest cannot be removed every rename in the chain meets EEXIST
    for name in ["ambyte.1.log", "ambyte.2.log", "ambyte.3.log", "ambyte.4.log", "ambyte.5.log"]:
        dev.prefill_file(name, 2000)
    dev.prefill_file("ambyte.log", 2 * MB - 6000)


def sl5(dev):
    """oldest-file remove fails: base storms (remove+renames on every write);
    head backs off (≤1 attempt / 60 s), file ≤ 2 MiB, then attributed drops"""
    _storm_setup(dev)
    _start(dev, [("remove", "eio", 1, 100000, "ambyte.5.log")])
    for i in range(200):
        dev.log(i, 100)
        dev.step(1)
    dev.step(SYNC_STEPS)
    a = dev.acct()
    dev.finish()
    rep = L.oracle(dev, a)
    attempts = len(_ops(dev, "remove", "ambyte.5.log"))
    size = (dev.state / "sdcard/logs/ambyte.log").stat().st_size
    base_red = attempts >= 200 or size > 2 * MB
    c = _common(dev, a, rep)
    if a.get("acct"):
        x = a["acct"]
        windows = a["tick"] // 60000 + 1
        c += [("C-05 rotation attempts bounded (≤1 per 60 s)", attempts <= windows, (attempts, windows)),
              ("C-05 rotate_err counted", x["rotate_err"] == attempts, (x["rotate_err"], attempts)),
              ("C-05 file ≤ 2 MiB", size <= 2 * MB, size),
              ("C-05 drops attributed rotate_blocked", x["dropped_rotate_blocked_bytes"] > 0, x)]
    return c, base_red


def sl6(dev):
    """mid-chain rename fails: bounded like SL5, no rotated file other than the
    oldest unlinked or overwritten"""
    for name in ["ambyte.1.log", "ambyte.2.log", "ambyte.3.log", "ambyte.4.log", "ambyte.5.log"]:
        dev.prefill_file(name, 1500)
    dev.prefill_file("ambyte.log", L.FILE_CAP)
    _start(dev, [("rename", "eio", 1, 100000, "ambyte.2.log")])
    for i in range(200):
        dev.log(i, 100)
        dev.step(1)
    dev.step(SYNC_STEPS)
    a = dev.acct()
    dev.finish()
    rep = L.oracle(dev, a)
    attempts = len(_ops(dev, "rename", "ambyte.2.log"))
    base_red = attempts >= 200
    c = _common(dev, a, rep)
    if a.get("acct"):
        removed = {o["path"] for o in _ops(dev, "remove") if o["ret"] == 0}
        windows = a["tick"] // 60000 + 1
        c += [("C-06 attempts bounded", attempts <= windows, (attempts, windows)),
              ("C-06 only the oldest file unlinked", removed <= {"./sdcard/logs/ambyte.5.log"}, removed),
              ("C-06 no rename onto an existing file succeeded",
               all(o["errno"] != 0 or o["ret"] == 0 for o in _ops(dev, "rename")), None)]
    return c, base_red


def sl7(dev):
    """ftell fails at open → open_err, no append at an assumed offset 0"""
    dev.prefill_file("ambyte.log", 3000)
    _start(dev, [("ftell", "eio")])
    for i in range(3):
        dev.log(i)
    dev.step(SYNC_STEPS)
    a = dev.acct()
    dev.finish()
    rep = L.oracle(dev, a)
    c = _common(dev, a, rep)
    if a.get("acct"):
        seq = [o["op"] for o in dev.ops() if o["path"].endswith("ambyte.log")]
        first_open_idx = seq.index("fopen")
        # the failing handle (fopen #1) is closed before any write
        between = seq[first_open_idx + 1: seq.index("fopen", first_open_idx + 1)] if seq.count("fopen") > 1 else seq
        c += [("C-07 open_err=1", a["acct"]["open_err"] == 1, a["acct"]["open_err"]),
              ("C-07 no write on the handle whose size was unknown", "fwrite" not in between, between)]
    return c, None


def sl8(dev):
    """pause with a failing sync: completes ≤1 s, errors counted, no FS ops while
    paused, resume reopens"""
    _start(dev)
    for i in range(3):
        dev.log(i)
    dev.step(1)
    dev.arm("fsync", "eio")
    p = dev.cmd("pause", expect=True)
    before = dev.opcount()
    dev.log(3)
    dev.step(10)
    during = dev.opcount()
    dev.cmd("resume")
    dev.step(1)
    dev.log(4)
    dev.step(SYNC_STEPS)
    a = dev.acct()
    dev.finish()
    rep = L.oracle(dev, a)
    c = _common(dev, a, rep)
    if a.get("acct"):
        c += [("C-08 pause ≤ 1 s", p["pause_ticks"] <= 1000, p),
              ("C-08 zero FS ops while paused", before == during, (before, during)),
              ("C-08 failing sync counted", a["acct"]["fsync_err"] == 1, a["acct"]["fsync_err"])]
    return c, None


def sl9(dev):
    """shutdown with a failing sync: bounded ≤1 s, counted"""
    _start(dev)
    for i in range(3):
        dev.log(i)
    dev.step(1)
    dev.arm("fsync", "eio")
    s = dev.cmd("stop", expect=True)
    a = dev.acct()
    dev.finish()
    rep = L.oracle(dev, a)
    c = _common(dev, a, rep)
    if a.get("acct"):
        c += [("C-08 shutdown ≤ 1 s", s["stop_ticks"] <= 1000, s),
              ("C-08 failing sync counted", a["acct"]["fsync_err"] == 1, a["acct"]["fsync_err"])]
    return c, None


def sl10(dev):
    """recursion storm: the writer's own failure reports never enter the ring;
    console failure lines ≤1 per 60 s per class"""
    _start(dev)
    dev.arm("fwrite", "eio", 1, 50)
    for i in range(60):
        dev.log(i)
        dev.step(1)
    dev.step(SYNC_STEPS)
    a = dev.acct()
    dev.finish()
    rep = L.oracle(dev, a)
    c = _common(dev, a, rep)
    if a.get("acct"):
        logs = b"".join((dev.state / "sdcard/logs" / n).read_bytes() for n in L.LOG_NAMES
                        if (dev.state / "sdcard/logs" / n).exists())
        classes = 6
        limit = classes * (a["tick"] // 60000 + 1)
        cl = L.sd_logger_console_lines(dev)
        c += [("C-09 no sd_logger lines in the SD files", b" sd_logger: " not in logs, None),
              ("C-09 console failure lines rate-limited", 1 <= cl <= limit, (cl, limit))]
    return c, None


def sl11(dev):
    """card lost mid-write: handle abandoned, ring keeps buffering, attributed"""
    _start(dev)
    for i in range(3):
        dev.log(i)
    dev.step(SYNC_STEPS)
    dev.arm("fwrite", "lose")
    dev.log(3, 100)
    dev.step(1)
    for i in range(4, 7):
        dev.log(i)
    dev.step(4)
    dev.cmd("remount")
    dev.step(SYNC_STEPS)
    a = dev.acct()
    dev.finish()
    rep = L.oracle(dev, a)
    c = _common(dev, a, rep)
    if a.get("acct"):
        ops = dev.ops()
        lose_i = next(i for i, o in enumerate(ops) if o["inj"] == "lose")
        # after the loss latched, no FATFS op on the SD until the remount made the card usable again
        after = [o for o in ops[lose_i + 1:] if o["lost"] == 1 and o["op"] in ("fclose",) and o["refs"] == 0]
        c += [("C-11 no fclose outside an io ref on the lost volume", not after, after[:3]),
              ("C-11 unwritten chunk attributed", a["acct"]["lost_unwritten_bytes"] > 0, a["acct"])]
    return c, None


def sl12(dev):
    """shutdown while a failed rotation backs off: bounded, counted"""
    _storm_setup(dev)
    _start(dev, [("remove", "eio", 1, 100000, "ambyte.5.log")])
    for i in range(5):
        dev.log(i, 100)
        dev.step(1)
    s = dev.cmd("stop", expect=True)
    a = dev.acct()
    dev.finish()
    rep = L.oracle(dev, a)
    c = _common(dev, a, rep)
    if a.get("acct"):
        c += [("C-11 shutdown during backoff ≤ 1 s", s["stop_ticks"] <= 1000, s),
              ("C-11 rotation failure counted", a["acct"]["rotate_err"] >= 1, a["acct"]["rotate_err"])]
    return c, None


FMT_NONL = b"W (0) drv: %s"
FMT_NL = b"W (0) drv: %s\n"


def sl13(dev):
    """messages without a trailing newline → one record each"""
    _start(dev)
    for i in range(6):
        dev.logf(FMT_NONL, b"SEQ=%06d|no-newline" % i)
    dev.step(SYNC_STEPS)
    a = dev.acct()
    dev.finish()
    rep = L.oracle(dev, a)
    return _common(dev, a, rep), not rep["ok"]


def _len_msg(seq: int, total_formatted: int) -> bytes:
    # total record length before framing = prefix(21) + "W (0) drv: "(11) + msg + "\n"
    n = total_formatted - 21 - 11 - 1
    head = b"SEQ=%06d|" % seq
    return head + b"x" * max(0, n - len(head))


def sl14(dev):
    """boundary lengths 254/255/256/257/1000 → cut with ~T, counted exactly"""
    _start(dev)
    for k, tot in enumerate([254, 255, 256, 257, 1000, 254, 300]):
        dev.logf(FMT_NL, _len_msg(k, tot))
        dev.step(1)
    dev.step(SYNC_STEPS)
    a = dev.acct()
    dev.finish()
    rep = L.oracle(dev, a)
    c = _common(dev, a, rep)
    if a.get("acct"):
        want = sum(1 for f, t in dev.calls if L.expected_record(f, t).endswith(b"~T\n"))
        c.append(("C-10 truncated_records exact", a["acct"]["truncated_records"] == want,
                  (a["acct"]["truncated_records"], want)))
    return c, not rep["ok"]


def sl15(dev):
    """embedded CR/LF → one record, CR/LF replaced by spaces"""
    _start(dev)
    for i in range(3):
        dev.logf(FMT_NL, b"SEQ=%06d|a\nb\rc\r\nd" % i)
    dev.step(SYNC_STEPS)
    a = dev.acct()
    dev.finish()
    rep = L.oracle(dev, a)
    return _common(dev, a, rep), None


def sl16(dev):
    """a boundary-length record wrapping the 8 KiB ring end at every offset 0..255"""
    _start(dev)
    ring = 8192
    pushed = 0
    since_step = 0
    seq = 100000

    def push(msg):
        nonlocal pushed, since_step
        dev.logf(FMT_NL, msg)
        pushed += len(L.expected_record(*dev.calls[-1]))
        since_step += 1
        if since_step >= 20:          # drain well before the ring could overflow
            dev.step(1)
            since_step = 0

    for k in range(256):
        target = (-k) % ring               # next record starts k bytes before the ring end
        while pushed % ring != target:
            gap = (target - pushed) % ring
            while gap < 44:                 # smallest filler record is 44 B: go round once more
                gap += ring
            size = gap if gap <= 255 else (255 if gap - 255 >= 44 else gap - 44)
            size = min(size, 255)
            head = b"SEQ=%06d|" % seq
            seq += 1
            push(head + b"f" * (size - 33 - len(head)))
        dev.step(1)
        since_step = 0
        push(_len_msg(k, 400))              # always cut at 256 B: a boundary-length record
        dev.step(1)
        since_step = 0
    dev.step(SYNC_STEPS)
    a = dev.acct()
    dev.finish()
    rep = L.oracle(dev, a)
    return _common(dev, a, rep), not rep["ok"]


def sl17(dev):
    """ring-full storm: whole-record drops, attributed apart from storage faults"""
    _start(dev)
    for i in range(100):
        dev.log(i, 200)
    dev.step(SYNC_STEPS * 3)
    a = dev.acct()
    dev.finish()
    rep = L.oracle(dev, a)
    c = _common(dev, a, rep)
    if a.get("acct"):
        x = a["acct"]
        c += [("C-10 dropped_records == missing records", x["dropped_records"] == rep["missing_records"] > 0,
               (x["dropped_records"], rep["missing_records"])),
              ("C-10 ring drops kept apart from storage buckets",
               x["rolled_back_bytes"] == x["indeterminate_bytes"] == x["lost_unwritten_bytes"] == 0, x)]
    return c, None


def sl18(dev):
    """empty and ANSI-only messages still produce exactly one record"""
    _start(dev)
    dev.logf(FMT_NL, b"")
    dev.logf(b"\x1b[0;33mW (0) drv: %s\x1b[0m\n", b"SEQ=000001|ansi")
    dev.logf(b"\x1b[0;33mW (0) drv: %s\x1b[0m\n", b"")
    dev.logf(b"I (0) drv: %s\n", b"info-not-captured")
    dev.step(SYNC_STEPS)
    a = dev.acct()
    dev.finish()
    rep = L.oracle(dev, a)
    return _common(dev, a, rep), None


def sl19(dev):
    """C-53: periodic fsync applied_eio (durable, reported failed) → rolled back"""
    _start(dev)
    for i in range(3):
        dev.log(i)
    dev.step(2)
    dev.arm("fsync", "applied_eio")
    dev.step(SYNC_STEPS)
    dev.log(3)
    dev.step(SYNC_STEPS)
    a = dev.acct()
    dev.finish()
    rep = L.oracle(dev, a)
    c = _common(dev, a, rep)
    if a.get("acct"):
        first3 = sum(len(L.expected_record(f, t)) for f, t in dev.calls[:3])
        c.append(("C-53 rolled_back exact", a["acct"]["rolled_back_bytes"] == first3,
                  (a["acct"]["rolled_back_bytes"], first3)))
    return c, None


def _rollback_fail(dev, second_op, second_nth):
    _start(dev)
    dev.log(0)
    dev.step(SYNC_STEPS)
    dev.log(1, 90)
    dev.step(2)
    dev.arm("fsync", "eio", 1, 1)
    dev.arm(second_op, "applied_eio", second_nth, 1)
    dev.step(SYNC_STEPS)
    for i in range(2, 4):
        dev.log(i)
    dev.step(SYNC_STEPS)
    a = dev.acct()
    dev.finish()
    rep = L.oracle(dev, a)
    c = _common(dev, a, rep)
    if a.get("acct"):
        x = a["acct"]
        rec1 = len(L.expected_record(*dev.calls[1]))
        c += [("C-54 quarantined → rotated before the next append",
               any(o["ret"] == 0 for o in _ops(dev, "rename")), None),
              ("C-54 indeterminate == bytes since commit", x["indeterminate_bytes"] == rec1, (x["indeterminate_bytes"], rec1)),
              ("C-54 tail absent (truncate was applied)", not rep["tails"], rep["tails"])]
    return c, None


def sl20(dev):
    """C-54: rollback ftruncate applied_eio"""
    return _rollback_fail(dev, "ftruncate", 1)


def sl21(dev):
    """C-54: rollback fsync applied_eio (the 2nd fsync)"""
    return _rollback_fail(dev, "fsync", 2)


SCENARIOS = {f"SL{i}": f for i, f in enumerate(
    [sl1, sl2, sl3, sl4, sl5, sl6, sl7, sl8, sl9, sl10, sl11, sl12, sl13, sl14, sl15, sl16, sl17, sl18, sl19, sl20,
     sl21], start=1)}
BASE_RED_EXPECTED = {"SL1", "SL2", "SL3", "SL4", "SL5", "SL6", "SL13", "SL14", "SL16"}
