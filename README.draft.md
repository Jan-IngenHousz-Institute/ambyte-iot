# Ambyte IoT

ESP32-S3 field IoT node that drives an external **AMBIT** fluorescence sensor (up to four of them) over UART, buffers every measurement locally in an append-only `event_log` on the SD card, and publishes MQTT-over-TLS telemetry to **AWS IoT Core**. The measurement schedule is a **Lua script on the SD card** (`/sdcard/main.lua`) — no reflash to change what/when the device measures. The firmware supports **self-OTA over MQTT** (dual-slot with rollback), two independent **AMBIT firmware-update paths**, and **solar/battery power management** so an unattended field unit only spends radio energy when it has external power.

Provisioning (Wi-Fi, MQTT identity, TLS certs, build-time clock) is generated on the host from `.env` + a `device_certs/<bundle>/` PEM set and flashed into the NVS partition next to the firmware — **no BLE companion app, no runtime provisioning round-trip** (BLE provisioning is deprecated/compiled out).

---

## Key facts

| | |
|---|---|
| **Hardware target** | ESP32-S3-DevKitM-1, ESP32-S3-WROOM-1, **16 MB flash**, no PSRAM |
| **Framework** | ESP-IDF **5.5.0** via PlatformIO (`espressif32` platform) |
| **PlatformIO env** | `esp32-s3-devkitm-1` (the only buildable env; the old 2 MB Adafruit Feather env has been removed) |
| **External sensor** | AMBIT (multispeq-style), ESP8685 / ESP32-C3, up to 4 channels over a shared UART/FFC link |
| **Onboard sensors** | BME280 (T/H/P), PCF2131 RTC, MP2731 battery charger / power-path (all I2C) |
| **Transport** | MQTT v5 over mutual TLS → AWS IoT Core (device cert + private key from NVS) |
| **Storage** | Append-only `event_log` on FAT/SD (`/sdcard/events/`); read cursor + `next_id` high-water mark in NVS. **SQLite has been removed.** |
| **Scripting** | Lua 5.4 VM running `/sdcard/main.lua`; hot-updatable over MQTT |
| **Power** | Radio publishing gated on external power (MP2731); DFS clock scaling 40–160 MHz (`esp_pm`) |
| **Console** | USB-Serial/JTAG @ 115200 |
| **License** | CERN Open Hardware Licence Version 2 — Strongly Reciprocal (see [LICENSE](LICENSE)) |

---

## Quick start

```sh
# 1. Clone + init the littlefs submodule (it is the only submodule)
git clone <repo-url>
cd ambyte-iot-ludo
git submodule update --init --recursive

# 2. Provisioning data (first time only)
cp .env.example .env && $EDITOR .env          # Wi-Fi creds, MQTT URI, topic root, ...
#   Drop your AWS IoT thing's PEM bundle into device_certs/<thing-name>/

# 3. Build + flash (host-side NVS pre-pop happens automatically via tools/extra_script.py)
pio run -e esp32-s3-devkitm-1 -t upload
pio device monitor -b 115200

# 4. Put a measurement schedule on the SD card
cp docs/exampleMain.lua  /path/to/AMBYTE_SD/main.lua
```

The device boots, seeds its Wi-Fi/MQTT/TLS from NVS, connects to AWS IoT Core, runs `/sdcard/main.lua`, stores measurements to the SD `event_log`, and `sync_runner` drains them to the cloud whenever it is on external power with a valid clock.

---

## Architecture / runtime overview

The firmware uses a hexagonal **ports-and-adapters** design. The `domain` component is header-only and defines the port interfaces (`sensing_port`, `persistence_port`, `messaging_port`, `uart_sensor_port`, `device_status_port`). `main/app_main.c` is the composition root that wires concrete driver adapters into `device_commands` (the business-logic core). `device_commands` is called directly by both the CLI and the Lua VM — there is no command queue/IPC between them.

### Data flow

```
  /sdcard/main.lua  (Lua 5.4 schedule: sync.*/device.*/ambit.*)
        |  measure + store  (never publishes directly)
        v
  device_commands  ──drives──> uart_sensors ──UART──> AMBIT x4 (ESP32-C3)
        |                       bme280 / mp2731 / rtc (I2C)
        |  store one JSON event per measurement
        v
  event_log  (append-only, /sdcard/events/ev-NNNNNN.log; cursor+next_id in NVS)
        |  claim oldest PENDING (one in-flight slot)
        v
  sync_runner  (SOLE publisher; wakes on store; power+clock gated)
        |  QoS1, one message in flight, PUBACK -> advance cursor
        v
  mqtt_client (esp-mqtt) ──mutual TLS──> AWS IoT Core
```

### Boot sequence (single-threaded in `app_main`)

1. `sd_logger_init()` — tee WARN/ERROR logs to SD (skipped in `SPIKE_LOG` builds), 5 s settle delay, log the build tag.
2. NVS init (`app_init_nvs`, auto-erases on version mismatch / no free pages).
3. `esp_pm_configure()` — DFS window `AMBYTE_PM_MIN_FREQ_MHZ`(40)…`AMBYTE_PM_MAX_FREQ_MHZ`(160), light-sleep off.
4. Status LED → certs → `device_config` → resolve MQTT `uri`/`client_id`/`topic_root`/topics from NVS (with Kconfig fallback and `{MAC}` token substitution) → one-time status-topic migration.
5. `mqtt_client_init` → `command_router_init` → `ota_update_init` → `ambit_ota_init` → `script_update_init`.
6. Non-blocking Wi-Fi (`wifi_manager_connect_stored_async`) + `on_got_ip`/`on_wifi_disconnect` handlers.
7. I2C + RTC + clock bootstrap + BME280 + MP2731.
8. `uart_sensors_init` (auto-pings 4 channels) → SD card mount → LittleFS mount → `event_log_init` → SD hot-plug monitor.
9. `device_commands_init` (composition) → **CLI** (first, so the operator has a prompt during the rest) → `sync_runner_start` (first drain held by the boot-complete latch) → LED blinker → `ambit_flash_boot_sync` (power-gated AMBIT auto-flash from SD, pre-Lua) → Lua runner → **boot complete**: deferred MQTT start + upload drain released. (`on_got_ip` parks the MQTT start while boot is in progress so the TLS handshake/backlog can't starve the startup sequence.)

### Tasks (each created inside its own component)

- **sync_runner** — the only MQTT publisher; also emits the STATUS heartbeat and runs a connectivity watchdog.
- **lua_runner** — runs `/sdcard/main.lua` once, self-deletes on return/stop, restarts on SD reinsert. **Pinned to core 1 (APP_CPU)** so latency-sensitive UART measurement isn't preempted by the Wi-Fi/LwIP stack on core 0.
- **sd_logger writer**, **RTC periodic sync** (3600 s), **SD hot-plug monitor** (2000 ms poll), **LED blinker**, **CLI**.
- **ota_update / ambit_ota / script_update** — lazy workers spawned on demand (zero steady-state heap).
- **mqtt_client / wifi** run on the esp-mqtt / esp-netif event loops.

### Clock bootstrap

At boot the system clock is seeded from the PCF2131 RTC. If the RTC is invalid or reads *behind* the NVS `flash_time` (image build epoch), the firmware writes `flash_time` into the RTC (clearing the oscillator-stop flag). Dev boards with no RTC `settimeofday()` from `flash_time`. Every measurement timestamp and MQTT envelope depends on this.

---

## Prerequisites

- [PlatformIO Core](https://platformio.org/install) (VS Code extension or `pio` CLI)
- [`uv`](https://docs.astral.sh/uv/) for the Python host tooling (provisioning + test client)
- Python 3.13 (auto-managed by `uv`)
- Git with submodule support

The repo root is a `uv` project ([pyproject.toml](pyproject.toml), [uv.lock](uv.lock)); `uv` auto-creates `.venv/` from the lockfile. (Note: `pyproject.toml` metadata still calls the project a "BLE provisioning helper" and lists `bleak` — this is stale; provisioning is now host-side NVS pre-pop and Bluetooth is compiled out.)

### ESP-IDF Python env

The NVS pre-pop step needs `esp_idf_nvs_partition_gen`, and the core build needs `idf_component_manager`, both installed into ESP-IDF's **own** venv (not PlatformIO's outer `penv`). If a build fails with `No module named idf_component_manager` or `No module named esp_idf_nvs_partition_gen`:

```powershell
# Windows PowerShell — adjust .espidf-5.5.0 to whatever PlatformIO created under ~/.platformio/penv/
uv pip install --python "$env:USERPROFILE\.platformio\penv\.espidf-5.5.0\Scripts\python.exe" esp_idf_nvs_partition_gen
uv pip install --python "$env:USERPROFILE\.platformio\penv\.espidf-5.5.0\Scripts\python.exe" -r "$env:USERPROFILE\.platformio\packages\framework-espidf\tools\requirements\requirements.core.txt"
```

```sh
# Linux/macOS
uv pip install --python ~/.platformio/penv/.espidf-5.5.0/bin/python esp_idf_nvs_partition_gen
uv pip install --python ~/.platformio/penv/.espidf-5.5.0/bin/python -r ~/.platformio/packages/framework-espidf/tools/requirements/requirements.core.txt
```

`build_nvs_image.py` searches `IDF_PYTHON_ENV_PATH`, then `~/.platformio/penv/.espidf-*` (newest first), then `sys.executable` to find the right interpreter.

---

## Build, flash, monitor

```sh
pio run   -e esp32-s3-devkitm-1               # build
pio run   -e esp32-s3-devkitm-1 -t upload     # build + flash app + NVS image
pio device monitor -b 115200                  # serial console
```

Editing `sdkconfig.defaults` alone does **not** re-apply — delete the per-env generated `sdkconfig.esp32-s3-devkitm-1` to regenerate it.

### Factory reset / re-provision

The NVS image is regenerated on every build, so to re-provision just edit `.env` (or swap the bundle) and re-flash:

```sh
pio run -e esp32-s3-devkitm-1 -t upload            # overwrites NVS, keeps field data
```

`erase_flash` is only needed if you deliberately want to wipe stored data:

```sh
pio run -e esp32-s3-devkitm-1 -t erase_flash -t upload
```

> **Caution:** the data partitions (`littlefs`, `storage`, `coredump`) keep their v1 offsets, so a plain `upload` (no `erase_flash`) preserves field data. To migrate a legacy single-app unit to the dual-OTA layout **without** losing data, do a plain `upload`.
> Note: every `upload` re-flashes the NVS image and wipes runtime NVS keys such as the `event_log` cursor / `next_id`. A plain reboot preserves them. The SD `event_log` itself always survives; `next_id` is re-seeded above the SD log's max on boot.

To flash stock firmware **without** provisioning data, set `AMBYTE_NVS_SKIP=1` for the build — the device boots, logs `Device not provisioned`, and stays up for CLI debugging.

---

## Provisioning (host-side NVS pre-pop)

Provisioning runs as a PlatformIO `pre:` hook ([tools/extra_script.py](tools/extra_script.py)) on every build. It calls [tools/build_nvs_image.py](tools/build_nvs_image.py), which bakes `.env` + a resolved cert bundle into a **24 KiB** NVS binary and appends it to `FLASH_EXTRA_IMAGES` at offset `0x9000` so `-t upload` writes it alongside the app. The hook skips (flashes stock firmware) if `AMBYTE_NVS_SKIP` is truthy or if there is no `.env` and no `AMBYTE_*` env var; on generator failure it fails the build.

### NVS namespaces / keys

| Namespace | Keys |
|---|---|
| `device_cfg` | `mqtt_uri`, `mqtt_client_id`, `mqtt_topic_root`, `device_id`, `protocol_id`, `device_name`, `device_ver`, `device_firm`, `firmware_ver`, + optional `cmd_topic`, `status_topic`, `timezone`, `heartbeat_s` (u32), `flash_time` (u32) |
| `certs` | `ca_cert`, `dev_cert`, `dev_key` (PEM file contents inlined) |
| `wifi_prov` | `provisioned` = 1 (u8; gates `app_main`) |
| `wifi_creds` | `ssid`, `pass` (consumed once at first boot, then the seed is erased) |

Missing required fields cause a loud non-zero exit listing them — never a silent half-provision. (Subtlety: each namespace is declared exactly once in the intermediate CSV, because the NVS generator treats a repeated `namespace` line as a *new* namespace.)

### Config / env resolution order

1. Shell env var (wins over `.env`)
2. `.env` entry at repo root (auto-loaded, gitignored, quotes stripped, `#` comments ignored)
3. For cert slots: explicit `AMBYTE_CA_CERT` / `AMBYTE_DEV_CERT` / `AMBYTE_DEV_KEY`, else bundle auto-discovery

### Cert bundle layout

Put each thing's AWS IoT PEM files in its own subdirectory under [device_certs/](device_certs/) (gitignored):

```
device_certs/
  <thing-name>/
    AmazonRootCA1.pem            # CA: name contains 'rootca', else *.pem containing 'ca'
    <hash>-certificate.pem.crt   # device cert
    <hash>-private.pem.key       # device key
```

Bundle selection precedence: `AMBYTE_CERT_BUNDLE` → the single subfolder if only one → interactive TTY prompt. The **bundle folder name is treated as the AWS IoT thing name and becomes the default `AMBYTE_CLIENT_ID`** (AWS IoT rejects the handshake if the cert is not bound to the client id). `AMBYTE_TOPIC_ROOT` is **not** auto-derived from the bundle name.

### `{MAC}` token expansion

The literal token `{MAC}` inside `AMBYTE_CLIENT_ID` / `AMBYTE_TOPIC_ROOT` (e.g. `AMBYTE_{MAC}`) is expanded **by the firmware at boot** to this board's STA MAC, giving every board a unique MQTT identity from a single shared `.env`. Omit `{MAC}` for a fixed value. The envelope `device_id` is always the STA MAC (formatted `XX:XX:...`). The AWS IoT policy must permit the resulting client id + topic.

### RTC from `flash_time`

`build_nvs_image.py` writes `device_cfg/flash_time = int(time.time())` (image build epoch) into every NVS image. The firmware sets the RTC from it at boot when the RTC is invalid or behind it. This replaces the old serial post-upload hook `set_rtc_on_upload.py`, which is now commented out in `platformio.ini`.

### Manual NVS build (no flashing)

```sh
uv run python tools/build_nvs_image.py --out ./nvs.bin
```

---

## Data & telemetry

### Why the append-only `event_log` replaced SQLite

Stress-testing 4 AMBIT channels every few seconds corrupted the SQLite events DB on consumer microSD cards (SQLite's in-place page/header rewrites are hostile to FATFS), and recovery leaked into an OOM spiral that killed MQTT. The workload is a store-and-forward FIFO, not relational queries. So SQLite was replaced — behind the same `persistence_port.h` interface — by [components/event_log](components/event_log) (a prior TXT-file logger had run for months on the same cards without corruption).

- **On-disk format v2:** rotating tab-delimited files `/sdcard/events/ev-NNNNNN.log` (rolled past 256 KiB), one newline-terminated record per event, 9 fields: `measure_id · channel · device · tag · cmd_raw · start_ms · end_ms · metadata · payload`.
- **Durability:** writes are appends only; read cursor + `next_id` high-water mark live in NVS. Periodic flush (every 1500 ms / 8 records), not per-event fsync. A torn final record is skipped on read (no boot-time repair); a short write is rolled back with `ftruncate`. Drained rotated files are deleted.
- **IDs:** `measure_id` is monotonic int64, HWM persisted to NVS every 64 ids and **re-seeded above the SD log's max on boot** (NVS is wiped on every reflash, but the SD log survives, and openJII dedupes on `(device_id, measure_id)`).

### Inspecting / decoding telemetry

To see decoded telemetry without standing up the full cloud, use the demo/analysis workbooks that ship in the repo:

- [ambyteiot_ambit_demo.ipynb](ambyteiot_ambit_demo.ipynb) — Jupyter notebook that decodes and plots AMBIT telemetry.
- [ambit-demo-workbook.jii](ambit-demo-workbook.jii) — openJII workbook counterpart.
- [docs/workbook-OpenJII.md](docs/workbook-OpenJII.md) — notes on the openJII workbook / consumer-side data flow.

For live wire inspection without the firmware in the loop, use [docs/mqtt_tls_test_client.py](docs/mqtt_tls_test_client.py) (see [Host tools](#host-tools)).

### Publish pipeline (`sync_runner`, sole publisher)

Lua **never** touches MQTT. [components/sync_runner](components/sync_runner) is the only publisher: it wakes on store (event-driven, not polled; a burst of N stores collapses into one drain) and drains all PENDING events **one message in flight at a time** at **QoS 1**, advancing the cursor on PUBACK (at-least-once; duplicates deduped downstream). Draining runs only while `sync_runner_is_allowed()`:

- **no measurement in progress** (don't compete with latency-sensitive UART reads), **and**
- **on external power** (Phase-1 power gate; battery-queued events flush when power returns), **and**
- **clock is valid** (≥ 2024-01-01 UTC — never ship 1970-stamped events into the wrong cloud date partition).

**Reliability hardening** (current branch): one in-flight slot is cleared on both Wi-Fi and MQTT-level disconnects (`device_commands_on_mqtt_disconnect`), and a slot held past 60 s without a PUBACK is reaped back to PENDING (`device_commands_reap_stale_inflight`) so a lost/expired QoS1 PUBACK on a marginal link can't silently wedge the drain forever. A separate connectivity **watchdog** reboots the unit if it *should* be publishing (external power, clock valid, events pending) but lands no PUBACK for 1 h — the SD backlog and NVS cursor survive.

### STATUS heartbeat

A firmware-owned STATUS heartbeat rides the `sync_runner` loop (default 300 s, NVS override via `heartbeat_s`) so telemetry survives a missing/crashed `main.lua`. It stores a `tag=STATUS` event with Wi-Fi/provisioned/DB/publish-gate flags plus MP2731 power keys and onboard BME280 T/H/P when those reads succeed.

### Payload schema v2

Each stored event becomes exactly one MQTT message under the v2 envelope. `tag` is a firmware-controlled origin enum (`MEASUREMENT` / `STATUS` / `DEVICE_INFO`); `channel` (`uart_<n>` / null) replaces the old `sensor` field; `device` is best-effort sensor self-identification (AMBIT `ambit_name`); `cmd_raw` is the command in the target device's own vocabulary (replayable). The envelope `timestamp` is the **measurement** time, so battery-queued events carry their capture time, not publish time. Full spec: [docs/mqtt-payload.md](docs/mqtt-payload.md) (v2) and [docs/payload-v2-plan.md](docs/payload-v2-plan.md).

---

## AMBIT integration

The **AMBIT** is an external multispeq-style measurement device (leaf photosynthesis / fluorescence, spectral, PAR, leaf/chip temperature), each built on an ESP8685 (ESP32-C3 die) running Arduino firmware. Up to four attach as channels; the ambyte is the host/gateway.

- **UART topology:** CH0=UART1, CH1=UART2, CH2/CH3 share UART0 (per-query GPIO remap). A bus mutex serializes measurement / OTA / ROM-flash / Lua so nothing collides (busy bus → `ESP_ERR_TIMEOUT`).
- **Binary protocol** ([ambit_protocol.h](components/device_commands/include/ambit_protocol.h)): `RUN`(21)/`RUN_MPF`(20) synchronous measurements; `GET_SPEC`(31), `GET_TEMP`(32); config `SET_GAINS`(1)/`SET_CURRENTS`(2); actions `BLINK`(5)/`CALIBRATE_BASELINE`(6)/`ACTINIC`(4); `GET_INFO`(33) sub-types CALIBRATION/FW/METADATA; `SET_METADATA`(37). Typed C wrappers are `cmd_ambit_*` in `device_commands`.
- **Parallel trigger → poll → fetch** (cmds 22/23/24): so long multi-second runs don't block the shared UART — `trigger` starts a retained run, `poll` reads one async state byte (BUSY inferred from poll timeout), `fetch` streams the buffered arrays. HW-working with real `arrun` events published via `run_trace`.
- **Identity/calibration caching:** fetched once per channel after (re)connect (`GET_INFO` 33): `device_id`, `fw_version` (major.minor.batch), `cal_version` (CRC32), `ambit_name`, `actinic_coef`. The first fetch emits one `DEVICE_INFO` event.

### AMBIT firmware update — two independent paths

- **Strategy B — cooperative app-OTA over UART** ([components/ambit_ota](components/ambit_ota)): the ambyte downloads a C3 `.bin` from HTTPS to SD, then streams it in ≤200-byte CRC16 chunks (cmds 25–29) into the AMBIT's spare OTA slot with C3-side rollback. Needs a **running, cooperating** AMBIT. Trigger: CLI `ambit_ota <ch> <url>` or MQTT `{type:ambit_ota}` (`ch` can be `all`/0xFF).
- **Strategy A — ROM-bootloader UART flasher** ([components/ambit_flash](components/ambit_flash) + vendored [components/esp_serial_flasher](components/esp_serial_flasher)): the **universal** path for **bare / bricked / pre-OTA** units. It drives the C3 hardware straps (shared `CHIP_EN` reset on IO1, per-channel `GPIO9` boot strap) to force one target into the ROM download mode, then flashes 4 region images from `/sdcard/ambit_fw/<ver>/` (`bootloader.bin@0x0`, `partitions.bin@0x8000`, `boot_app0.bin@0xe000`, `app.bin@0x10000`) with per-region MD5 verify. **NVS at `0x9000` is never written**, so per-unit AMBIT calibration survives. CLI: `ambit_probe`, `ambit_dl`, `ambit_flash <ch> <ver>`.
- **Version-drift detection** ([ambit_flash_check](components/ambit_flash), CLI `ambit_check` / `ambit_versions`): on demand, reads each AMBIT's running version (cmd 33/2), compares against the highest complete `/sdcard/ambit_fw/<ver>/` folder, and logs match/mismatch plus the exact `ambit_flash <ch> <ver>` to run. Read-only + bus-mutex-serialised so safe with Lua active.
- **Boot auto-flash** ([ambit_flash_boot_sync](components/ambit_flash)): once per boot, before Lua starts, every present AMBIT is version-checked (silent channels are ROM-probed, so bare/bricked units are found and revived) and any channel whose version differs from the SD target is flashed automatically and verified. Gated on the same power condition as MQTT publishing (skips to the next powered boot on battery) + a per-channel NVS fail cap (3 unverified attempts per target ⇒ give up until a different version is staged).

---

## Firmware OTA (ambyte self-update)

[components/ota_update](components/ota_update) (docs/ota-update-plan.md Stage 3): publish `{type:ota_update,id,url}` to the command topic. The worker suspends MQTT (frees TLS heap — the board can't hold two TLS sessions on its ~17 KB largest block), runs `esp_https_ota` into the spare OTA slot, and reboots. The new image boots `PENDING_VERIFY` and marks itself valid **only after MQTT reconnects** (proving health), else it rolls back within 5 min.

```sh
uv run docs/mqtt_tls_test_client.py --publish "$AMBYTE_COMMAND_TOPIC" --qos 1 --mqtt5 \
  --message '{"type":"ota_update","id":"ota-1","url":"https://github.com/<owner>/<repo>/releases/download/<tag>/firmware.bin"}'
```

- `id` dedupes retries (latched only on success — a failed download doesn't burn the id).
- `url` must be a **direct** `.bin`: a GitHub Release asset (`…/releases/download/<tag>/firmware.bin`) or `raw.githubusercontent.com/...`. `github.com/.../tree/` or `/blob/` web URLs serve HTML and are rejected.
- Requires the **dual-OTA partition layout** (`ota_0`/`ota_1` + `otadata`, `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`) — a one-time USB reflash migrates legacy single-app units (see Factory reset caution above).

### Remote Lua delivery (Stage 4)

[components/script_update](components/script_update), dispatched by `command_router`:

- `{type:script_update,id,script,checksum?}` — sha256 (if given) → Lua syntax check → write `main.lua.new` + fsync → stop runner → keep `main.lua.bak` → atomic rename → restart runner → NVS id latch on success only. Inline cap 16 KiB.
- `{type:lua_exec,...}` — runs a snippet in an ephemeral Lua state (120 s budget) and publishes the result.
- CLI twins: `lua start|stop|status|exec`. See [device-script-delivery.md](device-script-delivery.md).

---

## Power management

- **Radio gating (Phase 1):** `device_commands_publish_power_ok()` allows backlog draining only when external (solar/USB) power is present (keyed on VIN via the MP2731 charger @ I2C 0x4B, not input current). Debounced with asymmetric dwell (≈15 s to open on power present, 60 s to close on power absent); the charger read is cached (5 s). Returns true (never gates) when no charger is wired (dev boards).
- **DFS clock scaling (Phase 2):** `esp_pm` dynamic frequency scaling `AMBYTE_PM_MIN_FREQ_MHZ=40` … `MAX=160` MHz (defined in `app_main.c` — PlatformIO `-D` flags don't reach IDF component sources). CPU coasts at the min during idle gaps. Light-sleep stays **off**. DFS is safe: AMBIT UART pinned to XTAL, SD/SPI holds its own APB lock, measurement windows hold a `NO_LIGHT_SLEEP` PM lock. `min=40` is HW-validated (1 h soak). Requires `CONFIG_PM_ENABLE`.
- **PM-independent quick wins:** Wi-Fi power-save, MQTT keepalive tuning, ping cache, BME280 forced mode.

---

## SD card

- The measurement schedule is `/sdcard/main.lua`, loaded via `luaL_loadfile()` once at boot ([components/lua_runner](components/lua_runner)). To iterate: edit on the host, swap the card, reset — no reflash. (Or push a new script over MQTT via `script_update`.)
- Behaviour when the script is missing: no SD mounted → Lua task skipped, CLI+MQTT continue; SD mounted but no `main.lua` → task starts, fails the load, exits cleanly.
- **Hot pull/reinsert recovery** ([components/sd_card](components/sd_card)): no card-detect pin, so a monitor polls `sdmmc_get_status` (CMD13, 2000 ms) plus a **lock-free error-driven loss latch** (writers call `sdcard_report_io_error/ok` and gate on `sdcard_io_lost()`) — needed because CMD13 alone loses the race to a task stuck in a multi-second failing transfer (priority inversion, the historic sdmmc `0x107` flood). On loss the Lua runner stops and `event_log_on_sd_lost` fires; on reinsert `event_log_on_sd_restored` runs and Lua restarts. `sd_logger` buffers WARN/ERROR in a RAM ring while the card is absent and flushes on remount.

Lua binding tables exposed to scripts (see the `luaL_Reg` arrays in `lua_runner.c`): `device.*` (rtc/status/power/sd_ready/sleep_ms/log/PWM/…), `uart.*` (raw transport), `db.*` (`store_event`/`next_id`, for custom/derived events), `ambit.*` (ping/spec/leaf_temp/run/trigger/poll/fetch/run_mpf/set_gains/set_currents/blink/calibrate/actinic/set_metadata), and `sync.*` (interval/clock/weekly/sunrise-sunset scheduling from lat/lon + tz). The old `mqtt` Lua table was removed — scripts no longer publish.

Example schedules in [docs/](docs/):

- [docs/exampleMain.lua](docs/exampleMain.lua) — minimal starter schedule.
- [docs/sync.lua](docs/sync.lua) — reference for the `sync.*` scheduling surface (interval/clock/weekly/sunrise-sunset).
- [docs/crashtest-main.lua](docs/crashtest-main.lua) — the crash-test cadence (SS @ 10 s + MPF @ 1 min) cited in the roadmap.

---

## Host tools

| Tool | Purpose |
|---|---|
| [tools/build_nvs_image.py](tools/build_nvs_image.py) | Build the 24 KiB NVS provisioning image from `.env` + cert bundle (run by the `pre:` hook, or manually with `--out`) |
| [tools/extra_script.py](tools/extra_script.py) | PlatformIO `pre:` hook that runs the NVS builder and appends the image at `0x9000` |
| [tools/get_mac.py](tools/get_mac.py) | Read the STA MAC from a **running** board via the CLI `status` command over USB-Serial/JTAG (does not reset the chip) |
| [tools/save_mac.py](tools/save_mac.py) | Read the base MAC from efuse via `esptool read_mac` (works on blank/bricked units) and append a row to `flashed_macs.csv` |
| [docs/mqtt_tls_test_client.py](docs/mqtt_tls_test_client.py) | Host-side paho-mqtt client using the same device cert/key — subscribe/publish against AWS IoT without the firmware in the loop |

`flashed_macs.csv` (gitignored) columns: `timestamp_utc, mac, port, git_commit, note`. All tools run via `uv run python tools/...`.

```sh
uv run docs/mqtt_tls_test_client.py --subscribe                 # $AMBYTE_TOPIC_ROOT/#
uv run docs/mqtt_tls_test_client.py --publish --mqtt5           # dummy measurement
uv run python tools/get_mac.py                                  # print running board's MAC
```

---

## Repository layout

```
components/
  domain/            # header-only ports (sensing/persistence/messaging/uart/device_status)
  device_commands/   # business-logic core; cmd_* ops; envelope builder; in-flight slot + reaper
  command_router/    # inbound MQTT JSON dispatch (ping/ota_update/script_update/lua_exec)
  event_log/         # append-only SD event store (REPLACES SQLite)
  sync_runner/       # sole MQTT publisher; power/clock gate; heartbeat; connectivity watchdog
  mqtt_client/       # esp-mqtt wrapper (mutual TLS to AWS IoT)
  uart_sensors/      # AMBIT UART bus (4 channels) + ROM-flash reset/boot control lines
  ambit_ota/         # AMBIT app-OTA over UART (Strategy B)
  ambit_flash/       # AMBIT ROM-bootloader UART flasher (Strategy A) + version-drift check
  esp_serial_flasher/# vendored Espressif esp-serial-flasher (used only by ambit_flash)
  ota_update/        # ambyte self-OTA over MQTT (dual slot + rollback)
  script_update/     # remote Lua delivery (main.lua replace + lua_exec)
  lua/ lua_runner/   # Lua 5.4 VM + bindings; runs /sdcard/main.lua
  time_sync/         # pure RTC/local-time scheduling math (sync.* Lua surface)
  bme280/            # I2C temp/humidity/pressure
  mp2731/            # I2C battery charger / power-path (power gate + battery telemetry)
  pcf2131tfy_rtc/    # PCF2131 RTC driver + system-clock sync
  sd_card/           # SDMMC mount + hot-plug monitor
  sd_logger/         # tees WARN/ERROR logs to /sdcard/logs (RAM ring + low-prio writer)
  ambyte_status/     # RGB status LED + firmware-owned blinker
  certs/             # NVS-backed TLS cert store
  device_config/     # NVS-backed device config (device_cfg namespace)
  wifi_manager/      # non-blocking STA connect/reconnect (NVS-seeded)
  hal/               # shared i2c_bus
  littlefs/          # submodule (mounted at /littlefs; currently vestigial as a data store)
  spike_log/         # SPIKE_LOG-gated SD soak test (dev/diagnostic only)

main/                # app_main.c composition root
tools/               # host scripts: NVS builder, PlatformIO hook, get_mac/save_mac
docs/                # payload/OTA/flash plans, mqtt_tls_test_client.py, example Lua scripts
planning/            # DDD phase plan + LLM prompts + HW-test notes
device_certs/        # (gitignored) AWS IoT PEM bundles
ambyteiot_ambit_demo.ipynb / ambit-demo-workbook.jii  # telemetry decode/analysis workbooks
.env / .env.example  # provisioning defaults
platformio.ini       # PlatformIO env + pre: hook
partitions.csv       # dual-OTA 16 MB layout
LICENSE              # CERN-OHL-S v2
```

> **Legacy / unused:** `littlefs` is mounted but no measurement data is written there (the event store is on FAT/SD); `spike_log` is a dev-only SD soak gated behind the `SPIKE_LOG` build flag. There is **no** `sqlite3` or `persistence` component in the built firmware — neither is present in the tree.

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| `No module named 'idf_component_manager'` / `No module named esp_idf_nvs_partition_gen` | Install into ESP-IDF's own venv — see [Prerequisites → ESP-IDF Python env](#esp-idf-python-env) |
| `littlefs/lfs.h: No such file` | `git submodule update --init --recursive` |
| `ambyte-nvs: missing required value(s)` at build | Add the listed `AMBYTE_*` keys to `.env`, or set `AMBYTE_NVS_SKIP=1` to flash without provisioning |
| Config changes in `sdkconfig.defaults` don't take effect | Delete the generated `sdkconfig.esp32-s3-devkitm-1` and rebuild |
| Device boots and logs `Device not provisioned` | NVS image wasn't flashed — confirm `extra_scripts = pre:tools/extra_script.py` in `platformio.ini` and that `.env` resolves to non-empty values |
| MQTT connects then immediately disconnects | `AMBYTE_CLIENT_ID` doesn't match the thing the cert is bound to — align `AMBYTE_CERT_BUNDLE` with the thing name and let client_id auto-derive |
| Telemetry stops but no data loss / device on external power | In-flight slot may have stalled — check the `inflight` CLI command; the 60 s reaper + MQTT-disconnect clear should recover it, else the 1 h watchdog reboots |
| Events pile up in `event_log`, nothing publishes | Expected on battery (power gate) or before the clock is valid (< 2024) — verify external power and RTC/clock |
| `Lua runner not started: SD card not mounted` | Insert an SD card with `main.lua` at the root (copy [docs/exampleMain.lua](docs/exampleMain.lua)) |
| `failed to load /sdcard/main.lua` | SD mounted but file missing/unreadable — check the card has `main.lua` at the root |
| sdmmc `0x107` errors after pulling the SD card | Hot-plug recovery should latch the loss and remount on reinsert; if it floods, this path needs the HW pull/reinsert verification still pending (see Status) |
| Wrong PAR / spectral values on an AMBIT | Check AMBIT calibration / `spec_coef`; version drift (`ambit_check`) may indicate a stale AMBIT firmware |

---

## Contributing / branch workflow

- `main` is the release branch; work happens on feature branches (e.g. the current `feature/publish-reliability-hardening`) and lands on `main` via pull request.
- **Build before you commit:** `pio run -e esp32-s3-devkitm-1` must pass (the single buildable env). Provisioning artifacts (`.env`, `device_certs/`, `flashed_macs.csv`) are gitignored — never commit them.
- Keep the ports/adapters boundary intact: business logic lives in `device_commands` behind the `domain` port interfaces; drivers are adapters wired in `main/app_main.c`. Don't let Lua or the CLI reach past `device_commands`.
- Much of the roadmap below is code-complete but **awaiting hardware verification** — note HW-test status in the PR when a change touches those paths.

---

## Status / roadmap

**Hardware-verified:**
- MQTT-over-TLS telemetry to AWS IoT Core (after resolving AWS policy / error-143 restrictions).
- Payload v2 schema (Phases 1–5 shipped).
- Parallel AMBIT measurement (trigger/poll/fetch) with real `arrun` events published.
- Single shared UART for all AMBIT channels.
- MQTT-triggered ambyte self-OTA, end-to-end with dual-slot rollback.
- AMBIT ROM-bootloader UART flasher (probe on all channels, full C3 flash on ch3).
- AMBIT version-drift detection (target detected, out-of-date channel flagged).
- Publish-reliability in-flight-slot reaper (stall → reaped → drain intact).
- DFS power management at `min=40` MHz (1 h clean soak).
- Cert provisioning end-to-end (host-side NVS pre-pop): TLS connect + publish to
  AWS IoT under the `AMBYTE_{MAC}` client identity.
- Large-publish defer-gate (no more TLS-write failure → MQTT drop on a
  fragmented heap; the drain stays connected and retries).

**Pending / needs hardware verification:**
- SD hot pull/reinsert recovery (builds clean; pull/reinsert HW test not yet done).
- AMBIT app-OTA over UART (Strategy B) — verification status ambiguous vs the self-OTA that passed.
- Crash-test measurement cadence tuning (benchmark: SS @ 10 s + MPF @ 1 min).
- Light-sleep and Wi-Fi radio gating (power management next steps).
- Two remaining openJII partner asks on the payload schema.
- Large arrun-trace publish fix (early event free + `OUT_CONTENT_LEN` 2048):
  the previously-wedging ~5.4 KB trace must publish, not defer — see
  [planning/hwtest-publish-reliability.md](planning/hwtest-publish-reliability.md).
- Connectivity watchdog reboot (`netwd test`) with a TLS transfer in flight
  (validates the sys_evt 6144 B stack bump on its real failure path).
- Reaper against a REAL lost PUBACK (link up, return path blocked) + MQTT-level
  disconnect clear with Wi-Fi associated — the synthetic `inflight stall` only
  proves the mechanism.
- `evlog rewind` re-queue + re-drain; fluo-drop payload shape on a live trace
  (openJII heads-up sent before field units get the fluo-dropping build).

**Recent (this branch, needs the HW pass above):** stale-slot reaper +
MQTT-disconnect clear, connectivity watchdog (1 h no-PUBACK reboot), large-publish
heap hardening (defer-gate + Lua-GC settle + early event free + 2 KiB TLS out
records), `evlog`/`inflight`/`netwd` CLI, dropped the derived `fluo` payload key
(computed downstream from `s_630`/`r_630`), AMBIT fw 1.0.0 SD image under
[docs/example_sdFolder/ambit_fw/1.0.0](docs/example_sdFolder/ambit_fw/1.0.0).

**Separate direction (not on this branch):** `USBHubPlan.md` proposes moving the console to a UART and using the native USB peripheral as a USB host through a powered hub, replacing the AMBIT/UART pipeline with a `usb_sensors` (CDC-ACM) component — a plan/branch, not merged.

---

## License

This project is released under the **CERN Open Hardware Licence Version 2 — Strongly Reciprocal (CERN-OHL-S v2)**. See [LICENSE](LICENSE) for the full text.
