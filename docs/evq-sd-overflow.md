# Event queue SD overflow (fw after v2.4.2)

This document explains how unsent measurements overflow from internal flash to the SD card without losing any, how that behaves under failure and rollback, and how it is tested.

Code: `components/event_log/event_log.c`, `evq_index.c`, `evq_render.c`.
Contract: `docs/evq-sd-overflow-contract.md` (Sprint 01, agreed).

## Why it exists

Up to v2.4.2 the internal store (the 9.4 MiB littlefs `storage` partition) was the only place an unsent record could live. The SD keeper copied **delivered** files to `/sdcard/archive` and nothing else.

When a broker outage outlasted the store (~17 h at default cadence), every later measurement was refused with `ESP_ERR_NO_MEM`, even though the card had hundreds of GB free. That is how the nine full gateways of September 2026 lost data.

`tests/test_evq_baseline_repro.py` reproduces the loss on the real v2.4.2 source.

## On-disk layout

| Path | Medium | Contents |
| --- | --- | --- |
| `/evstore/events/ev-<seq>.log` | flash | Queue segments. The tail is the highest seq. Record format v2 is unchanged. |
| `/evstore/events/quarantine.log` | flash | Intact copies of records passed *without* an ACK (see below). |
| `/evstore/evq.idx` | flash | Segment index: append-only, one line per state change, CRC32 per line. |
| `/sdcard/events/ev-<first_id %06u>.log` | SD | **Primary** copy of an unsent segment. It sits in the directory every firmware since v1.10.0 imports. |
| `/sdcard/evq/m-<seq %06u>.log` | SD | **Mirror** copy. Older firmware never reads it. |
| `/sdcard/archive/arc-<first_id>[-<n>].log` | SD | Delivered records (bulk archive, unchanged naming). |

Both copies are byte-identical to the flash file, so a cursor offset means the same thing on either medium.

The primary name must be exactly `ev-%06u.log`. Released importers rebuild the path from the parsed number with that format, so any other spelling of the same number is invisible to a rolled-back device. The harness found this.

Every name we create is collision-free and never overwrites an existing file. If a name is taken:

- primaries fall back to `ev-<3000000000+seq+k>.log`, which is still importable and sits above the measure-id range;
- mirrors use `m-<seq>-<k>.log`;
- archives use `arc-<id>-<seq+k·10^8>.log`.

## Segment states

Index lines have the form `<kind> <fields> *<crc32>`.

| Line | State | Meaning |
| --- | --- | --- |
| `S seq first last count bytes crc` | FLASH | A rotated file with a known CRC. |
| `P seq cid primary mirror` | SPOOLED | Both SD copies verified. The flash copy is kept. |
| `O seq` | SD_ONLY | The flash copy was reclaimed under pressure. |
| `D seq` | DELIVERED | This firmware's own ACK prefix passed the end of the file. |
| `R seq off remaining` | REIMPORT | Another firmware moved the cursor past it. It is re-appended into the flash tail from a surviving copy. |
| `A seq` | (retired) | Archived, re-imported, or delivered and gone. |
| `W seq off` | (watermark) | The first byte this firmware wrote. Malformed lines before it are pre-upgrade residue. After it, they are post-commit corruption. |

## Invariants

**Durable acceptance.** `store_event` returns `ESP_OK` only after the record is fsynced. Before this change, v2.4.2 batched 8 records or 1.5 s, sized for FAT. On littlefs a per-record fsync costs only flash wear.

**Flash is reclaimed only after two verified copies.** The spool order is:

1. `.tmp` write, fsync, close;
2. `rename`;
3. reopen by the new name and verify size, CRC and structure;
4. the same for the mirror;
5. index `P` line;
6. flash `remove`;
7. index `O` line.

Reclaim re-verifies both copies first. If either fails, the segment reverts to FLASH and is re-spooled. The harness checks **INV-1** after every simulated power loss: an effective SD_ONLY entry always has a durable, verifying SD copy or its flash copy.

**Durability events.** These come from ESP-IDF 5.5.3 source:

- `vfs_fat_rename` returns 0 only on `FR_OK`.
- `f_rename` runs `dir_register` → `dir_remove` → `sync_fs`.
- `f_unlink` and `f_mkdir` also end in `sync_fs`.
- `diskio_sdmmc` `CTRL_SYNC` is `RES_OK` because sector writes are synchronous.

So a successful return of `rename()`, `remove()` or `mkdir()` means the directory change is durable. A card whose FTL cache loses acknowledged writes is outside what any host model can prove.

**The cursor never passes an accepted record without an ACK.** Only two classes may be passed without one, and neither is ever an accepted record. In both cases the intact bytes are fsynced to `quarantine.log` first.

- **q1:** publish-impossible poison. This is the existing oversize and OOM-stuck path, which HEAD-accepted records cannot trigger because of the cap chain.
- **q2:** malformed lines. These are pre-upgrade residue or post-commit corruption.

A missing file, a torn rotated tail, or an unreadable SD copy of an **indexed** segment is never skipped. The only counted skip left (`skipped_unindexed_gap`) is a pre-upgrade gap with no index entry.

**Unreadable SD at the head waits, in original order.** Claim returns `ESP_ERR_NOT_FINISHED`, never `NOT_FOUND`, in these cases:

- the card is absent, lost or parked (low battery);
- the card is a different one (CID);
- the primary and mirror are both missing or both corrupt.

When this happens:

- The cursor, NVS and index stay untouched.
- `pending` still counts the records. `deliverable_pending` is 0, and the no-PUBACK watchdog uses it, so a missing card cannot cause a reboot loop.
- Stores continue while flash has safe capacity. After that they are refused with `blocked_reason=flash_full_backlog_waiting`.
- When a verified copy is readable again, delivery resumes at the same record.

**Mismatched cards get zero operations.** A card whose CID is not the one holding SD_ONLY or REIMPORT obligations is never read, written, renamed or imported from, and `sd_state` is `mismatch`.

A new card is adopted only if no such obligation exists elsewhere. Adoption re-spools every SPOOLED file from its verified flash copy, which cannot be reclaimed until that has happened.

**Operational consequence.** A permanently dead card that holds SD-only data halts delivery of every later record. After flash fills, acquisition is blocked too, with an explicit reason, until someone intervenes. Nothing forfeits those records automatically. Exposure is minimised because flash is reclaimed only under pressure, so SD-only data exists only after an outage longer than flash capacity, and every SD-only segment has two copies.

## Transfer policy

| Trigger | Action |
| --- | --- |
| Every 1000 stores (`EVLOG_ARCHIVE_EVERY_N`, unchanged) | Archive delivered files, then spool unsent rotated files. |
| Flash free below 25 % (`EVQ_PRESSURE_PCT`) | The store wakes the keeper at once. It spools, then reclaims verified SPOOLED files oldest-first until 40 % is free (`EVQ_RECLAIM_PCT`). |
| A card is mounted or un-parked | The keeper is woken. Any remount invalidates cached SD verifications, because `event_log_sd_notify` bumps the epoch. |

- The keeper task is `event_log_sd_keeper_start`: a 60 s period plus notifications. It copies and verifies without `s_mtx`: rotated files are immutable, and the file being copied is pinned against eviction. A slow card therefore never delays a store.
- The lock order is `s_mtx` → `sdcard_io_begin`. The host harness aborts if `s_mtx` is taken while an SD ref is held.
- The SD reserve is 64 MiB (`EVQ_SD_RESERVE_BYTES`). Below it, `sd_state=full` and spooling pauses.
- Eviction under pressure removes only delivered flash copies (`seq < cursor`), never a REIMPORT source.

## Rollback

On every boot the cursor is persisted as an NVS blob `cur={seq,off,crc}`, followed by the legacy keys `rd_seq`/`rd_off` that older firmware reads. At boot:

| NVS state | Meaning | Action |
| --- | --- | --- |
| Legacy keys behind the blob | Our own torn write | Use the earlier cursor (duplicates only). |
| Legacy keys ahead of the blob | Another firmware moved the cursor | Adopt the legacy cursor and mark every indexed segment in `[blob, legacy)` REIMPORT, even flash-resident ones. The cursor move is never taken as proof of delivery. |
| Offset not on a record boundary | Corrupt or misaligned cursor | Replay from the file start. |

The older firmware re-imports SD-only data from `/sdcard/events`. `tests/evq_host/rollback_floor.py` hashes the import path, line gate, name parser, cursor clamp, missing-file path and keeper call site of every release tag. There are two variants: v1.10.0–v2.2.3 and v2.3.0–v2.4.2. The harness **executes** one of each (v2.2.3 and b3f9b8a) in H4: HEAD → baseline → HEAD, with every accepted id delivered and no id reused afterwards.

**Proven rollback targets:** all 18 tags from v1.10.0 to v2.4.2, per `rollback_floor.json`. Anything older used `/sdcard/events` as its live store and remains unsupported.

## Accounting (STATUS `storage.evq` and `evlog`)

`pending = flash_pending + sd_pending + reimport_pending`. `reimport_pending` includes records in unowned `/sdcard/events` files awaiting legacy import.

`pending_exact=false` whenever any of the following holds; `pending` is then a floor, and the CLI prints `pending>=N`:

- an unindexed pre-upgrade file exists;
- the cursor offset has not been validated yet;
- a remainder is unknown;
- the card has not been scanned this session.

Refusals are counted by reason:

- `refused_full` (`ESP_ERR_NO_MEM`)
- `refused_media` (`ESP_FAIL`)
- `refused_too_large`
- `refused_unavailable`

All counters are boot-relative; STATUS carries uptime.

`storage_blocked` is paired with a `blocked_reason`. The reasons are:

- `flash_full_sd_unavailable`
- `flash_full_sd_full`
- `flash_full_sd_error`
- `flash_full_sd_mismatch`
- `flash_full_backlog_waiting`
- `flash_full_index_cap`
- `flash_full_transfer_pending` (the SD is fine, but the keeper has not freed space yet)

Both surfaces render through production code:

- STATUS goes through `payload_v3.c`, which fails closed; the heartbeat buffer is now 6144 B.
- The CLI goes through `evq_render_health_text`, which returns -1 rather than truncating into its 1536 B buffer.

## Host tests and their limits

| Command | What it proves |
| --- | --- |
| `python -m unittest tests.test_evq_baseline_repro` | v2.4.2 refuses measurements while the SD has room. |
| `python -m unittest tests.test_evq_storage` | Groups A–H and Q on the real event_log. |
| `python -m unittest tests.test_evq_integration` | Real `device_commands.c` + `sync_runner.c` + event_log: drain, window, refusal hold, watchdog, STATUS, CLI and automatic wakes (group I). |
| `python tests/evq_host/run.py --matrix --seeds 1,7,1337 --coverage-report` | Every registered fault point × {crash, eio, enospc} × nth {1, 2, last}, with the durability oracle and INV-1 at each checkpoint and end-to-end reconciliation afterwards. |

**Models.**

- **Flash (littlefs).** A power loss reverts every file to its last sync; a directory operation is atomic.
- **SD (FAT, adversarial).**
  - An unsynced file is cut to a random length between its synced size and its current size, sometimes with garbage appended.
  - A never-synced new file may vanish.
  - A crash *inside* a rename leaves {not applied, applied, both names}. On real FAT, "both names" can be a cross-link, so boot repair renames such a `.tmp` aside and never unlinks it.
- **NVS.** Only committed values survive.

**Not proven here:**

- real SDMMC, FATFS or littlefs behaviour;
- physical power removal;
- a card's write cache;
- esp-mqtt, TLS or the broker;
- warehouse arrival.

Sprint 02 covers the bench device and real resets.

**Cleanup.** The harnesses work only inside `$EVQ_TMPDIR` (or `mkdtemp(prefix="evq-")`) and remove it on success. Evidence goes to `$EVQ_OUT`.
