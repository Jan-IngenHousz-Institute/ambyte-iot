---
title: "Sprint 01 contract: durable SD overflow on the real event store"
kind: spec
comments: none
---

**Status: AGREED (DRAFT v3 accepted by the Evaluator in contract exchange 4 of 4, 2026-09-26).** Contents below are binding for Sprint 01 and are not relaxed during implementation.

## Changes from DRAFT v2 (exchange 3)

| Evaluator point | v3 resolution |
| --- | --- |
| P0: quarantine laundering | The accepted set now has a single obligation: every id in `accepted.jsonl` stays byte-identical and reachable, and is delivered at least once. Every row except Q1–Q3 also asserts `quarantined_ids ∩ accepted_ids == []` (§1.5, H-9). Quarantine is **not** a way to fulfil an ACCEPTED obligation. q1 and q2 are tested only with records that sit outside the accepted set (new group Q). G2 now uses a pre-upgrade malformed fixture. Mutating a line after it was accepted is post-commit corruption, and G8 treats it as a detected-and-reported failure, never as a passing quarantine. |
| P0: rename durability | §1.8 now cites the exact source. The H-4 "durability event" is defined per operation, and a crash can only reach the durable-name outcome once that event has happened. Neither/both outcomes are asserted only before it. The same rule covers the archive rename, the mirror rename, the index-compaction rename and SD `remove`. New invariant INV-1 states that an index line reading `SD_ONLY` never coexists with a state that has no durable SD copy and no flash copy. It is checked after every transform. |
| P1: B1/B2 feasibility | B1 and B2 use an explicit small-record profile P-small (200–600 B) with 16 MiB flash for B2. Both assert that pressure notifications == 0 before each batch boundary. B3 keeps the production-like distribution. |
| P1: automatic wake | New I7: the test starts the production `sync_runner_start`, uses no direct `sync_runner_drain` call, closes and reopens the real gate, and observes the drain firing from its notifier or fallback timer. New I8: the keeper becomes a production task in `event_log` (`event_log_sd_keeper_start`), and the test proves a pressure notification wakes it with no manual service call. |
| P1: truthful operations on both surfaces | The `evlog` CLI text moves into a pure production renderer, `evq_render.c`, which `CLI.c` calls. STATUS goes through the production `payload_v3.c` telemetry builder. New I9 parses the STATUS JSON, asserts the CLI tokens in every listed state, and checks for no truncation at production buffer limits with worst-case values. I4 now also asserts that `NOT_FINISHED` is never mapped to an empty queue, and that the drain does not busy-spin. |
| P1: migration and rollback | H4 now asserts `next_id > max id` across flash, indexed SD and legacy SD, and that a later store never reuses an id. The rollback-floor claim is narrowed to evidence: `rollback_floor.py` hashes the relevant baseline functions for every tag from v1.10.0 to v2.4.2. H4 **executes** both variants found (v2.2.3 and b3f9b8a). The claim covers only the tags whose functions are identical to an executed variant. |
| P1: mirror collisions | H5 pre-seeds collisions for both primary and mirror names. Colliding files keep their hash, and the collision-free names used are recorded in the index. §1.3 adds a global no-overwrite rule. |
| P2: C5 and cleanup | C5 adds the exact preflight gate, compares collected ids between baseline and branch, and requires branch failures ⊆ baseline failures. New suites run separately. Temp directories are exact paths created by the evaluator with `mktemp -d`; no globbing. |
| §1.7 adoption wording | A different CID is adoptable only when no `SD_ONLY` obligation exists on any other CID. Every `SPOOLED` file must be re-spooled from its verified flash copy onto the new card before old references are forgotten, and those files cannot be reclaimed until that happens. |

Baseline: `b3f9b8a` (v2.4.2) in `/tmp/ambyte-sd-overflow`, branch `fix/durable-sd-overflow-20260925`. See also [spec](../../spec), [rubric](../../rubric), [evaluation](../../evaluation).

## Changes from DRAFT v1

| Evaluator exchange-2 point | v2 resolution |
| --- | --- |
| Remove cursor-skipping deferral | The `DEFERRED` state and its hold timer are gone. The cursor never moves past an accepted record unless that record was ACKed (v3: quarantine applies only to non-accepted records, §1.6). When an `SD_ONLY` segment at the cursor cannot be read, delivery waits in place, and acquisition continues only while flash has safe capacity (§1.7). |
| Park mismatched cards | If the card's CID differs from the card that holds `SD_ONLY` obligations, that card is **parked**. Nothing on it is read, written, renamed or deleted, and nothing is imported from it. The state is `sd_state=mismatch`. A different card is adopted only when no `SD_ONLY` obligation exists on any other CID (§1.7). |
| Preserve accepted-record obligations | The "corrupt SD-only data is skipped" class is gone. Each unsent spool is written **twice** on SD: a primary copy plus a mirror. If both copies fail verification, the cursor blocks with `sd_state=backlog_corrupt`, and nothing after it is passed. The only records that may be passed without an ACK are *non-accepted* records (q1/q2) whose intact bytes are first copied to quarantine, with a reason counter (§1.6, tightened in v3). Existing cursor skip paths now apply to indexed files (§1.6). |
| Strengthen rename durability | Commit sequence (§1.8): the `.tmp` is fsynced, renamed, then the *new name* is reopened and its full CRC verified before any index line or reclaim. The power model now also produces "rename left both names" and "rename left neither name", and boot repair is tested against each outcome (B4, D). The archive rename of delivered files follows the same rule. |
| Runtime operational / integration proof | New §2.2 integration harness, compiled on the host from real `device_commands.c` + `sync_runner.c` + `event_log.c`. It drives the real `cmd_mqtt_publish_next_event`, the ACK completion queue, refusal hold, disconnect abort, `sync_runner_drain` and the watchdog predicate, and renders the STATUS metadata. New matrix group **I**. |
| Rollback clamp / foreign cursor movement (made precise) | A foreign-cursor detection rule replaces the old v1 heuristic (§1.8). Whenever a file's obligation leaves the queue without this firmware's ACK, it is **re-imported**. The cursor is never trusted as proof of delivery. |

## 1. Mechanism

### 1.1 One queue, two media, one cursor

The delivery queue is still the seq-ordered `ev-<seq>.log` sequence, and the durable cursor `(rd_seq, rd_off)` means the same thing it means today. What changes is where a file can live: flash, SD, or both. Files are copied to SD **byte for byte**, so an offset is valid on either medium.

`claim` resolves each file in this order:

1. the flash copy;
2. otherwise the verified SD primary;
3. otherwise the verified SD mirror.

SD access always happens between `sdcard_io_begin` and `sdcard_io_end`, with lock order `s_mtx` → SD ref. No blocking lock is taken inside the bracket. Because the drain reads SD directly, draining an SD backlog needs no free flash.

### 1.2 Segment index

The segment index lives at `/evstore/evq.idx` on internal littlefs.

- It is append-only, one text line per state transition, and every line ends in a CRC32.
- Compaction writes a `.tmp` file and then does a littlefs `rename`, which is atomic.
- For each rotated file it records: `seq`, `first_id`, `last_id`, record count, bytes, whole-file CRC32, SD primary and mirror names, and CID.

The states are:

- `FLASH` — rotated; CRC recorded.
- `SPOOLED` — primary and mirror committed and verified; flash copy kept.
- `SD_ONLY` — flash copy reclaimed.
- `DELIVERED` — this firmware's own ACK prefix has passed EOF, and the cursor was persisted first.
- `ARCHIVED` — SD primary renamed to `/sdcard/archive/arc-<first_id>[-<seq>].log`, mirror removed; or, for a never-spooled delivered file, copied, verified, and the flash copy deleted.
- `REIMPORT` — the obligation left the queue without our ACK: a rollback clamp, a foreign cursor move, or a lost index. The file is re-appended, verified, from the recorded offset into the flash tail, and only then is its SD copy removed.

### 1.3 Placement and rollback compatibility

- **Unsent primary:** `/sdcard/events/ev-<first_id>.log`. Released firmware since v1.10.0 imports this directory verbatim and oldest-first, so after a rollback the old firmware can recover `SD_ONLY` data. The *proven* scope is limited to the tags marked `proven` in `rollback_floor.json` (below).
- **Mirror:** `/sdcard/evq/m-<seq>.log`, which old firmware never reads.
- Neither is a committed copy while it is still named `.tmp`.
- Delivered files leave `/sdcard/events`, so a rollback does not bulk re-import them.
- Rollback to firmware older than v1.10.0 is unsupported, as it already is.
- **The import behaviour is proven only where it is evidenced.** `tests/evq_host/rollback_floor.py` hashes six pieces of each release tag from v1.10.0 to v2.4.2:
  - `event_log_import_sd_backlog`
  - `evlog_append_verbatim_locked`
  - `parse_ev_name`
  - the open-time cursor clamp
  - the missing-file claim path
  - the keeper call site
Preflight found two variants: v1.10.0–v2.2.3 and v2.3.0–v2.4.2 (the bench runs v2.3.3-rc.1). H4 executes one representative of each, v2.2.3 and b3f9b8a. The rollback claim covers only tags whose hashes equal an executed variant. Any other tag is listed as "not proven".
- **No-overwrite rule** for every SD name we create: primary, mirror, archive and tmp.
  - Before creating a file we stat under the SD bracket, and the create itself uses `"wx"` (exclusive).
  - FatFs `f_rename` returns `FR_EXIST` when the target exists (`ff.c` f_rename, `follow_path(&djn…) → FR_EXIST`), so a rename can never replace a file.
  - When a name is taken, a numbered suffix is chosen and recorded in the index. Mirrors use `m-<seq>[-<k>].log`. Primaries fall back to the first free `ev-<3000000000+seq+k>.log`. That name still parses as `ev-<digits>.log` for rollback import and stays inside the `uint32` parse range. It also sits above the measure-id range, so it cannot collide with a normal primary.

### 1.4 Transfer triggers

**Batch trigger.** Every `EVLOG_ARCHIVE_EVERY_N` stores (1000, unchanged), each rotated file not yet on SD is handled in seq order:

- a delivered file is archived (copied and verified before the flash delete);
- an unsent file is spooled as primary + mirror, and its flash copy is kept.

**Pressure trigger.** When flash free space falls below `EVQ_PRESSURE_FREE` (25 %), the store wakes the keeper immediately, without waiting for the next 1000 stores. The keeper spools, then reclaims verified `SPOOLED` files oldest-first until free space reaches `EVQ_RECLAIM_TARGET` (40 %).

**Never reclaimed:**

- the tail;
- any file without a verified primary **and** a verified mirror;
- any file on a parked or mismatched card.

The keeper becomes a production task inside `event_log`. `event_log_sd_keeper_start()` creates it, and it runs `event_log_sd_service()`:

- once per 60 s period;
- immediately on a task notification. The store sends one when it crosses the pressure watermark or reaches the batch count, and the SD monitor sends one when a card is mounted or unparked.

`app_main` only calls start. It no longer contains the keeper loop.

### 1.5 Accepted, refused, delivered

| Store result | Meaning | Obligation |
| --- | --- | --- |
| `ESP_OK` | **ACCEPTED**: the full line was fsynced to littlefs before the call returned. Today up to 8 records / 1.5 s are acknowledged before fsync. | Delivered at least once**, byte-identical, with its original id and capture times.** Quarantine never satisfies this obligation. |
| `ESP_ERR_NO_MEM` | Refused: no safe capacity. Counted in `refused_full`. | none; never delivered |
| `ESP_FAIL` | Refused: media error. Counted in `refused_media`. | none |
| `ESP_ERR_INVALID_SIZE` | Refused: too large. Counted in `refused_too_large`. | none |
| `NOT_SUPPORTED` / `INVALID_STATE` / `TIMEOUT` | Refused: store unavailable. Counted in `refused_unavailable`. | none |
| Crash inside the store call | INDETERMINATE | If delivered, it must be complete and byte-identical. |

Blocked **state.** `storage_blocked=true` is reported together with a `blocked_reason`, one of:

- `flash_full_sd_unavailable`
- `flash_full_sd_full`
- `flash_full_sd_error`
- `flash_full_sd_mismatch`
- `flash_full_backlog_waiting` (the cursor is waiting on an unreadable `SD_ONLY` segment, so the drain cannot free flash)
- `flash_full_index_cap`

**Delivered** means a PUBACK with reason < 0x80 followed by `mark_event_synced`. A reason-coded PUBACK or a disconnect leads to `mark_event_pending`. The cursor advances only over the contiguous ACKed prefix.

### 1.6 Records the cursor may pass without an ACK

The cursor may move past a record without a PUBACK in exactly two cases. In both, an intact copy of the bytes is first written to `/evstore/events/quarantine.log` and fsynced, and only then does the cursor move. **Neither case fulfils an ACCEPTED obligation.** Both exist only for records outside the accepted set.

- **(q1) Publish-impossible poison.** These are the existing oversize and OOM-stuck paths. They are tripped only by records that HEAD's publisher can never send: pre-upgrade fixtures, or an injected allocation failure on a purpose-built record. The cap chain guarantees that every record HEAD accepts is publishable. Counters: `quarantined_poison` and the existing `oversize_skipped`.
- **(q2) Malformed or over-long lines.** HEAD's store always writes framed 9-field lines, so a malformed line can only be pre-upgrade residue or post-commit corruption. The raw bytes are preserved and counted in `quarantined_malformed`. Today such a line is skipped without being copied; that is fixed.

**Post-commit corruption of an accepted line is a detected failure, not a pass.** For rotated files, the whole-file CRC is checked the first time a file is claimed. On a mismatch, `claim` falls back to the verified SD primary or mirror, so no loss occurs. If no verified copy exists, the cursor waits with `sd_state=backlog_corrupt` and `corrupt_medium=flash`. In the tail file, where no file CRC exists yet, a malformed line goes through q2 so that its bytes are preserved. It is also counted in `corrupt_detected`, so it is never silent. G8 asserts that this report is exact, and it never counts the id as delivered.

For any file the index knows about, these are **never** skipped:

- a missing file at the cursor;
- a torn rotated tail at the cursor;
- an unreadable SD file.

In each of these cases the cursor waits, with explicit state. The existing counted skip survives only for a missing file that has no index entry and predates the upgrade. That path is logged and counted as `skipped_unindexed_gap`.

### 1.7 Unreadable, missing, or mismatched SD while the cursor is in `SD_ONLY`

This covers a card that is absent, lost, or parked for low battery; a file that is missing; and a CRC or size mismatch on the primary.

1. The primary fails, so `claim` tries the mirror.
2. If both fail, `claim` returns the new code `ESP_ERR_NOT_FINISHED`, never `NOT_FOUND`. The cursor, NVS and index stay unchanged. `pending` still counts those records. `deliverable_pending` excludes them, and that is the figure the no-PUBACK watchdog now reads.
3. `sd_state` reports one of `absent`, `lost`, `parked`, `backlog_missing` or `backlog_corrupt`.

While the cursor waits, `store` keeps accepting into flash while safe capacity remains. After that it refuses with `flash_full_backlog_waiting`. The recovery service keeps running: it polls the card and re-verifies. As soon as a verified copy is readable, the drain resumes at the same cursor. Delivery order stays the original order in every case.

**Mismatched CID.** If a card's CID is not the CID that holds `SD_ONLY` or `SPOOLED` obligations, that card is parked: no reads, writes, renames, deletes or imports on it. The state is `sd_state=mismatch`.

If no `SD_ONLY` obligation exists on any other CID, the new card is adopted instead. This is only safe because every remaining `SPOOLED` file still has a verified flash copy. Any `SD_ONLY` obligation on another CID keeps the new card parked.

Adoption proceeds in this order:

1. Each `SPOOLED` file is re-spooled from its verified flash copy onto the new card, as primary + mirror, and verified.
2. Its index entry is rewritten to the new CID.
3. Only then are the old CID references for that file forgotten.

Until step 2 completes for a file, that file cannot be reclaimed. The old card's archive references are metadata only, so forgetting them loses no data.

**Documented operational consequence.** A permanently dead card that holds `SD_ONLY` data halts delivery of every later record. After flash fills, it also blocks acquisition, with an explicit reason, until a person acts. Sprint 01 adds no automatic or operator path that forfeits those records. Exposure is kept small because flash copies are reclaimed only under pressure, so `SD_ONLY` exists only after an outage longer than flash capacity, and it is written twice.

### 1.8 Write ordering, rename durability, and boot repair

**Spool.** Each file goes through these steps in order:

1. write primary `.tmp`, fsync, close;
2. rename it to `.log`;
3. reopen it by the **new** name, stat the size, and read back and compare the full CRC plus a structural scan (count, first and last id, every line framed);
4. repeat steps 1–3 for the mirror;
5. write the `SPOOLED` index line and fsync;
6. `remove` the flash copy;
7. write the `SD_ONLY` index line.

**Durability events.** These are defined mechanically, from source.

Source cited is the ESP-IDF 5.5.3 package at `/home/dv/.platformio/packages/framework-espidf`, the one this project builds with:

- **FatFs rename.** `components/fatfs/vfs/vfs_fat.c`, function `vfs_fat_rename`, around line 873, calls `f_rename` and returns 0 only on `FR_OK`. `components/fatfs/src/ff.c`, function `f_rename`, starting around line 5181, runs `dir_register(&djn)`, then `dir_remove(&djo)`, then `res = sync_fs(fs)` before `LEAVE_FF`.
- **sync_fs.** `ff.c`, function `sync_fs`, around line 1113, performs `sync_window` (writing the dirty directory/FAT sector) plus the FSInfo update, then `disk_ioctl(CTRL_SYNC)`.
- **Unlink and mkdir.** `f_unlink` and `f_mkdir` also end in `sync_fs`.
- **CTRL_SYNC.** `components/fatfs/diskio/diskio_sdmmc.c`, around line 96, answers `CTRL_SYNC` with `RES_OK` directly, because sdmmc sector writes are synchronous: each `disk_write` completes before returning.

Therefore a **successful return** of `rename()`, `remove()` or `mkdir()` on SD is the **directory-durable event**. Likewise, a successful `fsync()` or `fclose()` of a file is its content-durable event. On littlefs, `rename` is atomic, and it is durable once it returns. The residual risk is a card's internal FTL write cache, which Sprint 01 cannot model. It is stated in §5.

**H-4 applies these events.**

| Crash timing | Allowed outcomes |
| --- | --- |
| Before the durable event (inside the call, or before it) | Any outcome may be produced: {not applied, applied, both names} for rename, {applied, not} for remove. |
| After the durable event returned | Only the applied outcome. |

The `SPOOLED` index line is written only after both renames **and** both verifications have returned. So by the time the flash `remove` can happen, both SD names are durable.

**INV-1** is checked by the oracle after every step and every transform. For every index entry, its effective state (after boot repair) that says `SD_ONLY` must have at least one durable, verifying SD copy on its CID, or it must still have its flash copy. The same kind of invariant holds for `ARCHIVED`: it needs a durable verifying archive, or the file was delivered and its obligation is already met. It also holds for compaction: the compacted index must describe the same states as the log it replaces.

**Delivery.** When the ACK prefix passes EOF:

1. commit the NVS cursor (a blob plus the legacy keys);
2. write the `DELIVERED` index line;
3. rename to the archive, then reopen and verify under the new name;
4. remove the mirror;
5. write the `ARCHIVED` line.

**Cursor persistence.** The primary record is the NVS blob `cur = {seq, off, crc}`. The legacy keys `rd_seq`/`rd_off` are written after it, for rollback.

Boot repair reads the cursor as follows:

| Found at boot | Action |
| --- | --- |
| Legacy keys and blob agree | Use them. |
| Legacy value is behind the blob | Our own write was torn. Use the legacy (earlier) value; this can only cause duplicates. |
| Legacy value is ahead of the blob | Another firmware moved the cursor. Adopt the legacy value, and mark every `SPOOLED`/`SD_ONLY` file in `[blob, legacy)` as `REIMPORT`. |
| An offset that is not on a record boundary | Fall back to offset 0 of that file. |

Boot repair of files:

| Found at boot | Action |
| --- | --- |
| `.tmp` files | Deleted. They are never the only copy. |
| **Both** a `.tmp` and a `.log` exist (the rename left both names) | Keep the `.log` if it verifies, else delete both. |
| **Neither** exists | Re-spool; the flash copy is still present. |
| An unindexed `ev-<first_id>.log` whose CRC matches a flash segment | Adopt it. |
| Any other unindexed `ev-*.log` | Legacy import, as today. |
| `DELIVERED` with `seq ≥ cursor` | Downgrade. |
| `SD_ONLY` and the flash copy still exists | Treat as `SPOOLED`. |

The cursor alone is never taken as proof that a spooled file was delivered.

### 1.9 Truthful counters

`pending` is exact, computed from the index counts, one bounded scan of the cursor file, and the tail counter. The old 20,000-line / 3 s scan applies only to unindexed files, which only exist on the first boot after upgrade. While those exist, `pending_exact=false` and `pending` is a floor.

`evlog_health_t`, STATUS metadata and `evlog` all report the following fields.

Pending:

- `pending_exact`
- `deliverable_pending`
- `flash_pending`
- `sd_pending`
- `reimport_pending`

SD state:

- `sd_state`
- `spool_files`
- `spool_errors`
- `mirror_used`

Blocked state:

- `storage_blocked`
- `blocked_reason`

Refusals:

- `refused_full`
- `refused_media`
- `refused_too_large`
- `refused_unavailable`

Quarantine and skips:

- `quarantined_poison`
- `quarantined_malformed`
- `skipped_unindexed_gap`
- `corrupt_detected`
- `corrupt_medium`

**Both surfaces use production renderers.**

- **STATUS.** Emitted through the production `payload_v3.c` telemetry builder. Its `json_writer_t` fails closed on overflow. New fields are added to `payload_v3` input.
- **`evlog` CLI.** Rendered by a new pure production function `evq_render_health_text(const evlog_health_t *, char *buf, size_t cap)` in `components/event_log/evq_render.c`. It returns `-1` rather than truncating, and `CLI.c` prints its output.

### 1.10 Code scope

Changes are limited to:

- `components/event_log/` (new `evq_index.c`, `evq_render.c`; spool, keeper task, service and repair; header)
- `main/app_main.c` (keeper start and health wiring only)
- `components/sync_runner/sync_runner.c` (watchdog input, quiet handling of `NOT_FINISHED`)
- `components/device_commands/` (health into STATUS input)
- `components/payload_codec/payload_v3.c` and its header (STATUS metadata fields)
- `components/CLI/CLI.c` (`evlog` display through `evq_render`)
- `tests/`
- `docs/`

The host seams are:

- `#ifndef` guards on the mount prefixes and on the tuning constants;
- an `EVQ_FAULT_POINT(name)` hook that compiles to nothing unless `EVQ_HOST_FAULTS` is defined.

Production values of all of these are unchanged, which J3 checks.

## 2. Harnesses: real production C on the host

### 2.1 Storage harness (`tests/evq_host/`)

**H-1 — what it compiles.**

- `event_log.c` and `evq_index.c` at HEAD, built with clang `-fsanitize=address,undefined -fno-sanitize-recover=all` and `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1`.
- For the repro, fixture and rollback scenarios: `git show <rev>:components/event_log/event_log.c` for `<rev>` ∈ {`b3f9b8a`, `v2.2.3`}, with only its path literals rewritten by a mechanical `sed`. The rewrite is printed into the evidence.
  - If `v2.2.3` does not compile against the stubs, the compiler output goes into the evidence, and the rollback claim is narrowed to the `b3f9b8a` variant. It is never widened without proof.

**H-2 — stubs.**

- FreeRTOS: the mutex is a pthread mutex, ticks come from a test clock, and the count task runs synchronously.
- `esp_littlefs_info`.
- NVS: a key/value map persisted by atomic rename on `nvs_commit`. Keys set but not committed are lost on a crash.
- `sd_card.h`: mounted state, CID, the `io_begin`/`io_end` refcount (asserted to be 1:1, and asserted that no `s_mtx` acquisition happens while a ref is held), `report_io_*` and `free_bytes`.
- `heap_caps` and `esp_log`, the latter written to stderr.

**H-3 — media shim.** A forced `-include` interposes stdio, POSIX and dirent calls. The shim:

- classifies each path as flash or SD;
- enforces capacity with 4 KiB block rounding and returns `ENOSPC` short writes when exceeded;
- injects `EIO` on any operation;
- simulates removal;
- tracks each file's durable size;
- logs every operation to `ops.jsonl`, which the ordering assertions read.

**H-4 — power loss.** A crash is `_exit(86)` at a named fault point inside a forked child. The parent then applies a power transform:

- **Flash **reverts to each file's last-sync size.
- **SD **is adversarial:
  - each file written since its last sync is cut to a seeded length between its synced and current size, sometimes with garbage appended;
  - a rename whose call had **not returned successfully** by the crash resolves to one of {not applied, applied, both names};
  - an equivalent remove or mkdir resolves to {applied, not applied};
  - a directory operation that returned successfully is **durable** (§1.8 source evidence). Only its applied outcome is produced.
- **Flash directory operations.** A littlefs rename, remove or mkdir is atomic. Before it returns it resolves to {old, new}; once it has returned, only new.
- **NVS** keeps only committed state.

The shim records each call's **durability event**, meaning its successful return, in `ops.jsonl`. The transform uses only that record, so which outcomes are allowed is machine-checkable.

The parent then reboots: a new child runs `event_log_init` against the same state.

**H-5 — fault points**.

- `run.py --list-faults` enumerates every registered fault point.
- `EVQ_FAULT=<point>:<nth>:<crash|eio|enospc>` arms one of them.
- A registered point that the matrix never reaches **fails the run**.

**H-6 — persistence-port driver.** The storage matrix exercises every port operation directly (`claim`, `mark_synced`, `mark_pending`, `quarantine`, `health`), with the same 16-slot / 64 KiB window and PUBACK semantics as the real publisher.

**H-7 — seeded records.**

| Share | Record size |
| --- | --- |
| 70 % | 200 B – 2 KiB |
| 25 % | 2 – 16 KiB |
| 5 % | 60 KiB up to the binding 65,191 B |

- Capture times are monotonic with jitter.
- `cmd_raw` is up to 540 B.
- Metadata is sometimes empty.
- Every payload carries `synthetic_test:true` plus the scenario and seed.
- Seeds are `1`, `7` and `1337`.
- **Profile P-small**, used only by B1 and B2: every record is 200–600 B, with the same fields as above.

**H-8 — manifests** in `$EVQ_OUT/<scenario>/<seed>/`:

- `accepted.jsonl`: id, channel, device, tag, capture times, sha256 of `cmd_raw`, metadata and payload, sha256 of the full line, and the store-call index.
- `refused.jsonl`
- `indeterminate.jsonl`
- `delivered.jsonl`
- `quarantined.jsonl`
- `reconcile.json`, with `missing_ids == []`, `mismatched_ids == []`, `refused_delivered == []`, `duplicate_count`, and the final health snapshot.
- `media_before.json` / `media_after.json`: sha256 of every file.
- `ops.jsonl`

**H-9 — oracles.** Both are independent Python code.

- **R-DUR** runs after every step and every power transform. Every accepted id must have a complete, byte-identical line in a location that the documented states make reachable **by the queue**: flash, or a durable SD copy on its CID. Quarantine does **not** count as a reachable location. INV-1 (§1.8) is checked at the same time.
- **R-E2E** runs after recovery. It restores delivery and drains until `pending == 0 && reimport_pending == 0`. The pass condition is:
  - every accepted id delivered at least once, with all hashes equal;
  - `missing_ids == []`;
  - `quarantined_ids ∩ accepted_ids == []`.
There is no quarantine alternative. This is global: every row except Q1–Q3 inherits it, including fault, capacity and rollback rows. G8 is a reported-failure row with its own assertions.

### 2.2 Integration harness (`tests/evq_integ/`), the runtime operational proof

The integration harness compiles the **real** `components/device_commands/device_commands.c`, `components/sync_runner/sync_runner.c` (included as a translation unit so that the static `sync_runner_drain` and the watchdog predicate are reachable) and `event_log.c` + `evq_index.c`. It links them against the H-2/H-3 stubs plus header-level stubs for wifi, ledc, pm, `esp_timer`, `app_desc`, `ota_update`, `clock_trust` and `fleet_jitter`. Those stubs provide API surface only; none of them implements publisher logic.

Messaging runs through `device_commands`' own `messaging_port` config:

- `publish` returns msg ids and records the published envelope;
- PUBACK and disconnect are delivered through the same callbacks the MQTT task calls;
- `sync_runner_drain` consumes the completion queue.

The FreeRTOS layer for this harness is a pthread-backed task and notification shim covering `xTaskCreate`, `xTaskNotifyGive`, `ulTaskNotifyTake`/`xTaskNotifyWait`, `vTaskDelay`, queues and portMUX. The production tasks from `sync_runner_start` and `event_log_sd_keeper_start` therefore run as real threads.

The harness also compiles the production `payload_v3.c` STATUS builder and `evq_render.c`.

Group-I results must repeat identically across 3 runs. Byte-identical determinism (J5) applies only to the single-threaded storage matrix.

If one of those dependencies proves impossible to stub without duplicating logic, the Generator reports that item with its compiler errors before claiming group I. The group is not quietly dropped.

## 3. Commands the Evaluator runs

All commands run from `/tmp/ambyte-sd-overflow`.

```sh
# Evaluator-owned exact temp paths (removed exactly at the end — no globs)
E1=$(mktemp -d /tmp/evq-ev1.XXXXXX); E2=$(mktemp -d /tmp/evq-ev2.XXXXXX)
BW=$(mktemp -d /tmp/evq-basewt.XXXXXX); rmdir "$BW"; RS=$(mktemp -d /tmp/evq-suites.XXXXXX)

# C0 preflight regression gate (exact)
python -m unittest discover -s tests -p 'test_evlog_inventory.py' -v
# C1 storage + integration suites (sanitized; all scenarios; seeds 1,7,1337; EVQ_TMPDIR confines harness scratch)
CC=clang EVQ_OUT="$E1" EVQ_TMPDIR="$E1/tmp" python -m unittest discover -s tests -p 'test_evq_*.py' -v
# C2 baseline reproduction (b3f9b8a event_log.c) — must REPRODUCE loss
CC=clang EVQ_OUT="$E1" EVQ_TMPDIR="$E1/tmp" python -m unittest tests.test_evq_baseline_repro -v
# C3 fault-point inventory + coverage; rollback-floor evidence
CC=clang python tests/evq_host/run.py --list-faults
CC=clang EVQ_OUT="$E1" EVQ_TMPDIR="$E1/tmp" python tests/evq_host/run.py --matrix --seeds 1,7,1337 --coverage-report
python tests/evq_host/rollback_floor.py --out "$E1/rollback_floor.json"
# C4 determinism across clean reruns (storage matrix)
CC=clang EVQ_OUT="$E2" EVQ_TMPDIR="$E2/tmp" python tests/evq_host/run.py --matrix --seeds 1,7,1337
python tests/evq_host/compare_evidence.py "$E1" "$E2"
# C5 pre-existing suite: identical shared collection; branch failures ⊆ baseline failures (new suites excluded)
git worktree add "$BW" b3f9b8a
(cd "$BW" && git submodule update --init components/littlefs)
(cd "$BW" && CC=gcc python -m pytest tests -p no:cacheprovider --collect-only -q) > "$RS/base-collect.txt"
CC=gcc python -m pytest tests -p no:cacheprovider --collect-only -q --ignore-glob='tests/test_evq_*' > "$RS/branch-collect.txt"
(cd "$BW" && CC=gcc python -m pytest tests -p no:cacheprovider -q --junitxml="$RS/base.xml") || true
CC=gcc python -m pytest tests -p no:cacheprovider -q --ignore-glob='tests/test_evq_*' --junitxml="$RS/branch.xml" || true
python tests/evq_host/compare_suites.py "$RS/base-collect.txt" "$RS/branch-collect.txt" "$RS/base.xml" "$RS/branch.xml"
# C6 production firmware
AMBYTE_NVS_SKIP=1 AMBYTE_PROJECT_VER=2.4.3-sprint01 /home/dv/.platformio/penv/bin/pio run -e esp32-s3-devkitm-1
# C7 scope + constants
git diff --stat b3f9b8a -- . ':!tests' ':!docs'
python tests/evq_host/check_constants.py

# Cleanup — exact paths only
git worktree remove --force "$BW"; rm -rf -- "$E1" "$E2" "$RS"
```

**`compare_suites.py`** fails if any of the following is true:

1. the pre-existing test-id collections differ, apart from test files added on the branch;
2. any branch failure or error id is not also a failure or error on baseline;
3. any pre-existing id is missing from the branch run.

The evaluator should also inspect the raw XML directly.

**Harness scratch space.** Every harness writes only inside `EVQ_TMPDIR`, which is `mkdtemp` beneath the evaluator's exact directory. On success it removes its own subdirectory. On failure it keeps that subdirectory and prints its path. Nothing touches USB, serial, AWS, GitHub or the fleet.

## 4. Assertion matrix

**Default parameters**:

- flash 2 MiB with a 64 KiB rotation;
- SD 256 MiB with an 8 MiB reserve;
- a batch of 1000 records.

**Full-scale** rows use production constants: 9,437,184 B, 256 KiB rotation, a batch of 1000, and 25 % / 40 % watermarks.

**Oracle shorthand:** R-DUR and R-E2E are defined in H-9. Every row requires:

- `mismatched_ids == []`;
- `refused_delivered == []`;
- `quarantined_ids ∩ accepted_ids == []`.

The only exceptions are Q1–Q3, whose records lie outside the accepted set by construction, and G8, a reported-failure row with its own assertions.

### A. Reproduction and headline fix

| ID | Scenario | Assertions |
| --- | --- | --- |
| A1 | **Baseline b3f9b8a**. Delivery unavailable; SD mounted with free space; baseline keeper doing import and archive every 1000 stores; 3× flash volume. | The test **passes only if loss reproduces**: `NO_MEM` refusals > 0; accepted < generated; 0 unsent records on SD; `dropped == refusals`. Exact numbers go into the evidence. |
| A2 | A1 on HEAD. | `refused == 0` while SD free > reserve. Flash usage ≤ cap at every step. `sd_pending > 0`. After delivery, R-E2E; delivered order strictly increasing, no gaps. |
| A3 | Full scale, ≥ 2.5 × 9 MiB. | Everything in A2. In addition, `ops.jsonl` shows that for every reclaimed file, the `SPOOLED` line and both verified SD copies precede the flash `remove`. |

### B. Batching and transfer

| ID | Scenario | Assertions |
| --- | --- | --- |
| B1 | Good delivery, 5000 **P-small** records, 2 MiB flash. | Pressure notifications stay at 0 for the whole run; the evidence prints the counter. SD bursts happen only on the batch trigger: ⌈5000/1000⌉ ± 1. Archived files are byte-identical to their flash source, checked before the delete. A pre-seeded `arc-<id>.log` keeps its hash and is never overwritten. |
| B2 | Outage, 5000 **P-small** records, **16 MiB** flash (pressure at < 4 MiB free). | Pressure notifications are 0 before each batch boundary. At each batch trigger, primary and mirror appear. The flash copy is kept (no reclaim). The drain does no SD reads. |
| B3 | Pressure before 1000 stores, H-7 production-like distribution, default flash. | At least one pressure notification fires before store #1000. Transfer and reclaim start early and go oldest-first. Free space ≥ target at the end. The tail and any unverified file are never removed. |
| B4 | Rename durability. For every rename site (primary, mirror, archive, index compaction), crash **before** its durable event, then crash **after** it. | Before: each of {not applied, applied, both names} is produced at least once across seeds and is repaired correctly, with no double adoption and no import of a half copy. After: only the applied name exists, and the transform never erases it. No reader sees a `.log` before fsync, rename and verify-under-new-name. A `.tmp` is never the only copy. INV-1 holds at every point. |
| B5 | Store durability. | `ops.jsonl` shows an fsync covering every accepted record's bytes before `ESP_OK`. A crash right after `ESP_OK` loses nothing. |

### C. Capacity and accounting

| ID | Scenario | Assertions |
| --- | --- | --- |
| C1 | Delivery unavailable up to SD capacity minus reserve. | 0 refusals until SD free < reserve. Then spooling stops with `sd_state=full`; flash fills; `NO_MEM` with `storage_blocked` and `flash_full_sd_full`. `refused_full` equals the number of `NO_MEM` returns. R-DUR. |
| C2 | C1, then SD space is freed. | Within one service: spooling resumes, blocked clears, and the next store returns `ESP_OK`. R-E2E. |
| C3 | SD absent from boot, then inserted. | Flash fills, then `flash_full_sd_unavailable`. Delivery of flash data works throughout and frees space. After insertion, spooling resumes. R-E2E. |
| C4 | SD removed mid-spool, then reinserted. | No reclaim without two verified copies. The partial copy is never adopted. `spool_errors` increases. R-DUR and R-E2E. |
| C5 | `EIO` on write, fsync, rename and read-back, one run each. | Same as C4. The keeper backs off exponentially (test clock). |
| C6 | Flash full + SD full, then delivery restored. | Claims and ACKs work while blocked. After archive or eviction, acquisition resumes. R-E2E. |
| C7 | Index cap (host override). | `flash_full_index_cap` once flash is full. Existing data unaffected. |
| C8 | Counter truth, all scenarios. | Wherever `pending_exact=true`, each of `pending`, `deliverable_pending`, `sd_pending`, `flash_pending`, `reimport_pending`, `refused_*`, `quarantined_*` and `corrupt_detected` equals the oracle's truth. |
| C9 | More than 20,000 pending records in unindexed files. | `pending_exact=false` and `pending` ≤ truth until indexed. Afterwards exact and equal to truth. |

### D. Crash and error at every persistence boundary

Every registered point is run as `crash` (with the H-4 transform, then a reboot), plus `eio`/`enospc` where applicable, at nth ∈ {1, 2, last}. Each run must satisfy R-DUR, INV-1 and R-E2E, including `quarantined_ids ∩ accepted_ids == []`. Each run reports its duplicates and keeps them within a bound:

- ≤ 16 + one file's record count per crash;
- the `reimport.*` and rollback rows only report.

**Minimum fault-point set.** More points may be added; none may be dropped.

| Area | Points |
| --- | --- |
| Store | `store.before_write`, `store.after_write_before_fsync`, `store.after_fsync_before_return`, `store.rotate_after_close_before_open`, `store.rotate_after_index_seg` |
| Spool (primary) | `spool.after_tmp_open`, `spool.mid_copy`, `spool.before_tmp_fsync`, `spool.after_tmp_fsync_before_rename`, `spool.after_rename_before_verify`, `spool.verify_read_error` |
| Mirror | `mirror.mid_copy`, `mirror.after_rename_before_verify` |
| Spool (index) | `spool.after_verify_before_index`, `spool.after_index_before_reclaim` |
| Reclaim | `reclaim.after_remove_before_index`, `reclaim.after_index` |
| ACK and delivery | `ack.after_ram_prefix_before_nvs`, `cursor.after_blob_before_legacy`, `cursor.after_nvs_before_index_delivered` |
| Archive | `delivered.after_index_before_archive_rename`, `archive.after_rename_before_verify`, `archive.after_verify_before_mirror_remove`, `archive.after_mirror_remove_before_index` |
| Reimport | `reimport.mid_append`, `reimport.after_fsync_before_sd_remove`, `reimport.after_sd_remove_before_index` |
| Legacy import | `import.mid_append`, `import.after_fsync_before_sd_remove` |
| Index | `index.append_torn`, `compact.after_tmp_before_rename`, `compact.inside_rename`, `compact.after_rename` |
| Durable-event boundaries | `rename.inside_call` and `rename.after_return` at every SD and flash rename site; `remove.inside_call` for flash reclaim and SD removals |
| Boot | `boot.index_rebuild_mid`, `boot.adopt_mid` |
| Claim and quarantine | `claim.sd_read_error`, `claim.sd_short_read`, `quarantine.after_copy_before_cursor` |

Group-wide assertions:

- The committed NVS cursor never lies beyond a record that was never ACKed. This is cross-checked against the delivered log. In the D matrix, quarantine never moves the cursor past an accepted record.
- No `.log` file on either medium ever exposes a torn or merged record to `claim`.

### E. Delivery and ACK ordering (storage harness)

| ID | Scenario | Assertions |
| --- | --- | --- |
| E1 | SD`_ONLY` → `SPOOLED` → flash tail, full drain. | The first-delivery sequence equals the accepted sequence exactly. SD reads happen only for `SD_ONLY` files. |
| E2 | PUBACK 0x87 on 10 % of records. | Refused records are re-claimed FIFO. The cursor never crosses a refused id before it is re-ACKed. R-E2E. |
| E3 | Disconnect mid-window. | Reverted slots are re-claimed first. No loss. |
| E4 | Out-of-order ACKs, then a crash. | The NVS cursor is at or before the contiguous prefix. Redelivery starts from that prefix. |
| E5 | Stale, duplicate and unknown ACK or `pending`. | `INVALID_STATE`. Cursor, index and counters are byte-identical before and after. |
| E6 | Crash after publish, before ACK. | The record is redelivered. |
| E7 | Crash after ACK, before the batched NVS write. | Duplicates ≤ 16. |
| E8 | Crash between the cursor commit and the `DELIVERED` line. | Boot repair treats the file as delivered **only** if the blob proves our own prefix passed EOF. Otherwise it is `REIMPORT`. Never archived while undelivered. |
| E9 | Upload gate closes mid-drain. | No claims while the gate is closed. The window reverts cleanly. |
| E10 | Boundary crossings SD→SD, SD→flash, flash→SD, including a reclaim race. | Within one boot, nothing is skipped and nothing is duplicated. |

### F. SD missing, mismatched or unreadable while the cursor is in SD_ONLY

| ID | Scenario | Assertions |
| --- | --- | --- |
| F1 | Card removed. | `claim` returns `NOT_FINISHED`, never `NOT_FOUND`. NVS cursor bytes and the index are unchanged, including over simulated days on the test clock. `pending` includes the SD records; `deliverable_pending` excludes them. The watchdog predicate is false. `sd_state=absent`, then `lost`. |
| F2 | F1 with the card left out until flash is full. | Stores return `NO_MEM` with `flash_full_backlog_waiting`, and `refused_full` is exact. Accepted-before-block records survive (R-DUR). The recovery service keeps polling. |
| F3 | F2, then the same card is reinserted. | The drain resumes at the same cursor. The delivered sequence is exactly the original order. Blocked clears. R-E2E. |
| F4 | A card with a different CID and free space. | Parked: the shim sees **zero** operations on that card (no read, write, rename, delete or import). `sd_state=mismatch`. `blocked_reason=flash_full_sd_mismatch` once flash is full. When the original card returns, the result is as in F3. |
| F5 | A new card while no `SD_ONLY` obligation exists. | Adopted. `SPOOLED` files are re-spooled onto it. The old card's data is never referenced as a copy again. |
| F6 | Primary missing, truncated or failing CRC on the correct card. | The mirror is used; `mirror_used` increases; the delivered order is original; R-E2E. The damaged primary is left for inspection (hash recorded), not rewritten. |
| F7 | Both copies damaged. | `NOT_FINISHED` with `sd_state=backlog_corrupt`. The cursor never passes the file. No later record is delivered. Nothing is counted as delivered. The files are unchanged. Restoring an intact copy (test action) resumes delivery with R-E2E. |
| F8 | Low-battery park (the `sd_logger_pause` + unmount path through the stubs) while the cursor is in `SD_ONLY`. | As F1 with `sd_state=parked`. No SD operation happens during the park. Everything resumes after the unpark. |

### G. Corruption and torn state

| ID | Scenario | Assertions |
| --- | --- | --- |
| G1 | Torn flash tail at boot. | Truncated to the last newline. No accepted (fsynced) record is lost. |
| G2 | A **pre-upgrade fixture**: a malformed or over-long line written raw into a baseline-era flash file. It was never returned `ESP_OK` by HEAD, so it lies outside `accepted.jsonl`. | The intact raw bytes are copied to quarantine and fsynced **before** the cursor moves (checked in `ops.jsonl` order). `quarantined_malformed` increases by exactly 1. The record is never marked delivered. Every accepted record after it is delivered (R-E2E). `quarantined_ids ∩ accepted_ids == []`. |
| G3 | Index line with a bad CRC, or a torn last line. | Ignored. State is rebuilt from the earlier lines plus a media scan. R-E2E. |
| G4 | Index deleted or zeroed. | Rebuilt from a media scan. SD spool files are adopted by `first_id` + CRC when a flash segment matches, and otherwise become `REIMPORT` or legacy import. No loss; duplicates are reported. |
| G5 | Torn NVS cursor in both orders (blob only; legacy keys only). | Follows the §1.8 rule. A misaligned offset falls back to 0. Nothing is skipped. |
| G6 | A spool `.log` truncated after `SPOOLED` and before reclaim. | Reclaim re-verifies and refuses. The file is re-spooled. |
| G7 | Missing flash file at the cursor that the index knows about. | Waits (`NOT_FINISHED`) with `sd_state`/`backlog_missing`. It is never skipped. |
| G8 | **Post-commit corruption, reported-failure row.** Bytes of an already-accepted line are mutated in three cases: (a) a rotated flash file with a verified SD copy; (b) a rotated flash file with no SD copy; (c) the tail file. | (a) The flash CRC mismatch is detected, the SD copy is used, and R-E2E passes in full. (b) `NOT_FINISHED` with `sd_state=backlog_corrupt` and `corrupt_medium=flash`; the cursor never passes and nothing later is delivered. (c) The raw bytes are preserved in quarantine and `corrupt_detected` equals the exact injected set. **This row is expected to fail R-E2E for exactly the injected ids.** The harness asserts `missing_ids == injected_ids`, never as a pass for those ids, and none of them is ever counted as delivered. |

### H. Discovery, migration and rollback

| ID | Scenario | Assertions |
| --- | --- | --- |
| H1 | Fixtures written **by baseline b3f9b8a code on the host**: synced retained files, cursor mid-file, torn tail, `quarantine.log`, NVS `nid`/`rd_*`. SD holds `arc-*.log` (including suffixed), legacy `/sdcard/events`, and unrelated `sd_logger` and AMBIT-firmware files. Then HEAD boots. | No erase or format. Every pre-existing file's hash is unchanged, or changes only through an itemised documented transition. `next_id ≥ max(nid, max id + 1)`. Cursor unchanged. Unrelated files byte-identical. R-E2E over the fixture's pending set. |
| H2 | H1 with crashes at every `boot.*` and import point. | Final index and media match an uninterrupted run, apart from reported duplicates. No double import. |
| H3 | H1 with SD absent at first boot. | No SD index entries. Archive and backlog are not treated as empty. When SD appears, the result equals H1. |
| H4 | **Rollback.** HEAD with `SD_ONLY` files, then the **baseline** code on the same state (baseline keeper plus the driver), then HEAD again. Run twice: with baseline = `b3f9b8a`, and with baseline = `v2.2.3`. | Baseline imports every `SD_ONLY` primary and delivers every accepted id (duplicates reported). Baseline never imports a `.tmp` or a mirror. Back on HEAD, the foreign-cursor rule fires and the index converges. R-E2E over the union. No file in `/sdcard/events` or `/sdcard/evq` is left untracked. After HEAD reboots: `next_id > max id across flash ∪ indexed SD ∪ legacy SD ∪ archive`. The next 1000 stores produce ids that do not overlap any id ever seen, whether accepted, imported or fixture. `rollback_floor.json` lists every tag from v1.10.0 to v2.4.2 with its function hashes, the executed variant it matches, and a `proven` flag. §1.3 and J7 claim only the tags marked `proven`. |
| H5 | Pre-seeded collisions: legacy `ev-<first_id>.log` at the primary name, `ev-<3000000000+seq>.log` at the first fallback, `m-<seq>.log` and `m-<seq>-1.log` at the mirror names, and `arc-<first_id>.log` at the archive name. | No pre-existing SD file changes (hash compared before and after for every file on the card). Every name used is collision-free and recorded in the index. Delivery reads the recorded names. R-E2E. |
| H6 | `evlog rewind` after spooling. | Clamped to `FLASH ∪ SPOOLED ∪ SD_ONLY`. Never targets archived files. Never moves forward. |

### I. Runtime integration (§2.2 harness, real `device_commands.c` and `sync_runner.c`)

| ID | Scenario | Assertions |
| --- | --- | --- |
| I1 | An `SD_ONLY` + flash backlog, drained through a **direct** call to `sync_runner_drain` → `cmd_mqtt_publish_next_event` → PUBACK callbacks → completion queue. This is precise ACK-path coverage only. Automatic delivery is proven in I7. | Every accepted id is published in order. Each envelope's `timestamp` equals the record's capture time. Payload bytes are spliced verbatim, so the payload sha256 recomputed from the envelope equals the manifest. The window never exceeds 16 slots or 64 KiB. |
| I2 | PUBACK reason 0x87. | Refusal hold engages. Records stay pending. No cursor movement until a clean re-ACK. |
| I3 | Disconnect with a full window. | Abort and revert. The redelivered set covers every unacked record. |
| I4 | Cursor waiting on an unreadable SD copy (`NOT_FINISHED`). | `cmd_mqtt_publish_next_event` returns `NOT_FINISHED`, **never** `NOT_FOUND`/"no pending measurements". `pending` stays > 0. No busy-spin: over 10 simulated minutes with the drain task running, the claim count is ≤ the number of drain wakes (notifier plus fallback timer) + 1. Log lines are ≤ 1 per state change. The real `sync_runner_wd_should_reboot` returns false with power and clock OK and a long `since`. |
| I5 | STATUS heartbeat through the production `payload_v3.c` builder, in each state: normal, `storage_blocked` for every `blocked_reason`, `sd_state` = absent/lost/parked/mismatch/full/backlog_corrupt, `pending_exact=false`, non-zero `refused_*`, and non-zero `quarantined_*`/`corrupt_detecte`d. | The JSON parses. Every §1.9 field is present and equal to `event_log_health`. With worst-case values (every counter `INT64_MAX`, the longest enum strings) the build fits the production buffer, or fails closed with an error and never emits truncated JSON. The evidence prints the production cap and the worst-case length. |
| I6 | `event_log_sd_service` on the integration clock. | Gives the same transfer results as the storage harness. |
| I7 | **Automatic delivery, no direct drain call.** Start the production tasks with `sync_runner_start` (plus its boot-complete seam) and `event_log_sd_keeper_start`. Build a backlog with MQTT disconnected and the upload gate closed. Open the real gate (`device_commands` publish-power / hold seam) and connect MQTT. Later close the gate mid-drain and reopen it. | The test never calls `sync_runner_drain` or `event_log_sd_service`. The drain runs only in response to the store/gate-end notifier or the production fallback wake, as the shim's call trace shows. All accepted ids are delivered with R-E2E semantics. No claims happen while the gate is closed. Delivery resumes after the reopen without any test intervention. |
| I8 | **Keeper wake from pressure.** With the keeper task running on a long period (60 s production default on the virtual clock), stores cross `EVQ_PRESSURE_FREE`. | Spooling starts within one scheduler turn of the notification, well before the 60 s period elapses, and there is no manual service call. The trace shows `store → notify → keeper wake → service`. Mount and unpark events also wake it. |
| I9 | **`evlog` CLI surface** via the production `evq_render_health_text`, with the same state set as I5. | The output contains the expected `key=value` tokens for every §1.9 field, and they equal `event_log_health`. `pending_exact=false` renders as an explicit floor marker (`pending>=N`). With worst-case values the output fits the CLI buffer, or returns `-1` and `CLI.c` prints an explicit error line; truncated output never appears. `CLI.c` calls this renderer, which a source check confirms and C6 compiles. |

### Q. Quarantine classes, outside the accepted set

Each record used here is purpose-built or a pre-upgrade fixture. It never appears in `accepted.jsonl`. The accepted records around it must still satisfy the full R-E2E.

| ID | Scenario | Assertions |
| --- | --- | --- |
| Q1 | q1 poison. A pre-upgrade fixture record whose envelope exceeds `AMBYTE_PUBLISH_MAX_BYTES` (the publisher's oversize path, run in the §2.2 harness), plus a purpose-built record whose parse allocation is forced to fail `EVLOG_OOM_STUCK_MAX` times. | The intact bytes are fsynced to quarantine before the cursor moves (`ops.jsonl` order). `quarantined_poison` and `oversize_skipped` each increase by exactly 1 per record. Neither record is ever delivered. All accepted neighbours are delivered in order. |
| Q2 | q2 malformed. The same assertions as G2, repeated on an SD-resident pre-upgrade legacy import file and on an over-long line. | The same quarantine ordering and exact counters as G2. Accepted neighbours pass R-E2E. |
| Q3 | Quarantine write failure (`EIO` or `ENOSPC` on `quarantine.log`). | The cursor does **not** move. The record stays at the frontier. The drain retries later. Nothing is skipped. |

### J. Build, sanitizers, repeatability, scope

| ID | Assertion |
| --- | --- |
| J1 | C6 exits 0. Size delta reported. No new warnings in touched components compared with the baseline build log. |
| J2 | Three checks: C0 passes (4 tests); C5 `compare_suites.py` passes (identical shared collection, branch failures ⊆ baseline failures); C1 new suites pass separately. |
| J3 | `check_constants.py`: production `EVSTORE_MOUNT`, `EVSTORE_PARTITION`, `EVLOG_ROTATE_BYTES`, `EVLOG_ARCHIVE_EVERY_N`, the record caps and the watermarks equal their documented values. Partition CSVs are unchanged. |
| J4 | Every C1/C3 binary runs under ASan + UBSan + LSan. One sanitizer report fails the run. |
| J5 | C4: identical `reconcile.json` and manifests per seed across two clean runs. |
| J6 | C7: only §1.10 paths change. Nothing is touched under `schedule/`, `sched_*`, `uart_*`, `ambit_*`, provisioning, partitions or littlefs. |
| J7 | `docs/evq-sd-overflow.md` covers: on-disk formats; the state machine; the rollback compatibility, stated only for the `proven` tags in `rollback_floor.json`; the dead-card operational consequence; q1/q2 as non-accepted classes; the durability events with their source citations; the host models and their limits; and cleanup. |

## 5. Not provable in Sprint 01

These limits are stated up front so nothing is over-read.

- **Hardware.** Physical power removal, real SDMMC/FATFS and real littlefs are only modelled (H-3/H-4). Sprint 02 will distinguish a CPU reset from a real power cut, and no rail cut has been established.
- **Card write cache.** The directory-durable event is derived from FatFs/diskio source (§1.8). A card whose internal FTL cache loses writes that were already acknowledged is outside what any host model can prove.
- **The broker.** Integration group I uses the real `device_commands` and `sync_runner`, but the broker, the esp-mqtt transport and TLS are simulated.
- **The warehouse.** Nothing here proves warehouse arrival.
- **Swapped-card foreign files.** Unindexed `ev-*.log` files on a swapped-in card that has **no** obligations are imported by the unchanged legacy path, which is existing behaviour. A card that does carry obligations for another CID is parked (F4).
