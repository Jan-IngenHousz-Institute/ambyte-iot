# Plan: replace SQLite with an append-only event log

## Why

The events DB corrupts on the SD card under real load (160-pt traces). Root cause
is **SQLite's write pattern on FATFS/SD**, not the card: SQLite rewrites pages in
place and rewrites the header page (the change counter) on *every* commit, then
fsyncs. In-place rewrites + header churn are what corrupts FAT/SD; one bad
cluster/header write malforms the **whole** database. Field evidence: the prior
firmware logged to an **append-only text file** on the *same* card for **months**
with no corruption.

The workload is a **store-and-forward FIFO queue**, not a relational query. An
append-only log is the right tool and the proven-robust one. Bonus: dropping
SQLite removes its page cache + library bulk, relieving the heap crunch (the NOMEM
stores, failed `CREATE`, `wifi:m f null`) and the flash footprint.

---

## Step 0 — Spike first (de-risk before any rewrite)

Before building anything, prove the core hypotheses on **the actual bad card**,
because if they don't hold, neither SQLite nor a log helps and we've learned that
for ~2 hours instead of a week.

A throwaway task (gated behind a build flag, no port/API changes) that:
1. **Appends** ~1 KB lines to `/sdcard/spike.log` at the **real schedule** rate
   (4 ch: spectra/1 min + trace/5 min), with **periodic** flush (every ~1 s or N
   lines — NOT per-line fsync; match the proven TXT logger).
2. Concurrently, a second task **reopens + reads back** the file from a moving
   offset (mimicking the publish reader) — to validate the read-while-append model
   on this FATFS/per-file-cache build, not just appends.
3. Logs free heap each cycle.

**Pass criteria after a few hours:** zero corruption (file always re-reads
cleanly; SD still mounts), the reader reliably sees freshly-appended lines, and
**free heap is flat**. If pass → build the full thing below. If it still
corrupts → the fault is deeper in the FATFS/SDMMC stack; stop and rethink storage
(e.g. LittleFS, different card interface) instead of writing the whole component.

---

## Strategy: keep the port, swap the implementation

`components/domain/include/persistence_port.h` defines six fn-pointer types that
`device_commands` and `sync_runner` consume. A new component implements the
**same** interface, so **only `app_main` wiring changes** — `device_commands.c`,
`sync_runner.c`, the publish path, the CLI, and Lua `db.store_event` stay as-is.

Port → append-log mapping:

| Port fn | Append-log |
|---|---|
| `next_id(out)` | RAM counter; high-water mark persisted to NVS **in blocks** |
| `store_event(...)` | append one record to the tail file; periodic flush |
| `claim_next_event(out)` | read the record at the read cursor; set in-RAM inflight id |
| `mark_event_synced(id)` | advance the read cursor past it (cursor batched to NVS) |
| `mark_event_pending(id)` | clear in-RAM inflight (cursor unchanged → re-read) |
| `db_stats(...)` | online flag, `pending` from a maintained counter, `next_id` |

The `claim → on_publish_ack → mark_synced/pending` flow in `device_commands.c` is
unchanged and already gives **at-least-once** delivery — so the cursor advancing
only on synced is behaviour-identical to today (NOT a new risk; the current
SQLite path also re-sends on a crash between publish and mark).

---

## Durability model — match the proven logger, don't over-tighten

- **Append + PERIODIC flush**, not per-event fsync. The old TXT logger that ran
  for months almost certainly let the FS flush lazily; we copy that. Per-event
  fsync would also rewrite the directory entry every event (an in-place metadata
  write — so "append-only" is *mostly*, not *purely*, append) and cost latency.
- Flush cadence: every ~1–2 s or every N records, whichever first; plus a flush
  on clean shutdown / SD-eject callback.
- Power-loss window: the last unflushed records can be lost — acceptable for a
  store-and-forward buffer, and matches the old behaviour.

## Crash safety — reader tolerance, NOT boot truncation

- A power loss mid-append leaves a partial/garbled final record.
- **The reader skips any record it can't parse** (logs a warning, advances past
  it). This handles a bad record *anywhere*, needs no `ftruncate`, and the next
  append simply continues. No boot-time repair scan required.

## Record format — robust framing

Directory `/sdcard/events/`, rotating files `ev-000001.log`, `ev-000002.log`, …
(monotonic zero-padded seq). One record per line:

```
<measure_id>\t<device>\t<sensor>\t<start_ms>\t<end_ms>\t<metadata_or_empty>\t<payload>\n
```

- `metadata`/`payload` are already-serialised cJSON-unformatted strings → contain
  no literal tab/newline (control chars are escaped), so the trailing payload
  field is unambiguous after splitting on the first 6 tabs.
- `device`/`sensor` are the only raw fields → **sanitised at store time** (strip
  any control char/tab); they're short controlled strings ("ambit", "AMBIT_1").
- Reader splits on the first 6 tabs; rejects (skips) a line that doesn't yield 7
  fields or whose payload doesn't begin with `{`/`[`.
- **No cJSON on store or claim** — store writes the verbatim strings, claim hands
  `payload_json`/`metadata_json` back verbatim, and the publish path already
  splices them verbatim into the envelope.
- *Alternative if binary payloads are ever needed:* length-prefixed records
  (`<u32 len><bytes>`) — unambiguous regardless of content, at the cost of
  human-readability. Tab+sanitise is preferred (greppable, matches the TXT era).

## The single-file read + append handshake (first-class design, not a footnote)

The tail file is appended to (store) *and* read from (publish events not yet
rotated). With `CONFIG_FATFS_PER_FILE_CACHE`, a separate reader handle won't see a
writer handle's buffered appends. The model (all under the existing mutex):

- One persistent **write** handle on the tail file (append mode).
- `claim_next_event` for the **live** (tail) file: `fflush`+`fsync` the writer to
  push appends to media, then **open a fresh read handle**, `fseek` to the cursor
  offset, read one complete line (`\n`-terminated; a partial trailing line ⇒ "no
  event yet" ⇒ `ESP_ERR_NOT_FOUND`), close. Reopen-after-sync guarantees a current
  view despite per-file caches.
- For a **rotated** (closed, `seq < tail`) file: read sequentially; at EOF delete
  it and advance the cursor to `(seq+1, 0)`.
- The fsync+reopen cost is **per drain**, not per event — negligible vs the
  network publish. The spike (Step 0) validates this read-back works on this FATFS.

## Cursor & next_id — bounded NVS writes

NVS namespace `evlog`:
- Read cursor `(rd_seq, rd_off)`: **batched** to NVS — persist every K acks or T
  seconds, not every ack (a crash re-sends ≤K events; at-least-once already allows
  that). This caps flash wear and scales to high event rates ("spectra as fast as
  possible").
- `next_id`: hand out from a RAM counter; persist a high-water mark every block
  (e.g. 64). Boot resumes at the HWM (≥ last used); gaps are fine (IDs need only
  be monotonic + unique). ≤1 NVS write per 64 IDs.
- Putting sync state in **NVS (internal flash), not on the SD**, keeps the SD to
  appends only.

## Stats / status

Maintain `pending` and `total` as **in-RAM counters** (++ on store, -- on
mark_synced), re-derived once on boot by a quick scan (count `\n` from the cursor
to the tail). `db_stats` reports those + `next_id` + an online flag, so the CLI
`status`/`db_status` work unchanged.

---

## Components & wiring

New `components/event_log/`:
- `include/event_log.h` — `event_log_init()`, `event_log_on_sd_lost/restored()`,
  six `event_log_get_*_fn()` getters returning `persistence_port.h` types.
- `event_log.c` — append/read/cursor/rotation; mutex; record format helpers; NVS
  cursor I/O; the read+append handshake; reader skip-bad-line; boot scan.
- `CMakeLists.txt` — `REQUIRES domain sd_card nvs_flash` (no sqlite3).

Removed from the build: `components/persistence` and `components/sqlite3`
(hundreds of KiB of flash + static RAM; and the defensive self-heal / bounded
recovery / journal=MEMORY all go with it).

Wiring (small, localised):
- `main/app_main.c`: `sqlite_persistence_init()` → `event_log_init()`;
  `cmd_cfg.{next_id,store_event,claim_next_event,mark_event_synced,
  mark_event_pending,db_stats}` ← `event_log_get_*_fn()`;
  `app_on_sd_state_change()` → `event_log_on_sd_lost/restored()`.
- `main/CMakeLists.txt` REQUIRES: drop `persistence`, add `event_log`.
- `device_commands.c`, `sync_runner.c`, CLI, `lua_runner` db module: **unchanged**.

---

## Pre-flight checks (do before coding)

- Confirm `sqlite3` is referenced **only** by `persistence` (grep) before removing it.
- Confirm the FATFS VFS read-back / reopen behaviour the handshake relies on
  (covered by the Step-0 spike).
- Confirm openJII ingestion **dedups on `measure_id`** (at-least-once is status
  quo, but verify it's actually tolerated).
- Decide flush cadence + cursor batch size K from the spike's measured rates.

## Edge cases

- Old `/sdcard/measurements.db*` ignored (optionally deleted on first boot). Any
  unpublished rows in it are lost at switchover — fine for a dev cut; for a field
  OTA, drain the old DB first.
- SD pulled/reinserted → `on_sd_lost/restored` close/reopen the tail handle and
  re-scan the directory + cursor.
- Backlog when offline for very long → optional cap: drop the oldest unpublished
  file with a warning rather than fill the card.
- Records published in **store order** (= append order ≈ measure_id order). A
  caller passing an explicit out-of-order `measure_id` publishes in store order,
  not id order — a minor semantic change from SQLite's `ORDER BY measure_id`.

## Scope

New component ~**600–800 LOC** (the read+append handshake, rotation, cursor
batching, boot scan, skip-bad-line push it past the earlier 400–500 guess),
~10-line `app_main`/CMake change, two components deleted. Behaviour-compatible at
the port boundary. **Gate the whole thing on Step 0 passing.**

## Verification

- Step-0 spike: hours on the bad card, real schedule → zero corruption, reader
  reads back cleanly, flat heap.
- Full build: same soak → zero corruption, flat/higher idle heap (no SQLite),
  events store + publish, `status` shows pending draining, power-cut mid-write
  loses ≤ last few events (reader skips the partial), reboot re-sends ≤K events.
