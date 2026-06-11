# Payload schema v2 — staged plan

Restructure the per-event (`sample[0]`) schema and the MQTT envelope so that
provenance is firmware-filled, semantics are uniform across sensors, and the
openJII ingestion pipeline receives correct measurement times. Companion to
[mqtt-payload.md](mqtt-payload.md) (documents the **current/v1** payload; gets
rewritten in Phase 5). Pre-beta: breaking changes are acceptable, no migration
shims.

## Decisions already taken

- **Provenance vs semantics split**: firmware fills `measure_id`, ticks,
  `channel`, `device`, `cmd_raw`, `tag`; main.lua supplies only `data` and
  optional `metadata` (an **object**, never a list).
- **`tag` is a firmware-controlled origin enum**, not user input:
  `MEASUREMENT` (anything script-originated), `STATUS` (background heartbeat),
  more later. Kills typo-fragmentation of the dataset.
- **`channel`** replaces `sensor`: `"uart_<n>"` / `"usb_<n>"`, `null` = onboard.
  USB numbering uses the hub **port** number (stable across boots), not
  enumeration order.
- **`device`** is best-effort sensor self-identification: discovered at boot,
  cached per channel, re-identified on ping failure, `null` when unknown.
  Nothing may rely on it being present.
- **`cmd_raw`** is the command that produced the data, in the **target
  device's own vocabulary** — for the AMBIT, the ASCII names from its
  firmware's `do_command.h` (`"arrun"` for any trace run incl. the async
  trigger/fetch pair, `"get_par"`, `"get_temp"`); the literal string for raw
  text queries; the firmware's logical name for onboard sources
  (`"device.bme280"`); `null` otherwise. Never hex frames (the segments
  metadata already encodes the AMBIT stimulus in decoded form).
  *(Revised 2026-06-11 from "Lua API names" — `ambit.fetch` told the analyst
  nothing about the stimulus.)*
- **Store is fused into typed measurement commands** (`store=false` to opt
  out). Transport/diagnostic commands (`ambit.query`, `uart.*`) never store.
  `ambit.trigger`/`poll` don't store — `fetch` is that pair's store point.
  `db.store_event` survives only for custom/derived events.
- **All AMBIT Lua APIs consolidate under `ambit.*`** (the cleanup
  [lua_runner.c](../components/lua_runner/lua_runner.c) already promises at
  `lua_register_ambit_module`). Namespaces are per protocol driver: `ambit.*`,
  `uart.*` (raw transport), `device.*` (onboard), future `co2.*` etc.
- **Heartbeat moves out of main.lua** into firmware (rides the sync_runner
  loop) so status telemetry survives a missing/crashed user script.
- **Envelope `timestamp` becomes the measurement time** (`startTicks` as
  ISO-8601). The centrum pipeline aliases it to `measurement_time_utc` and
  partitions by its date; publish time moves into the sample as `published`.
- **The platform does NOT dedupe** (silver row id hashes ingestion metadata;
  `measure_id` is never read). `measure_id` stays as the consumer-side dedupe
  key; duplicates on disconnect-before-PUBACK are documented, not fixed here.
- **New envelope fields the pipeline already understands**: `device_battery`
  (Double, aggregated per device) and `timezone` (IANA name → free local-time
  columns). `firmware_version` is *not* in the pipeline schema (dropped at
  bronze) — fold it into `device_firmware`, which is the platform's
  firmware-tracking groupBy key.
- `sample` stays a **JSON array of objects** (single element) — required by the
  pipeline's `$.macros` extraction; everything inside it is opaque VARIANT, so
  the inner restructure cannot break ingestion.

## Target contract

```jsonc
{
  "sample": [{
    "v": 2,
    "measure_id": 1000042,            // event_log counter (per-device unique)
    "startTicks": 1765459200123,      // epoch ms, measurement start
    "endTicks":   1765459210456,      // epoch ms, measurement end
    "published": "2026-06-11T09:30:00Z", // publish time (was envelope timestamp)
    "channel": "uart_1",              // "uart_<n>" | "usb_<n>" | null (onboard)
    "device": "ambit",                // discovery cache; null unknown
    "cmd_raw": "ambit.spec",          // logical cmd | literal text cmd | null
    "tag": "MEASUREMENT",             // origin enum: MEASUREMENT | STATUS | ...
    "metadata": { "segments": [...] }, // optional OBJECT, script-extendable
    "data": { ... }                   // firmware-defined per command
  }],
  "timestamp": "2026-06-11T07:02:11Z", // = startTicks ISO-8601  (CHANGED)
  "device_battery": 3.91,              // NEW: last known battery volts
  "timezone": "Europe/Amsterdam",      // NEW: provisioned IANA name, optional
  "device_id": "10:00:3B:72:22:44",    // STA MAC (unchanged)
  "device_name": "...", "device_version": "...",
  "device_firmware": "..."             // firmware_version folded in / dropped
}
```

## Open decisions (settle during Phase 1/2, none block starting)

1. `device_firmware` content after the fold — keep the provisioned string, or
   start sending the running app version (useful once self-OTA lands)?
2. Heartbeat default period (current main.lua uses 5 min) and its NVS key name.
3. Clock-invalid behaviour: Phase 4 gates *publishing*; do we also refuse to
   *store* (events measured before RTC sync get bogus startTicks either way)?
4. Whether `device.bme280()` fuses store-by-default like external-sensor
   commands (leaning yes — one mental model everywhere).

---

## Phase 1 — storage record + envelope v2 (plumbing)

Everything schema-shaped, end-to-end, while the producer call sites are only
minimally shimmed. After this phase the wire format IS v2.

- `measurement_event_t` + the `store_event`/`claim` fn typedefs gain
  `channel`, `tag`, `cmd_raw` (device already exists). All short controlled
  strings; same 24-byte caps and `sanitize_field` treatment as device/sensor.
- **event_log record v2**: 9 tab-separated fields
  `measure_id, channel, device, tag, cmd_raw, start_ms, end_ms, metadata, payload`.
  `parse_record` requires exactly 9; 7-field v1 rows are skipped with a log
  (pre-beta: deploy procedure is "drain or wipe `/sdcard/evlog`", see
  Migration).
- **Envelope builder** (`cmd_mqtt_publish_next_event`): emit the target
  contract — `v`, `published`, `channel`/`device`/`cmd_raw`/`tag` quoting with
  `null` for empty; envelope `timestamp` rendered from `e.start_ticks_ms` (not
  `time(NULL)`); add `device_battery` (latched from the last power read in
  `device_commands`, omit when never read); add `timezone` from NVS; drop
  `firmware_version`.
- **Provisioning**: new optional NVS key `AMBYTE_TIMEZONE` through
  `.env.example` → `tools/build_nvs_image.py` → `device_config` getter →
  `app_main` wiring.
- **Shims** (temporary, removed in Phase 2): existing call sites pass
  `tag="MEASUREMENT"`, `channel` derived from their channel arg
  (`"uart_%u"`), old sensor names move into `cmd_raw`-ish slots only where
  trivial; heartbeat keeps flowing via Lua with `channel=NULL`.

**Verification:** build + flash; store one event of each producer type;
subscribe with `mqtt_tls_test_client.py`; validate the JSON against the target
contract by eye + `python -m json.tool`. Confirm on a battery-queued event
(power gate closed → reopen) that envelope `timestamp` equals the measurement
time, not the publish time.

## Phase 2 — Lua API surface (the user-facing change)

- **`ambit.*` consolidation**: move `device.ambit_get_spec` → `ambit.spec`,
  `ambit_get_leaf_temp` → `ambit.leaf_temp`, `ambit_set_metadata` →
  `ambit.set_metadata`, `device.uart_ping` → `ambit.ping` (the 0xAA/0x80 wake
  is AMBIT protocol). Raw text/stream queries move to `uart.*`. No aliases —
  pre-beta, main.lua is updated in the same commit.
- **Fused stores**: `ambit.spec`/`ambit.run`/`ambit.fetch` (and
  `device.bme280` per open decision 4) store by default with
  `{store=false}` opt-out; each stamps its own `cmd_raw` logical name and
  channel; optional `metadata=` table merges with/extends the firmware-built
  metadata (segments).
- **`db.store_event` slims** to `{ data=MANDATORY, metadata=, channel= }`;
  `tag` forced to `MEASUREMENT`; when `channel=` is given, firmware attaches
  the cached `device` (Phase 3) and that channel's last `cmd_raw`; explicit
  `measure_id`/ticks overrides are removed.
- **Deletions**: `cmd_record_env`'s store path (CLI `record_env` becomes
  read-and-print or calls the fused bme280), `uart_text_query(save=true)`,
  and the stale "three rows" comment at lua_runner.c:162.
- Update [exampleMain.lua](exampleMain.lua) to the new surface (heartbeat
  still in-script until Phase 4).

**Verification:** run the updated example script on HW: spectra, a short
`ambit.run`, a trigger/poll/fetch cycle, a custom `db.store_event` — confirm
each lands with correct `tag`/`channel`/`cmd_raw` and that `ambit.trigger`
alone stores nothing.

## Phase 3 — device identity discovery

- Per-channel identity cache in the uart_sensor layer:
  `{ name[24]; valid; }` per channel.
- **Boot scan** (after UART init, before the Lua task): per channel, binary
  ping first (0xAA → 0x80 ⇒ `device="ambit"`); only if silent, ASCII
  `hello\n` with a short timeout, parse `{"name":…}` ⇒ that name. Order
  matters: ASCII must never hit a live AMBIT parser.
- **Re-identify** on ping failure (the existing 10 s ping cache is the hook)
  and on first successful contact on a channel whose identity is unknown —
  covers sensors plugged in after boot.
- Store paths read the cache; `device=null` stays legal everywhere.
- *Dependency note:* a real AMBIT name (vs the generic `"ambit"`) needs an
  identify command in the ambit firmware (31–34 query family precedent) and
  AMBIT-over-UART OTA to roll it out — out of scope here; the cache design
  absorbs it later without schema change.

**Verification:** boot with an AMBIT on ch0 and nothing on ch1–3: ch0 events
carry `"device":"ambit"`, others would carry `null`; unplug/replug mid-run and
confirm re-identification after a ping failure.

## Phase 4 — background heartbeat + clock-validity gate

- Heartbeat moves into firmware: sync_runner's loop stores a `STATUS` event
  (`channel=null`, `cmd_raw=null`, `data` = the status snapshot incl. power)
  every `heartbeat_s` (NVS-config, default per open decision 2). Delete the
  `status_heartbeat` job from exampleMain.lua. No new task — tight heap.
- **Clock gate**: sync_runner refuses to drain while the system clock is
  implausible (`time(NULL)` before a build-time floor, e.g. 2024-01-01);
  `store_event` logs a warning when stamping with an implausible clock.
  Rationale: 1970 timestamps pass the pipeline's not-null gate and land in
  wrong partitions, undetectably.

**Verification:** boot with no SD script — heartbeat still publishes. Boot
with RTC cleared — events queue, nothing publishes, log shows the clock gate;
set RTC, confirm drain resumes (already-stored bogus startTicks are accepted
loss pre-beta unless open decision 3 says otherwise).

## Phase 5 — docs, tooling, cloud asks

- Rewrite [mqtt-payload.md](mqtt-payload.md) for v2 (it currently documents
  v1); update the test client's dummy payload to the v2 shape; update
  `test/read_export.ipynb` filters (`sensor=="AMBIT_1"` → channel/tag).
- README pointer stays valid (already links mqtt-payload.md).
- **Platform team asks** (openJII): (a) silver-layer
  `dropDuplicates` on `(device_id, sample[0].measure_id)` — the pipeline
  currently keeps QoS-1 redeliveries as distinct rows; (b) confirm
  `timestamp` ISO format with `Z` suffix parses in their from_json config
  (Spark default accepts it).

## Migration / deploy notes

- **event_log format bump**: deploying Phase 1 firmware over a device with
  pending v1 records makes them unparseable (skipped, logged). Procedure:
  let the device drain fully (power it externally, watch pending hit 0)
  *before* flashing, or accept the loss and delete `/sdcard/evlog/` at
  flash time. This is a **planned wipe**, distinct from the corruption-only
  `.corrupt` archive policy.
- NVS re-flash at upload wipes runtime keys (cursor/next_id) anyway — the
  event_log reseeds `next_id` from the card; no extra action.
- The notebook/export filters break at Phase 1 (key renames) — pin the old
  notebook to old exports, or land Phase 5's notebook update alongside.
