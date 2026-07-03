# Plan: Ambyte ingests an openJII workbook at runtime

## Context

An openJII **workbook** (`.jii`) is a document of cells. For the ambyte/AMBIT `device_family`
it has two cells that both target the **edge** (not the cloud):

- **Protocol cell** — a list of instrument command strings that the ambyte relays to the AMBIT
  (or any other attached device). In the demo: `get_par` and
  `arrun,3,0,2,0,0,6,0,2,0,1,2,0,0,10,0,10,250,1,2,0,0,6,0,2,0,1`.
- **Macro cell** — repurposed (vs openJII's Python post-processing) to hold the **Lua
  measurement-sequence program**: scheduling + which commands to run and when. This is exactly
  our `main.lua` orchestration layer (`sched.*`, `ambit.ping`, per-channel loops).
- **No edge post-processing** — the Python Fo/Fm/Φ2/LEF analysis is not run on the device
  (cloud's job, if at all). The device stays *store-raw*.

**Why on-device ingestion** (vs pre-compiling a `main.lua` on the host): the protocol cell is the
device-agnostic command definition, authored/edited in openJII independently of the Lua. If the
device parses the protocol strings itself, openJII stays the single source of truth — editing a
protocol never requires regenerating Lua, the macro references protocols by id (decoupled from
their byte encoding), and the same string can later target "any other device" via a
device-appropriate parser.

**What already exists and is reused wholesale:**
- Inbound MQTT command channel + reassembly up to 16 KB, dispatched by `type`
  ([command_router.c:46-168](../components/command_router/command_router.c#L46-L168)). The demo
  workbook (with `entitySnapshots`) is ~2 KB — well under the limit.
- Lazy worker that syntax-checks Lua, atomically swaps `/sdcard/main.lua`, restarts the runner,
  dedupe-latches in NVS, and replies on the status topic
  ([script_update.c](../components/script_update/script_update.c)).
- The Lua runner that loads + runs `/sdcard/main.lua` and exposes `sched`/`ambit`/`device`/`db`/
  `sync` ([lua_runner.c:1811](../components/lua_runner/lua_runner.c#L1811)).
- The AMBIT commands the protocol maps to: `get_par` → `cmd_ambit_get_spec`
  ([lua_runner.c:618-661](../components/lua_runner/lua_runner.c#L618-L661)), `arrun` → the packed
  run-array built by `ambit_build_run_arr`
  ([lua_runner.c:1182-1252](../components/lua_runner/lua_runner.c#L1182-L1252)) whose inverse we need
  ([lua_runner.c:1168-1176](../components/lua_runner/lua_runner.c#L1168-L1176) is the encoder).

**Verified decode of the demo protocol** (8 bytes/segment = `type/IR/pulses-hi/lo/freq-hi/lo/actinic-DAC/subsample`):
`arrun 3,0` → 3 segments: `{6 pulses@2Hz, off} · {10 pulses@10Hz, DAC 250} · {6 pulses@2Hz, off}`.
Because `ambit.run` fixes `type=2`/`subsample=1`, an on-device interpreter reproduces these bytes
**identically** to today's `ambit.run` — the stored event and its `cmd_raw` are unchanged.

## The three pieces to build

### 1. Ingestion — `workbook_update` command + `workbook` worker component
Clone the `script_update` pattern into a new `components/workbook/` (worker `workbook.c`, header
`workbook.h`), and add a `workbook_update` branch to `command_router.c` next to `script_update`
([command_router.c:126-148](../components/command_router/command_router.c#L126-L148)).

The worker (lazy task, dedupe latch, status reply — all mirrored from script_update):
1. `cJSON_Parse` the workbook JSON. Require the **resolved** form (with `entitySnapshots`); the
   thin on-disk `.jii` (references only) is insufficient — reject with a clear `detail`.
2. Walk `cells[]`:
   - **macro cell**: base64-decode `entitySnapshots.macros[macroId].code`
     (`mbedtls_base64_decode`, mbedtls already linked). Require `language == "lua"` (reject
     `javascript`/`python`). Then reuse script_update's exact machinery — syntax-check
     (`luaL_loadstring`), stage to `main.lua.new`, `fsync`, `lua_runner_stop(5000)`, atomic
     `rename`, `lua_runner_start`. **Refactor** that block
     ([script_update.c:186-226](../components/script_update/script_update.c#L186-L226)) into a shared
     `lua_script_install(text, err, cap)` helper (in lua_runner or a small util) called by both
     workers — do not duplicate the swap logic.
   - **protocol cell(s)**: extract `entitySnapshots.protocols[protocolId].code` (array of
     `{label:"…"}`) and write a protocol store `/sdcard/protocols.json` mapping
     `protocolId → [command strings]` (SD, consistent with `main.lua`; survives reboot).
3. Dedupe latch on `workbookId+version`; reply `{"type":"workbook_status","state":"applied|failed",…}`.

### 2. Protocol interpreter — ASCII command → existing `cmd_ambit_*`
Add to `lua_runner.c` (where the store helpers `ambit_store_small` / `ambit_decode_store_push` and
the encoder `ambit_build_cmd_ascii` already live, so reuse is maximal) a table-driven parser:

`static cmd_result_t protocol_run_ascii(uint8_t ch, const char *cmd, bool store)` with a
verb→handler table (the "command-table interpreter"):
- `get_par` → `cmd_ambit_get_spec` + store `{"spec":[…],"par":…}` with `cmd_raw="get_par"`
  (identical to `l_ambit_spec`).
- `get_temp` → `cmd_ambit_get_temp`.
- `arrun <len>,<persist>,<b0>,<b1>,…` → parse the CSV bytes into the `nseg*8` run array →
  `cmd_ambit_run` (cmd 21) → `ambit_decode_store_push` (identical events to `ambit.run`).

This is the inverse of `ambit_build_cmd_ascii` and mirrors the AMBIT's own `do_command.h` grammar.
It dispatches to the **existing** transport + store paths, so *store-raw* is preserved and events
are byte-identical to today's.

### 3. Lua binding — reference a protocol by id from the macro/schedule
Register a `protocol` module (or extend `ambit`) in `lua_open_ambyte_env`
([lua_runner.c:1784](../components/lua_runner/lua_runner.c#L1784)):
- `protocol.run(ch, protocol_id [, {store=true}])` — lazy-load `/sdcard/protocols.json` (once,
  cached), look up the id's command list, run each string through `protocol_run_ascii`, return a
  summary `{stored=…}`. `nil,err` on unknown id / SD missing.

The macro cell (the workbook's Lua) then reads naturally:
```lua
sched.every("10m", function()
  for ch = 0, 3 do
    if ambit.ping(ch) then protocol.run(ch, "692731ee-74f9-4d4c-864f-c450505ef760") end
  end
end, { when = "day" })
sched.run()
```

## Scope / decisions (defaults baked in — flag at approval if any should change)
- **Transport**: new `workbook_update` type (device ingests the whole resolved workbook). Reuses
  the existing ≤16 KB inbound path; no new topic.
- **Protocol store**: `/sdcard/protocols.json` (SD, like `main.lua`).
- **Verbs v1**: `get_par`, `arrun`, `get_temp` (covers the demo), table-driven for easy extension.
- **arrun execution**: blocking `cmd_ambit_run` (correct + simplest).

**Non-goals / explicit follow-ups (not in v1):**
- Parallel `protocol.trigger/poll/fetch`-by-id (the concurrency of `run_trace`) — v1 is blocking
  per channel; note the trade-off.
- Generic "any other device" parser (non-AMBIT verbs via `uart.query`).
- Any edge post-processing (Fo/Fm/Φ2/LEF) — stays out of the device by design.
- Cloud-side `sample[]`→`set[]` schema bridge for the openJII macro — separate concern, not edge.

## Files
- **New**: `components/workbook/workbook.c`, `.../include/workbook.h`, `.../CMakeLists.txt`.
- **Edit**: `components/command_router/command_router.c` (add `workbook_update` branch + include).
- **Edit**: `components/lua_runner/lua_runner.c` (parser + `protocol` module + protocols.json
  loader) and `include/lua_runner.h` (export `lua_script_install` if factored there).
- **Edit**: `components/script_update/script_update.c` (call the shared `lua_script_install`).
- **Edit**: `main/app_main.c` (`workbook_init(...)` wiring, mirroring the `script_update_init`
  call block).
- **Docs**: extend `device-script-delivery.md` (new `workbook_update` contract + example payload)
  and add a test to `docs/ambit-ota-hwtest.md`.

## Verification
1. **Build**: `pio run` clean.
2. **Interpreter equivalence (bench)**: on device via `lua exec`, run the demo `arrun` both ways
   and confirm identical stored events / `cmd_raw`:
   `return protocol.run(0, "<demo-id>")` vs the equivalent `ambit.run(0, {…})`.
3. **End-to-end (HW)**: publish a `workbook_update` MQTT message carrying the demo workbook
   (macro cell = Lua schedule above; protocol cell = `get_par` + `arrun,3,0,…`). Confirm, per
   `docs/ambit-ota-hwtest.md`:
   - `main.lua` swapped (`.bak` kept) + `protocols.json` written + runner restarted;
   - `workbook_status:applied` reply on the status topic;
   - raw `get_par` and `arrun` events stored and published in the existing **payload v2** shape
     (unchanged from today);
   - dedupe: re-publishing the same `workbookId+version` is ignored; a bad macro (`language`
     wrong / syntax error) is rejected with `main.lua` untouched.
