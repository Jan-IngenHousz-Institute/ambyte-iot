# Ambit Firmware Merge Plan — `openJII` ⇄ `ambitPIO`

Goal: collapse the two diverged forks of the same Ambit ESP32-C3 firmware into a
single source tree that builds **both** product variants from one codebase via
build flags:

- **`ambyte`** variant — binary FSM protocol over UART0/FFC, idle light-sleep,
  the field/companion-board use case (today = `ambitPIO`).
- **`cloud`/`usb`** variant — text + JSON protocol over native USB-CDC, feeds the
  OpenJII platform (today = `openJII`).

Sources compared:
- `P1` = `esp32-c3-devkitm-1 - ambit openJII`  (cloud/JSON line)
- `P2` = `ambitPIO`  (ambyte/binary line, this repo) — also ships `REFACTOR_PLAN.md`

Reviewed 2026-06-04. Back-compat is **not** a constraint; git holds history.

---

## 0. Executive summary

The two share an essentially identical **measurement core** (`PAM.cpp` engine,
all three device drivers, `nvs1`, `devices_init`, `pin_config`, the `dataclass`
buffer, `self_test`). They diverge almost entirely in **one layer: host transport
+ command parsing**, plus a handful of **semantic** changes inside `PAM.cpp`
(env-temperature encoding, ambient correction) that are independent of transport
and must be reconciled by *decision*, not by union.

**Chosen base direction: `ambitPIO` (P2) is the trunk.** It already carries the
protocol/semantic improvements, the power management, the `CONNECTION_TYPES`
enum (the right seam), and the checksum fix. The merge re-grafts P1's **cloud/JSON
output sink** onto that trunk as one additional `CONNECTION_TYPES` value, rather
than the reverse.

Estimated effort: **~2–4 focused days**, gated by 3 decisions (§2) and dominated
by the `PAM.cpp` 3-way reconciliation (§6) and `dataclass` send-path merge (§7).

---

## 1. Target architecture

```
                         ┌──────────────────────────────────────────┐
host bytes ─► framing ──►│  ONE command dispatch table (verbs)        │
   │  (build flag:       │   set_currents / set_gains / run_arr /     │
   │   USB-CDC vs UART)  │   run_mpf / get_par / get_temp / baseline /│
   │                     │   set_act / set_name / nvs / info / ...    │
   ├─ text frontend  ────┤                                            │
   │  (line + JSON,      └─────────────────┬──────────────────────────┘
   │   cloud variant)                      │
   └─ binary frontend ────────────────►    ▼
      (ambyte FSM)              ONE config struct (currents+gains)
                                           │
                                           ▼
                              measurement core  (PAM + drivers, shared)
                                           │
                                           ▼
                       output sink selected by CONNECTION_TYPE:
                         PLOTTING  → CSV Serial.printf
                         COMPUTER  → send_serial()
                         AMBYTE    → dataclass::fsm_send_esp()   (FSM)
                         CLOUD     → dataclass::send_json()      (re-grafted)
```

Key idea: **`CONNECTION_TYPE` is the single output selector** and already exists
in [src/config.h](src/config.h). P1's `json_output` bool and P2's per-call
branching both collapse onto it by adding **`CLOUD`** to the enum.

---

## 2. Decisions required before coding (blockers)

These change *what code survives*; resolve them first.

| # | Decision | Options | Recommendation |
|---|----------|---------|----------------|
| **D1** | **Wrench scripting VM** (`src/src/wrench.cpp`, 17,420 LOC + `do_c.cpp`) | (a) drop entirely, route `C…?` through the unified dispatch; (b) keep behind `-DENABLE_WRENCH` default-off | **(b)** gate it off by default; delete later once the dispatch covers the command surface. Removes ~17k LOC from the default image and the `arr_reset` overflow (REFACTOR_PLAN #5) from the hot path. |
| **D2** | **Env-temperature wire format** | P1 bit-packed `time<<16 \| type<<12 \| data`, `(mlx+20)*20`, carries step-mark events; P2 plain `int16` centi-°C, marks dropped | **Keep P2 centi-°C.** Consequence: P1's `send_env_json_decoded` must be rewritten for the new format and **loses step-mark events** (they're no longer produced). Confirm the cloud consumer tolerates a plain temp series. |
| **D3** | **Ambient (sun/leaf) correction** | P1 stores raw `ret[0]/ret[1]`; P2 stores `ret > 65000 ? ret-65000 : 0` | **Keep P2 correction**, BUT confirm the OpenJII backend ingestion expects corrected vs raw. If cloud needs raw, emit raw on the `CLOUD` sink only. **Verify with the data consumer before locking.** |

Secondary (can defer, but note now):
- **D4** `adpd_gains_config_t.SunRint/LeafRint` + `read_light_env()` are P1-only and
  used only by the JSON spectral+PD snapshot. Keep **iff** the cloud variant needs
  that command; otherwise drop the two fields and the function.

---

## 3. File-by-file mapping

Legend — **Action**: `TRUNK`=take P2 as-is · `MERGE`=3-way reconcile · `PORT`=bring
P1 feature onto trunk behind a flag · `DROP`=remove · `KEEP`=unchanged (identical
in both).

### Shared core (low risk)

| File | P1 vs P2 | Action |
|------|----------|--------|
| `src/self_test.cpp` | **identical** | KEEP |
| `src/nvs1.cpp` | **identical** | KEEP |
| `src/nvs1.h` | +1 line (`tick_factor`) | TRUNK (P2) |
| `src/src/adpd/*` (ADI driver, `u_adpd6100.*`) | only a log line (`ESP_LOGI`→`Serial.printf`) | TRUNK (P2); restore `ESP_LOGI` (logs are silenced anyway) |
| `src/src/as7341/*`, `src/src/mlx90632/u_mlx.cpp` | identical | KEEP |
| `src/src/mlx90632/u_mlx.h` | +`extern bool FLAG_DEICE` | TRUNK (P2) |
| `src/src/devices_init.*`, `src/src/pin_config.h` | identical | KEEP |

### Diverged engine (the real work)

| File | Nature of divergence | Action |
|------|----------------------|--------|
| `src/PAM.cpp` | transport (json_output vs CONNECTION_TYPE) **+ semantics** (D2,D3) | **MERGE** — see §6 |
| `src/PAM.h` | P1 has extra overload + `read_light_env` + 2 gain fields | MERGE — trunk P2; re-add only what D4 keeps |
| `src/data_utils.cpp` | P1 `send_json`; P2 FSM send + checksum fix | **MERGE** — see §7 |
| `src/data_utils.h` | P2 adds protocol constants + FSM decls | TRUNK (P2); re-add `send_json` decl |
| `src/config.h` | P2 adds `CONNECTION_TYPES`, `AMBIT_BOOT_IDLE` | TRUNK (P2); **add `CLOUD`** to enum |

### Transport / command layer (consolidate)

| File | Belongs to | Action |
|------|-----------|--------|
| `src/serial.cpp` / `src/serial.h` | P2 only (extracted helpers) | TRUNK; **also move `serial_read_until`/`flush_serial` here** (fixes REFACTOR_PLAN #15 — today re-declared in 4 TUs) |
| `src/do_command.h` | P2 text dispatch (`hash()`) | TRUNK → becomes the **unified dispatch table** (§5) |
| `src/run_esp.cpp` | P2 binary FSM frontend | TRUNK → thin **binary frontend** feeding the unified dispatch |
| `src/do_c.cpp` | P2 Wrench bridge | KEEP behind `-DENABLE_WRENCH` (D1) |
| `src/src/wrench.{cpp,h}` | P2 VM | KEEP behind `-DENABLE_WRENCH` (D1); excluded from default build |
| `src/commands.cpp` / `src/commands.h` | **P1 only** (line + JSON parser) | **PORT** the JSON/line frontend → new `frontend_text.cpp`; **DROP** the duplicated command bodies (they already exist as core funcs) |
| `src/ambit-1.ino` | both, heavily diverged | **MERGE** — see §8 |

### Build / project

| File | Action |
|------|--------|
| `platformio.ini` | MERGE — restore P1's multi-env (`usb`/`uart`) + add feature flags (§9) |
| `open-jii/` (whole monorepo), `*.ipynb` | **OUT OF SCOPE** — server/tooling, not firmware; keep in the cloud repo, don't vendor into the merged FW tree |
| `REFACTOR_PLAN.md`, `MERGE_PLAN.md` | keep in repo |

---

## 4. Output-sink unification (`CONNECTION_TYPE`)

Single enum drives every measurement function's output. Extend P2's enum:

```c
enum CONNECTION_TYPES { PLOTTING, AMBYTE, COMPUTER, CLOUD };  // CLOUD = new
```

Replace, everywhere in `PAM.cpp` / `data_utils.cpp`:

- P1's `if (json_output) { … send_json … }`  → `case CLOUD:`
- P2's `if (CONNECTION_TYPE == AMBYTE) { … fsm_send_esp … }` → unchanged
- `PLOTTING`/`COMPUTER` branches already correct in P2.

Net: drop the `json_output` parameter and the 5-arg `run_arr_type1` overload; the
sink is read from the global `CONNECTION_TYPE` like every other path.

---

## 5. Command-surface unification

The same command bodies are exposed **four** ways today. Collapse to **one
dispatch table** (`do_command`) + thin frontends that translate wire→verb.

Coverage matrix (✓ = exposed in that dialect; bodies are shared core functions):

| Logical command | P1 line/JSON (`commands.cpp`) | P2 text (`do_command.h`) | P2 binary (`run_esp.cpp`) | P2 Wrench (`do_c.cpp`) |
|---|:--:|:--:|:--:|:--:|
| set_currents | ✓ | ✓ | ✓ `cmd 2` | ✓ |
| set_gains | ✓ | ✓ | ✓ `cmd 1` | ✓ |
| set_pd_gain / detector preset | ✓ | — | ✓ `cmd 1` | ✓ `detector_preset` |
| arr config + run | `arrun` | `arrun/1/2` | `cmd 10`+`cmd 21` | `set_arr`+`run`+`arr_reset` |
| run_mpf | `q` | `q` | `cmd 20` | `run_mpf` |
| get_par / PAR | ✓ | ✓ | `cmd 31` | `get_par` |
| get_temp (mlx) | `temp`/`mlx` | `temp`/`mlx` | `cmd 32` | `get_temp` |
| get_temp+raw | — | — | `cmd 34` | — |
| baseline | ✓ | ✓ | `cmd 6` | — |
| set_act | ✓ | ✓ | `cmd 4` | — |
| set_name / metadata | `set_name` | `set_name` | `cmd 37` | — |
| set_emit (emissivity) | ✓ | ✓ | — | — |
| set_spec (coef) | ✓ | ✓ | — | — |
| external trigger run | `ww` | `ww` | — | — |
| flash trigger | `ff` | `ff` | — | — |
| read_light_env (spec+PD) | ✓ | — | — | — |
| nvs clean / update | `clean_nvs` | `clean_nvs` | `cmd 17/18` | — |
| info / check | `check` | `check` | `cmd 33` | — |
| reboot / reset | `reboot` | `reboot` | — | `reset` |
| test / test1 | ✓ | ✓ | — | — |
| q / r / w / a / aa (manual ops) | ✓ | ✓ | — | — |
| awake / wake handshake | — | `awake` | via `170` | — |

Plan:
1. **`do_command` (verb table) is the single source of truth.** Audit P1's
   `commands.cpp` for any verb missing from P2's `do_command.h` (notably
   `set_pd_gain`, `read_light_env`) and add the verb there (calling the shared core
   fn), once.
2. **Binary frontend** (`run_esp.cpp`): keep its `cmd_arr[0]` switch but have each
   case call the **same core function** the verb table calls — no parallel logic.
3. **JSON/line frontend** (ported from `commands.cpp`): parse → emit the same verb
   strings into `do_command`, or call the core fn directly. ArduinoJson is pulled in
   only for the `cloud`/`usb` env (§9).
4. Replace `hash()` dispatch's silent-collision risk (REFACTOR_PLAN #19) only if
   time allows; not a merge blocker.

---

## 6. `PAM.cpp` 3-way reconciliation (the hard part)

`PAM.cpp` diverged on **two orthogonal axes**. Treat them separately.

### Axis A — transport/output (mechanical)
P1 added a `json_output` bool threaded through `run_arr_type1` (+5-arg overload),
`send_env_json_decoded()`, and `send_json` calls. P2 replaced all of it with
`CONNECTION_TYPE` branches + `fsm_send_esp`.
→ **Resolution:** keep P2's structure; fold P1's JSON branch in as `case CLOUD`
(§4). Delete the 5-arg overload and `json_output` param.

### Axis B — measurement semantics (needs D2/D3 sign-off)
These are **not** transport and exist in P2 only — they change the numbers:

1. **Env temp encode/decode** (`PAM_get_env`, leaf_temp):
   - P1: `data=(mlx+20)*20`, packed `time_16<<16 | d_type<<12 | data`; decode
     `((int16_t)(_tmparr & 0xFFF))/20.0 - 20`. Also emits step-mark events
     (`PAM_get_env(0/1,…)` at step start/end).
   - P2: `centi=(int16_t)(mlx*100)`, stored as plain `uint16`; decode
     `(int16_t)(_tmparr & 0xFFFF)/100.0`. **Step-mark events removed.**
   - → **Keep P2 (D2).** Because of this, P1's `send_env_json_decoded` (which
     unpacks the old format and prints `"mark"`/`"temp_c"`) is **stale and wrong**
     against the new encoding (this is REFACTOR_PLAN #7). For the `CLOUD` sink,
     **rewrite** it to print a plain centi-°C series. Step marks are gone unless you
     deliberately re-introduce a separate marker channel.

2. **Ambient correction** in the subsampling save path and `external_trigger_run`:
   - P1: `d_sun->put(ret[0])` (raw).
   - P2: `d_sun->put(ret[0] > 65000 ? ret[0]-65000 : 0)` (offset-corrected).
   - → **Keep P2 (D3)**, pending consumer confirmation. If cloud must get raw,
     branch on `CONNECTION_TYPE==CLOUD` at the `put`.

3. **Removed bits to drop on merge:** `read_light_env()` (D4), the
   `SunRint/LeafRint` gain fields (D4), and `send_env_json_decoded` in its old form.

### Concrete procedure
1. Start from **P2 `PAM.cpp`** (it already has B + Axis-A-as-CONNECTION_TYPE).
2. Add `case CLOUD:` blocks mirroring the existing `COMPUTER`/`AMBYTE` blocks at
   each send site (≈4 sites: `run_arr_type1`, `external_trigger_run`, `MPF`, and
   the `fluor_offset`/env path). Each `CLOUD` block calls the **new** centi-°C
   `send_*_json`.
3. Re-add `read_light_env()` **only if D4=keep**, restoring the two gain fields in
   `PAM.h` and its `set_pd_gain` verb.
4. Delete dead `send_env_json_decoded` (old format).
5. While here, sweep the REFACTOR_PLAN correctness items co-located in this file
   (uninit locals #10, leak-on-early-return #11) — cheap, isolated, reduces churn
   risk later.

---

## 7. `data_utils.cpp` / `dataclass` reconciliation

- **Base = P2.** It has the full ambyte send FSM (`fsm_wake_up_calls`,
  `fsm_send_length_info`, `fsm_send_data`, `fsm_send_esp`) and a **checksum
  correctness fix** (`u32_byte_sum` — byte-sum of each 4-byte word vs P1 summing
  only the low byte). The fix is strictly better; keep.
- **Re-graft `send_json(const char* tag)`** from P1 (removed in P2) for the `CLOUD`
  sink, plus its decl in `data_utils.h`. It's ~10 lines and independent of the FSM.
- `serial_read_until` / `flush_serial`: today defined in `data_utils.cpp` and
  re-declared with default args in 4 TUs. **Move definition + single prototype into
  `serial.h/.cpp`** during the merge (REFACTOR_PLAN #15/#16) and **drop the
  echo-on-parse** `Serial.write(b)` of non-matching bytes (corrupts the binary
  link — REFACTOR_PLAN #16/#20).
- `dataclass` ring-buffer over-engineering (#18) — out of scope for the merge.

---

## 8. `ambit-1.ino` (main) reconciliation

Base = **P2** (it has the power management). Reconcile the loop's transport
dispatch with the merged frontends:

Keep from P2:
- `ambit_light_sleep()`, UART/GPIO sleep-wake config in `setup()`, sleep-threshold
  timers, `AMBIT_BOOT_IDLE` heartbeat — **the energy-valuable code**.
- Triple-tap-BOOT `RB_toggle` reset ISR (UX/recovery).

Fix during merge (REFACTOR_PLAN #20 — the obscurity blocker):
- The `c > 127` MSB mode-switch is glitch-sensitive. Wrap it in a **framed sync**
  (preamble + length + checksum) so junk bytes (DTR/RTS open glitch) are rejected
  cleanly instead of triggering `Unknown cmd` + a 50 ms header hunt.
- Route the two branches to the **merged frontends**: high-MSB/framed → binary
  frontend (`do_esp_cmd`); text → `Serial_Input_Chars` → unified `do_command`.

For the **cloud/usb** variant, the loop must additionally accept the JSON/line
text path (P1's `loop()` parser). Gate that block on the build flag so the ambyte
binary path isn't compiled into the cloud image and vice-versa, OR keep both and
let `CONNECTION_TYPE` + first-byte framing pick — prefer the framed approach so one
binary can serve both.

---

## 9. `platformio.ini` + build flags

Merge P1's multi-env layout with P2's single env, parameterised by feature flags:

```ini
[platformio]
default_envs = ambyte

[env]                         ; shared
platform  = espressif32
board     = esp32-c3-devkitm-1
framework = arduino
monitor_speed = 115200
lib_deps =
  adafruit/Adafruit BusIO
build_flags =
  -DDEBUG=0

; ── Field/companion variant: binary FSM over UART0/FFC, light-sleep ──────────
[env:ambyte]
build_flags =
  ${env.build_flags}
  -DARDUINO_USB_CDC_ON_BOOT=0     ; Serial -> UART0 (GPIO20/21), FFC to ambyte
  -DVARIANT_AMBYTE=1
  ; -DENABLE_WRENCH=1             ; (D1) opt-in; off by default

; ── Cloud variant: text+JSON over native USB-CDC ────────────────────────────
[env:cloud]
build_flags =
  ${env.build_flags}
  -DARDUINO_USB_MODE=1
  -DARDUINO_USB_CDC_ON_BOOT=1
  -DUSB_VID=0x16C0
  -DUSB_PID=0x0483
  '-DUSB_MANUFACTURER="JII"'
  '-DUSB_PRODUCT="Ambit"'
  -DVARIANT_CLOUD=1
lib_deps =
  ${env.lib_deps}
  bblanchon/ArduinoJson          ; only the cloud image pays for JSON
```

Flag usage:
- `VARIANT_CLOUD` → compile JSON/line frontend + `send_json` + (D4) `read_light_env`.
- `VARIANT_AMBYTE` → compile binary frontend + FSM + light-sleep idle loop.
- `ENABLE_WRENCH` → compile `do_c.cpp` + `wrench.*` (default off).
- Core (PAM, drivers, dispatch table) compiles in both.

Note (from memory `platformio-sdkconfig-stale`): after changing envs, **delete the
per-env `sdkconfig.*`** so it regenerates.

---

## 10. Execution order (with checkpoints)

1. **Branch + scaffold.** New branch off `ambitPIO`. Add `CLOUD` to the enum; add
   the two PIO envs and flags (§9). *Checkpoint: `ambyte` env still builds & runs.*
2. **Unify config struct** (REFACTOR_PLAN #2 — prerequisite). Collapse
   `adpd_current_config` (global), `adpd_current_config_local` (`run_esp.cpp`),
   and the `do_c.cpp` statics into one struct all paths read/write. *Checkpoint:
   set via text, run via binary → same currents applied.*
3. **Consolidate serial helpers.** Move `serial_read_until`/`flush_serial` into
   `serial.*`, single prototype, drop echo-on-parse (#15/#16). *Checkpoint: clean
   binary round-trip, no stray echoed bytes.*
4. **Unify dispatch.** Make binary + (later) text frontends call the same core
   funcs via `do_command` (§5). Add P1-only verbs (`set_pd_gain`, etc.). *Checkpoint:
   every command in the §5 matrix reachable from the ambyte path.*
5. **`PAM.cpp` merge** (§6) — add `CLOUD` sinks, rewrite env-JSON for centi-°C,
   apply D2/D3, drop dead code. *Checkpoint: AMBYTE numbers byte-identical to
   pre-merge `ambitPIO`; CLOUD JSON validated against backend schema.*
6. **`data_utils.cpp` merge** (§7) — re-graft `send_json`, keep FSM + checksum fix.
7. **`ambit-1.ino` merge** (§8) — framed sync, route to frontends, gate the text
   path. *Checkpoint: DTR/RTS open glitch no longer produces `Unknown cmd`.*
8. **Port JSON/line frontend** from `commands.cpp` into `frontend_text.cpp`
   (cloud env only); delete `commands.cpp/.h`.
9. **Build both envs; HW test both variants.** *Checkpoint: `ambyte` on FFC link
   to a real ambyte; `cloud` on USB to the demo notebook.*
10. **Decide Wrench fate** (D1) — once dispatch covers the surface, delete
    `do_c.cpp` + `wrench.*` or leave flag-gated.

---

## 11. Risks & mitigations

| Risk | Impact | Mitigation |
|------|--------|-----------|
| `PAM.cpp` semantic merge (D2/D3) changes measured values | scientific data continuity | Freeze AMBYTE output as the reference; diff numbers before/after; get consumer sign-off on raw-vs-corrected & temp format **before** step 5 |
| Cloud JSON decode mismatch after env-format change (#7) | broken cloud ingestion | Rewrite `send_env_json_decoded` for centi-°C; validate against OpenJII backend (`open-jii/apps/backend`) parser/AsyncAPI spec |
| One binary serving two transports via first-byte framing | mode confusion | Prefer build-flag-separated frontends if framing proves fragile; the framed sync (#20) is the long-term fix either way |
| Wrench removal drops a needed scripting feature | lost capability | D1 keeps it flag-gated until the dispatch is confirmed to cover the surface (`run/set_arr/config/set_currents/set_gains/run_mpf/reset/disp/print/get_par/get_temp`) |
| Hidden reliance on P1's step-mark env events | lost timing markers | Confirm no consumer needs marks; else add a dedicated marker channel |

## 12. Test checklist (per variant)

- [ ] `ambyte`: full wake→length→data FSM round-trip to a real ambyte; checksum OK.
- [ ] `ambyte`: idle current draw drops to light-sleep level between commands.
- [ ] `ambyte`: triple-tap BOOT resets; UART activity wakes within threshold.
- [ ] `cloud`: line commands + JSON protocol over USB-CDC; `read_light_env` (if kept).
- [ ] both: `set_currents`/`set_gains` from any frontend reflected in the run.
- [ ] both: `get_par`, `get_temp`, `baseline`, `arrun`, `run_mpf` produce expected output.
- [ ] regression: AMBYTE measurement numbers unchanged vs pre-merge `ambitPIO`.
- [ ] glitch: opening the USB port (DTR/RTS) does not emit `Unknown cmd`.
