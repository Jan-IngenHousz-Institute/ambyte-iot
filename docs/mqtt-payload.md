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
| sensor-known (identical from both producers, given §5's encoding rules) | `schema`, `device`, `series`, `protocol.segments`, `protocol.cal_version`, `protocol.tick_factor`, `time.duration_ms` | series decoded from the binary FSM arrays; `segments` from the sent run bytes; `cal_version`/`tick_factor` from the cal-struct read (cmd 33) | emitted by the AMBIT firmware itself |
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
considered and rejected (decision log, §11).

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
**Watch the metadata cap:** the Ambyte's per-event metadata blob is capped at
`AMBIT_RUN_METADATA_CAP` 896 B; a worst-case 16-segment run plus
`tick_factor` + `protocol.id` leaves only ~30 B headroom before user metadata
(`protocol.name`) is silently dropped — the builder change (T4) must either
raise the cap or fail loudly, and `protocol.cmd` (≤ ~522 B) must stay in the
record's command column, not move into the metadata blob.
gzip of `sample` (`_sample_encoding: "gzip+base64"`, already supported by the
pipeline) is the deferred headroom lever; not part of v3.

## 11. Decision log

| date | decision |
|---|---|
| 2026-08-10 | Series names are spelled-out (`fluo_630_signal`, …, table §5); no terse+`long_name` variant. |
| 2026-08-10 | `protocol.name` stays free-form; optional `protocol.id` joins the openJII `protocols` table and is the sole protocol attribution on the lean ingest topic. |
| 2026-08-10 | `v` int replaced by self-identifying `schema` string; dual-read keyed on its presence. |
| 2026-08-10 | Raw `timing` dropped from the payload; sensor duration surfaces as `time.duration_ms`. `timestamp_local` and `published` dropped as derivable/observable. |
| 2026-08-10 | Time encodings are exactly two: explicit `t`, or regular (`t0`,`dt`). A piecewise per-segment descriptor (`seg`) was considered and rejected — mixed-frequency multi-segment runs emit explicit `t` instead, keeping every consumer two-form. |
| 2026-08-10 | The `count` saturation clamp (65535) and the number-formatting rules (§5) are contract requirements on producers, not wire artifacts. |
| 2026-08-10 | `t_est` estimator formula (§6) is normative so the firmware fallback and the SQL compat view agree. |

## 12. On-disk record

The event_log v2 tab-separated record format is unchanged by this contract;
whether the stored `metadata`/`payload` columns move to the v3 field names is
an implementation decision of the builder change (documented in
[components/event_log/include/event_log.h](../components/event_log/include/event_log.h)
when it lands).
