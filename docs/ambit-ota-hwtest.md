# AMBIT-over-UART OTA — hardware test plan

Covers the four follow-ups added 2026-06-12: **MQTT trigger**, **C3 rollback**,
**per-channel rollout (`all`)**, and the **fleet version report**. Builds on the
already-passed end-to-end OTA (v0.0.4 → v0.0.5).

Repos: ambyte = `ambyte-iot-ludo` (this repo); C3 = `ambit-IoT/fw_new` (Arduino).
A bad C3 image is always recoverable over **USB-Serial-JTAG** — keep a USB cable handy.

---

## Prerequisites

1. **Flash the new ambyte build** (has the MQTT dispatch, rollback-confirm, `all`
   sweep, and version commands):
   ```powershell
   pio run -e esp32-s3-devkitm-1 -t upload
   pio device monitor -b 115200
   ```
2. **A hosting spot for C3 images** — commit each `firmware.bin` to a repo and use its
   **raw** URL (`https://raw.githubusercontent.com/<owner>/<repo>/<branch>/<path>`), the
   same way the ambyte image is hosted. Not a `/blob/` or `/tree/` page.
3. At least one AMBIT on a channel (tests assume **ch0**; the `all` test wants ≥2).

> **Rollback only arms for C3 images built from the current code** (they contain
> `verifyRollbackLater()` + cmd 29). The AMBIT is currently on an older image, so do
> **Test 0** first to get a rollback-capable image onto it.

### Build a C3 image (repeat per step, bumping the version so it's distinguishable)

In `ambit-IoT/fw_new/src/nvs1.h` bump `BATCH_VERSION`, then:
```powershell
pio run -d "C:\Users\LudovicoCaracciolo\Documents\Git-repo\ambit-IoT\fw_new" -e ambyte
```
The image is `%LOCALAPPDATA%\pio_build\ambit_fw_new\ambyte\firmware.bin`. Copy it to your
hosting repo path and push; that's the URL you pass to `ambit_ota`.

---

## Test 0 — baseline: rollback-capable receiver on the C3 (USB, one time)

Get a current-code image onto the AMBIT so later OTAs can roll back.

1. `BATCH_VERSION = 6`, build, and **USB-flash the C3 directly**:
   ```powershell
   pio run -d "C:\Users\LudovicoCaracciolo\Documents\Git-repo\ambit-IoT\fw_new" -e ambyte -t upload
   ```
2. On the ambyte console: `ambit_versions`
   - **PASS:** `AMBIT1: v0.0.6  (built …)`.

---

## Test 1 — MQTT trigger (+ dedupe)

1. Host a `BATCH_VERSION = 7` image; note its raw URL.
2. Publish to your **command topic** (the one `ota_update` uses) with your MQTT client:
   ```json
   {"type":"ambit_ota","id":"ambit-7-a","channel":0,"url":"https://raw.githubusercontent.com/.../firmware.bin"}
   ```
3. Watch the ambyte log.
   - **PASS:** `AMBIT OTA requested: ch=0 id=ambit-7-a …` → `streaming … 100%` →
     `OTA_END ok` → `fw after : v0.0.7` → `image confirmed — rollback cancelled` →
     `AMBIT OTA SUCCESS`. If MQTT is connected, an `ambit_ota_status` `success` message
     is published.
4. **Dedupe:** publish the **same** payload again (same `id`).
   - **PASS:** log shows `ambit_ota id=ambit-7-a already applied — ignoring` (no re-flash).
5. Re-publish with a **new** `id` (`ambit-7-b`, same url) → runs again (re-flashes v0.0.7).
   - **PASS:** runs to `SUCCESS` (proves a new id is not deduped).

---

## Test 2 — C3 rollback

### 2a — confirm + persistence (happy path)

After any successful OTA (e.g. Test 1), the new image was confirmed.
1. Power-cycle (or `reboot` over the C3's USB console) the AMBIT.
2. `ambit_versions` on the ambyte.
   - **PASS:** still `v0.0.7` — a confirmed image survives a reboot (no rollback).

### 2b — actual revert (the safety net)

Push a deliberately **unhealthy** image and confirm the C3 reverts.
1. Make a C3 image that boots but never services UART: in `ambit-1.ino` `setup()`, right
   after `Serial.begin(115200);` add a hang:
   ```cpp
   Serial.begin(115200);
   while (1) { delay(1000); }   // TEST 2b ONLY — boots but never answers commands
   ```
   Set `BATCH_VERSION = 8`, build, host it.
2. `ambit_ota 0 <url-to-broken-v0.0.8>` (or via MQTT).
   - Expect: `OTA_END ok — AMBIT1 rebooting` → after the wait + 3 retries,
     `AMBIT1 not answering after OTA — NOT confirming; it will roll back …` →
     `AMBIT OTA FAILED`. (The broken image is now running, **PENDING_VERIFY**, unconfirmed.)
3. **Power-cycle the AMBIT.** The bootloader sees the unconfirmed image and rolls back.
4. `ambit_versions`.
   - **PASS:** back to **v0.0.7** (the previous, confirmed image) — rollback worked.
5. Remove the `while(1)` line before any real build.

> A C3 image that *crashes* on boot rolls back automatically on the crash-reboot (no
> manual power-cycle needed); the `while(1)` variant hangs instead, so it needs the
> power-cycle in step 3. The C3 has no hardware watchdog.

---

## Test 3 — per-channel rollout (`all`)

Needs ≥2 AMBITs connected (e.g. ch0 + ch1).
1. Host a `BATCH_VERSION = 9` image.
2. `ambit_ota all <url>`  (CLI)  — or MQTT `{"type":"ambit_ota","id":"all-9","channel":"all","url":"…"}`.
3. Watch the log.
   - **PASS:** `AMBIT2: not present — skipping` for empty channels; each present channel
     runs `streaming…OTA_END…confirmed`; ends with `AMBIT OTA all: N/N present channels updated`.
4. `ambit_versions`.
   - **PASS:** every present channel reads `v0.0.9`.

---

## Test 4 — fleet version report

1. **CLI:** `ambit_versions`
   - **PASS:** one line per channel, e.g. `AMBIT1: v0.0.9  (built …)` / `AMBIT2: absent`.
2. **MQTT:** publish `{"type":"ambit_versions","id":"v1"}` to the command topic.
   - **PASS (log):** `AMBIT1: v0.0.9`, `AMBIT2: absent`, … (runs on the worker, not the
     MQTT task).
   - **PASS (MQTT):** an `ambit_versions` report is published to the status topic:
     `{"type":"ambit_versions","device_id":"…","id":"v1","channels":[{"ch":0,"present":true,"version":"0.0.9"},{"ch":1,"present":false},…]}`.

---

## Notes

- Each OTA suspends Lua + MQTT for the duration and resumes after; a normal measurement
  cycle should run on the channel afterward (regression check: `ambit_spec 0` / a Lua round).
- `ambit_ota_status` / `ambit_versions` MQTT messages only publish if the status topic is
  authorized (watch for `PUBACK rc=135`); the **console log is always authoritative**.
- Status reports are best-effort right after `comms_resume` — MQTT may not have reconnected
  yet when `success`/`failed` is sent, so rely on the log for the final verdict.

---

# Lua remote control — hardware test plan (added 2026-06-12)

Covers the CLI `lua` command, MQTT `lua_exec`, and MQTT `script_update`
(`components/script_update`; contract in `device-script-delivery.md`).

## Test 5 — CLI lua control

1. `lua status` → `RUNNING` (with a main.lua on SD) or `stopped`.
2. `lua stop` → `stopped`; LED leaves the measuring colours; `lua status` agrees.
3. `lua start` → reloads `/sdcard/main.lua`; schedule log lines resume.
4. `lua exec return device.uptime_ms()` → `ok: <number>` — **while main.lua runs**
   (proves the parallel ephemeral state).
5. `lua exec return ambit.ping(0)` → `ok: true` (serializes with the running
   schedule on the UART mutex; may wait a moment during a measurement).
6. `lua exec syntax error here` → `error (ESP_ERR_INVALID_ARG): …` (nothing ran).

## Test 6 — MQTT lua_exec

1. Publish to the command topic:
   ```json
   {"type":"lua_exec","id":"x1","code":"return 1+1"}
   ```
   - **PASS:** log `lua_exec ok: 2`; status topic gets
     `{"type":"lua_exec_result","id":"x1","ok":true,"result":"2"}`.
2. A hardware one: `{"type":"lua_exec","id":"x2","code":"return ambit.ping(0)"}`
   → `"result":"true"`.
3. A failing one: `{"type":"lua_exec","id":"x3","code":"error('boom')"}`
   → `"ok":false,"result":"…boom"`.
4. Runaway guard: `{"type":"lua_exec","id":"x4","code":"while true do end"}`
   → after ~120 s, `"ok":false` with `exec timeout` (the worker survives).

## Test 7 — MQTT script_update

1. Small valid script (JSON-escape the newlines as `\n`):
   ```json
   {"type":"script_update","id":"s1","script":"device.log('hello from pushed script')\nwhile true do device.sleep_ms(5000) end"}
   ```
   - **PASS:** log `main.lua replaced (… bytes) + runner restarted`; the pushed
     script's log line appears; status topic gets `state:"applied"`. SD now has
     `main.lua.bak` (the previous script).
2. **Dedupe:** re-publish the exact same message (same `id`) →
   `script_update id=s1 already applied — ignoring` (no restart).
3. **Bad script:** `{"type":"script_update","id":"s2","script":"this is not lua"}`
   → `state:"failed"` with the syntax error; the running script is untouched
   (no stop/restart happened).
4. **Checksum:** send with a deliberately wrong `"checksum"` → `failed` +
   `sha256 mismatch`; then with the correct one (`printf '%s' '<script>' |
   shasum -a 256`) → `applied`.
5. **Large script:** push the full `docs/exampleMain.lua` (~8 KB) inline (new id)
   → exercises the >2 KB transient-heap reassembly; `applied`, schedule resumes.
6. **Oversize guard:** any message > 16 KB → log
   `inbound message … > cap 16384 — dropped`, device unaffected.
7. **Recovery:** restore the real schedule afterwards (re-push exampleMain.lua or
   copy it back to SD).
