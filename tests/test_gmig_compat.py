"""G-MIG synthetic: v1.11.0 <-> candidate event-store backward compatibility.

Builds v1.11.0's production event_log (git tag, commit 8af0c76) and the
candidate's (GMIG_C2_REV, default 6fcbeba; WORKTREE = working tree) as two
host executables over one shared state (littlefs dir + NVS model), then runs
the scenarios in tests/gmig_host/gmig.py. Method, oracle and limits:
tests/gmig_host/README.md. A compact JSON line per scenario is printed; full
results land in $TMPDIR/gmig_host/results/.

Two tests are expectedFailure. Each documents a behaviour that is not a loss of
undelivered data inside the bench envelope; see the message on each.
Everything else must pass: a failure there means the rollback or the upgrade
can lose, skip, reorder or corrupt a pending record.
"""
from __future__ import annotations

import concurrent.futures as cf
import json
import multiprocessing as mp
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "gmig_host"))

import gmig as G  # noqa: E402

RESULTS: dict[str, dict] = {}
IN_ENVELOPE = ["bench_cursor0", "bench_cursor0_sd", "bench_cursor_gt0", "cand_acks_stores_rotates",
               "cand_crash", "sd_present"]


def _one(name: str) -> dict:
    r = G.SCENARIOS[name]()
    r["failures"] = G.failures(r)
    return r


def setUpModule() -> None:
    G.exes()                                           # build once, before forking
    with cf.ProcessPoolExecutor(max_workers=4, mp_context=mp.get_context("fork")) as ex:
        for r in ex.map(_one, list(G.SCENARIOS)):
            RESULTS[r["scenario"]] = r
    out = G.B.tmp_root() / "results"
    out.mkdir(exist_ok=True)
    for name, r in RESULTS.items():
        (out / f"{name}.json").write_text(json.dumps(r, indent=1, default=str))
        print(json.dumps(G.summary(r), default=str))


def _fail_text(r: dict) -> str:
    return json.dumps(G.summary(r), indent=1, default=str)


class GmigCompat(unittest.TestCase):
    def test_bench_state_is_the_devices(self):
        """The v1.11.0-created state matches the bench unit: files 1..1, cursor 1:0, 309 pending, next_id 387242."""
        s = RESULTS["bench_cursor0"]["v1110_shape"]
        self.assertEqual(s["files"], [1, 1], s)
        self.assertEqual(tuple(s["cursor_legacy"]), (1, 0), s)
        self.assertEqual(s["pending"], 309, s)
        self.assertEqual(s["next_id"], 387242, s)

    def test_cursor_gt0_state(self):
        """The cursor>0 variant really has a delivered prefix mid-file and several rotated files."""
        s = RESULTS["bench_cursor_gt0"]["v1110_shape"]
        self.assertGreater(s["files"][1], s["files"][0], s)
        self.assertGreater(s["cursor_legacy"][1], 0, s)
        self.assertEqual(s["pending"], 209, s)

    def test_candidate_first_boot_footprint(self):
        """Candidate first boot on v1.11.0 state: creates only evstore/evq.idx and NVS evlog/cur; touches no record file."""
        for name in ("bench_cursor0", "bench_cursor0_sd", "bench_cursor_gt0", "sd_present"):
            fb = RESULTS[name]["first_boot_changes"]
            self.assertEqual(fb["created"], ["evstore/evq.idx"], name)
            self.assertEqual(fb["removed"], [], name)
            self.assertEqual(fb["modified"], [], name)
            self.assertEqual([n.get("key") for n in fb["nvs"]], ["cur"], name)
            ops = RESULTS[name]["first_boot_nvs_ops"]
            self.assertEqual(ops["namespaces_opened"], ["evlog"], name)
            self.assertEqual(ops["keys_written"], ["evlog/cur:blob", "evlog/rd_off:u32", "evlog/rd_seq:u32"], name)
            self.assertEqual(ops["type_conflicts"], 0, name)
        self.assertEqual(RESULTS["bench_cursor0_sd"]["preexisting_sd_files_changed"], [])

    def test_v1110_reopen_is_a_noop(self):
        """v1.11.0 reopening the migrated store changes no file and no NVS key (no reformat, no rewrite)."""
        for name in ("bench_cursor0", "bench_cursor0_sd", "bench_cursor_gt0", "cand_acks_stores_rotates"):
            rc = RESULTS[name]["v1110_reopen_changes"]
            self.assertEqual((rc["created"], rc["removed"], rc["modified"], rc["nvs"]), ([], [], [], []), name)

    def test_v1110_ignores_candidate_files(self):
        """Every /evstore path the candidate creates is evq.idx or an ev-<seq>.log; v1.11.0 never names evq.idx."""
        import re
        made = {f for r in RESULTS.values() for st in r["steps"] if st["fw"] == "candidate"
                for f in st["diff"]["created"] if f.startswith("evstore/")}
        odd = sorted(f for f in made if f != "evstore/evq.idx" and not re.fullmatch(r"evstore/events/ev-\d{6,}\.log", f))
        self.assertEqual(odd, [])
        src = (Path(G.exes()["info"]["old_exe"]).parent / "src-old/components/event_log/event_log.c").read_text()
        self.assertNotIn("evq", src)
        self.assertIn('if (strcmp(p, ".log") != 0) return false;', src)   # parse_ev_name: exact ev-<digits>.log

    def test_in_envelope_scenarios_pass_every_check(self):
        """No loss, no skip, exact bytes, FIFO, cursor never beyond an undelivered id, views agree (bench envelope)."""
        for name in IN_ENVELOPE:
            with self.subTest(scenario=name):
                r = RESULTS[name]
                self.assertEqual(r["failures"], [], _fail_text(r))
                self.assertEqual([d for d in r["drops"] if d["fw"] == "candidate"], [], _fail_text(r))

    def test_crash_only_replays_acked_records(self):
        """A candidate crash (no shutdown handler) replays at most one 16-ack cursor batch, and nothing is lost."""
        v = RESULTS["cand_crash"]["checks"]["after-v1110-reopen"]["probe"]["v1.11.0"]["checks"]
        self.assertTrue(v["no_loss"]["ok"])
        self.assertLessEqual(v["exact_pending"]["redelivered_acked"], 15)

    def test_reupgrade_keeps_every_pending_record(self):
        """candidate -> v1.11.0 (acks + stores) -> candidate: every undelivered record still delivered, in order."""
        r = RESULTS["reupgrade"]
        hard = [f for f in r["failures"] if not f.endswith("/corpus/delivered_retained")]
        self.assertEqual(hard, [], _fail_text(r))

    @unittest.expectedFailure
    def test_reupgrade_retains_delivered_records(self):
        """DOCUMENTED DEVIATION (not pending loss): on re-upgrade after a rollback, the candidate's re-import of a
        file the rollback firmware passed without the candidate's ACK (evq_reimport_one) removes the whole flash
        source (components/event_log/event_log.c:3423, `remove(fp)`), including its prefix that the candidate itself
        had already delivered and never archived. Those delivered records then exist nowhere on the device, which
        contradicts the retention rule "flash copies of a delivered file are dropped only after a verified archive
        copy exists" (eviction under pressure excepted)."""
        r = RESULTS["reupgrade"]
        self.assertEqual([d for d in r["drops"] if d["unexplained_ids"]], [], _fail_text(r))

    def test_pressure_no_loss_with_card(self):
        """Outside the envelope (flash pressure): v1.11.0 with the card still delivers every record eventually."""
        v = RESULTS["pressure"]["checks"]["v1110-with-card"]
        self.assertTrue(v["corpus"]["ok"], _fail_text(RESULTS["pressure"]))
        self.assertTrue(v["probe"]["v1.11.0"]["checks"]["no_loss"]["ok"], _fail_text(RESULTS["pressure"]))

    @unittest.expectedFailure
    def test_pressure_strict_rollback(self):
        """DOCUMENTED ROLLBACK FLOOR (outside the bench envelope; the bench store is ~2.5 % full, pressure starts
        at 75 % used): after the candidate reclaims flash copies of unsent segments that exist only on SD
        (SD_ONLY), v1.11.0 has no index. Its open clamps the cursor to the lowest flash file (v1.11.0
        event_log.c:712, `if (cseq < min_seq)`), passing the SD-only records without counting a skip. It
        delivers newer records first and re-imports the SD-only ones out of order through
        /sdcard/events only while a card is present and flash has 256 KiB free. With the card out at
        rollback, those records are not on the device at all."""
        v = RESULTS["pressure"]["checks"]
        self.assertTrue(v["v1110-no-card"]["probe"]["v1.11.0"]["checks"]["no_loss"]["ok"])
        self.assertTrue(v["v1110-with-card"]["static"]["views_agree"]["ok"])

    def test_negative_controls(self):
        """The oracle has teeth: a skipped cursor, a corrupted line and a deleted queue file are each caught."""
        snap = G.runs_root() / "bench_cursor0" / "snap-01-v1110-state"
        expect = {"cursor_skip": ["tampered/cursor/legacy_not_beyond_undelivered", "tampered/v1.11.0/no_loss"],
                  "line_corrupt": ["tampered/corpus/undelivered_intact", "tampered/v1.11.0/bytes_exact"],
                  "file_gone": ["tampered/corpus/undelivered_intact", "tampered/v1.11.0/no_loss"]}
        for kind, must_fail in expect.items():
            with self.subTest(kind=kind):
                f = G.negative_control(kind, snap)["failures"]
                for m in must_fail:
                    self.assertIn(m, f, f)

    def test_device_dump_mode(self):
        """The real-dump entry point (evstore dir + NVS JSON) reproduces the synthetic bench result."""
        snap = G.runs_root() / "bench_cursor0" / "snap-01-v1110-state"
        ev, nv, sd = G.export_dump(snap, G.runs_root() / "dump-input")
        r = G.check_dump(ev, nv, sd, name="dump_selftest")
        self.assertEqual(G.failures(r), [], _fail_text(r))
        self.assertEqual(r["dump"]["must_have"], 309)


if __name__ == "__main__":
    unittest.main()
