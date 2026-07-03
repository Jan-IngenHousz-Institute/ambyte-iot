# AMBIT UART firmware flasher — implementation plan

Status: **Phase 0 PASSED; Phase 1 done; Phase 2 probe slice done (builds clean).** Next: on-HW
`ambit_probe` check, then full multi-region flashing + orchestration.

Progress:
- ✅ **Phase 0 (bench)** — esptool synced to the ESP8685 (ESP32-C3, QFN28 rev v0.4, 4 MB XMC)
  in ROM download mode over the FFC, driven by the ambyte reset/boot lines; stub flasher ran.
- ✅ **Phase 1 (sequencer)** — implemented in `uart_sensors` + a `ambit_dl` CLI test command.
- ✅ **Phase 2a (flasher probe)** — vendored `esp-serial-flasher` v2 at
  `components/esp_serial_flasher` (source only — the registry tarball's deep `test/` paths
  exceed Windows MAX_PATH on unpack). New `components/ambit_flash` supplies a custom v2 port
  (`esp_loader_port_ops_t`) that reuses uart_sensors' UART0 + the sequencer, and `ambit_flash_probe()`
  (CLI `ambit_probe <ch>`): enter download → `esp_loader_connect` → read chip+MAC → reset to app.
  **HW-PASSED on all channels 2026-07-02.**
- ✅ **Phase 2b (full ROM flash)** — `ambit_flash_image(ch, dir, baud)` (CLI `ambit_flash <ch> <version>`):
  fail-fast checks the 4 region files exist, `esp_loader_connect_with_stub` + raise baud (default
  460800), asserts target == C3, then streams each region from SD (`flash_start`/`write`/`finish`
  with MD5 verify) at `0x0/0x8000/0xe000/0x10000`, and resets into the new app. **NVS (0x9000)
  never touched → calibration preserved.** **HW-PASSED 2026-07-02** (`ambit_flash 3 I01`, 448992 B,
  all regions MD5-verified, 460800 baud).
- ✅ **Phase 3 v1 (detect + report; no auto-flash)** — `ambit_flash_find_target()` picks the
  HIGHEST `/sdcard/ambit_fw/<major.minor.batch>/` folder that has all 4 region files;
  `ambit_flash_check()` reads each present AMBIT's version (cmd 33/2) and logs match / mismatch
  (+ the exact `ambit_flash <ch> <ver>` to run). Runs once at boot (report-only) and on demand via
  CLI `ambit_check`. Read-only + bus-mutex-serialised (safe with Lua up). Builds clean; needs HW
  verify. Decisions: **version-named folders** + **detect-and-report only** (no autonomous flash).
  NOTE: folders must be named `major.minor.batch` for the check — e.g. rename `I01` → the image's
  version (the manual `ambit_flash <ch> I01` still works with any folder name).

## 1. Goal

Let the ambyte (ESP32-S3) reflash a connected AMBIT (ESP8685 = ESP32-C3 die) from a
firmware image carried on the ambyte's SD card, over the existing FFC/UART link, when the
AMBIT's firmware version differs from the SD image.

Hard requirements (from product owner):

- **Must work on *unmodified* field units** — including AMBITs whose current firmware
  predates any OTA support.
- **Must recover a fully-bricked AMBIT** — an AMBIT with no working application.

These two requirements are the crux of the whole design (see §3).

## 2. Background — what already exists, and why it isn't enough

A cooperative, application-level OTA over UART ("Strategy B") is **already built and
hardware-proven** end-to-end (v0.0.4 → v0.0.5, `docs/ambit-ota-hwtest.md`):

- ambyte: [`components/ambit_ota/ambit_ota.c`](../components/ambit_ota/ambit_ota.c) stages an
  image on SD and streams it in ≤200-byte CRC-checked chunks via
  `cmd_ambit_ota_begin/data/end/abort/confirm`
  ([`device_commands.h`](../components/device_commands/include/device_commands.h)), quiescing
  Lua+MQTT and using the C3's rollback.
- AMBIT: [`../ambit-IoT/src/run_esp.cpp`](../../ambit-IoT/src/run_esp.cpp) cmds 25-29 =
  `Update.begin/write/end` + `esp_ota_mark_app_valid_cancel_rollback`; `verifyRollbackLater()`
  keeps a new image `PENDING_VERIFY` until confirmed.
- Version read already exists: **cmd 33/2** → `ambit_fw_info_t{major,minor,batch,size}`
  (AMBIT version = `MAJOR/MINOR/BATCH` `#define`s in
  [`../ambit-IoT/src/nvs1.h`](../../ambit-IoT/src/nvs1.h), currently `0.0.6`).

**Why Strategy B cannot satisfy the requirements:** it is *cooperative* — it needs the
AMBIT's running app to understand cmds 25-29. It therefore cannot:

- bootstrap a unit whose firmware predates OTA (it would NACK/time-out on cmd 25), nor
- touch a bricked unit (no running app to cooperate).

The only mechanism that can flash an AMBIT regardless of its app state is the **ESP32-C3 ROM
serial bootloader**, which is baked into silicon and always present. That is "Strategy A".

## 3. Architecture — hybrid A (primary/universal) + B (fast path)

```
                    ┌─────────────────── per present channel, at boot/on command ──────────────────┐
                    │                                                                               │
  ping (cmd_uart_ping) ──not present──▶ skip                                                        │
        │ present                                                                                   │
        ▼                                                                                           │
  read version (cmd 33/2)                                                                           │
        │                                                                                           │
   ┌────┴───────────────┬───────────────────────────────┬──────────────────────────────┐          │
   │ no answer / bricked │ answers, but < OTA_MIN_VER    │ answers, OTA-capable          │          │
   │ / not OTA-capable   │ (pre-OTA firmware)            │ and version != SD target      │          │
   ▼                     ▼                               ▼                               ▼          │
 STRATEGY A          STRATEGY A                      STRATEGY B                     versions equal   │
 (ROM flash)         (ROM flash, bootstrap)          (app-OTA, fast, rollback)      → nothing to do  │
```

- **Strategy A (ROM flasher)** — universal, guaranteed path. Enters ROM download mode via the
  hardware reset/boot lines (§4) and writes the code regions via `esp-serial-flasher` (§7).
  Handles bricked, pre-OTA, and current units.
- **Strategy B (app-OTA)** — kept as an *optimization* for units already running OTA-capable
  firmware: no reset needed, has C3-side rollback, and only writes the app slot (faster).

**Detecting a bare/blank board.** A never-flashed AMBIT does not answer the app-level wake/ping,
so it is indistinguishable from an empty channel at the application layer. Detection therefore
falls through to a **ROM-level probe**: on a channel that fails `cmd_uart_ping`, enter download
mode and attempt `esp_loader_connect` (SYNC). SYNC success ⇒ a C3 is present (bare or bricked)
⇒ Strategy A; SYNC failure ⇒ the channel is genuinely empty ⇒ skip.

**B-vs-A without a version table.** Rather than hard-coding `OTA_MIN_VER`, probe by capability:
if the unit answers cmd 33/2 and its version differs from the SD target, *attempt* Strategy B
(`OTA_BEGIN`); if it NACKs/times out (pre-OTA firmware), fall back to Strategy A. This is
self-detecting and needs no release-history table. (`OTA_MIN_VER` remains an optional
optimisation to skip the failed B attempt on known-old fleets.)

## 4. Hardware interface

### 4.1 Wiring (confirmed against schematic)

Boot lines are **bound to their UART pin pair**, not to a channel number (the schematic's
`ch0..ch3` labels are ordered differently from the firmware's `s_ch[]` index — see the
"firmware `s_ch[]`" column):

| AMBIT on ambyte UART pins (rx/tx) | schematic label | boot line (→ C3 GPIO9) | firmware `s_ch[]` |
|---|---|---|---|
| 3 / 46  | ch0 | **IO2** | `s_ch[0]` |
| 17 / 18 | ch2 | **IO7** | `s_ch[1]` |
| 47 / 48 | ch3 | **IO6** | `s_ch[2]` |
| 40 / 41 | ch1 | **IO5** | `s_ch[3]` |

- **Reset**: ambyte **IO1** → C3 **CHIP_EN**, **shared across all four AMBITs**.
- AMBIT side: boot = GPIO9 (strap), reset = CHIP_EN.
- ambyte GPIOs 1,2,5,6,7 verified currently unused in firmware — no conflict.

### 4.2 Drive type & reset RC (confirmed from schematic)

Each of these lines has a **10 kΩ pull-up to 3V3** at the AMBIT; **CHIP_EN additionally has a
2.2 µF cap to GND** (RC power-on-reset, τ ≈ 10 kΩ × 2.2 µF ≈ **22 ms**).

- Drive **open-drain**: pull **low** to assert, **release to Hi-Z** and let the AMBIT pull-ups
  bring the line high. Never push-pull (would fight the EN cap and could back-feed the AMBIT
  rail). Configure with `GPIO_MODE_OUTPUT_OD`, internal pull-ups off (external pull-ups win).
- Idle state = released (Hi-Z / high).

### 4.3 Calibration-preservation constraint

The AMBIT keeps **per-unit calibration in NVS @ `0x9000`** via `Preferences` (namespace
`"config"`: `spec_coef`/`ambit_calibration_info_t`, `adpd_dark`, `actinic`, `name`, `emit` —
[`../ambit-IoT/src/do_command.h`](../../ambit-IoT/src/do_command.h)). The flasher therefore
writes **only the code regions** and **never** `erase_flash` / NVS / spiffs — exactly like the
factory jig [`../ambit-IoT/src/uploader.py`](../../ambit-IoT/src/uploader.py). A full/merged
image flashed contiguously from `0x0` would overwrite NVS and wipe calibration — **forbidden**.

## 5. SD card layout & versioning

```
/sdcard/ambit_fw/<version>/
    bootloader.bin      → flash 0x0
    partitions.bin      → flash 0x8000
    boot_app0.bin       → flash 0xe000   (otadata init)
    app.bin             → flash 0x10000
```

- `<version>` folder encodes the target version as `major.minor.batch`, e.g.
  `/sdcard/ambit_fw/0.0.7/`. The ambyte parses the version from the folder name (the AMBIT's
  own version scheme is the `#define` triple, *not* `esp_app_desc`, so the folder name is the
  agreed source of truth).
- Region → offset table lives in one place in code, matching `uploader.py`.
- Strategy B (fast path) uses only `app.bin` from the same folder.

> **ACTION — `releases/ambit/` is app-only today.** `releases/ambit/firmware.bin` (424 KB) is
> just the app image (`ambit-1.ino.bin` @ 0x10000). Strategy B works with it, but **bare-board /
> bricked bootstrap (Strategy A) needs the other three regions** the factory jig uses
> ([`../ambit-IoT/src/uploader.py:81`](../../ambit-IoT/src/uploader.py#L81)):
> `bootloader.bin` (@0x0), `partitions.bin` (@0x8000), `boot_app0.bin` (@0xe000, a fixed
> Arduino-esp32 framework artifact). These must be produced by the AMBIT build and staged in the
> versioned SD folder alongside the app; `app.bin` = the current `firmware.bin`. Note: because
> only these four code regions are written (never `0x9000`), a full flash preserves a calibrated
> unit's NVS as long as `partitions.bin` keeps NVS at `0x9000`/same size (it does — pinned
> `default.csv`).

## 6. Module design

### 6.1 `uart_sensors` — carry the boot pin with the channel

Add `boot_pin` to `channel_t` and populate it bound to the UART pins so the naming trap is
structurally impossible:

```c
// components/uart_sensors/uart_sensors.c
s_ch[0] = (channel_t){ .uart_num=UART_NUM_0, .rx_pin= 3, .tx_pin=46, .boot_pin=2, .shared=true };
s_ch[1] = (channel_t){ .uart_num=UART_NUM_0, .rx_pin=17, .tx_pin=18, .boot_pin=7, .shared=true };
s_ch[2] = (channel_t){ .uart_num=UART_NUM_0, .rx_pin=47, .tx_pin=48, .boot_pin=6, .shared=true };
s_ch[3] = (channel_t){ .uart_num=UART_NUM_0, .rx_pin=40, .tx_pin=41, .boot_pin=5, .shared=true };
#define AMBIT_RESET_GPIO 1   // shared CHIP_EN, active-low, open-drain
```

Expose a getter for `boot_pin` and the shared reset so the sequencer/flasher stay decoupled
from the pin table.

### 6.2 `ambit_boot` — GPIO reset/boot sequencer (Phase 1)

Small module owning IO1 + the four boot lines as open-drain, taken under the existing
`uart_sensors` channel/bus mutex so it can't collide with measurement traffic.

```c
esp_err_t ambit_boot_init(void);                 // config 5 GPIOs OD, idle released
esp_err_t ambit_boot_enter_download(uint8_t ch); // → target in ROM download mode
esp_err_t ambit_boot_run(uint8_t ch);            // → normal reboot into app
```

`enter_download(ch)` sequence (timings from the §4.2 RC; final values tuned in Phase 0):

1. Drive `boot_pin[ch]` **low**; leave the other three boot lines released (high) so only
   `ch` straps to download.
2. Drive `AMBIT_RESET_GPIO` (IO1) **low**; hold ≥ **10 ms** (discharge the 2.2 µF cap).
3. **Release** reset (Hi-Z); EN ramps up over ~22 ms.
4. Keep `boot_pin[ch]` low for ~**50–100 ms** after releasing reset (across the full EN rise
   + margin), so the C3 samples GPIO9 = low at the EN threshold and enters ROM download.
5. Release `boot_pin[ch]`.

Note: because reset is shared, this briefly reboots all four AMBITs; the other three (boot
released/high) come up normally into their app. Always done inside the quiesce window.

`run(ch)`: ensure all boot lines released, pulse reset low→release → clean app boot.

### 6.3 `esp-serial-flasher` integration + custom port (Phase 2)

Add Espressif's [`esp-serial-flasher`](https://components.espressif.com/components/espressif/esp-serial-flasher)
as a managed IDF component (via `idf_component.yml` / `dependencies.lock`). It cleanly
separates the ROM protocol (`esp_loader_*`) from the "port" (serial + reset). We provide a
**custom/user-defined port** so the flasher reuses `uart_sensors`' UART0 instead of installing
its own driver:

- `loader_port_serial_read/write` → read/write on UART0 (already remapped to `ch` by
  `channel_acquire`).
- `loader_port_delay_ms`, timer hooks → FreeRTOS.
- `loader_port_enter_bootloader` → call `ambit_boot_enter_download(ch)`.
- `loader_port_reset_target` → `ambit_boot_run(ch)`.
- `loader_port_change_transmission_rate` → `uart_set_baudrate(UART_NUM_0, ...)`.

Flash flow per channel:

1. `channel_acquire(ch)` (owns the bus), raise nothing yet (ROM starts at 115200).
2. `esp_loader_connect()` (SYNC). Optionally `esp_loader_connect_with_stub()` for speed +
   write compression (recommended; costs a small embedded stub blob — evaluate footprint on
   the ~17 kB-largest-block heap).
3. `esp_loader_change_transmission_rate(460800)` (conservative for the FFC; validate before
   going to 921600 like the jig).
4. For each region {0x0, 0x8000, 0xe000, 0x10000}: `esp_loader_flash_start(off, size, block)`,
   stream from the SD file via `fread` → `esp_loader_flash_write` (no whole-image buffering),
   then `esp_loader_flash_verify()` (MD5).
5. `ambit_boot_run(ch)`; wait for boot; read cmd 33/2 to confirm the new version.

Never `esp_loader_erase_flash()` whole-chip; only the four code regions are written.

### 6.4 Orchestration + trigger (Phase 3)

Extend the existing `ambit_ota` worker (reuse its lazy task, quiesce, and status-report
infra):

- New op "sync from SD folder": on boot and/or on command, quiesce (Lua+MQTT), for each
  present channel run the §3 selection logic.
- **Gating** (avoid battery drain / loops):
  - Gate the *flash* on external power present (MP2731 telemetry, already read in
    [`main/app_main.c`](../main/app_main.c)) and/or an explicit config flag.
  - Record `last_flashed → version` per channel in NVS; a channel that failed to confirm the
    same target twice is not retried indefinitely (report instead).
- Version compare: parse target from the SD folder name; compare `major.minor.batch`; flash
  when `running != target` (the SD image is the source of truth — allows intentional
  downgrade). Direction (`!=` vs `<`) is a config knob; default `!=`.

## 7. Reset → download-mode sequence (summary)

See §6.2. Key correctness points: only the target's boot line is low; hold EN low long enough
to discharge 2.2 µF; keep boot low across the whole ~22 ms EN rise; shared reset reboots all
four (acceptable inside quiesce).

## 8. Phases, effort, and validation

### Phase 0 — bench validation (HARDWARE, run by you; no ambyte code) — ~0.5 day

Goal: prove the ambyte-driven reset/boot nets actually force **one** AMBIT into ROM download
mode and that a host `esptool` connects over the FFC. This is firmware-free so we don't build
the sequencer on an unverified path.

1. Pick one AMBIT (e.g. the one on ambyte GPIO3/46). Tap its FFC **UART0 TX/RX + GND** with a
   3V3 USB-serial adapter to a PC.
2. Emulate the open-drain asserts with jumpers on the ambyte-side test points:
   - Jumper the target's **boot line** (IO2 for that unit) to **GND** (holds GPIO9 low).
   - Momentarily jumper **IO1 (reset)** to **GND** for ~50 ms, then remove (emulates the EN
     low→release; the 2.2 µF + 10 k does the ramp). Keep boot grounded across this.
3. On the PC, with the AMBIT now expected to be in download mode:
   ```
   esptool --chip esp32c3 --port <COM> --before no_reset --after no_reset chip_id
   esptool --chip esp32c3 --port <COM> --before no_reset --after no_reset flash_id
   ```
   **PASS** = esptool syncs and prints the chip/flash id. Remove the boot jumper afterward.
4. (Optional) Repeat holding boot released (high) → esptool should **fail** to sync (confirms
   the boot strap is what selects download mode, not a fluke).
5. (Optional) Repeat for a second channel/unit to confirm per-channel boot selectivity while
   reset is shared.

Record the observed EN-low hold and post-release boot-hold that work reliably; those tune the
Phase-1 timings.

**✅ Phase 0 PASSED (2026-07-02):** esptool synced to the ESP8685 (C3, QFN28 rev v0.4, 4 MB
XMC) in ROM download mode over the FFC via the ambyte-driven reset/boot lines; the stub flasher
ran. The whole hardware premise is validated.

### Phase 1 — GPIO sequencer ✅ DONE (builds clean)
Implemented **inside `uart_sensors`** (not a separate `ambit_boot` component — the sequencer
needs the channel table + bus mutex it already owns):
- `channel_t.boot_pin` added and populated bound to the UART pins (`{2,7,6,5}` by `s_ch[]`
  index); `AMBIT_RESET_GPIO 1` shared; both configured open-drain in `ambit_boot_gpio_init()`,
  idle released.
- Public API: `uart_sensors_flash_session_begin/end(ch)`, `uart_sensors_enter_download(ch)`,
  `uart_sensors_run_app(ch)`. Timings: EN low ≥20 ms, boot held 100 ms across the EN rise.
- **`ambit_dl <0-3> <enter|run>` CLI command** to validate firmware-driven download entry with
  the Phase-0 esptool tap.

**On-hardware check before Phase 2:** `lua stop` → `ambit_dl 0 enter` → on the PC run
`esptool --chip esp32c3 --port <COM> --before no_reset --after no_reset chip_id` (expect sync) →
`ambit_dl 0 run` (chip returns to app). Repeat per channel to confirm per-channel selectivity.

### Phase 2 — ROM flasher (~3–5 days)
`esp-serial-flasher` custom-port integration (§6.3). Flash the four regions from SD, verify,
reboot, confirm via cmd 33/2. Decide stub on/off + baud after measuring footprint/time.

### Phase 3 — orchestration + trigger (~2–3 days)
Selection logic, gating, anti-loop marker, SD folder parsing (§6.4).

### Phase 4 — HW validation (~2–3 days)

| Test | Expectation |
|---|---|
| Bricked unit (erased app) | Strategy A recovers it; boots; cmd 33/2 = target |
| Pre-OTA unit | Strategy A bootstraps; subsequently Strategy B works |
| Routine sync (OTA-capable, older) | Strategy B path; version updated |
| Versions equal | No flash |
| **Power loss mid-flash** | Retry succeeds; **NVS calibration intact** (read `spec_coef`) |
| All-channel sweep | Correct boot line per unit; others unaffected |
| Wrong/short image | Verify fails; no bad boot |

Total: **~1.5–2.5 weeks** firmware + validation. No hardware work.

## 9. Risk register

| Risk | Severity | Mitigation |
|---|---|---|
| Power loss during bootloader/partition write (no rollback on ROM path) | High | Gate flash on external power; per-region MD5 verify; small/fast bootloader write window |
| Full/merged image wipes NVS calibration | Critical | Write **only** 4 code regions; never `erase_flash`/NVS; Phase-4 explicitly verifies `spec_coef` survives |
| Shared reset reboots all four AMBITs | Low | Only inside quiesce; others (boot high) reboot normally |
| Wrong boot line → wrong unit into download | High | `boot_pin` bound into `channel_t` by UART pins; Phase-0 per-channel selectivity test |
| esp-serial-flasher footprint on tight heap | Medium | Transient worker task; evaluate stub blob size; stream from SD (no whole-image buffer) |
| Shipping AMBIT `.bin` may lack cmds 25-29 | Medium | Confirm AMBIT release history; `OTA_MIN_VER` gates Strategy B; A is unconditional fallback |
| Reflash loop on bad version compare | Medium | NVS `last_flashed` marker + confirm-after; config-gated |

## 10. Open items before / during implementation

- [x] **Phase 0 bench** — PASSED. Confirm the exact EN-low / boot-hold that worked so the
      Phase-1 `#define`s (20 ms / 100 ms) can be trimmed if desired.
- [ ] **`ambit_dl` on-hardware check** — validate firmware-driven download entry (per channel).
- [ ] **Stage the 3 missing region files** (`bootloader/partitions/boot_app0.bin`) in
      `releases/ambit/` + the SD `<version>/` folder so bare boards can be flashed (§5 ACTION).
- [ ] `OTA_MIN_VER` — now OPTIONAL (capability probe handles B-vs-A). Set only to skip a doomed
      Strategy-B attempt on known pre-OTA fleets.
- [ ] Stub on/off + flash baud (footprint vs speed), decided from a Phase-2 measurement (stub
      already confirmed runnable in Phase 0).
- [ ] Confirm which AMBIT build target emits the 4 region files and how versions are cut into
      `/sdcard/ambit_fw/<version>/`.

## 11. Key references

- ambyte OTA orchestrator: [`components/ambit_ota/ambit_ota.c`](../components/ambit_ota/ambit_ota.c)
- UART channel table / mutexes: [`components/uart_sensors/uart_sensors.c`](../components/uart_sensors/uart_sensors.c) (`s_ch[]` ~L578, `channel_acquire` ~L143)
- command wrappers: [`components/device_commands/include/device_commands.h`](../components/device_commands/include/device_commands.h), protocol [`ambit_protocol.h`](../components/device_commands/include/ambit_protocol.h)
- AMBIT OTA receiver + version: [`../ambit-IoT/src/run_esp.cpp`](../../ambit-IoT/src/run_esp.cpp) (cmds 25-29, 33), [`../ambit-IoT/src/nvs1.h`](../../ambit-IoT/src/nvs1.h)
- AMBIT calibration in NVS: [`../ambit-IoT/src/do_command.h`](../../ambit-IoT/src/do_command.h)
- factory flash offsets: [`../ambit-IoT/src/uploader.py`](../../ambit-IoT/src/uploader.py)
- app wiring / power telemetry: [`main/app_main.c`](../main/app_main.c)
