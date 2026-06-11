# MQTT payload reference (schema v2)

What the firmware actually publishes, as implemented. The single envelope
builder is `cmd_mqtt_publish_next_event()` in
[components/device_commands/device_commands.c](../components/device_commands/device_commands.c)
— if this document and the code disagree, the code wins.

> Phases 1 (v2 wire format), 2 (`ambit.*` consolidation, fused stores,
> slimmed `db.store_event`) and 4 (firmware STATUS heartbeat + clock gate +
> status-LED blinker) of [payload-v2-plan.md](payload-v2-plan.md) landed
> 2026-06-11. Device discovery (Phase 3) is the remaining gap: `device` is
> hardcoded `"ambit"` on AMBIT store paths and `null` elsewhere.

## Model: store, then publish

Lua scripts never talk to MQTT. They *measure and store* events
(`db.store_event`, `ambit.run{store=true}`) into the append-log event store;
a background task (`sync_runner`) is the sole publisher and drains the store
when **all** of these hold:

- MQTT is connected,
- no UART measurement is in progress (measurement-activity gate),
- the publish power gate is open (external power present — on battery,
  events queue and flush when power returns).

One stored event (one `measure_id`) becomes exactly one MQTT message.
Events publish in store order (FIFO).

## Topic

```
<topic_root>/1234
```

- `topic_root` comes from NVS (`AMBYTE_TOPIC_ROOT`, e.g.
  `experiment/data_ingest/v1/<experiment-uuid>/multispeq/v1.0/AMBYTE{MAC}`;
  the `{MAC}` token is expanded at boot to the board's STA MAC).
- The cloud pipeline extracts `experiment_id` from the 4th topic segment;
  the `/1234` suffix is a hardcoded literal it ignores.

## Envelope

Built as a direct `snprintf` string splice — the stored `data`/`metadata`
JSON is inserted verbatim, no cJSON round-trip (heap reasons). Annotated
example:

```jsonc
{
  "sample": [{                          // always exactly ONE element
    "v": 2,                             // schema version
    "measure_id": 1000042,              // int64, unique per device (see below)
    "startTicks": 1765459200123,        // epoch ms, measurement start
    "endTicks":   1765459210456,        // epoch ms, measurement end
    "published": "2026-06-11T09:30:00Z",// UTC ISO-8601 at PUBLISH time
    "channel": "uart_1",                // physical port, null = onboard
    "device":  "ambit",                 // discovered sensor name, null = unknown
    "cmd_raw": "arrun 1,0,0,0,0,4,0,4,0,1", // full device-vocabulary command (with args), null = none
    "tag": "MEASUREMENT",               // origin enum (firmware-assigned)
    "metadata": { "segments": [ ... ] },// object, or null when absent
    "data":     { ... }                 // object — the measurement quantities
  }],
  "timestamp": "2026-06-11T07:02:11Z",  // = startTicks as ISO-8601 (MEASUREMENT time)
  "device_battery": 3.912,              // volts, last charger read; omitted if never read
  "timezone": "Europe/Amsterdam",       // AMBYTE_TIMEZONE (NVS); omitted if unset
  "device_id":        "10:00:3B:72:22:44", // STA MAC — NOT AMBYTE_DEVICE_ID
  "device_name":      "AmbyteOnAir",    // AMBYTE_DEVICE_NAME (NVS)
  "device_version":   "1",              // AMBYTE_DEVICE_VERSION (NVS)
  "device_firmware":  "1"               // AMBYTE_DEVICE_FIRMWARE (NVS)
}
```

Field notes:

- `sample` is an array purely for compatibility with the cloud's
  `sample:[…]` ingestion contract; the firmware always sends one element.
- **Envelope `timestamp` is the measurement time** (`startTicks` rendered as
  ISO-8601): the openJII pipeline aliases it to `measurement_time_utc`, so
  battery-queued events carry their capture time. Publish time lives in
  `sample[0].published`.
- `tag` is firmware-assigned, never user input: `MEASUREMENT` for anything a
  script originated; `STATUS` for the firmware heartbeat (one event per
  `AMBYTE_HEARTBEAT_S`, default 300 s, stored by sync_runner — survives a
  missing/crashed main.lua).
- `channel` is `"uart_<n>"` (0-based) for the UART sensor ports, `"usb_<n>"`
  reserved for the USB hub; JSON `null` = onboard source.
- `device` is best-effort sensor self-identification — `null` until Phase 3
  discovery lands (the AMBIT store path hardcodes `"ambit"` already).
- `measure_id` is a plain monotonic `int64` from the event log (starts at 1,
  reseeded above the highest id found on the SD card at boot). Unique **per
  device only** — and note the cloud does NOT dedupe: QoS-1 redeliveries land
  as duplicate rows, so consumers dedupe on (`device_id`, `measure_id`).
- `firmware_version` is gone from the envelope: the pipeline's schema never
  read it; `device_firmware` is the platform's firmware-tracking key.
- The identity strings are provisioned via `.env` → NVS (see
  [.env.example](../.env.example)); empty strings appear when unprovisioned.

## Event types (what fills the provenance + `data`)

`data` is whatever the producer serialises; the shapes below are what the
current firmware paths and the shipped schedule script produce.

Measurement commands **store by default** (`{store=false}` to probe without
storing); transport/diagnostic commands (`uart.query`, `ambit.query`,
`ambit.run_raw`, `ambit.leaf_temp_raw`, …) never store. `db.store_event{
data=, metadata=, channel= }` remains for derived/custom script events.

| Event | `channel` | `device` | `cmd_raw` | `data` |
|---|---|---|---|---|
| AMBIT trace (`ambit.run` / `trigger`+`fetch`) | `uart_<ch>` | `ambit` | `arrun <len>,<persist>,<bytes…>` | `{"env":[…],"s_fluo":[…],"r_fluo":[…],"sun":[…],"leaf":[…],"s_730":[…],"r_730":[…],"timing":[…]}` + `metadata.segments` (730 arrays only on IR-enabled traces) |
| Spectrum + PAR (`ambit.spec`) | `uart_<ch>` | `ambit` | `get_par` | `{"spec":[10 ints],"par":f}` |
| Leaf temperature (`ambit.leaf_temp`) | `uart_<ch>` | `ambit` | `get_temp` | `{"leaf":f,"chip":f}` |
| BME280 (`device.bme280` / CLI `record_env`) | `null` | `null` | `device.bme280` | `{"temperature":f,"humidity":f,"pressure":f}` |
| **STATUS heartbeat** (firmware, tag `STATUS`) | `null` | `null` | `null` | `{"wifi":b,"provisioned":b,"db_online":b,"publish_gate":b,"battery_v":f,"input_v":f,"system_v":f,"input_ma":n,"charge_ma":n,"input_present":b,"charge_status":n}` (power keys omitted if charger read fails) |
| Lua `db.store_event{}` (custom/derived) | `uart_<n>` if `channel=` given, else `null` | `null` (Phase 3 attaches the cache) | `null` | script-defined |

`cmd_raw` is the **full command in the target device's own vocabulary**,
arguments included — for the AMBIT, the literal ASCII command its console
accepts (`do_command.h`): `arrun <len>,<persist>,<byte,byte,…>` for a trace
(the segment bytes are the wire encoding; the binary sync run, cmd 21, and the
async trigger/fetch pair, cmds 22/24, are the same stimulus and store the same
command), and the argument-free `get_par` / `get_temp`. Onboard sources use
the firmware's logical name (`device.bme280`). `cmd_raw` is the literal command
(replayable); `metadata.segments` is the decoded, analysis-friendly view of the
same trace, and the AMBIT run/trigger `opts.metadata` table is merged into it.


### AMBIT `data` keys

Array index → JSON key, fixed in `ambit_array_tag()`
([components/lua_runner/lua_runner.c](../components/lua_runner/lua_runner.c)),
matching the AMBIT firmware's send order:

| idx | key | content |
|---|---|---|
| 0 | `env` | leaf temperature, °C (decoded, 2 decimals) |
| 1 | `s_fluo` | fluorescence signal (raw uint32) |
| 2 | `r_fluo` | fluorescence reference |
| 3 | `sun` | sun sensor |
| 4 | `leaf` | leaf sensor |
| 5 | `s_730` | 730 nm signal |
| 6 | `r_730` | 730 nm reference |
| 7 | `timing` | `[tick_begin, tick_end]` µs |

Unknown indices fall back to `"arr<idx>"`. All values except `env` are raw
uint32 counts. `metadata.segments` carries the wire-encoded stimulus
(`actinic` is the DAC byte after PAR→current conversion, not the requested
PAR value).

## Delivery semantics

- **QoS 1, retain 0**, one message in flight at a time.
- **Clock gate**: nothing publishes while the system clock reads pre-2024
  (RTC never set and no NVS flash-time) — prevents 1970-stamped events from
  landing in wrong cloud partitions. Stores proceed; the queue drains once
  the clock is set.
- PUBACK marks the event `SYNCED`; a publish failure or MQTT disconnect
  re-marks it `PENDING` for retry. Delivery is therefore **at-least-once**,
  and the platform keeps duplicates (no cloud-side dedupe) — dedupe on
  (`device_id`, `measure_id`) when analysing.
- Payloads larger than 128 KiB (the AWS IoT maximum) are still attempted
  but logged as likely to be rejected. The AMBIT run payload buffer is
  capped at 8 KiB (`AMBIT_RUN_PAYLOAD_CAP`); larger runs truncate with a
  warning and are not stored.

## On-disk record (event_log format v2)

One tab-separated line per event in `/sdcard/events/ev-*.log`:

```
<measure_id>\t<channel>\t<device>\t<tag>\t<cmd_raw>\t<start_ms>\t<end_ms>\t<metadata>\t<payload>\n
```

v1 (7-field) records are skipped as malformed. **Deploying v2 firmware over
a device with pending v1 records loses them** — drain the device first
(external power, watch pending hit 0) or delete `/sdcard/events/` at flash
time. This is a planned wipe, distinct from the corruption-only `.corrupt`
archive policy.

## Inbound messages

Moving to AWS IoT Jobs — see [ota-update-plan.md](ota-update-plan.md)
(Stage 2) and [device-script-delivery.md](../device-script-delivery.md).

## Caveat: host test client

The dummy payload sent by
[docs/mqtt_tls_test_client.py](mqtt_tls_test_client.py) `--publish`
(`{"sample":[{"protocol_id":…,"set":[]}]}`) is a connectivity smoke test
only — it does **not** match the firmware's envelope above.
