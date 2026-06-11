# MQTT payload reference (schema v2)

What the firmware actually publishes, as implemented. The single envelope
builder is `cmd_mqtt_publish_next_event()` in
[components/device_commands/device_commands.c](../components/device_commands/device_commands.c)
— if this document and the code disagree, the code wins.

> Schema v2 wire format (Phase 1 of [payload-v2-plan.md](payload-v2-plan.md))
> landed 2026-06-11. The Lua API, device discovery, and the firmware heartbeat
> are later phases — the **Transitional notes** below mark what still carries
> v1 shims.

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
    "cmd_raw": "arrun",                 // device-vocabulary/literal command, null = none
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
  script originated; `STATUS` arrives with the Phase-4 firmware heartbeat.
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

| Event | `channel` | `device` | `cmd_raw` | `data` |
|---|---|---|---|---|
| AMBIT fluorescence run (`ambit.run` / `trigger`+`fetch`) | `uart_<ch>` | `ambit` | `arrun` | `{"env":[…],"s_fluo":[…],"r_fluo":[…],"sun":[…],"leaf":[…],"s_730":[…],"r_730":[…],"timing":[…]}` + `metadata.segments` |
| BME280 via CLI/`device.record_env` | `null` | `null` | `device.bme280` | `{"temperature":f,"humidity":f,"pressure":f}` |
| UART text query (`save=true`) | `uart_<n>` | `null` | the literal command | `{"response":"…"}` (empty string on timeout) |
| Lua `db.store_event{}` (spectra, heartbeat, custom) | `null` | script `device` field | *transitional:* the legacy `sensor` string | script-defined |

`cmd_raw` uses the **target device's own command vocabulary** — for the AMBIT,
the ASCII command names from its firmware (`do_command.h`): `arrun` for any
trace run (the binary sync run, cmd 21, and the async trigger/fetch pair,
cmds 22/24, are the same stimulus — sync vs async is transport). Phase 2's
fused commands follow suit: `get_par` (spectrum+PAR), `get_temp` (leaf temp).
Onboard sources use the firmware's logical name (`device.bme280`).

**Transitional notes (until Phase 2/4 of the plan):**
- `db.store_event`'s legacy `sensor` string (e.g. `"status"`, `"AMBIT"`)
  rides in `cmd_raw` so its discriminator survives; the Lua API slims to
  `{ data, metadata, channel }` in Phase 2.
- The status heartbeat is still a script job storing via `db.store_event`
  (so it appears with `tag:"MEASUREMENT"`, `cmd_raw:"status"`); it becomes a
  firmware task with `tag:"STATUS"` in Phase 4.

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
