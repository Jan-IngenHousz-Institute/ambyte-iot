# MQTT payload reference (persisted event schemas v3)

This reference defines three persisted-event object families. The AMBIT trace
measurement object is implemented by **two producers**:

- the **Ambyte** envelope builder (`cmd_mqtt_publish_next_event()` in
  [components/device_commands/device_commands.c](../components/device_commands/device_commands.c)
  + the run-payload builder in
  [components/lua_runner/lua_runner.c](../components/lua_runner/lua_runner.c)),
  which decodes the AMBIT's binary FSM arrays and wraps them for MQTT, and
- the **AMBIT firmware's own openJII JSON envelope** (`frontend_json.cpp` in the
  ambit repo), used when a host app drives the sensor directly over serial.

Both must emit the identical *trace measurement object* (§3); they differ only
in the transport-filled fields (§4). The Ambyte also produces the telemetry
and attached-device objects in §9 and §10. This document is normative for
these persisted-event schemas: until the implementations land, the spec wins;
after that, divergence is a bug in whichever side moved.

**Status:** contract. Firmware on `main` still emits v2 (see §8 for the v2→v3
mapping and the dual-read rule). The v2 spec is recoverable with
`git show c713c52^:docs/mqtt-payload.md`.

## 1. Model: store, then publish (unchanged from v2)

Lua scripts measure and store events (`db.store_event`, `ambit.run{store=true}`)
into the append-log event store; the `sync_runner` background task is the sole
publisher and drains the store when MQTT is connected, no UART measurement is
in progress, the external-power gate is open, and the clock is valid
(≥ 2024-01-01 UTC). One stored event (one `measure_id`) becomes exactly one
MQTT message, FIFO. QoS 1, retain 0, publish window of 16 slots / 64 KiB
outstanding; delivery is **at-least-once** and the cloud does not dedupe —
consumers dedupe on (`device_id`, `measure_id`).

## 2. Envelope (outer object — Ambyte only, unchanged from v2)

```jsonc
{
  "sample": [{ /* exactly ONE persisted event object: §3, §9, or §10 */ }],
  "timestamp": "2026-08-05T21:26:00Z",  // event observation/start time as ISO-8601
  "device_battery": 3.912,              // volts; omitted if never read
  "timezone": "Europe/Amsterdam",       // NVS; omitted if unset
  "device_id":       "10:00:3B:72:22:44", // Ambyte STA MAC
  "device_name":     "AmbyteOnAir",
  "device_version":  "1",
  "device_firmware": "1"
}
```

`sample` stays an array purely for compatibility with the cloud's `sample:[…]`
ingestion contract; the firmware always sends one element. The envelope
`timestamp` is the **event** time (battery-queued events carry their capture
time, not publish time): `time.start_utc` for `ambit.trace/3`, and
`time.observed_utc` for `ambyte.telemetry/1` and `ambit.device/1`. The other
envelope fields are populated exactly as in v2. `device_battery` is the latest
gateway battery reading at envelope-build time and is not a replacement for
the observation-time value in `ambyte.telemetry/1.health.power`.

The schema belongs to the single `sample[0]` object. Producers MUST NOT put a
schema tag on the outer envelope, mix v2 and v3 keys in one sample object, or
batch multiple stored events into `sample`. Delivery remains at-least-once and
consumers use (`device_id`, `measure_id`) as the event identity (§1).

## 3. The v3 measurement object (normative, both producers)

```jsonc
{
  "schema": "ambit.trace/3",            // REQUIRED. Self-identifying schema tag.
  "measure_id": 26337,                  // transport-filled, OPTIONAL (§4)
  "channel": "uart_1",                  // transport-filled, OPTIONAL (§4)
  "device": "AmbitV003",                // sensor self-identification (ambit_name)
  "sensor_id": "10:91:A8:4F:4F:D4",     // stable AMBIT efuse MAC (see §4)
  "tag": "MEASUREMENT",                 // transport-filled origin enum
  "time": {
    "start_utc": 1785965160359,         // epoch ms, host clock, command start
    "end_utc":   1785965213985,         // epoch ms, host clock, command end (see note)
    "duration_ms": 50386                // sensor-measured run duration (wrap-safe
  },                                    //   diff of the AMBIT's µs tick pair)
  "protocol": {
    "name": "SS",                       // free-form label (Lua/user metadata); OPTIONAL
    "id": "b1946ac9-…",                 // registered openJII protocol id; OPTIONAL —
                                        //   present only when the run was launched from
                                        //   a platform protocol. See the delivery note
                                        //   below: no Ambyte-side mechanism exists yet.
    "cmd": "arrun 1,0,2,0,0,59,0,1,0,1",// replayable device-vocabulary command
    "segments": [                       // decoded stimulus, one per arrun line
      { "pulses": 59, "freq": 1, "actinic": 0 }   // actinic = post-PAR→DAC byte
    ],
    "cal_version": "6a4356a8",          // CRC32 over the AMBIT calibration struct;
                                        //   join key to the DEVICE_INFO event
    "tick_factor": 0.854,               // point-period scale: true dt = tick_factor/freq
    "gains": [1,1,2,2,1,1],             // OPTIONAL, set-time tracked (no read-back)
    "currents": [55,0,10]               // OPTIONAL, same
  },
  "series": { /* §5 */ }
}
```

Notes:

- `time.start_utc`/`end_utc` bracket the **host command window** (on the
  Ambyte: trigger → fetch complete; `end_utc` can trail the optical end by up
  to one poll interval plus stream time). `duration_ms` is the sensor's own
  measurement window and is the authoritative run length.
- `schema` replaces v2's `"v": 2`. It is a string on purpose: it survives
  SD-card storage and platform exports where topic context is lost, and it
  names the object family, not just a number. Any breaking change to this
  object bumps the suffix (`ambit.trace/4`); additive optional fields do not.
- `device` is the human-readable `ambit_name`; it is mutable and not unique.
  `sensor_id` is the stable identity used with `protocol.cal_version` to join
  the trace to `ambit.device/1`. An Ambyte producer MUST include `sensor_id`.
  A direct/app producer MUST include it whenever the sensor or host exposes the
  efuse identity, and MAY omit it only for a legacy/direct path that genuinely
  cannot obtain that identity. Consumers MUST NOT substitute `device` as a
  stable join key when `sensor_id` is absent.
- Everything under `protocol` describes the **requested** stimulus. An
  interrupted run is not currently marked; consumers detect it by series
  lengths shorter than `segments` promise.
- **`protocol.id` delivery is future work on the Ambyte path.** On the lean
  ingest topic (no `{protocolId}` segment) this field is the pipeline's only
  protocol attribution — but today **no mechanism delivers a protocol id to
  an Ambyte**: the cloud→device script channel (`DeviceScriptMessage`) has no
  backend sender and carries no protocol id, and the NVS `protocol_id` key is
  static and unused. Until script delivery lands (and gains a `protocolId`
  field the Ambyte stores per job and surfaces via the Lua job metadata key
  `opts.metadata.protocol_id`), Ambyte-path measurements omit `protocol.id`
  and land with NULL protocol attribution on the lean topic — deliberately
  visible here rather than silently "optional". Direct/app-path producers
  fill it from the workbook's protocol context, which exists today.

## 4. Sensor-known vs transport-filled fields

The measurement object is produced twice; fields split by who can know them:

| class | fields | Ambyte path | direct/app path |
|---|---|---|---|
| sensor-known (identical from both producers when available, given §5's encoding rules) | `schema`, `device`, `sensor_id`, `series`, `protocol.segments`, `protocol.cal_version`, `protocol.tick_factor`, `time.duration_ms` | `sensor_id` is REQUIRED from the cached cmd-33 identity; series decoded from the binary FSM arrays; `segments` from the sent run bytes; `cal_version`/`tick_factor` from the cal-struct read | emits `sensor_id` when the sensor/host exposes it; omission is allowed only for a legacy/direct host that genuinely cannot obtain it |
| transport-filled (the sensor has no wall clock or ID counter) | `time.start_utc`, `time.end_utc`, `measure_id`, `channel`, `tag`, `protocol.name`, `protocol.id`, `protocol.cmd`, `protocol.gains`, `protocol.currents` | Ambyte clock, event log, Lua job metadata | host app (phone/browser) clock and protocol context |

`measure_id` and `channel` are therefore **optional**: a host without an event
log omits `measure_id` (the pipeline's row hash covers identity) and a host
without multiple ports omits `channel`. The relative-time model (§6) is what
makes this split work: the sensor emits only time relative to run start, and
any host anchors it with `time.start_utc`.

## 5. Series (normative name table)

Each channel is a self-describing object:

```jsonc
"series": {
  "fluo_630_signal": { "u": "count", "t0": 0, "dt": 0.854, "v": [159, 164, …] },
  "fluo_630_ref":    { "u": "count", "t0": 0, "dt": 0.854, "v": [4605, 4604, …] },
  "ambient_sun_vis": { "u": "count", "t0": 0, "dt": 0.854, "v": [670, 678, …] },
  "ambient_leaf_ir": { "u": "count", "t0": 0, "dt": 0.854, "v": [760, 755, …] },
  "leaf_temp":       { "u": "Cel", "t": [0.0, 8.6, 17.1, 25.7, 34.2, 42.8],
                       "v": [25.29, 24.60, …] }
}
```

Fixed mapping from the AMBIT's FSM array index (send order in `PAM.cpp` /
`ambit_array_tag()`), **settled 2026-08-10 — spelled-out names, no
`long_name` attribute**:

| FSM idx | v2 key | v3 series | `u` | content |
|---|---|---|---|---|
| 0 | `env` | `leaf_temp` | `Cel` | MLX90632 leaf (object) temperature, °C, irregular cadence |
| 1 | `s_630` | `fluo_630_signal` | `count` | 630 nm chlorophyll-fluorescence signal, dark-corrected counts |
| 2 | `r_630` | `fluo_630_ref` | `count` | 630 nm fluorescence reference |
| 3 | `sun` | `ambient_sun_vis` | `count` | sun-facing photodiode, visible ambient (ADPD ch1; NOT the AS7341/PAR) |
| 4 | `leaf` | `ambient_leaf_ir` | `count` | leaf-facing photodiode, IR ambient (ADPD ch2; NOT a temperature) |
| 5 | `s_730` | `refl_730_signal` | `count` | 730 nm reflectance signal (IR-enabled runs only) |
| 6 | `r_730` | `refl_730_ref` | `count` | 730 nm reflectance reference |
| 7 | `timing` | — (consumed) | — | µs tick pair → `time.duration_ms`; never emitted raw in v3 |
| 8 *(new)* | — | → `leaf_temp.t` | — | env sample offsets, ms since run start (additive AMBIT array) |

Unit names come from the SenML units registry (RFC 8428): `count`, `Cel`.
Unknown future indices keep the `arr<idx>` fallback — under v3 an unknown
array is emitted as a series object with `u: "count"` and (`t0`,`dt`) of the
main sample clock. (Note: an idx-8-aware AMBIT publishing through a pre-v3
Ambyte shows up as a v2 `arr8` key — those ARE device-recorded env offsets,
and the compat view may use them.) There is still **no derived `fluo`
series** — the ratio is `fluo_630_signal[i] / fluo_630_ref[i]` downstream
(ref 0 → 0).

### Value encoding rules (normative for both producers)

Two independent emitters must produce byte-identical values; therefore:

- **`count` series values are integers in 0…65535.** Producers MUST clamp to
  65535 before emitting — the clamp is part of the contract, not a wire
  artifact. (On the Ambyte path the binary wire already clamps; the AMBIT's
  direct JSON emitter must clamp itself, since its internal values exceed
  uint16.) A value of exactly **65535 means saturated**.
- **`leaf_temp` values**: fixed two fractional digits (`25.29`, `24.60`) —
  the source is centi-°C.
- **`dt`, `t0`, `t` values**: seconds, rendered as `%.4f` with trailing
  zeros then a trailing dot stripped (`0.854`, `2`, `42.8`, `6.832`).
  Consumers must not assume more than 0.1 ms resolution.
- Plain decimal JSON numbers only — no exponent notation, no `NaN`/`Infinity`.
  An unavailable field is omitted, never null (JSON `null` is reserved for
  the envelope's v2-era `channel`/`device` semantics).

## 6. Time model (normative)

Sample *i* of a series is at

```
t_ms(i) = time.start_utc + 1000 · (
            t[i]                     if the series has explicit "t"
          | t0 + i · dt              otherwise )
```

with `t`, `t0`, `dt` in **seconds relative to run start**. A series carries
exactly one of the two forms: explicit `t`, or regular (`t0`,`dt`). There is
no third encoding.

**True sample period.** The nominal `freq` (Hz) in `protocol.segments` is not
the real point rate; the true period is `dt = tick_factor / freq` seconds
(`tick_factor` from the AMBIT calibration, currently 0.854). Producers compute
`dt` — consumers never re-derive it from `freq`.

**Multi-segment runs.** Series arrays concatenate across segments. When all
segments share one `dt`, a single (`t0`,`dt`) covers the run. When segment
frequencies differ, the producer emits **explicit `t`** for the whole series
(segment *k*'s samples continue the timeline: segment *k+1* starts at segment
*k*'s `t0 + n_k·dt_k`). Explicit `t` costs bytes only on mixed-frequency
runs, and keeps every consumer two-form — a piecewise descriptor was
considered and rejected (decision log, §15).

**Subsampled ambient channels.** With subsampling = 2 the ambient series are
the mean of 8 consecutive pulses; the producer emits
`dt' = 8·tick_factor/freq` and `t0' = t0 + 3.5·(tick_factor/freq)` (center of
the 8-pulse window). No consumer special-casing: the series stays
self-describing.

**Worked example** (`arrun 1,0,2,0,0,59,0,1,0,1`; 59 pulses @ 1 Hz,
`start_utc = 1785965160359`):
`dt = 0.854/1 = 0.854 s` → `fluo_630_signal[10]` is at
`1785965160359 + 1000·(0 + 10·0.854)` = `1785965168899` (epoch ms).

**Env timestamps.** `leaf_temp` cadence is irregular, so it always uses
explicit `t`. The offsets come from the AMBIT's array idx 8 (device-recorded).
Until the sensor firmware ships idx 8, the producer emits **estimated**
offsets and flags them:

```jsonc
"leaf_temp": { "u": "Cel", "t": [0, 8.6, …], "t_est": true, "v": [ … ] }
```

The estimator is **normative** (the v2 compat view, §8, must produce the same
numbers). Given `n` = the number of `leaf_temp` values actually received,
`freq₁` = the first segment's nominal frequency, and
`duration_s = time.duration_ms / 1000`:

```
Δ = max(2.0, 8/freq₁)                    // firmware env cadence model
if n > 1 and (n-1)·Δ > duration_s:       // clamp into the measured window
    Δ = duration_s / (n-1)
t_est[k] = k·Δ                           // k = 0 … n-1
```

The anchor is the firmware's pre-loop env sample at run start (`t = 0`);
in-run samples are gated by a 2000 ms minimum spacing on a loop that wakes
every `8/freq` s, hence `Δ = max(2.0, 8/freq₁)`. Runs that are ineligible for
in-run env sampling (actinic ≥ 50, `freq ≥ 50`, or env disabled) produce
exactly one sample at `t = 0`. Estimated times are accurate to about ±Δ;
`t_est` absent means device-recorded. Consumers needing better than ±Δ must
require idx-8 firmware.

## 7. Removed vs v2 (and where it went)

| v2 field | v3 |
|---|---|
| `v: 2` | `schema: "ambit.trace/3"` |
| `startTicks_UTC` / `endTicks_UTC` | `time.start_utc` / `time.end_utc` |
| `timestamp_local` | **removed** — derive from envelope `timezone` downstream |
| `published` | **removed** — publish latency is observable from broker/ingest timestamps; drain health lives in the telemetry heartbeat |
| `cmd_raw` | `protocol.cmd` |
| `metadata.segments` / `cal_version` / `gains` / `currents` | `protocol.*` |
| `metadata.protocol` (free-form) | `protocol.name` (+ optional `protocol.id`, see §3) |
| `metadata.sensor_id` / `device_id` / `deviceID` | `sensor_id` (first present spelling); omit only when the legacy row has none |
| `data.{env,s_630,…}` | `series.{leaf_temp,fluo_630_signal,…}` (§5) |
| `data.timing` | `time.duration_ms` |

## 8. Dual-read rule (v2 ↔ v3 coexistence)

- A persisted event object **with** a `schema` key uses the named schema. A
  trace measurement without it is v2 (`v: 2`, or the pre-rename `startTicks`
  spelling — both exist under v2).
- The platform keeps a compat view normalizing v2 rows to the v3 shape
  (renames per §7; `dt` reconstructed as `0.854/freq` from
  `metadata.segments`; `leaf_temp.t` estimated with `t_est: true`). It maps the
  first present v2 identity spelling in this order: `metadata.sensor_id`,
  `metadata.device_id`, `metadata.deviceID`. A v2 row with none is a genuine
  legacy exception: normalized `sensor_id` is absent and the row cannot be
  joined to inventory by its non-unique `device` name.
- Consumers must accept both shapes indefinitely. Old firmware backlogs on SD
  cards continue to publish v2, and new Ambyte firmware deliberately stores a
  completed trace as v2 when it cannot truthfully construct canonical v3:
  missing sensor identity or calibration timing, an out-of-range tick factor,
  duplicate/anomalous array indices, a known series longer than its decoded
  protocol/time model, or a canonical payload that exceeds its bounded output
  region while the denser v2 representation still fits. The fallback retains
  every raw array, uses `cal_version:null` when unavailable, and includes
  `metadata.sensor_id` whenever identity is known. It has no `schema` key and
  never mixes v2/v3 fields.
- `ambit.spec`, `ambit.temp`, and generic Lua `db.store_event` measurement
  families remain permanently v2 unless separately migrated. T5 dual-read and
  the v2 compat view are therefore permanent, not a backlog-drain window.

## 9. Ambyte telemetry object (normative, Ambyte only)

The firmware-owned five-minute heartbeat stores exactly one
`ambyte.telemetry/1` event. It reads the onboard BME280 once and puts the
science observations and operational health under sibling keys at one
observation time. It MUST NOT also store an automatic `device.bme280` event for
that reading, and BME280 values MUST NOT be copied into any channel-specific
`ambit.trace/3` event.

```jsonc
{
  "schema": "ambyte.telemetry/1",
  "measure_id": 26338,
  "device": "28:37:2F:FF:E7:04",
  "tag": "TELEMETRY",
  "time": { "observed_utc": 1785965213985 },
  "observations": {
    "air_temperature":   { "u": "Cel", "v": 24.67 },
    "relative_humidity": { "u": "%RH", "v": 61.20 },
    "air_pressure":      { "u": "Pa",  "v": 101325.0 }
  },
  "health": {
    "connectivity": {
      "wifi": true,
      "provisioned": true,
      "publish_gate": true,
      "mqtt_reconnects": 0,
      "last_disc_reason": "",
      "conn_age_s": 86400,
      "pending": 3
    },
    "power": {
      "battery_v": 3.912,
      "input_v": 5.040,
      "system_v": 3.920,
      "input_ma": 518,
      "charge_ma": 297,
      "input_present": true,
      "charge_status": 2
    },
    "storage": {
      "db_online": true,
      "sd_free_kb": 1832448,
      "sd_skipped": 0,
      "sd_dropped": 0,
      "last_acked_id": 26335,
      "sd_io_lost": false
    },
    "runtime": {
      "uptime_s": 86400,
      "psram_free_kb": 7210,
      "psram_largest_kb": 7168,
      "psram_size_kb": 8192,
      "heap_dma_largest_kb": 32,
      "heap_int_free_kb": 121,
      "heap_int_largest_kb": 64,
      "wd_armed": true,
      "last_wd_reboot_reason": ""
    },
    "clock": { "source": "rtc", "suspect": false },
    "software": {
      "firmware": "1.6.6",
      "script_sha256": "7c222fb2927d828af22f592134e8932480637c0d2d88184a5be625042c22a6cd",
      "script_version": "1.0.0",
      "script_built_against_fw": "1.6.6",
      "script_installed_on_fw": "1.6.6",
      "script_metadata_verified": true
    },
    "attached_sensors": [
      {
        "channel": "uart_0",
        "sensor_id": "10:91:A8:4F:4F:D4",
        "firmware": "0.1.0",
        "hardware_revision": 1,
        "name": "AmbitV003",
        "cal_version": "6a4356a8"
      }
    ]
  }
}
```

`observations` and `health` are REQUIRED objects, even when empty. A failed
BME280 read produces `"observations": {}`; it does not suppress the health
snapshot or emit null-valued scalars. The seven health members shown above are
REQUIRED containers; their leaves are omitted when the source is unavailable.
This stable grouping lets the platform project environment and health views
from the same raw row, with the same `measure_id` and observation time, without
republishing or duplicating that row.

### 9.1 Top-level and observation fields

| field | req. | type / unit | meaning |
|---|---:|---|---|
| `schema` | yes | string constant | Exactly `ambyte.telemetry/1`. |
| `measure_id` | yes | integer | Ambyte event-log id; unique only within the gateway. |
| `device` | yes | string | Ambyte STA MAC, uppercase colon-separated hex. |
| `tag` | yes | string constant | Exactly `TELEMETRY`; legacy `STATUS` is not emitted for this persisted heartbeat. |
| `time.observed_utc` | yes | integer, epoch ms | Time at which the grouped snapshot is taken. The single BME280 read and health probes belong to this snapshot. |
| `observations.air_temperature` | no | scalar `{u:"Cel",v:number}` | Onboard BME280 air temperature in degrees Celsius. |
| `observations.relative_humidity` | no | scalar `{u:"%RH",v:number}` | Onboard BME280 relative humidity in percent, 0–100. |
| `observations.air_pressure` | no | scalar `{u:"Pa",v:number}` | Onboard BME280 pressure in pascals. |

Every scalar has exactly `u` and `v`; `u` is the constant shown in the table,
and `v` is a finite JSON number. Missing readings are omitted, never null. This
is deliberately the scalar counterpart of the unit-bearing series objects in
§5, not a second environment-specific schema.

### 9.2 Health fields

All sizes are KiB (1024 bytes), times are seconds, voltages are volts, and
currents are milliamperes. Unless the type says otherwise, counters are
non-negative integers.

| group | leaf | type / unit | v2 source and meaning |
|---|---|---|---|
| `connectivity` | `wifi` | boolean | `wifi`; gateway has a Wi-Fi connection. |
| | `provisioned` | boolean | `provisioned`; Wi-Fi credentials are provisioned. |
| | `publish_gate` | boolean | `publish_gate`; external-power publication gate is open. |
| | `mqtt_reconnects` | integer | `mqtt_reconnects`; MQTT connection count for this boot. |
| | `last_disc_reason` | string | `last_disc_reason`; most recent Wi-Fi/MQTT disconnect reason, empty when none is known. |
| | `conn_age_s` | integer, s | `conn_age_s`; current MQTT connection age, `-1` when unavailable. |
| | `pending` | integer | `pending`; persisted events awaiting acknowledgement. |
| `power` | `battery_v` | number, V | Charger battery voltage. |
| | `input_v` | number, V | Charger input voltage. |
| | `system_v` | number, V | Charger system-rail voltage. |
| | `input_ma` | integer, mA | Charger input current. |
| | `charge_ma` | integer, mA | Battery charge current. |
| | `input_present` | boolean | External input-power detection. |
| | `charge_status` | integer | Raw MP2731 charge-state enum; preserve 0–3 without reinterpretation. |
| `storage` | `db_online` | boolean | Event log is available. |
| | `sd_free_kb` | integer, KiB | Free SD-card space. |
| | `sd_skipped` | integer | Records skipped as unreadable. |
| | `sd_dropped` | integer | Records dropped by persistence. |
| | `last_acked_id` | integer | Highest event id durably acknowledged. |
| | `sd_io_lost` | boolean | SD I/O-loss latch is set. |
| `runtime` | `uptime_s` | integer, s | Monotonic uptime. |
| | `psram_free_kb` | integer, KiB | Free PSRAM. |
| | `psram_largest_kb` | integer, KiB | Largest free PSRAM block. |
| | `psram_size_kb` | integer, KiB | Total PSRAM. |
| | `heap_dma_largest_kb` | integer, KiB | Largest DMA-capable heap block. |
| | `heap_int_free_kb` | integer, KiB | Free internal heap. |
| | `heap_int_largest_kb` | integer, KiB | Largest free internal-heap block. |
| | `wd_armed` | boolean | Connectivity watchdog is armed. |
| | `last_wd_reboot_reason` | string | Persisted watchdog reboot reason, empty when none is known. |
| `clock` | `source` | string enum | `clock_src`; `rtc`, `hwm` (persisted high-water mark), or `sntp`. Unknown future values are preserved. |
| | `suspect` | boolean | `clock_suspect`; wall clock may be wrong. |
| `software` | `firmware` | string | `app_version`; running Ambyte firmware version. |
| | `script_sha256` | 64-char lowercase hex string | Digest of the active Lua file. |
| | `script_version` | string | Independently released Lua version. |
| | `script_built_against_fw` | string | Firmware version targeted by that Lua release. |
| | `script_installed_on_fw` | string | Firmware version running when the Lua release was installed. |
| | `script_metadata_verified` | boolean | Release metadata digest matches the active Lua file. |

### 9.3 Numeric encoding (normative)

The serialized precision from v2 remains part of the compatibility contract:

- `observations.air_temperature.v` and
  `observations.relative_humidity.v` use exactly two fractional digits
  (`24.67`, `61.20`); `observations.air_pressure.v` uses exactly one
  (`101325.0`).
- `health.power.battery_v`, `input_v`, and `system_v` use exactly three
  fractional digits (`3.912`, `5.040`).
- Currents, sizes, counters, ids, uptime, and hardware revisions serialize as
  base-10 integers. Other floating calibration values follow §10, not this
  telemetry rule.
- Producers use plain finite decimal JSON numbers only: no exponent notation,
  `NaN`, or infinity. They omit unavailable values rather than serializing
  null. A v2 compat projection MUST produce the same precision from the source
  values so v2 and new-schema fixtures compare deterministically.

`health.attached_sensors` is an array of lightweight, cache-only
last-identified references; it MUST NOT contain calibration coefficients. Each
entry has required `channel` (`uart_0` through `uart_3`) and required
`sensor_id` (AMBIT MAC). `firmware`, `hardware_revision`, `name`, and the
eight-lowercase-hex-digit `cal_version` are optional cached references. At most
one entry exists per channel. The heartbeat MUST use only identity already in
the channel cache: it MUST NOT initiate UART I/O, ping a sensor, or contend
with a measurement run to populate or validate this array. Omission of a
channel means no cached identity/no claim and must not be interpreted as live
absence. Any future liveness field is optional and may be populated only from
state learned outside the heartbeat, without heartbeat-initiated UART I/O.

### 9.4 Emission rules and explicit BME280 reads

- The configured heartbeat period defaults to 300 s. Each due heartbeat stores
  exactly one `ambyte.telemetry/1` event, whether the BME280 read succeeds or
  fails. It never stores a companion environment event.
- `device.bme280{store=false}` remains a read-only Lua call and stores nothing.
  `device.bme280()` or `device.bme280{store=true}` stores one intentional extra
  `ambyte.telemetry/1` event with all three BME280 scalars and the complete
  required health shape (unsampled object groups are `{}` and
  `attached_sensors` is `[]`). It does not use a MEASUREMENT-only shape or
  create a parallel schema.
- An explicit stored BME280 snapshot gets its own `measure_id` and
  `time.observed_utc`; it is not folded into the preceding/following heartbeat.
- At every path, the gateway BME280 is gateway-level context only. Its values
  are never copied into `ambit.trace/3.series`, once per attached channel, or
  into `ambit.device/1`.

## 10. Attached AMBIT device object (normative, Ambyte only)

Identity and full calibration are slowly changing inventory, not heartbeat
telemetry. They are stored as a separate event:

```jsonc
{
  "schema": "ambit.device/1",
  "measure_id": 26339,
  "channel": "uart_0",
  "device": "AmbitV003",
  "tag": "DEVICE_INFO",
  "time": { "observed_utc": 1785965214102 },
  "identity": {
    "sensor_id": "10:91:A8:4F:4F:D4",
    "name": "AmbitV003",
    "firmware": "0.1.0",
    "hardware_revision": 1,
    "cal_version": "6a4356a8"
  },
  "calibration": {
    "mlx_coef": [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14],
    "adpd": [100, 101, 102, 103, 104, 105],
    "temp_offset": 0.0000,
    "temp_slope": 1.0000,
    "actinic_coef": 0.012345,
    "spec_coef": 1.000000,
    "act": [12, 24, 36, 48, 60],
    "mlx_emissivity": 0.9800,
    "sun_coef": 1.000000,
    "tick_factor": 0.854000
  }
}
```

| field | req. | type / unit | meaning |
|---|---:|---|---|
| `schema` | yes | string constant | Exactly `ambit.device/1`. |
| `measure_id` | yes | integer | Ambyte event-log id. |
| `channel` | yes | string | Port at observation time, `uart_0` through `uart_3`; location is not identity. |
| `device` | yes | string | AMBIT `ambit_name`; `ambit` only when a legacy normalized row has no name. |
| `tag` | yes | string constant | Exactly `DEVICE_INFO`. |
| `time.observed_utc` | yes | integer, epoch ms | Successful identity/calibration read time. |
| `identity.sensor_id` | yes | string | AMBIT efuse MAC, uppercase colon-separated hex. |
| `identity.name` | yes | string | `ambit_name` stored in calibration. |
| `identity.firmware` | yes | string | AMBIT `major.minor.batch` firmware version. |
| `identity.hardware_revision` | no | integer | AMBIT hardware revision; omit zero from pre-revision firmware. |
| `identity.cal_version` | yes | string | CRC32 of the exact calibration struct, rendered as eight lowercase hex digits. |
| `calibration.mlx_coef` | yes | array[14] integer | Raw MLX90632 calibration coefficients, in firmware struct order. |
| `calibration.adpd` | yes | array[6] integer | Raw ADPD calibration coefficients, in firmware struct order. |
| `calibration.temp_offset` | yes | number, Cel | Leaf-temperature offset. |
| `calibration.temp_slope` | yes | number, 1 | Leaf-temperature scale factor. |
| `calibration.actinic_coef` | yes | number, DAC count / (µmol m⁻² s⁻¹) | PAR-to-actinic-DAC coefficient. |
| `calibration.spec_coef` | yes | number, 1 | Spectral calibration coefficient. |
| `calibration.act` | yes | array[5] integer, DAC count | Calibrated actinic values for nominal PAR 50, 100, 150, 200, 250, in that order. |
| `calibration.mlx_emissivity` | yes | number, 1 | MLX object emissivity. |
| `calibration.sun_coef` | yes | number, 1 | Sun-channel scale coefficient. |
| `calibration.tick_factor` | yes | finite number, 1 | Dimensionless point-period scale used by `ambit.trace/3` (§6), strictly greater than 0 and no greater than 100. A complete calibration outside that sanity bound is not announced or persisted as a stable tuple. |

The stable identity/change tuple is
(`identity.sensor_id`, `identity.firmware`, `identity.cal_version`). The
producer persists this tuple across transient UART disconnects. It emits one
`ambit.device/1` after first complete discovery and another only when that
tuple changes. Reconnect, channel movement, or a repeated identical cmd-33
read does not by itself emit an event. A sensor replacement changes
`sensor_id`; a firmware update changes `firmware`; any byte-level calibration
change (including `name`) changes `cal_version`. The new event always carries
the complete `identity` and `calibration` objects; producers MUST NOT emit a
partial calibration announcement. `ambit.trace/3` references the announcement
with stable `sensor_id` + `protocol.cal_version` and, when hosted by an Ambyte,
the current `channel`; the human `device` name is descriptive only. A trace
never repeats the full calibration.

## 11. v2 STATUS, BME280, and DEVICE_INFO normalization

Normalization is one input persisted row to one canonical object. It does not
merge adjacent rows, synthesize a second raw row, or copy gateway observations
into AMBIT traces. The normalized row retains the input row's `measure_id` and
event time. A pipeline may expose environment and health projections from one
normalized telemetry row, but both projections point back to that same row.

### 11.1 v2 STATUS → `ambyte.telemetry/1`

Recognize a v2 STATUS sample by transport tag `STATUS` or legacy
`sensor:"status"`. Set the v3 constants and envelope-derived fields as follows:

| target | v2 source / rule |
|---|---|
| `schema` | constant `ambyte.telemetry/1` |
| `measure_id` | `measure_id` |
| `device` | sample `device` when it is a MAC; otherwise outer `device_id` |
| `tag` | constant `TELEMETRY` |
| `time.observed_utc` | `startTicks_UTC`, else `startTicks`; if neither exists, parse outer `timestamp` as epoch ms |
| `observations.air_temperature` | `data.temperature` → `{u:"Cel",v:...}` |
| `observations.relative_humidity` | `data.humidity` → `{u:"%RH",v:...}` |
| `observations.air_pressure` | `data.pressure` → `{u:"Pa",v:...}` |

Map each health leaf from the same-named v2 `metadata` key into the group in
§9.2, except `app_version` → `health.software.firmware`, `clock_src` →
`health.clock.source`, and `clock_suspect` → `health.clock.suspect`. Some
older STATUS producers placed health keys in `data` and emitted null
`metadata`; for every health leaf, fall back to the same-named `data` key when
the metadata key is absent. If both are present, `metadata` wins. The three
BME280 keys are observations only and are never copied into health.

For each zero-based `N` where `ambitN_id` exists, make one attached-sensor
entry: `channel = "uart_N"`, `sensor_id = ambitN_id`, `firmware = ambitN_fw`,
`hardware_revision = ambitN_hw` (omit zero), `name = ambitN_name`, and
`cal_version = ambitN_cal`. Omit absent optional leaves and do not create
entries for an `N` without `ambitN_id`. The compat view does not infer
liveness from the existence of cached v2 identity. Normalize `cal_version` per
§11.3.

### 11.2 v2 standalone `device.bme280` → `ambyte.telemetry/1`

Recognize the row by `cmd_raw:"device.bme280"` (or the equivalent retained
command column). Map `measure_id`, `device`, time, and the three `data` values
exactly as in §11.1; use outer `device_id` when sample `device` is null. Emit
the six health object groups as `{}` and `attached_sensors` as `[]`. This
normalized row represents the intentional BME280 snapshot itself and remains
separate from any STATUS row.

### 11.3 v2 DEVICE_INFO → `ambit.device/1`

Recognize the row by transport tag `DEVICE_INFO` or `cmd_raw:"get_info"`.

| target | v2 source / rule |
|---|---|
| `schema` | constant `ambit.device/1` |
| `measure_id` | `measure_id` |
| `channel` | sample `channel` |
| `device` | sample `device`, else `ambit` |
| `tag` | constant `DEVICE_INFO` |
| `time.observed_utc` | `startTicks_UTC`, else `startTicks`; if neither exists, parse outer `timestamp` as epoch ms |
| `identity.sensor_id` | `data.device_id` |
| `identity.name` | sample `device`, else `ambit` |
| `identity.firmware` | `data.fw` |
| `identity.hardware_revision` | sample metadata `hw_rev` when present and nonzero; legacy rows normally omit it |
| `identity.cal_version` | `data.cal_version` normalized below |
| `calibration.*` | same-named `data` field for every calibration leaf in §10 |

For `cal_version`, preserve an eight-digit hex string after lowercasing; convert
a numeric unsigned CRC32 to lower-case base-16, left-padded to eight digits.
The same rule applies to v2 STATUS attached-sensor references. A legacy
DEVICE_INFO row without the complete calibration remains queryable as legacy
inventory but cannot be represented as a conforming `ambit.device/1` object;
the compat view MUST flag or exclude it rather than invent coefficients.

## 12. Persisted experiment events vs command status

The three schemas in this document describe records stored in the append log
and published on the experiment data-ingest topic. OTA updates, Lua/script
updates, remote command acknowledgements, progress, failures, and terminal
results use the separate command **status topic**. Those replies are not
`ambyte.telemetry/1`, not `ambit.device/1`, do not enter the experiment event
log merely because they contain device state, and are outside this contract.

## 13. Topic

Unchanged by this document. The lean ingest topic
(`experiment/data_ingest/v1/{experimentId}/{sensorType}/{sensorVersion}/{sensorId}`,
openJII `feat/lean-ingest-topic`) carries **no protocol segment** — protocol
attribution on that topic comes exclusively from `protocol.id` (§3).

## 14. Size budget

The accepted production reference is the type-2 59-point run (four count
series, no 730-nm reflection arrays), using the fields the v2 producer actually
emits and no unavailable `protocol.id`. For identical values and compact
serialization, the raw inner wire strings are 1,655 B (v2) and 1,865 B (v3),
a 210-B increase (+12.69%). Complete MQTT envelopes with identical gateway
identity fields are 1,866 B and 2,076 B, also a 210-B increase (+11.25%). Production v3
is accepted when this full-envelope growth remains ≤ 13%; the executable
production-builder fixture locks both exact sizes and that bound.

The increase is required schema/identity, protocol/time, and self-describing
series naming/unit/time-model overhead. gzip of `sample`
(`_sample_encoding: "gzip+base64"`, already supported by the pipeline) is the
headroom lever and is implemented as an opt-in transport switch (§14a); it is
not part of the v3 payload schema itself.

## 14a. Transport gzip (opt-in, default off)

The publisher can gzip the v3 `sample` before the envelope is built. The
switch is the NVS-backed config key `publish_gzip` (`cfg set publish_gzip 1`,
read per publish, no reboot needed) and it **defaults to off**: fleets publish
plain v3 JSON until the OpenJII ingest confirms gzip support for this producer,
then the flag flips without a firmware change.

With the flag on, a canonical v3 event publishes as:

```json
{
  "sample": "<base64(gzip("[<canonical v3 object>]"))>",
  "_sample_encoding": "gzip+base64",
  "timestamp": "…", "device_id": "…", "device_name": "…",
  "device_version": "…", "device_firmware": "…"
}
```

Contract points:

- The compressed text is exactly the plain envelope's `sample` array
  (`[<object>]`), so Silver's existing `decompress_sample_value`
  (`gzip.decompress(base64.b64decode(sample))`) restores one identical shape
  for both transport modes. Only `sample`/`_sample_encoding` differ; the outer
  envelope stays plain JSON for broker routing and raw storage.
- Fail-open to plain: any compression failure (allocation, compressor error)
  or a not-strictly-smaller result publishes the ordinary plain v3 envelope.
  Enabling the flag can therefore never lose, delay, or grow a measurement.
- Scope: canonical v3 families only. The frozen v2 backlog/fallback path and
  the on-disk event_log record never compress; the flag changes transport
  encoding, not storage.
- Mechanism: RFC 1952 gzip (header `1f 8b 08 00 … ff`, CRC-32 + ISIZE trailer)
  around a raw-DEFLATE body from the ESP32-S3 ROM tdefl compressor (128
  probes ≈ zlib level 6 — the benchmarked setting: the 60-point compact v3
  fixture measured 1,707 B plain vs 920 B gzip+base64). Framing, CRC-32, and
  base64 live in the host-tested domain module `payload_gzip.c`; the publish
  cap, heap gate, and MQTT window all account the real compressed envelope.

Caps that bound the design: `AMBIT_RUN_PAYLOAD_CAP` 63,000 B (62,999 payload
bytes), `AMBYTE_PUBLISH_MAX_BYTES` ≈ 68 KiB (build cap), and the 128 KiB AWS IoT
hard limit. The smaller run cap reserves room for the unchanged v2 fallback's
bounded 1,536-B metadata plus the full command and event-log framing, so an
ordinary v3 representability failure cannot make the completed measurement
unstorable. `protocol.cmd` (≤ ~522 B) stays in the record's command column.

## 15. Decision log

| date | decision |
|---|---|
| 2026-08-10 | Series names are spelled-out (`fluo_630_signal`, …, table §5); no terse+`long_name` variant. |
| 2026-08-10 | `protocol.name` stays free-form; optional `protocol.id` joins the openJII `protocols` table and is the sole protocol attribution on the lean ingest topic. |
| 2026-08-10 | `v` int replaced by self-identifying `schema` string; dual-read keyed on its presence. |
| 2026-08-10 | Raw `timing` dropped from the payload; sensor duration surfaces as `time.duration_ms`. `timestamp_local` and `published` dropped as derivable/observable. |
| 2026-08-10 | Time encodings are exactly two: explicit `t`, or regular (`t0`,`dt`). A piecewise per-segment descriptor (`seg`) was considered and rejected — mixed-frequency multi-segment runs emit explicit `t` instead, keeping every consumer two-form. |
| 2026-08-10 | The `count` saturation clamp (65535) and the number-formatting rules (§5) are contract requirements on producers, not wire artifacts. |
| 2026-08-10 | `t_est` estimator formula (§6) is normative so the firmware fallback and the SQL compat view agree. |
| 2026-08-10 | One periodic raw `ambyte.telemetry/1` row owns sibling environment observations and grouped health; an explicit stored BME280 read uses the same schema, and gateway environment is never copied into AMBIT traces. |
| 2026-08-10 | Full `ambit.device/1` inventory is keyed for change detection by sensor id, firmware, and calibration CRC; reconnect alone does not announce it again. |
| 2026-08-10 | `ambit.trace/3.sensor_id` is the stable inventory join key; heartbeat attached-sensor references are cache-only and carry no required live-presence state. |
| 2026-08-10 | The production type-2 59-point full-envelope size gate is ≤ 13%; measured raw-wire v2→v3 growth is 1,866→2,076 B (+11.25%). gzip remains deferred. |
| 2026-08-10 | New firmware's lossless v2 trace fallback and the existing `ambit.spec`, `ambit.temp`, and `db.store_event` v2 families are permanent; platform dual-read support has no retirement date. |
| 2026-08-12 | Transport gzip of the v3 `sample` (`_sample_encoding: "gzip+base64"`, §14a) ships behind the NVS flag `publish_gzip`, default OFF, fail-open to plain JSON; the compressed text is byte-identical to the plain `sample` array so Silver's existing decoder needs no change. Chosen over SenML JSON/CBOR after exact same-trace benchmarks (920 B vs 1,740/1,900 B gzipped). |

## 16. On-disk record

The event_log v2 tab-separated record format is unchanged by this contract;
whether the stored `metadata`/`payload` columns move to the v3 field names is
an implementation decision of the builder change (documented in
[components/event_log/include/event_log.h](../components/event_log/include/event_log.h)
when it lands).
