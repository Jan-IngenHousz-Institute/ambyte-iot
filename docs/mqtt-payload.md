# MQTT payload reference (measurement schema v3)

The contract for AMBIT measurement objects, implemented by **two producers**:

- the **Ambyte** envelope builder (`cmd_mqtt_publish_next_event()` in
  [components/device_commands/device_commands.c](../components/device_commands/device_commands.c)
  + the run-payload builder in
  [components/lua_runner/lua_runner.c](../components/lua_runner/lua_runner.c)),
  which decodes the AMBIT's binary FSM arrays and wraps them for MQTT, and
- the **AMBIT firmware's own openJII JSON envelope** (`frontend_json.cpp` in the
  ambit repo), used when a host app drives the sensor directly over serial.

Both must emit the identical *measurement object* (§3); they differ only in the
transport-filled fields (§4). This document is normative for schema v3: until
the implementations land, the spec wins; after that, divergence is a bug in
whichever side moved.

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
  "sample": [{ /* exactly ONE measurement object, §3 */ }],
  "timestamp": "2026-08-05T21:26:00Z",  // = time.start_utc as ISO-8601 (measurement time)
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
`timestamp` is the **measurement** time (battery-queued events carry their
capture time, not publish time). STATUS and DEVICE_INFO events keep their v2
shapes — v3 changes only the MEASUREMENT sample element.

## 3. The v3 measurement object (normative, both producers)

```jsonc
{
  "schema": "ambit.trace/3",            // REQUIRED. Self-identifying schema tag.
  "measure_id": 26337,                  // transport-filled, OPTIONAL (§4)
  "channel": "uart_1",                  // transport-filled, OPTIONAL (§4)
  "device": "AmbitV003",                // sensor self-identification (ambit_name)
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
                                        //   a platform protocol. On the lean ingest
                                        //   topic (no {protocolId} segment) this is the
                                        //   pipeline's ONLY protocol attribution.
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
- Everything under `protocol` describes the **requested** stimulus. An
  interrupted run is not currently marked; consumers detect it by series
  lengths shorter than `segments` promise.

## 4. Sensor-known vs transport-filled fields

The measurement object is produced twice; fields split by who can know them:

| class | fields | Ambyte path | direct/app path |
|---|---|---|---|
| sensor-known (bit-identical from both producers) | `schema`, `device`, `series`, `protocol.segments`, `protocol.cal_version`, `protocol.tick_factor`, `time.duration_ms` | decoded from the binary FSM arrays | emitted by the AMBIT firmware itself |
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
Unknown future indices keep the `arr<idx>` fallback. There is still **no
derived `fluo` series** — the ratio is `fluo_630_signal[i] / fluo_630_ref[i]`
downstream (ref 0 → 0). Values are uint16-clamped on the wire: a sample equal
to **65535 means saturated**.

## 6. Time model (normative)

Sample *i* of a series is at

```
t_ms(i) = time.start_utc + 1000 · (
            t[i]                     if the series has explicit "t"
          | t0 + i · dt              otherwise )
```

with `t`, `t0`, `dt` in **seconds relative to run start**. Precedence:
explicit `t` wins over (`t0`,`dt`).

**True sample period.** The nominal `freq` (Hz) in `protocol.segments` is not
the real point rate; the true period is `dt = tick_factor / freq` seconds
(`tick_factor` from the AMBIT calibration, currently 0.854). Producers compute
`dt` — consumers never re-derive it from `freq`.

**Multi-segment runs.** Series arrays concatenate across segments. When all
segments share one `dt`, a single (`t0`,`dt`) covers the run. When segment
frequencies differ, the series carries a piecewise descriptor instead:

```jsonc
"fluo_630_signal": {
  "u": "count",
  "seg": [ { "t0": 0.0,    "dt": 0.854,  "n": 59 },
           { "t0": 50.386, "dt": 0.0854, "n": 100 } ],
  "v": [ /* 159 values */ ]
}
```

Segment *k+1*'s `t0` = segment *k*'s `t0 + n·dt` (contiguous timeline).
Precedence overall: `t` > `seg` > (`t0`,`dt`).

**Subsampled ambient channels.** With subsampling = 2 the ambient series are
the mean of 8 consecutive pulses; the producer emits
`dt' = 8·tick_factor/freq` and `t0' = t0 + 3.5·(tick_factor/freq)` (center of
the 8-pulse window). No consumer special-casing: the series stays
self-describing.

**Worked example** (`arrun 1,0,2,0,0,59,0,1,0,1`; 59 pulses @ 1 Hz,
`start_utc = 1785965160359`):
`dt = 0.854/1 = 0.854 s` → `fluo_630_signal[10]` is at
`1785965160359 + 1000·(0 + 10·0.854)` = `1785965168899` (epoch ms).

**Env timestamps.** `leaf_temp` cadence is irregular (≈ one sample per
`8/freq` s, gated by sleep/actinic conditions), so it always uses explicit
`t`. The offsets come from the AMBIT's array idx 8. Until the sensor firmware
ships idx 8, the Ambyte emits **estimated** offsets from its gating model and
flags them:

```jsonc
"leaf_temp": { "u": "Cel", "t": [0, 8.6, …], "t_est": true, "v": [ … ] }
```

`t_est: true` = times are model-derived, ±1 sampling interval; absence of
`t_est` = device-recorded.

## 7. Removed vs v2 (and where it went)

| v2 field | v3 |
|---|---|
| `v: 2` | `schema: "ambit.trace/3"` |
| `startTicks_UTC` / `endTicks_UTC` | `time.start_utc` / `time.end_utc` |
| `timestamp_local` | **removed** — derive from envelope `timezone` downstream |
| `published` | **removed** — publish latency is observable from broker/ingest timestamps; drain health lives in the STATUS heartbeat |
| `cmd_raw` | `protocol.cmd` |
| `metadata.segments` / `cal_version` / `gains` / `currents` | `protocol.*` |
| `metadata.protocol` (free-form) | `protocol.name` (+ optional `protocol.id`, see §3) |
| `data.{env,s_630,…}` | `series.{leaf_temp,fluo_630_signal,…}` (§5) |
| `data.timing` | `time.duration_ms` |

## 8. Dual-read rule (v2 ↔ v3 coexistence)

- A measurement object **with** a `schema` key is v3. One **without** it is v2
  (`v: 2`, or the pre-rename `startTicks` spelling — both exist under v2).
- The platform keeps a compat view normalizing v2 rows to the v3 shape
  (renames per §7; `dt` reconstructed as `0.854/freq` from
  `metadata.segments`; `leaf_temp.t` estimated with `t_est: true`).
- Old firmware backlogs on SD cards publish v2 after a fleet upgrade began —
  consumers must accept both indefinitely; producers never emit a mixed
  object.

## 9. Topic

Unchanged by this document. The lean ingest topic
(`experiment/data_ingest/v1/{experimentId}/{sensorType}/{sensorVersion}/{sensorId}`,
openJII `feat/lean-ingest-topic`) carries **no protocol segment** — protocol
attribution on that topic comes exclusively from `protocol.id` (§3).

## 10. Size budget

Reference 59-point run: v2 ≈ 2.45 KB, v3 ≈ 2.55 KB (+4%). Caps that bound the
design: `AMBIT_RUN_PAYLOAD_CAP` 64 000 B (run buffer),
`AMBYTE_PUBLISH_MAX_BYTES` ≈ 68 KiB (build cap), 128 KiB AWS IoT hard limit.
gzip of `sample` (`_sample_encoding: "gzip+base64"`, already supported by the
pipeline) is the deferred headroom lever; not part of v3.

## 11. Decision log

| date | decision |
|---|---|
| 2026-08-10 | Series names are spelled-out (`fluo_630_signal`, …, table §5); no terse+`long_name` variant. |
| 2026-08-10 | `protocol.name` stays free-form; optional `protocol.id` joins the openJII `protocols` table and is the sole protocol attribution on the lean ingest topic. |
| 2026-08-10 | `v` int replaced by self-identifying `schema` string; dual-read keyed on its presence. |
| 2026-08-10 | Raw `timing` dropped from the payload; sensor duration surfaces as `time.duration_ms`. `timestamp_local` and `published` dropped as derivable/observable. |

## 12. On-disk record

The event_log v2 tab-separated record format is unchanged by this contract;
whether the stored `metadata`/`payload` columns move to the v3 field names is
an implementation decision of the builder change (documented in
[components/event_log/include/event_log.h](../components/event_log/include/event_log.h)
when it lands).
