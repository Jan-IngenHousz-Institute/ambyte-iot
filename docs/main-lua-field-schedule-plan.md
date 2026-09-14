# Plan: field-deployable `main.lua` (1 Hz SS, cmd-35 spec, edge/day MPF, qE)

Date: 2026-09-03. Branch base: `main` (ambyte-IoT). Two release units touched:
firmware (one new Lua binding) and `lua/main.lua` (full rewrite, ~200 lines).

## What the firmware already gives us (verified in source)

| Fact | Where | Consequence for the design |
|---|---|---|
| `sched.run` is single-threaded: jobs fire in registration order, then it sleeps until the next due job. A due instant that falls *inside* a running job is missed; for `sched.sun` that means missed **for the whole day** (`time_sync_until_sun` only returns future events). | `components/lua_runner/sched.lua`, `time_sync.c:162` | Long jobs must not straddle a sun-job instant. The SS filler must size itself to the next hard event. |
| `is_daytime()` = sunrise < now < sunset. `sun()` jobs ignore `when`. | `time_sync.c:174` | "after sunrise / before sunset" = `{when="day"}`. |
| AMBIT `arrun` with `persist=1` skips `AS_LED_OFF()` at each segment end, so the actinic LED stays at the **last segment's level** after the trace. Each segment (re)applies its own actinic at start; `actinic ≤ 3` turns the LED off. | `ambit/src/PAM.cpp:82-87, 532` | "Keep LED on at 500 µmol" = a trace whose last segment has `actinic=500`, triggered with `persist=true`. Any trace with `persist=false` (or a 0-actinic first segment) turns it off. |
| The LED is the AS7341's own driver (I²C register). Nothing but boot (`ambit-1.ino:111`) and cmd 4 (`run_esp.cpp:456`) turns it off; AMBIT light sleep does not touch it. | `spec_meas.cpp:23-34` | LED survives idle gaps between qE steps. It also survives an **Ambyte reboot** mid-qE → script must switch lights off at boot. |
| cmd 4 (`ambit.actinic(ch,type,var,var2)`) has no "hold on" mode: type 5 = on for `var2×100 ms` then off; every type starts with `AS_LED_OFF()`. | `run_esp.cpp:456-491` | `ambit.actinic(ch, 5, 0, 0)` is a guaranteed, instant "lights off". Use it as the fallback. |
| Positive `actinic` (1..9999) in a segment is PAR in µmol, converted with the AMBIT's own actinic coefficient (cmd 33); negative is raw DAC. | `lua_runner.c:1143` | `actinic = 500` gives the calibrated 500 µmol; the MPF's `-250/-200/-160` stay raw. |
| The Ambyte has **no cmd 35 support**: no opcode in `ambit_protocol.h`, no binding. `ambit.spec` is cmd 31 (legacy, uncalibrated PAR). | grep of `components/` | Need one small firmware addition (below). |
| `db.store_event` cannot set `device` (AMBIT name) or `cmd_raw`; only the fused C stores do. | `lua_runner.c` `l_db_store_event` vs `ambit_store_small` | Decoding cmd 35 in Lua would produce anonymous events. Do it in C, mirroring `ambit.spec`. |
| cmd 35 exists only on the AMBIT `deterministic-adpd`/calibration branch; no tag ships it, HW conformance still pending. Response = 80 B (raw[10] u16, chan[10] f32, par, par_tier2, exposure + flags). | `ambit/plans/AMBIT_COMMAND35_SPECPAR.md §5,§9` | External dependency: AMBIT firmware release + Calibratron tier-3 per device. Script must fall back to cmd 31 until then. |
| Poll/trigger/fetch overhead is ~0.3–0.5 s per channel per call; `run_trace` already handles per-channel failure and broken sensors. | `lua/main.lua` | Keep `run_trace` as is; add `opts` for `persist`/`store`. |

## Part A — firmware: `ambit.spec_raw(ch [, opts])` (one small change, then release)

1. `components/device_commands/include/ambit_protocol.h`: `#define AMBIT_CMD_GET_SPEC_RAW 35` (+ the 80-byte layout as a comment, copied from the AMBIT plan §5).
2. `components/device_commands/device_commands.c`: `cmd_ambit_get_spec_raw(ch, ambit_spec_raw_t *out)` — same shape as `cmd_ambit_get_spec` but `expect_raw = 80`, LE decode by field (memcpy, no packed struct). Reject any other length with a clear message (`"cmd 35 unsupported/legacy"`) so Lua can latch the fallback.
3. `components/lua_runner/lua_runner.c`: `l_ambit_spec_raw`, registered as `ambit.spec_raw`. Fused store exactly like `ambit.spec`: `ambit_store_small(ch, dev, "get_spec_raw", meta, …)` with payload
   `{"raw":[10],"chan":[10],"par":f,"par_tier2":f,"atime":n,"astep":n,"gain_low":n,"gain_high":n,"flags":n,"sat_mask":n,"clip_mask":n}`.
   Returns `{par=, par_tier2=, chan={…}, flags=, id=}` or `nil,err`. Accept `opts.store` (like `ambit.spec`) and `opts.metadata` (like `ambit.run`, merged into the event metadata) so qE steps can be tagged.
4. CLI: extend `ambit_spec <ch>` with a `raw` variant (optional, cheap, useful on the bench).
5. Bump `version.txt`, touch `CMakeLists.txt`. Ships in the next firmware release; `main.lua` records it as `built_against_fw`.

Not doing: Lua-side `string.unpack` decode of `ambit.query(...)` (anonymous events), auto-exposure logic (AMBIT-side), any change to `sched.lua`.

## Part B — `lua/main.lua` rewrite

### Protocol tables

```lua
local SS_SEG   = { pulses = <n>, freq = 1, actinic = 0 }     -- n set per minute (see filler)
local MPF      = { …as today… }                               -- persist=false → LED off after
local MPF_L    = MPF with the two actinic=0 segments (baseline, relaxation) set to 500
                 -- run with persist=true → LED stays at 500 µmol after the trace
local LIGHT_ON = { { pulses = 2, freq = 1, actinic = 500 } }  -- persist=true, store=false: only turns the LED on
local QE_PAR   = 500
```

### Helpers (keep `estimate_ms` / `run_trace`; small additions)

- `run_trace(tag, trace, opts)` — `opts = { hold_window=, persist=, store=, metadata= }`; passes `persist` to `ambit.trigger` and `store` to `ambit.fetch`. `metadata` merged with `{protocol=tag}`.
- `spec_round(meta)` — for each pinging channel: `ambit.spec_raw` unless the per-channel latch `no_cmd35[ch]` is set; on `nil,err` mentioning unsupported/legacy → set the latch, log once, use `ambit.spec` from then on. If the `ambit.spec_raw` global is nil (old Ambyte firmware) use `ambit.spec` throughout. Records `last_spec_ms` so a spec is not taken twice at the same tick.
- `lights_off_all()` — `ambit.actinic(ch, 5, 0, 0)` per pinging channel, up to 3 attempts, log failures. Called at boot, at the end of qE (always), and after any qE error.
- `until_next_sun_job()` — `math.min` over `sync.until_sun(event, offset)` of the four sun jobs (nil → ignore). Single source of the "hard deadline" horizon.

### Jobs and registration order (order matters: it is the firing order)

```lua
sched.every("10m", mpf_day,     { when = "day" })   -- 1. spec + MPF; skipped if a sun job is due < 30 s
sched.every("5m",  spec_day,    { when = "day" })   -- 2. spec only; skipped if last_spec < 30 s ago (the :x0 tick already took one)
sched.sun("sunrise", -3600, edge_round)             -- 3. spec + MPF in the dark
sched.sun("sunset",   3600, edge_round)
sched.sun("sunrise",  3*3600, qe_round)             -- 4. qE morning
sched.sun("sunset",  -3*3600, qe_round)             -- 5. qE afternoon
sched.every("1m",  ss_fill)                          -- 6. LAST: steady-state filler
sched.run()
```

- **SS filler (`ss_fill`)**: `budget = math.min(sync.until_interval(60,0), until_next_sun_job()) - MARGIN_S` (MARGIN_S ≈ 5: fetch of 4 channels + next trigger). If `budget < 10` → skip this minute; else one segment `{pulses = min(budget, 60), freq = 1, actinic = 0}`, `hold_window=false` so the publisher drains in the poll gaps. Result: 1 Hz steady-state for ~50–55 s of a normal minute, ~40 s of an MPF minute, none during edge/qE (~5 min twice a day). This is the "as close to every second as possible without hammering the CPU": one trigger + one fetch per channel per minute, 500 ms poll sweeps, no busy loop.
- **Why the horizon matters**: with the old 45 s SS the sun jobs fell inside a running SS ~75 % of the time and were silently lost for the day. Sizing the filler (and skipping a day-MPF) against `until_next_sun_job()` makes the sun jobs land on time. MPF/spec/qE never overlap by construction (edge is outside daytime; qE is 4 h from edge; a due-but-skipped 10-min MPF costs one slot).
- **`mpf_day` / `edge_round`** = `spec_round()` then `run_trace("MPF"|"edge", MPF, {hold_window=true})` (persist=false → LED off afterwards regardless).

### qE sequence (`qe_round`)

```
body:
  spec_round{protocol="qE", step=0}   ; run_trace("qE", MPF,   {hold_window=true, metadata={step=0, par=0}})
  run_trace("qE_light_on", LIGHT_ON, {persist=true, store=false})   -- LED → 500 µmol, stays on
  sync.wait(5)
  for step = 1..4:
      spec_round{step}; run_trace("qE", MPF_L, {hold_window=true, persist=true, metadata={step, par=500}})
      if step < 4 then sync.wait(30) end
  lights_off_all()
  for step = 5..8:
      run_trace("qE", MPF, {hold_window=true, metadata={step, par=0}})  -- persist=false: LED off
      wait {10, 20, 40}[step-4] unless step == 8
wrapper:
  ok, err = pcall(body); lights_off_all(); if not ok then log
```

- Duration ≈ 9 MPF (~11 s each) + 5 spec + 165 s waits ≈ 5 min. SS resumes at the next minute mark.
- Light stays on through the light phase because `MPF_L` never has an `actinic ≤ 3` segment and runs with `persist=true`; the `-250/-200/-160` saturating segments are raw DAC as today.
- **Lights-off guarantees** (in order of defence): every dark-phase trace has `persist=false`; explicit `lights_off_all()` after the light phase; `pcall` + unconditional `lights_off_all()` on any error (including `lua stop`, whose hook raises through our pcall); `lights_off_all()` at script boot (covers an Ambyte reboot/power cut mid-qE, since the AMBIT keeps its LED register). Bench-verify the last two.
- `measurement_window` is held only during each MPF trace, not during waits, so the publisher drains between steps.

### Cleanup

- Remove the dead loop after `sched.run()` and the unused `edge_round` alias duplication.
- Header comment (house style): the sun-job-miss hazard and why the filler sizes itself; the persist/LED contract; the cmd-35 fallback latch.
- `lua/release.config.js` untouched; this is a `lua-vX.Y.Z` release with `built_against_fw` = the firmware from Part A.

## Part C — verification (unit E8:F6:0A:B1:1F:34, AMBIT on the cmd-35 firmware branch)

1. `lua exec` smoke: `ambit.spec_raw(0)` returns `par`/`chan`/`flags`; `evlog` shows a `get_spec_raw` event with device name + metadata. Then against a 1.1.4 AMBIT: latch fires once, `ambit.spec` events follow.
2. 24 h soak: per minute one SS event per channel with 45–55 points; at :x0 exactly one spec + one MPF; at :x5 one spec; heap flat in `status`; publisher drains during SS (`inflight`).
3. Sun jobs: use `set_time`/`sync.set_location` to place the clock ~2 min before sunrise-1h, sunrise+3h, sunset-3h, sunset+1h; confirm the SS filler shortens and the job fires within seconds of the target.
4. qE: LED visibly on ~2.5 min, off after; 9 MPF + 5 spec events tagged `protocol=qE, step=n`. Then (a) `lua stop` mid-light-phase → LED off; (b) power-cycle the Ambyte mid-light-phase → LED off within seconds of boot; (c) unplug one AMBIT mid-qE → other channels complete, no hang.
5. Release: firmware `vX.Y.Z` first (binding), then `lua-vX.Y.Z`; fleet deploy of the Lua only after the AMBIT cmd-35 firmware + Calibratron tier-3 is on the target devices (until then the script runs on the cmd-31 fallback, which is acceptable but uncalibrated).

## Open decisions for the user

- SS at night too (as specified: "every second", ungated). Doubles nightly event volume vs today; say so if you want `{when="day"}` on the filler instead.
- MPF under light in qE: I replace the two `actinic=0` segments with 500 µmol so the light never drops during the trace. If the baseline/relaxation should instead be measured with the actinic off, say so (then `persist` alone cannot keep the light on and the sequence needs a re-light step after each MPF).
- Spec events in qE are tagged via the new `opts.metadata`; if Part A ships without it, they correlate by timestamp only.
