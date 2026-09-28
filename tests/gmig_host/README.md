# G-MIG host proof: v1.11.0 <-> candidate event store

This is executable evidence for one question. A bench Ambyte on **v1.11.0** holds 309 undelivered records (files 1..1, cursor seq=1 off=0, next_id 387242). If the candidate boots on that state, migrates it, and the health gate then rolls back to v1.11.0, is any pending record lost, skipped, reordered or corrupted? And does either firmware choke on the other's files or NVS?

Run it:

```
CC=clang TMPDIR=<scratch> python3 -m unittest tests.test_gmig_compat -v
CC=clang TMPDIR=<scratch> python3 tests/gmig_host/gmig.py scenarios [NAME ...]   # JSON line per scenario
```

- `GMIG_C2_REV=<rev>` picks the candidate commit (default: C2 `6fcbeba`, rebuilt from `tests/fixtures/pinned_base/` by `tests/pinned_revs.py`, so no branch or tag is needed). `GMIG_C2_REV=WORKTREE` builds the working tree instead.
- Full results are written to `$TMPDIR/gmig_host/results/<scenario>.json`.
- Every state directory, snapshot and probe copy is kept under `$TMPDIR/gmig_host/runs/`.

## Method

**Two executables, one device** (`build.py`). The harness builds the production event store twice:

- `git show v1.11.0:` builds `event_log.c` and `event_log.h`, plus v1.11.0's own `domain/include` and `sd_card.h`.
- `git show <candidate>:` builds `event_log.c`, `evq_index.c` and `evq_render.c`.

The only source edit is the evq_host path-literal rewrite (`"/evstore"` becomes `"./evstore"`, `"/sdcard/` becomes `"./sdcard/`). Each rewrite is counted and the original SHA-256 is recorded in `build_info.json`.

Production constants are **not** tuned. That means 256 KiB rotation, the 1000-store SD burst, 25 %/40 % pressure thresholds and a 9,830,400 B partition, the same `storage` partition in both tags.

Both executables link the same pieces:

- the tests/evq_host media shim and stubs, taken from the candidate revision;
- `gmig_driver.c`;
- the NVS model `nvs_model.c`.

That is how one device's state passes between two images: the same `evstore/` directory, the same `nvs.db`, the same `sdcard/`. The evq_host committed-only NVS stub is compiled out by renaming its symbols. clang builds with ASan and UBSan, and any sanitizer report fails the run.

**NVS model** (`stubs/nvs.h` documents it):

- typed `(namespace, key)` entries;
- a get with the wrong type returns `TYPE_MISMATCH`;
- a set that would change an existing key's type is refused and counted. The tests assert zero such sets.
- a set is durable as soon as it returns (IDF behaviour) and `commit` is a no-op;
- a read-only open of a missing namespace returns `NOT_FOUND`;
- a failed get never writes its output.

Every call is logged to `.shim/nvs_ops.jsonl`, and the "keys read/written" evidence comes from that log.

**Driver** (`gmig_driver.c`). It runs one boot per invocation. The commands are:

- `store`: deterministic records with multi-KB payloads, rendered in the exact v2 line format;
- `deliver`: claim, then in-order PUBACK with a window of 16;
- `peek`: claim without an ACK, revert, then check that the re-claim is FIFO;
- `service`: the candidate's `event_log_sd_service`, or on v1.11.0 its `app_sd_keeper_task` body;
- `sd insert|remove`;
- `crash`: exit without the shutdown handler, which models a CPU reset.

When a script ends the driver shuts down gracefully (`event_log_prepare_shutdown`).

**Oracle** (`gmig.py`). It checks each state in several ways:

- **corpus**: every stored record, keyed by id and the SHA-256 of its exact line. It must stay findable with identical bytes on flash, the SD primary or mirror, or the SD archive. An undelivered record that is gone is a hard failure. A delivered record that leaves the device is attributed to a step and a firmware log reason. The candidate may drop a delivered record only by eviction.
- **must_have**: the corpus minus records PUBACKed in real sessions.
- **probe**: the state is copied and one firmware delivers everything, so the original state is untouched. Checks:
  - no loss;
  - exact bytes;
  - no phantoms;
  - FIFO over the undelivered records;
  - no skip or quarantine counters;
  - drained with `ESP_ERR_NOT_FOUND`;
  - exact equality with must_have in strict views.
- **static**: the pending list derived only from NVS and the files, under v1.11.0 semantics and separately under candidate semantics. v1.11.0 semantics means `rd_seq`/`rd_off`, clamped to the files present. Candidate semantics means the `cur` blob reconciled with the legacy keys as in `evq_reconcile_cursor_locked`, plus the index. Both must agree with each other and with the probe.
- **cursor**: neither the legacy cursor nor the blob cursor may sit past an undelivered record.
- **footprint**: a before/after snapshot of every file and NVS key for each boot.
- **negative controls**: each of these tampers must be caught:
  - a cursor moved past one record;
  - one flipped byte in a pending line;
  - a deleted queue file.

## Scenarios

| name | what | strict |
|---|---|---|
| bench_cursor0 | v1.11.0 stores 309 records (files 1..1, cursor 1:0, next_id 387242). Candidate first boot runs peek 16 and 3 keeper passes. Then v1.11.0 reopens. | yes |
| bench_cursor0_sd | Same, with a v1.11.0-era SD card holding an archive, sd_logger output and an AMBIT image. Pre-existing SD files must be untouched. | yes |
| bench_cursor_gt0 | 309 multi-KB records in 8 files with 100 delivered, so the cursor sits at 3:170422. | yes |
| cand_acks_stores_rotates | After first boot the candidate acks 150 across file boundaries (D lines) and stores 400 (S lines). Then v1.11.0. | yes |
| cand_crash | The candidate acks 37 and stores 50, then crashes with no shutdown handler. Then v1.11.0. Only already-acked records may replay. | no (duplicates allowed) |
| sd_present | Card in. The candidate stores 1100 records, which triggers the burst: it spools unsent files to SD and archives delivered ones. v1.11.0 then runs with the card (it imports the SD primaries) and without it. | with card: no (duplicates) |
| reupgrade | Candidate, then v1.11.0 (acks 60, stores 30), then the candidate again. The legacy keys are now ahead of the `cur` blob, a foreign move, so the candidate re-imports. | after re-boot: no |
| pressure | **Outside the bench envelope**: a 3 MiB partition with the card in. The candidate reclaims flash copies of spooled unsent files, leaving them SD_ONLY. | documents the rollback floor |

`gmig.py check --evstore DIR --nvs nvs.json [--sdcard DIR]` runs the same round trip on a **real device dump**: candidate first boot, then v1.11.0 reopen. The corpus and must_have come from the dump itself: every flash record, and the records at or after its v1.11.0 cursor. The NVS JSON looks like this:

```
{"evlog": {"rd_seq": {"type": "u32", "value": 1}, "rd_off": {"type": "u32", "value": 0},
           "nid": {"type": "u64", "value": 387253}, "cur": {"type": "blob", "hex": "..."}}}
```

It maps `{namespace: {key: {"type": u8|i8|u16|i16|u32|i32|u64|i64|str|blob, "value" or "hex"}}}`. Other namespaces are carried through untouched. `export_dump()` writes a state in this format, and `test_device_dump_mode` proves the path end to end.

## Limits (what the host shims cannot show)

- **littlefs itself.** `evstore/` is a host directory. On-flash format, power-fail atomicity, block exhaustion and mount failure are not modelled, so a `format_if_mount_failed` reformat cannot happen here. The risk is reduced by static facts: the littlefs submodule is `509339f` at both tags, `partitions.csv` is identical, and there are no LFS sdkconfig changes.
- **Real NVS.** Page layout, entry budget, wear and torn page writes are not modelled. Sets are atomic per key here.
- **Crashes.** `crash` is a CPU reset: every byte written survives. Power cuts that roll back unsynced data are not simulated. The tests/evq_host fault matrix covers the candidate's own power-loss behaviour.
- **Tasks.** The v1.11.0 boot-time pending-count task is not started, because the host `xTaskCreate` fails. That count is only a statistic. There is also no concurrency: keeper passes run where the script calls them.
- **Scope.** Only event_log's NVS namespace (`evlog`) is exercised. Other candidate components with their own NVS namespaces (`sd_diag` key `snap`, `ambit_ann`) are outside this proof. v1.11.0's event_log never reads them.
