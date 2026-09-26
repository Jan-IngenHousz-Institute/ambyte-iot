# CLAUDE.md — ambyte-iot firmware

ESP32-S3 firmware for Ambyte field devices (plant-measurement loggers carrying up to 4 AMBIT
sensor boards over UART). Data flows: declarative schedule → internal event log (littlefs `/evstore`, append-only FIFO; SD = overflow for unsent + bulk archive) →
QoS1 MQTT → AWS IoT Core (dev: account 084375565727, eu-central-1) → Kinesis/S3 →
Databricks `open_jii_dev.centrum.clean_data`.

## Commands

- Build: `pio run -e esp32-s3-devkitm-1` (ESP-IDF 5.5 via PlatformIO)
- Flash without touching device NVS/identity: `AMBYTE_NVS_SKIP=1 pio run -e esp32-s3-devkitm-1 -t upload`
- Bench diagnostics build: `pio run -e bench` (+ `bench-spiram-internal`, `bench-wifi-ps-none`,
  `bench-pm-none` single-toggle bisect envs) — see `docs/bench/RUNBOOK.md`
- If littlefs is missing at CMake: `git submodule update --init --recursive components/littlefs`
- The schedule runner generates `default_yaml_embed.h` under each build directory
- Timezone table: `python tools/gen_tz_table.py` regenerates the checked-in
  `components/timezone/tz_zone_table.inc` + `flash_gui/tz_zone_table.py` from IANA
  tzdata (never hand-edit them; `tests/test_timezone_config.py` asserts freshness)
- Serial console: 115200 on `/dev/ttyACM0` (USB-JTAG; opening the port can reset the device).
  Useful CLI: `status`, `netwd [test]`, `inflight`, `evlog`, `cfg`, `wifi_join <ssid> <pass>`,
  `schedule <status|run|start|stop|reload|actions|validate|release|install>`,
  `record_env`, `ambit_spec <ch>`, `ping_uart <ch>`, `reboot`,
  `selftest` (factory PCBA test; host runner: `python -m flash_gui.factory_test`)

## Architecture (delivery pipeline invariants — do not break)

- **At-least-once, never skip**: the event_log cursor (+ NVS frontier, batched per 16 acks)
  advances past the *contiguous ACKED prefix only*, and only on the sync_runner task.
  Duplicates are acceptable (platform dedups on `(device_id, measure_id)` — flagged, verify);
  skips never are.
- **A refused PUBACK is not delivery**: the client runs MQTT 5; a PUBACK reason `>= 0x80`
  (AWS `0x87 Not authorized` on a missing topic policy) is reported as `ESP_ERR_NOT_ALLOWED`,
  the record stays PENDING, the drain backs off (1–30 min) and the no-PUBACK watchdog counts
  it as liveness. Never map a reason-coded PUBACK back to `ESP_OK` (16–18 Sep 2026 lost
  ~0.8M records that way). Store capacity (~17 h at default cadence) is the outage budget.
- **Archive is append-only and named by first id**: `event_log_archive_to_sd` must never
  overwrite an existing `arc-*.log` (replayed ranges re-archive under `arc-<id>-<seq>.log`).
  `evlog_replay` re-appends archive records through `event_log_append_verbatim` behind the
  live queue and never moves the cursor; replayed ids publish without workbook provenance
  (`provenance_suppressed` port).
- **Windowed publisher (1.0.6)**: ≤16 slots / ≤64 KiB outstanding QoS1 envelopes; synchronous
  `esp_mqtt_client_publish` (NOT `enqueue` — esp-mqtt drains its outbox one message per task
  loop, enqueue serializes the wire); pre-publish reservation + early-ack parking makes sub-ms
  PUBACKs safe.
- **Task boundaries**: the esp-mqtt task touches only the portMUX latch table + completion
  queue — never event_log's `s_mtx` (blocking socket servicing *causes* disconnects). All
  event-log ACK/cursor mutations happen on sync_runner. Producers and the direct
  heartbeat allocate unique IDs under event_log's mutex; they never advance the cursor.
- **Cap chain (compile-verified)**: `AMBIT_RUN_PAYLOAD_CAP` (64,000) < `EVLOG_RECORD_CAP_NORMAL`
  (65,552) < `AMBYTE_PUBLISH_MAX_BYTES` (record+4 KiB). PSRAM-absent boots fall back to the
  12 KB record cap at runtime.
- **Storage layout (since the internal-store PR)**: events live on INTERNAL littlefs
  (`/evstore` = the 9.4 MiB `storage` partition — label is load-bearing, partition tables
  can't be OTA'd); `/littlefs/schedule.yaml` is installed atomically after compile
  validation and falls back to the embedded default. The SD card holds the
  **overflow of unsent records** (primary `/sdcard/events/ev-<first_id %06u>.log` +
  mirror `/sdcard/evq/m-<seq>.log`), the bulk archive, sd_logger and AMBIT firmware
  (docs/evq-sd-overflow.md). The STORE path never waits on the card (keeper task does all
  SD I/O, `s_mtx` → SD ref lock order); DELIVERY of SD-only segments does, and when their
  card is absent/parked/swapped/unreadable the cursor WAITS (`ESP_ERR_NOT_FINISHED`) —
  an indexed segment is never skipped. Primary names must stay `ev-%06u.log`: every
  released importer (v1.10.0–v2.4.2) rebuilds that exact spelling on rollback.
- **Overflow/retention invariants**: a rotated file moves to SD every 1000 stores or at
  once under flash pressure (<25 % free, reclaim to 40 %). A flash copy is reclaimed ONLY
  after both SD copies are durable and re-verified; mirror/flash copies of a delivered file
  are dropped only after a verified archive copy exists. Store `ESP_OK` means fsync'd.
  Eviction removes only delivered flash copies behind the cursor, never a REIMPORT source.
  A cursor moved by other firmware (legacy keys ahead of the `cur` blob) is never proof of
  delivery: the passed segments are re-imported. A `.tmp` left beside its committed `.log`
  on SD is retired to `/sdcard/evq/xlk-<n>.junk`, NEVER unlinked (an interrupted FAT
  `f_rename` can leave both names on one cluster chain); damaged primaries move aside to
  `evq/bad-*` rather than being deleted. Evidence: tests/evq_host (fault matrix,
  `run.py --matrix`) and tests/test_evq_*.py.
- **SD is treated as corruption-prone**: FATFS has no journal, so the SDMMC bus runs at
  20 MHz (40 MHz was marginal on this wiring) and the low-battery power guard in app_main
  parks the SD (sd_logger flush/close → unmount, `event_log_set_sd_parked`) below 3300 mV on
  battery. Measurement and the event store KEEP RUNNING through a park — internal littlefs
  is power-loss-safe; delivery of SD-only backlog waits until unpark. New SD writers
  must use sdcard_io_begin/end AND survive the park/unpark cycle (see sd_logger_pause).
- **Self-reboot paths** (nightly maintenance, conn-health, memory, no-PUBACK watchdogs) each
  have their own NVS anti-loop latch + uptime gate; maintenance lock (OTA/AMBIT flash) is an
  absolute veto. `wd test` must never write production latches.
- **semantic-release owns firmware versions**: the validated PR title determines the next
  `vX.Y.Z`, and PR CI injects that preview through `AMBYTE_PROJECT_VER` into the exact binary
  later promoted on `main`. Do not add a manual `version.txt`; local builds use ESP-IDF's
  Git-derived version. STATUS reports the compiled `app_version` (the NVS
  `device_firmware`/`device_version` strings are junk — the whole fleet says "1").
- **Two release units**: firmware keeps `vX.Y.Z`; `schedule/**` releases independently as
  `schedule-vX.Y.Z`. The path filter must be applied to commit analysis *and* release notes so a
  schedule-only commit cannot bump firmware later. Schedule assets carry SHA + built-against firmware.
- STATUS schema (since 1.0.6): sample `data` = environment readings only; device health lives
  in sample `metadata`; `device` = MAC. Heartbeat every 5 min from the watchdog task. Script
  release metadata is trusted only while its stored SHA matches `/littlefs/schedule.yaml`.

## Key dates / incident context (2026-07)

- Jul 27–28 incident: fleet-wide MQTT connection churn (~55 devices, site uplink suspected —
  RUTX50 + carrier CGNAT/fair-use candidates); devices' TCP writes stall (poll timeout,
  errno=0) and the serial publisher collapsed to ~10 msgs/connection. Separate per-device
  defect: TX-path stall after ~120–150k messages, heap-invisible (static pools), reboot-cured
  in isolation — root cause unproven, bench repro kit in `docs/bench/`.
- fw 1.0.6-rc1 (branch `traycer/ambyte-iot-ludo-brave-yak`, PR #1): tickets 01–06 + 08;
  bench-measured drain 2.4 → ~17 events/s. Fleet OTA gated on: measurement-quality A/B
  (gate split), degraded-link 64 KB publish demo, disconnect/power-cut corpus + 24 h soak,
  platform dedup live, site uplink fixed.
- Planning/review artifacts live in Traycer epic `c7e0ba78-5c54-4ac7-b4b6-4cf47a5608db`
  (tech plan, per-ticket review notes, changeset walkthrough for 1.0.6-rc1).

## Conventions

- Heavy rationale comments are the house style — a constraint's *why* belongs at its
  definition (this codebase reads like a field-incident logbook; keep it that way).
- Commits: Conventional Commits for every commit message and PR title —
  `type(scope): description` (e.g. `feat(evlog): …`, `fix(rtc): …`, `docs: …`,
  `test(bench): …`). No Claude co-author trailers.
- Fleet realities: all devices share one X.509 cert (fleet-provisioning migration planned);
  client ids are `AMBYTE_<MAC>`; ~500 devices/site is the target scale.
