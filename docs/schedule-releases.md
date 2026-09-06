# Schedule releases and authoring

The field schedule catalog is an independent release unit. Firmware tags remain
`vX.Y.Z`; changes confined to `schedule/**` produce `schedule-vX.Y.Z` without
bumping firmware. Every catalog entry is published as the exact authored YAML
plus a schema-1 manifest containing its SHA-256, byte length, firmware
compatibility, immutable URL, and unchanged `script_update` command object.

The initial catalog contains:

| Name | Purpose |
| --- | --- |
| `default.yaml` | Multi-channel steady-state, spectra, saturating-flash, edge, and health jobs |
| `legacy_1hz_spec.yaml` | Channel-0 spectra at 1 Hz around daylight and 10-minute fallback outside it |
| `actions.schema.json` | JSON Schema generated from the firmware action table |

Devices install every selected asset as `/littlefs/schedule.yaml`. Installation
downloads or serial-stages `schedule.yaml.new`, checks its SHA-256, compiles it
with the same parser and action catalog used by the runner, retains the previous
file as `schedule.yaml.bak`, and atomically swaps it. `schedule release` reports
the active file SHA and the existing `script_*` provenance tuple.

## Document header

Every schedule begins with the namespaced schema constant:

```yaml
schema: jii.ambyte-schedule/v1-draft
id: urn:jii:schedule:example       # optional provenance
version: 1.0.0                    # optional provenance
workbookVersionId: 01234567-89ab-cdef-0123-456789abcdef  # optional
name: example                     # optional display name
description: A readable note.     # optional
```

Per-device latitude, longitude, timezone, deployment, and identity belong in
NVS `device_config`; never fork a schedule per site for those values.

Set site facts through one of three supported paths:

- Provisioning: pass `--lat`, `--lon`, and optional `--deployment` to
  `tools/build_nvs_image.py`, or enter the same values in the flash GUI.
- Console: `sync loc 52.173 5.819` persists latitude and longitude. Its optional
  numeric timezone offset is runtime-only; persist an IANA zone separately with
  `cfg set timezone Europe/Amsterdam`.
- MQTT: publish the retain-safe command
  `{"type":"set_location","id":"site-a","lat":52.173,"lon":5.819,"deployment":"greenhouse-a"}`.
  The `set_location_result` reply echoes the persisted deployment tag.

All three paths write one fixed, versioned `device_cfg/site_state` NVS blob, so
latitude, longitude, and deployment cross a power loss as one tuple. Firmware
still reads the former individual keys as a read-only compatibility fallback
for already-provisioned devices; the next explicit site write migrates them.

## YAML subset

The device accepts indented block mappings and sequences, comments, quoted or
plain strings, integers, floats, `true`/`false`, durations, and `HH:MM` values.
Flow collections (`{...}` and `[...]`) are deliberately rejected so released
schedules remain readable diffs rather than JSON embedded in YAML. The limits
are 16 KiB, 256 characters per line, nesting depth 6, 512 nodes, and an 8 KiB
scalar-string arena.

Anchors, aliases, merge keys, tags, block scalars, multiple documents, tabs,
escapes other than quote/backslash, and YAML's
`yes`/`no` booleans are rejected. Unknown keys are errors. Run the host compiler
before release:

```sh
sched_host --check schedule/default.yaml
sched_host --simulate schedule/default.yaml \
  --date 2026-06-21 --tz-offset-s 7200 --lat 52.173 --lon 5.819
```

## Jobs, triggers, and gates

Jobs run in file order; steps within a job run sequentially. Every job has
`schedule:` and `steps:`. Available triggers are:

- `cron: "m h dom mon dow"` using conventional five-field cron.
- `cron: "s m h dom mon dow"` using the common seconds-first extension for
  one-second and other sub-minute cadences.
- `solar: sunrise|sunset` with an optional signed `offset`; this is the sole
  schedule extension because a cron expression cannot know site-dependent
  solar times.
- `boot`, once after the clock is trusted.
- `dispatch`, only by `schedule run <job>` or MQTT `schedule_run`.

Cron supports stars, comma-separated lists, ranges, steps, month names, and
weekday names. Day-of-month and day-of-week follow Vixie cron's OR rule when
both are restricted. `L`, `W`, `#`, and `@` extensions are not supported.
Regular seconds-first cron grids are armed on monotonic deadlines, so small RTC
corrections do not stretch acquisition cadence. Five-field cron retains normal
calendar and DST semantics.

Cron is an orchestration clock, not a real-time sample loop. Rates above 1 Hz
belong inside the invoked action or protocol—for example a protocol segment's
`freq: 10000`—where the implementation can use the appropriate hardware timer
or DMA peripheral without dispatching thousands of YAML jobs per second.

Every job may be dispatched manually regardless of its declared triggers.
Optional `when.window` accepts `day`, `night`, or explicit `from`/`to` edges.
Explicit sun windows must state `unresolved: run|skip`; use `skip` for fast
measurement and `run` for a safe fallback cadence. `overlap` is `skip`,
`queue-one`, or `reject`; `missed` is `skip` or `run-once`. Gated jobs fire once
on window entry unless `on_enter: false` is set.

## Actions

`channels` is optional for all AMBIT actions. When absent, the action probes all
four ports and operates on every channel that answers; do not hard-code channel
lists in site-independent schedules.

| `uses` | Inputs |
| --- | --- |
| `ambit/trace` | required `protocol`; optional `channels`, `hold_window`, `tag`, `deadline_margin` |
| `ambit/spectrum` | optional `channels` |
| `ambit/leaf-temp` | optional `channels` |
| `ambit/actinic` | optional `channels`; required `level` and bounded `duration` |
| `device/status-report` | optional flat `tags` map |
| `db/store-event` | optional `channel`, flat `data` and `metadata` maps, and `kind` |
| `device/log` | required `message` |
| `device/sleep` | required `duration`, at most 60 seconds |

Protocols used by `ambit/trace` are declared at top level as named arrays of
segments. Each segment requires `pulses`, `freq`, and `actinic`; `type`,
`far_red`, and `subsampling` are optional.

`db/store-event` scalar values may use device placeholders: `$deployment`,
`$lat`, `$lon`, `$tz`, `$boot_epoch`, `$uptime_ms`, `$sd_ready`, and the current
job's `$job.runs`, `$job.failures`, `$job.skipped`, and `$job.fail_streak`.

## Examples

All-present spectrum every five minutes during daylight:

```yaml
schema: jii.ambyte-schedule/v1-draft
id: urn:jii:schedule:day-spectrum
jobs:
  spectrum:
    schedule:
      cron: "*/5 * * * *"
    when:
      window: day
    steps:
      - uses: ambit/spectrum
```

Fail closed to a slow cadence when sunrise cannot be resolved:

```yaml
schema: jii.ambyte-schedule/v1-draft
id: urn:jii:schedule:windowed-spectrum
jobs:
  fast:
    schedule:
      cron: "* * * * * *"
    when:
      window:
        from: sunrise-1h
        to: sunset+1h
        unresolved: skip
    missed: skip
    steps:
      - uses: ambit/spectrum
        with:
          channels:
            - 0
  slow:
    schedule:
      cron: "*/10 * * * *"
    when:
      window:
        from: sunset+1h
        to: sunrise-1h
        unresolved: run
    steps:
      - uses: ambit/spectrum
        with:
          channels:
            - 0
```

For the complete shipped examples, comments, and precise defaults, read
`schedule/default.yaml` and `schedule/legacy_1hz_spec.yaml`. The generated
`schedule/actions.schema.json` is the machine-readable action contract.
