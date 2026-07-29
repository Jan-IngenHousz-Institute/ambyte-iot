# CLAUDE.md — ambyte-iot firmware

ESP32-S3 firmware for Ambyte field devices (plant-measurement loggers carrying up to 4 AMBIT
sensor boards over UART). Data flows: `main.lua` schedule → SD event log (append-only FIFO) →
QoS1 MQTT → AWS IoT Core (dev: account 084375565727, eu-central-1) → Kinesis/S3 →
Databricks `open_jii_dev.centrum.clean_data`.

## Commands

- Build: `pio run -e esp32-s3-devkitm-1` (ESP-IDF 5.5 via PlatformIO)
- Flash without touching device NVS/identity: `AMBYTE_NVS_SKIP=1 pio run -e esp32-s3-devkitm-1 -t upload`
- Bench diagnostics build: `pio run -e bench` (+ `bench-spiram-internal`, `bench-wifi-ps-none`,
  `bench-pm-none` single-toggle bisect envs) — see `docs/bench/RUNBOOK.md`
- If littlefs is missing at CMake: `git submodule update --init --recursive components/littlefs`
- Builds regenerate `sched_lua_embed.h`; keep it out of unrelated diffs
- Serial console: 115200 on `/dev/ttyACM0` (USB-JTAG; opening the port can reset the device).
  Useful CLI: `status`, `netwd [test]`, `inflight`, `evlog`, `cfg`, `wifi_join <ssid> <pass>`,
  `lua <start|stop|status|exec>`, `record_env`, `ambit_spec <ch>`, `ping_uart <ch>`, `reboot`

## Architecture (delivery pipeline invariants — do not break)

- **At-least-once, never skip**: the event_log cursor (+ NVS frontier, batched per 16 acks)
  advances past the *contiguous ACKED prefix only*, and only on the sync_runner task.
  Duplicates are acceptable (platform dedups on `(device_id, measure_id)` — flagged, verify);
  skips never are.
- **Windowed publisher (1.0.6)**: ≤16 slots / ≤64 KiB outstanding QoS1 envelopes; synchronous
  `esp_mqtt_client_publish` (NOT `enqueue` — esp-mqtt drains its outbox one message per task
  loop, enqueue serializes the wire); pre-publish reservation + early-ack parking makes sub-ms
  PUBACKs safe.
- **Task boundaries**: the esp-mqtt task touches only the portMUX latch table + completion
  queue — never event_log's `s_mtx` (blocking socket servicing *causes* disconnects). All
  event_log mutations happen on sync_runner.
- **Cap chain (compile-verified)**: `AMBIT_RUN_PAYLOAD_CAP` (64,000) < `EVLOG_RECORD_CAP_NORMAL`
  (65,552) < `AMBYTE_PUBLISH_MAX_BYTES` (record+4 KiB). PSRAM-absent boots fall back to the
  12 KB record cap at runtime.
- **Self-reboot paths** (nightly maintenance, conn-health, memory, no-PUBACK watchdogs) each
  have their own NVS anti-loop latch + uptime gate; maintenance lock (OTA/AMBIT flash) is an
  absolute veto. `wd test` must never write production latches.
- **version.txt is load-bearing**: STATUS telemetry reports the compiled `app_version` (the
  NVS `device_firmware`/`device_version` strings are junk — whole fleet says "1"). Bump it
  every release; IDF reads it at CMake *configure* time (touch CMakeLists.txt to force).
- STATUS schema (since 1.0.6): sample `data` = environment readings only; device health lives
  in sample `metadata`; `device` = MAC. Heartbeat every 5 min from the watchdog task.

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
