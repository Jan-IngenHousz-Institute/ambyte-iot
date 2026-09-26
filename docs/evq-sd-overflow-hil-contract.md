---
title: "Sprint 02 contract: preserved-data hardware verification"
kind: spec
comments: none
---

**Status: AGREED (DRAFT v3 accepted by the Evaluator in contract exchange 4 of 4, 2026-09-26; see [acceptance](../contract-acceptance)). Binding for Sprint 02.**

Baseline: reviewed commit `2c0c714` (Sprint 01 PASS), branch `fix/durable-sd-overflow-20260925`, worktree `/tmp/ambyte-sd-overflow`. Links: [spec](../../spec), [rubric](../../rubric), [evaluation](../../evaluation), [critique of v1](../contract-critique-01), [critique of v2](../contract-critique-02), [Sprint 01 contract](../../sprint-01/contract).

## Changes from DRAFT v2 (critique 02)

| # | Evaluator point | v3 resolution |
| --- | --- | --- |
| P0-1 | Flash snapshot races schedule writes | `flash_inv` takes the snapshot **under `s_mtx`**: the ordered list of `ev-*.log` names, each file's byte length, `cutoff_id = next_id`, and the tail seq. It then releases the lock and hashes only those prefixes. Every length is a prefix of an append-only file; rotated files are immutable; the keeper is paused, so no reclaim, eviction or archive runs mid-walk. It emits `EVQ_FSNAP <cutoff_id> <tail_seq> <nfiles>`, one `EVQ_FF` per snapshotted file, and `EVQ_FEND <nfiles_hashed> <missing>`. Rotation during the walk is handled as follows: a file created after the snapshot is not listed (its records have ids ≥ `cutoff_id`); a listed file is hashed only to its snapshotted length. A listed file absent at hash time counts as `missing`, and missing > 0 → the inventory is invalid and is retaken. HW-INV compares only obligations with id < `cutoff_id` against that inventory; later accepts carry forward to the next inventory. `sd_inv` runs with the keeper paused and the gate held, so SD record files are immutable during the walk. |
| P0-2 | Trace ring can wrap | The trace records **only relevant ops**: SD ops under `/sdcard/events`, `/sdcard/evq` and `/sdcard/archive`; `evq.idx` writes and renames; flash `remove`/`rename` of `ev-*.log`. Store appends and fsyncs are counters only. The host drains it with `io trace drain` every 100 fill records and at every phase end. Each drain prints `EVQ_TR_HDR <total> <lost> <first_seq> <last_seq>`; seqs are contiguous across drains. **Any `lost` > 0 or seq gap in a phase → F-3 FAIL** for that phase. |
| P1-3 | M-1 has no claim observability | HIL-only counters and trace at the real `sync_runner` drain's `cmd_mqtt_publish_next_event` → `claim_next_event` result: counts per `esp_err_t` (`OK`, `NOT_FOUND`, `NOT_FINISHED`, other), last result, and drain-loop iterations/min. Printed by `evq_hil claims`. M-1 asserts, while parked with an SD_ONLY head: NOT_FINISHED increases; NOT_FOUND delta = 0; the drain rate stays ≤ the fallback-timer rate (no busy-spin: ≤ 2 claim attempts per fallback period plus wake notifications); the NVS cursor blob `cur` and the legacy `rd_seq/rd_off` are byte-identical before and after the park (`evq_hil cursor` prints the raw NVS values' sha256 plus the RAM cursor). |
| P1-4 | App write boundary | Before W-1: `firmware.bin` size > 0 and ≤ `0x2F0000`, and its sha256 equals the agreed build. After W-1: besides `verify_flash`, `read_flash 0x20000 <len>` of exactly the programmed length must have sha256 equal to the image. |
| §2.1 | Duplicate occurrences | Byte-identical duplicates of one id (for example the old firmware's archive copy and a flash copy, or retained re-imports) are legal at-least-once evidence and are counted. Only one id mapping to more than one **distinct** line sha256 fails. |

## Changes from DRAFT v1 (critique 01)

| # | Evaluator point | v2 resolution |
| --- | --- | --- |
| P0-1 | Counts are no proof of flash bytes | New read-only **`flash_inv`** (§1.3) emits every stored line of every `/evstore/events/ev-*.log` as (seq, line, measure_id, sha256(line)), taken under a quiescent snapshot: fill stopped, gate held, keeper paused, tail read under `s_mtx` to a snapshotted length. `sd_inv … lines` does the same for SD. `HW-INV` (§4) is now a set reconciliation over these line hashes plus warehouse rows, after every phase and every reset. Counts are only cross-checked against it. |
| P0-2 | Ambiguous (run, k) across resets | Every fill has a **unique run id** and an explicit `k_start`. The device prints `EVQ_BEGIN`/`EVQ_END` and the host log stores them. Records found anywhere with `device=evq_hil` but no `EVQ_ACC` are **indeterminate**: each must be complete (payload recomputes from run and k, line hash consistent) wherever it appears. A new global check **ID-UNIQ** requires every measure_id to map to one line hash across all sources, so no id is reused. |
| P0-3 | Cleanup contradicts A1 | **The pair deletes nothing** on SD or flash (§5 C-3). `xlk-*` and `bad-*` stay and are reported. The rollback leg is sequenced so the firmware itself retires every synthetic mirror before the final state (§4 P8, §5). |
| P0-4 | No `ota_mark_valid` escape | Removed. The otadata entry is written as `ESP_OTA_IMG_NEW` (the same state `esp_ota_set_boot_partition` writes with rollback enabled), so the bootloader boots PENDING_VERIFY. The image must reach VALID through production `confirm_pending_after_boot` (MQTT connected and `event_log_db_stats` available), shown by its log lines. Otherwise STOP: power-cycle via reset, the bootloader rolls back to `ota_1`, and nothing is injected. |
| P0-5 | First-boot SD baseline ordering | The hold becomes a separate RTC-retained flag, `s_hil_hold`, set at the **first statement of `app_main`**, before `sdcard_init_default`/`sdcard_mount`, `sd_logger_init`, `event_log_init_ids`, `event_log_init`, `event_log_sd_keeper_start`, `sync_runner_start` and `ambit_flash_boot_sync`. It is folded into `evq_sd_observe_locked`, so the keeper, repair, import, archive, reclaim and claim all see `sd_state=parked`. The power guard's `event_log_set_sd_parked(false)` cannot clear it. It is proven by: a new host startup-order test **HOLD-1** with sentinel primary, mirror, archive, legacy and orphan files; the device boot trace `HIL_TRACE` with esp_timer µs for each step; the complete boot-time SD-writer list (§2.2); and two complete record-directory manifests taken ≥ 120 s apart before release, which must be identical. |
| P1-6 | Payload-only hash | `EVQ_ACC` now carries **sha256 of the exact stored line**, captured inside `event_log_store_event` under `s_mtx` (HIL-only) and read back by id after ESP_OK. It also carries the payload hash and the full envelope tuple. Warehouse reconciliation checks every field in §4 D-2. |
| P1-7 | Real schedule records | `flash_inv` and `sd_inv` taken before delivery (P7-0) enumerate every non-HIL record with an id at or after the first test id. They form set `REAL`, reconciled by id, capture time and JSON-semantic sample equality. Schedule health deltas (runs, fail, skip, streak) and the absence of dispatch/overrun/drop log lines are asserted (O-5). |
| P1-8 | Copy proof must use index names | New read-only **`index`** prints every segment (seq, state, first/last id, count, bytes, crc, cid, primary, mirror). F-3 verifies the index-recorded primary and mirror (any collision-free name), CID == card CID, size, and CRC32 (`sd_inv` prints each file's CRC32). The ordered I/O trace shows both copies' fsync → rename → verify-read before that seq's flash `remove`. |
| P1-9 | M-2 refusals vs stop rule | A narrow exception, only between `reserve <big>` and `reserve off`: refusals must all be `ESP_ERR_NO_MEM` with `blocked_reason=flash_full_sd_full`; their count equals the `refused_full` delta and the `EVQ_REF` count; no `EVQ_REF` id appears in any inventory or row; after `reserve off` a follow-up fill of 200 is fully accepted. Refusals outside that interval still STOP. |
| P1-10 | Re-establish after the backup reset | PRE-6: the full PRE-1 identity/state session is repeated after the backup's hard reset and immediately before the app write. |

## 0. Facts re-established (read-only, 2026-09-26 01:00 UTC; Generator was serial owner)

Opening the port reset the CPU; the boot was captured. The raw log is private (`recon-01.log`). The Planner confirmed the route and profile on 2026-09-26.

| Item | Observed | Source |
| --- | --- | --- |
| Identity | MAC `28:37:2F:FF:E7:04`, client `ambyte_28:37:2F:FF:E7:04`, name `AmbyteOnAir-bench` | `status`, `cfg` |
| Flash / layout | 16 MB GD, DIO 80 MHz; partitions as `partitions.csv` (storage `0x6A0000`+`0x960000` = 9,830,400 B) | boot log |
| Firmware | `2.3.3-rc.1` on `ota_1` @`0x310000` VALID; next `ota_0` @`0x20000`; `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` | boot log, `ota_status`, sdkconfig |
| Schedule | RUNNING INSTALLED, sha256 `943c294243a0254b364172dd3252069e350cbc54428fed88fb84f65c642ea2b7`, workbook `75c59552-…` | `schedule status` |
| Ambits | AMBIT1 1.4.0, AMBIT3 1.4.0 (AMBIT2 and AMBIT4 absent). SD target 1.1.5. `ambit_flash` is unchanged v2.3.3-rc.1..b3f9b8a and flashes only below target ("match or exceed … nothing to flash") | `ambit_versions`, source |
| Event store | cursor `ev-000707.log`@60846, pending 2, next_id 137433 | `evlog`, `status` |
| Power / gate | Vin 5.10 V external, Vbat 4.18 V. Gate CLOSED in the first ~16 s (on-dwell), then records published (id `137431`) | `status`, log |
| Broker | `a2s5vvyojsnl53-ats.iot.eu-central-1` = **dev** account 084375565727 (`jii-dev-084`) | `cfg`, `aws iot describe-endpoint` |
| Delivery surface | `open_jii_dev.centrum.raw_data`, experiment `b3c8d242-dbb3-4520-9a5c-664e364ae6bd` (debug, not scientific): 7,567 rows since 2026-09-24, newest ingestion ~2 min after publish | Databricks `sandbox`, read-only |

## 1. What will be built (HIL-only, compile-time isolated)

### 1.1 Build isolation

- New Kconfig `CONFIG_AMBYTE_EVQ_HIL` (default n) and PlatformIO env `evq-hil` (`sdkconfig.defaults;sdkconfig.evq-hil.defaults`). The release env `esp32-s3-devkitm-1` is unchanged.
- New component `evq_hil`, compiled only with the flag, following the `bench_diag` pattern.
- Every hook in a production file is wrapped in `#if CONFIG_AMBYTE_EVQ_HIL`.
- Test **ISO-1**: the release `firmware.elf` has no symbol matching `evq_hil|evq_fault_point|hil_` and no string matching `EVQ_ACC|HIL_TRACE|evq_hil`, while the `evq-hil` ELF has them. Sprint 01's C7 check "no `evq_fault_point`" stays.

### 1.2 Device hooks

| Hook | Behaviour |
| --- | --- |
| `EVQ_FAULT_POINT(n)` | Resolves to `evq_hil_fault_point(n)`; the production macro stays empty. |
| `evq_hil_io.h` | A forced include for `event_log.c` only. Wraps `fopen fread fwrite fflush fsync rename remove mkdir opendir readdir stat`. It counts SD and flash ops and keeps an ordered trace ring of **relevant ops only** (8192 entries, PSRAM: global seq, µs, op, path basename, result, plus lost-entry accounting; see P0-2 of v3). It can arm one `eio`, one `enospc`, or a mid-`fwrite` reset. |
| Gate override | RTC_NOINIT `hold / auto / force` in `sync_runner_is_allowed`. It survives CPU resets and is cleared by power-on reset (magic checked). |
| SD hold | RTC_NOINIT `s_hil_hold`: set at the first statement of `app_main` when the RTC magic says "not released" (power-on default = held); ORed into `evq_sd_observe_locked`. Cleared only by `evq_hil sd_release`. |
| Keeper pause | For inventories (HIL flag checked at the start of each keeper pass). |
| Overrides | SD reserve, card CID. |
| Line hash | Inside `event_log_store_event`, after fsync and under `s_mtx`, the sha256 of the exact written line is stored in a HIL-only 8-entry table keyed by id. |
| `HIL_TRACE <step> <µs>` | Printed at `app_main` entry and after each of: hold set, sd mount, sd_logger init, event_log init, keeper start, sync_runner start, ambit boot sync. |

### 1.3 HIL CLI (`evq_hil …`)

Every command prints one line per result, with a `HIL_` prefix.

| Command | Effect |
| --- | --- |
| `fill <run> <k_start> <n> <bytes> <hz>` | Stores through production `cmd_store_event` → `event_log_store_event`. Prints `EVQ_BEGIN <run> <k_start> <n> <bytes> <next_id>`, then per record `EVQ_ACC <run> <k> <id> <start_ms> <end_ms> <line_bytes> <sha256(line)> <sha256(payload)>` **only after ESP_OK**, or `EVQ_REF <run> <k> <id> <err> <blocked_reason>`. Ends with `EVQ_END <run> <k_last> <acc> <ref> <next_id>`. Stops on the first refusal unless `-c` is given (M-2 only). |
| `flash_inv` / `sd_inv <dir> [lines]` | Read-only. `flash_inv` snapshots under `s_mtx` (P0-1 of v3) and prints `EVQ_FSNAP`, then `EVQ_FF <path> <snap_len> <sha256> <crc32>` per file and `EVQ_FL <path> <line#> <id> <sha256(line)>` per line, then `EVQ_FEND`. `sd_inv` prints `EVQ_FF`/`EVQ_FL` for each file under `<dir>`. Torn tails are flagged `EVQ_FT`. The keeper must be paused, else the command refuses. |
| `claims` | Claim result counters at the production drain (P1-3 of v3). |
| `cursor` | RAM cursor, plus sha256 of the NVS `cur` blob and the `rd_seq`/`rd_off` values. |
| `index` | `EVQ_IX <seq> <state> <first> <last> <count> <bytes> <crc> <cid> <primary> <mirror>` |
| `io [trace drain]` | Counters (SD mutations, SD reads, flash mutations, store appends/fsyncs) and the relevant-op trace drain with `EVQ_TR_HDR <total> <lost> <first_seq> <last_seq>`. |
| `gate <hold\|auto\|force>` | Gate override. |
| `sd_release` | Clears the SD hold. |
| `keeper <pause\|run>` | Keeper pause for inventories. |
| `sd_park` / `sd_unpark` | Calls the production power-guard park/unpark routine in `app_main`: sd_logger pause, unmount, `event_log_set_sd_parked`. |
| `fault <point> <reset\|eio\|enospc> [nth]` | `reset` = `esp_rom_software_reset_system()`: no shutdown handlers, no coredump. |
| `reserve <bytes\|off>`, `cid <hex\|off>` | Overrides. |
| `stacks` | `uxTaskGetStackHighWaterMark` for evq_keeper, sync_runner, mqtt_task, cli, sched and maint. |
| `boot_slot <ota_0\|ota_1>` | `esp_ota_set_boot_partition`. |
| `state` | All RTC overrides and the hold. |

There is **no** SD delete command and **no** manual keeper-service command.

### 1.4 Synthetic records

- Payload, canonical compact JSON exactly: `{"evq_hil":"<run>","k":<k>,"pad":"<p>"}`.
- `<p>` is `bytes`-length ASCII `[a-z0-9]` from xorshift64(fnv1a64(run) ⊕ k). It is deterministic and incompressible, and one C file is compiled for both the device and the host.
- Envelope inputs: `device="evq_hil"`, `cmd_raw="evq_hil <run>"`, `channel=""`, `tag=MEASUREMENT`, `metadata=NULL`, `start_ms=end_ms=time(NULL)*1000`.
- Run ids follow `hil-20260926-<phase>-<nn>` and are never reused.
- Isolation: the destination is the dev debug experiment only; every row is identifiable by `device`, `cmd_raw` and `data.evq_hil`; there is no `schema`, so no macro `when:` applies. D-2 asserts per row that no `macros` field is present.

### 1.5 Host tools (`tools/evq_hil/`, each writes JSON evidence)

| Tool | Purpose |
| --- | --- |
| `console.py` | Single-owner serial (exists). |
| `backup.py` | Dual full read in one ROM session, `0600`. |
| `flash_regions.py` | Per-partition sha256 and diff. |
| `lfs_manifest.py` | littlefs-python 0.19: storage region → per-file and per-line (id, sha256), cursor from NVS; littlefs region → `schedule.yaml` sha256. |
| `nvs_diff.py` | ESP-IDF `nvs_tool.py` JSON; key/type diff and value-hashes only. |
| `ota_select.py` | Renders one otadata sector {ota_seq = max+1 selecting `ota_0`, ota_state = `ESP_OTA_IMG_NEW`, crc32} identical to `esp_ota_set_boot_partition`, and parses both sectors back. |
| `manifest.py` | Parses `EVQ_*`, recomputes payloads, rejects any mismatch. |
| `reconcile.py` | HW-INV over flash, SD and warehouse. |
| `reconcile_warehouse.py` | Databricks `sandbox`, read-only SQL by client and ingestion window, results cached. |
| `run_hil.py` | Phase runner: stops on the first failed check and writes `sprint-02/evidence/<phase>/`. |

### 1.6 Host tests (`tests/test_evq_hil_*.py`)

| Test | Checks |
| --- | --- |
| PAY-1 | The C and Python generators are byte-identical over 1000 (run, k, bytes) cases, and the output is canonical compact JSON. |
| MAN-1 | The manifest parser rejects a tampered line/payload hash, a duplicate (run, k), and an id reused with a different line. |
| IO-1 | The I/O wrapper compiled on the host: arm/fire semantics, counters, trace order. |
| HOLD-1 | evq_host harness with pre-existing sentinel files: legacy `/sdcard/events/ev-*.log`, a primary + mirror pair referenced by the index, an orphan mirror, `arc-*.log`, `.tmp` siblings, `xlk-*`, `bad-*`. The hold is set before `event_log_init`; then init, index rebuild, 10 keeper passes, pressure, claims at an SD_ONLY head and a remount notify all run. Asserts zero SD mutating ops and zero SD opens by event_log, and every sentinel sha256 unchanged. After release, normal processing starts (sentinel legacy files are imported). |
| OTA-1 | `ota_select.py` output parses with ESP-IDF `otatool.py`'s reader and selects `ota_0` with state NEW. |
| ISO-1 | See §1.1. |

Regression: Sprint 01 C0–C7 must pass at the final commit. Any hardware-found firmware fix gets a host regression test before it is re-flashed.

## 2. Preservation precondition (before any app write)

| ID | Check | Pass rule |
| --- | --- | --- |
| PRE-1 | Identity | One read-only session (`status evlog ota_status "schedule status" ambit_versions cfg log_status`). MAC, client, flash, running slot, app version, ELF sha prefix, schedule sha, AMBIT1/AMBIT3 = 1.4.0 must equal §0; the cursor, pending and next_id may only have progressed. Otherwise STOP. |
| PRE-2 | Backup | Read A: `esptool --before default_reset --after no_reset read_flash 0 0x1000000 A.bin`. Read B: `--before no_reset --after hard_reset read_flash 0 0x1000000 B.bin`, in the same ROM session so no app runs in between. Each is 16,777,216 B and sha256(A) == sha256(B). Stored in private `0700`/`0600`. Artifacts carry only size, sha256 and per-partition sha256. |
| PRE-3 | Decode | The partition table at `0x8000` equals the build's `partitions.bin`. `lfs_manifest.py` of storage gives `PRE_FLASH` (per line: id, sha256) with the pending subset derived from NVS `rd_seq/rd_off`. `schedule.yaml` sha256 in the littlefs region == `943c2942…`. Otherwise STOP. |
| PRE-4 | NVS baseline | `nvs_tool.py` JSON: namespace/key/type plus value sha256. Identity values are never printed. |
| PRE-5 | Otadata | Both sectors parsed: seq, state, crc. |
| PRE-6 | Re-establish | PRE-1 is repeated after the backup's hard reset and immediately before W-1. The same pass rule applies. |

### 2.1 SD baseline (first HIL boot, held)

The running 2.3.3-rc.1 cannot list or hash SD files; its CLI set was verified at the tag. The Evaluator accepted the logical first-boot baseline in principle, subject to these proofs:

1. **Hold-before-writer ordering.** The `HIL_TRACE` sequence must be `app_main_entry < hold_set < sd_mount < sd_logger_init < event_log_init < keeper_start < sync_runner_start < ambit_boot_sync`, and `evq_hil state` must report `hold=1`. HOLD-1 is the host proof for event_log's record paths.
2. **Zero record-path I/O before release.** `io` must show event_log `sd_mut=0` and `sd_open=0` from boot until after snapshot S2, and the trace must be empty of SD paths.
3. **Two complete record-directory manifests.** S1 and S2, taken ≥ 120 s apart, each run `sd_inv / lines` over the whole card with logs listed by name only. They must be identical for every file outside `/sdcard/logs` (path, size, sha256, crc32, per-line hashes). Their union is `SD_BASE`. Only then `sd_release`.
4. **Identity continuity with the pre-update state.** Every `SD_BASE` record line whose id is also in `PRE_FLASH` must have an identical line sha256, since the old firmware's archive is a verbatim copy. Each id in `/sdcard/events` (if any) and `/sdcard/archive` must map to one distinct line hash; byte-identical duplicate occurrences are legal and counted. A random sample of 200 archived ids (seeded; only ids ≥ the first dev-experiment row, 2026-09-06 22:10 UTC) must have `raw_data` rows whose sample JSON-semantically equals the archived line's payload. Any mismatch → STOP.
5. **If any of 1–4 fails,** the logical baseline is void. The Generator escalates to the Planner for physical card imaging and does not proceed.

### 2.2 Boot-time SD writers (complete list, from source at `2c0c714`)

| Writer | Paths | When | Shares the hold? |
| --- | --- | --- | --- |
| `event_log` | `/sdcard/events`, `/sdcard/evq`, `/sdcard/archive` | keeper, claim | **Yes** (only writer of record directories) |
| `sd_logger` | `/sdcard/logs/*` | from `sd_logger_init` | No. Logs are excluded by name, and their FAT writes cannot change record-file bytes, which S1 == S2 and the final diff would show. |
| `ambit_flash` | `mkdir /sdcard/ambit_fw[/ver]` | only when a download or flash is needed; the AMBITs are above target | No. Not a record path; a flash attempt is a STOP condition. |
| `ambit_ota` | `/sdcard/ambit_fw.bin` | only on an MQTT `ambit_ota` command; none is sent | No. Not a record path. |
| `sd_card` mount | FAT volume | `format_if_mount_failed=false` | n/a |

Read-only SD users: `evlog_inventory`, `evlog_replay` (reads the archive, only on command), `command_router` archive scan. The littlefs mount (`format_if_mount_failed=true`) is pre-existing production behaviour: a littlefs mount failure on the new image is a STOP (W-4).

## 3. Write boundary

| ID | Rule |
| --- | --- |
| W-1 | The only pair-issued flash writes are:<br>(i) `esptool write_flash --flash-mode dio --flash-size 16MB 0x20000 evq-hil.bin` plus `verify_flash`;<br>(ii) one otadata sector from `ota_select.py` (`write_flash 0xf000` or `0x10000`, whichever is older), read back and parsed;<br>(iii) the later `boot_slot` calls in P8 and C-2, and one more `ota_select.py` for RB-2.<br>Everything else is firmware's own writes: event store, NVS cursor/frontier/latches, otadata state, coredump only on a crash. No `erase_flash`, no bootloader/partition-table/`phy_init`/NVS-image write, no `-t upload`, no schedule install, no AMBIT flash, no SD format or delete. |
| W-2 | Before (i): `read_flash 0x8000 0xC00` equals the build's `partitions.bin`; the image is non-empty, ≤ `0x2F0000` B, and its sha256 equals the `evq-hil` build of the agreed commit. After (i): `verify_flash`, and `read_flash 0x20000 <image_len>` has sha256 equal to the image. |
| W-3 | After the test, the `ota_1` region sha256 equals the backup's (fallback intact). |
| W-4 | These regions must equal the backup by sha256 after the test: `0x0–0x8FFF` (bootloader + partition table), `phy_init`, `ota_1`. The `littlefs` region is compared by sha256. If it differs because firmware wrote files, it is accepted only with a file-level diff where `schedule.yaml` is unchanged, and it is reported as a file-level result. |
| W-5 | NVS: no key, type or value-hash change in identity namespaces (device_cfg, certs, wifi and provisioning namespaces as found in PRE-4). Changed keys must be within an enumerated set (event_log cursor/blob/frontier, watchdog latches, OTA/boot bookkeeping) and are listed as changed, not "preserved". |
| W-6 | Otadata before and after every transition: both sectors parsed and recorded. This is the OTA metadata change and is not presented as a preservation result. |
| W-7 | First HIL boot: the bootloader must report PENDING_VERIFY; the log must show `booted a PENDING_VERIFY image … confirming via MQTT` then `image confirmed valid (MQTT + persistence healthy)`; `ota_status` then reads VALID. Known side effect: the production confirm path publishes an `ota_report("success", <latched id>)` on the dev broker; this is recorded. No confirm within the production 300 s budget → STOP, and the device is reset so the bootloader rolls back to `ota_1`. No injection before VALID. |

## 4. Hardware checks

### 4.0 Global oracles (after every phase and every reset)

**Sources:**

- `ACC`: all `EVQ_ACC` rows.
- `REF`: all `EVQ_REF` rows.
- `IND`: records in any source with `device=evq_hil` whose (run, k) has no `EVQ_ACC`.
- `REAL`: non-HIL records with an id ≥ the first test id.
- `PRE_FLASH`, `SD_BASE`.
- `FL`: the latest `flash_inv`.
- `SL`: the latest `sd_inv lines` of `/sdcard/events`, `/sdcard/evq` and `/sdcard/archive`, excluding `bad-*` and `xlk-*`, which are reported separately.
- `WH`: warehouse rows.

| ID | Rule |
| --- | --- |
| HW-INV | Every id < the inventory's `cutoff_id` in `ACC ∪ IND ∪ REAL ∪ PRE_FLASH-pending` has its exact line sha256 in `FL ∪ SL`, or has been delivered (a `WH` row passing D-2). Ids ≥ `cutoff_id` carry forward to the next inventory. Before P7 the `WH` term is empty for HIL runs. |
| ID-UNIQ | Across `FL ∪ SL ∪ ACC ∪ IND ∪ PRE_FLASH ∪ SD_BASE`, each measure_id maps to exactly one **distinct** line sha256. Byte-identical duplicate occurrences are legal and counted. No `EVQ_REF` id is in any source. |
| IND-OK | Each `IND` record's line parses; its payload recomputes from (run, k, bytes); its id lies inside that fill's `[EVQ_BEGIN.next_id, next EVQ_BEGIN.next_id)`. |
| CNT | `evlog` `pending == flash_pending + sd_pending + reimport_pending`, and it equals the undelivered count derived from `FL ∪ SL` and the cursor when `pending_exact=1`. |

### 4.1 Phases

**P3 First boot and baseline** (gate hold, SD held): W-7, then §2.1 steps 1–4**.**

| ID | Check |
| --- | --- |
| B-1 | Running `ota_0`; `app_version` and ELF sha prefix equal the build. |
| B-2 | Schedule sha unchanged, INSTALLED, RUNNING; AMBIT versions unchanged; no `need flashing` / `auto-flashing` line. |
| B-3 | `flash_inv` equals `PRE_FLASH` for every file older than the tail, and the tail is a prefix-extension (new real records only). |
| B-4 | After `sd_release`, the first keeper pass leaves every `SD_BASE` file byte-identical except legacy `/sdcard/events` files. Those may only be imported, with every line then found in `FL`, per the Sprint 01 import rule. |

**P4 Fill beyond flash **(gate hold; schedule running).

| ID | Check |
| --- | --- |
| F-1 | Run `hil-…-P4-01`: `fill <run> 0 3200 4000 20` (≈ 13.1 MB). Σ `EVQ_ACC.line_bytes` **> 9,830,400 B**, the whole storage partition. Zero `EVQ_REF`. |
| F-2 | Every 200 records (`evlog` + `status`): pressure notification by the first < 25 % free; `spool_files` grows; `reclaimed ≥ 1`; free ≥ 40 % after each pressure pass; `sd_pending > 0`; `storage_blocked=0`; all `refused_*` = 0; no keeper-service command exists. |
| F-3 | For every reclaimed seq, taken from `index` before reclaim and the trace: the index names primary P and mirror M, CID == card CID; `sd_inv` shows P and M with size == bytes and crc32 == crc, and line hashes equal the `FL` lines captured before reclaim. The trace order is `P.tmp fsync → rename → P verify-read`, `M.tmp fsync → rename → M verify-read`, index P line, then `remove ev-<seq>.log` on flash. Any flash remove without both → FAIL. Any trace drain in the phase with `lost` > 0 or a seq gap → FAIL. |
| F-4 | Nothing published: `WH` has 0 rows for the run. `pending` equals `ACC` + real pending (exact). |
| F-5 | `stacks`: evq_keeper ≥ 1024 B free (of 8 KiB), sync_runner ≥ 1024 B, every listed task ≥ 512 B. No uncommanded reset. |

**P5 Restart and interruption **(gate hold). Each item uses a fresh run id; the fill is re-issued after reboot; then quiescent inventories and §4.0 run.

| ID | Physical kind | Boundary |
| --- | --- | --- |
| R-1 | graceful CPU reset (`reboot` = `esp_restart`) | SD_ONLY present |
| R-2 | ROM system reset | `store.after_fsync_before_return` → exactly 1 `IND` record expected |
| R-3 | ROM system reset | `store.after_write_before_fsync` → 0 or 1 `IND`; a torn tail is flagged `EVQ_FT` and never delivered |
| R-4 | mid-`fwrite` ROM reset | `spool.mid_copy` |
| R-5 | ROM system reset | `spool.after_verify_before_index` |
| R-6 | ROM system reset | `mirror.rename.after_return` |
| R-7 | ROM system reset | `reclaim.after_remove_before_index` |
| R-8 | ROM system reset | `reclaim.after_index` |
| R-9 | ROM system reset | `compact.after_rename` |
| R-10 | ROM system reset | `archive.after_rename_before_verify` (in P7) |
| R-11 | ROM system reset | `ack.after_ram_prefix_before_nvs` (in P7) |
| R-12 | ROM system reset | `cursor.after_blob_before_legacy` (in P7) |

After each reset:

- the reset reason equals the expected kind;
- `ota_0` VALID;
- the RTC gate hold survived;
- §4.0 holds;
- the cursor is never ahead of an undelivered `ACC`/`REAL` id.

**P6 Media paths**

| ID | Kind (as reported) | Check |
| --- | --- | --- |
| M-1 | **Production park/unmount** (`sd_park`); card powered | During P7 with SD_ONLY at the head, for ≥ 10 min parked: the `claims` NOT_FINISHED count increases; the NOT_FOUND delta is 0; claim attempts ≤ 2 per fallback period plus wake notifications (no busy-spin); `head_block=sd_parked`; `cursor` shows byte-identical NVS `cur`/`rd_seq`/`rd_off` hashes and an unchanged RAM cursor; stores continue while flash has room. `sd_unpark` then resumes delivery without a command. |
| M-2 | **Injected** full (`reserve` > free) | `sd_state=full`, spooling pauses, flash acceptance continues to pressure, then refusals under the P1-9 exception in the change table. `reserve off` → a fill of 200 is fully accepted and the backlog spools. |
| M-3 | **Injected** EIO at `claim.sd_read_error` and `spool.verify_read_error` | Mirror used or backoff; no skip; §4.0. |
| M-4 | **Injected** CID mismatch | `sd_state=mismatch`, trace shows 0 SD ops on that card, delivery waits; `cid off` → resumes. |
| M-5 | Physical card removal/swap; SD rail or device power removal | **NOT exercised** by the pair. Reported as unexercised and never replaced by M-1–M-4 in any claim. |

**P7 Delivery and reconciliation**

| ID | Check |
| --- | --- |
| P7-0 | Before opening the gate: quiescent `flash_inv` + `sd_inv` → final `REAL` and `IND` sets. |
| D-1 | `gate auto` (production gate opens on external power; `force` only if Vin is absent, recorded). No manual drain. `pending` reaches the real-only steady state, and `sd_pending=0`, within 90 min. |
| D-2 | For every id in `ACC ∪ IND`, ≥ 1 `WH` row with: `client_id`; `experiment_id` == b3c8d242; sample `v=2`; `measure_id`; `startTicks_UTC == start_ms`; `endTicks_UTC == end_ms`; `channel == null`; `device=="evq_hil"`; `cmd_raw`; `tag=="MEASUREMENT"`; `metadata==null`; sha256(canonical compact `data`) == payload sha256; envelope `device_id` == MAC; `device_name`, `device_version`, `device_firmware` equal the `cfg` values; `workbook_version_id` equals the installed schedule header's (recorded from `schedule status`); **no `macros`**. For every `REAL` / `PRE_FLASH`-pending id: ≥ 1 row whose sample object is JSON-semantically equal to the stored line's payload (v3 objects, spliced verbatim) and whose `timestamp` equals the stored start time. |
| D-3 | Missing = 0. `REF ∩ WH = ∅`. Duplicates are counted per id, with bound ≤ (resets + replays) × 16 + REIMPORT segment record counts. The first `kinesis_arrival_time` per id is non-decreasing in cursor order within the 16-slot window. |
| D-4 | Transport evidence (PUBACK counts, `inflight`) is reported separately and never counted as delivery. |

**P8 Hardware rollback leg**

| ID | Check |
| --- | --- |
| RB-0 | Gate hold. Run `hil-…-P8-01`, sized to create SD_ONLY segments: `fill` of 1800 × 4000 B, which exceeds the reclaim trigger. |
| RB-1 | `boot_slot ota_1` + `reboot` → 2.3.3-rc.1 (the RTC gate hold does not exist there). It imports `/sdcard/events` primaries and delivers them. `WH` must contain every P8 `ACC` id (D-2 rules). Serial is observe-only. |
| RB-2 | `ota_select.py` selects `ota_0` (NEW) → the HIL image confirms VALID through the production path again (W-7). The foreign-cursor rule marks the range REIMPORT, mirrors are re-imported and delivered (duplicates counted), and the firmware itself retires the P8 mirrors. §4.0 holds, with no stuck block. |

**P9 Operational surfaces** (throughout)

| ID | Check |
| --- | --- |
| O-1 | `evlog`, `status` and the STATUS `storage.evq` in warehouse telemetry rows agree field by field, with no truncation (JSON parses; the CLI renderer returns ≥ 0). |
| O-2 | Each state seen (ok, full, parked, mismatch, backlog wait, blocked) carries its reason token. |
| O-3 | Every reset reason is commanded or injected. no-PUBACK, conn-health and memory watchdog latches are unchanged, or any change is explained. |
| O-4 | Version and slot are re-read at the end of every phase. |
| O-5 | Schedule health: per job, the `runs` delta ≥ the due count over the window − 1; `fail` and `skip` deltas = 0; no sched overrun/drop/missed log lines; the schedule sha is unchanged. |

## 5. Restore and cleanup

| ID | Rule |
| --- | --- |
| C-1 | `gate auto`, `reserve off`, `cid off`, `keeper run`, unpark. `state` shows no override, and `status` shows the gate as auto. |
| C-2 | Final firmware: before switching, `evlog` must show `sd_pending=0` and `reimport_pending=0`, and no synthetic mirror may remain (`sd_inv /sdcard/evq`). Then `boot_slot ota_1` → the original 2.3.3-rc.1, re-verified (slot, version, schedule sha, AMBITs). `ota_0` keeps the HIL image, inactive. |
| C-3 | **No deletions by the pair.** Synthetic `arc-*`, `xlk-*` and `bad-*` files stay and are listed in the final manifest with their classification (all-synthetic / mixed). |
| C-4 | Final evidence: a post-test dual backup (same method, private); W-3/W-4/W-5/W-6; `SD_FINAL` vs `SD_BASE`, where every non-log `SD_BASE` file must be byte-identical except documented legacy imports; `PRE_FLASH` reconciliation, where every pre-existing line is found identical in the final flash/SD state or delivered. |

## 6. Stop conditions

Any of the following stops all writes. The Generator reports; the device is left as is or rolled back.

- An identity, backup or partition mismatch.
- W-7 not reached.
- An uncommanded reset.
- An AMBIT flash attempt.
- A schedule sha change.
- A littlefs mount failure.
- `SD_BASE` S1 ≠ S2.
- Any §2.1 step 4 mismatch.
- Any `EVQ_REF` outside the M-2 interval.
- Any §4.0 violation.
- The warehouse unreachable for more than 2 h during P7, reported as a blocker, not a pass.

**Rollback:**

- From the HIL image: `boot_slot ota_1`.
- From ROM: `ota_select.py --slot 1`.
- Last resort: rewrite only the `ota_0` region or otadata from the private backup.
- Never a full-image restore over NVS or storage without Planner approval.

## 7. Serial ownership and evidence

- **Token:** `sprint-02/serial-owner.json` `{owner, since, released_at, state}`, written only by the current owner.
- **Handoff:**
  1. The owner stops every process on the port, verifies `fuser /dev/ttyACM0` is empty, and writes `released_at` plus the device state (phase, slot, gate, hold, pending).
  2. The owner messages the recipient with `expectReply:true`.
  3. The recipient confirms and writes ownership.
- esptool counts as a port user. At most one owner at any time.
- **Evidence:**
  - Public: `sprint-02/evidence/<phase>/*.json` (hashes, counts, id/time/sha manifests, redacted log excerpts).
  - Private: `~/.local/share/ambyte-evq-hil/` (`0700`): backups, raw serial logs, raw warehouse extracts.
- **Redaction:** no identity NVS values, certs/keys, Wi-Fi credentials, or AWS/Databricks tokens. Published log lines matching `cert|key|psk|pass|token|secret` are dropped.

## 8. Non-goals and limits

- No fleet, release, policy, production replay, prod warehouse writes, workbook change or AMBIT change.
- M-5 is not exercised.
- A 244 GB card is not physically filled; "full" is injected.
- ROM/CPU resets do not remove SD or device power.
- Every verdict line carries its physical/injected/reset kind.
