# Plan: Restructure Ambyte IoT Firmware Toward DDD

## TL;DR

Incrementally refactor the ESP32 IoT firmware into a Domain-Driven Design architecture by introducing domain-layer abstractions (ports/interfaces), isolating infrastructure behind anti-corruption layers, and enforcing boundaries through CMake dependencies. The approach works within ESP-IDF's component model — no framework overhead, no big-bang rewrite.

---

## Current State Assessment

### DDD Layer Mapping (As-Is)

| Component | Implicit Layer | Clean API? | Key Issues |
|-----------|---------------|------------|------------|
| `hal/i2c_bus/` | Infrastructure (Shared Kernel) | ✓ Clean | None — good reference |
| `bme280/` | Infrastructure | ✓ C API clean | Arduino dependency leaked via `I2C_device` |
| `I2C_device/` | Infrastructure | ✗ Leaks Arduino | `TwoWire` in public header |
| `pcf2131tfy_rtc/` | Infrastructure | ✓ C wrapper clean | C++ hierarchy + register enums leaked in headers |
| `wifi_manager/` | Infrastructure | ✓ Clean | No issues |
| `sd_card/` | Infrastructure | ✓✓ Excellent | Best API in project |
| `certs/` | Infrastructure | ✓ Clean | Isolated config |
| `ambyte_status/` | Interface | ✓ Clean | No issues |
| `lua_runner/` | Application | ✗ | Public REQUIRES on 3 components; no module registration |
| `CLI/` | Interface | ✗ | PRIV_REQUIRES 3 sensors; hardcoded I2C addresses |
| `main/app_main.c` | Application | ✗ | REQUIRES all components; orchestration mixed with init |

### Key Dependency Violations
- **CLI → sensors directly** (interface layer bypasses application/domain)
- **lua_runner → sensors + storage directly** (application bypasses domain ports)
- **app_main → everything** (no dependency inversion)
- **BME280 → I2C_device → Arduino** (infrastructure leak into build)

---

## Bounded Context Map

| Bounded Context | Domain Concepts | Current Components |
|---|---|---|
| **Sensing** | Measurement, SensorReading, SensorId | `bme280/`, `pcf2131tfy_rtc/` |
| **Connectivity** | Connection, NetworkCredentials | `wifi_manager/`, `certs/` |
| **Persistence** | MeasurementLog, LogEntry, MeasurementRecord | `sd_card/`, `sqlite3` (new) |
| **Automation** | Script, MeasurementJob | `lua_runner/`, `lua/` |
| **Device Management** | DeviceStatus, Command | `CLI/`, `ambyte_status/` |

Relationships:
- Automation *consumes* Sensing (reads measurements) and *uses* Persistence (writes logs)
- Device Management *queries* Sensing and *controls* Device Status
- Sensing produces domain events (new measurement available)
- Persistence *subscribes* to domain events from Sensing

---

## Phase 0: Clean Up Redundancies

Remove dead code, unused files, and redundant abstraction layers before restructuring. This reduces noise and makes subsequent phases cleaner.

### Phase 0A: Quick Wins (zero risk)

0A.1. Delete `src/` folder — empty, only contains an auto-generated CMakeLists.txt
0A.2. Delete `dom_ludo_prototype_ambyte_thing_certs/` — leftover cert archive + macOS metadata; real certs are in `components/certs/`
0A.3. Remove dead `sensor_report_task()` function (lines ~31–85) and its commented-out call (`// app_start_sensor_task();` line ~227) from `main/app_main.c`
0A.4. Update `.gitignore`:
  - Add: `build/`, `temp_inipreview.txt`
  - Remove: `sdkconfig.defaults` (must be tracked — defines project build defaults)
  - Remove: `MQTT TLS test client.py` (being moved to `docs/`; can't `git mv` an ignored file)
  - Remove: `sdkconfig.adafruit_feather_esp32s3` (obsolete after board change)
  - Keep: `components/certs/certs.c`, `components/certs/certs.h` (secrets)
0A.5. Move `AI prompts.txt`, `MQTT TLS test client.py`, `overall_architecture.txt` to `docs/` folder
0A.6. Update `platformio.ini`: change `board` to `esp32-s3-devkitm-1`, `board_upload.flash_size` to `16MB`, remove `arduino` from frameworks (only `espidf`), update `board_upload.maximum_size` to `16777216`, add `board_build.partitions = partitions.csv`
0A.7. Update `sdkconfig.defaults`:
  - Change `CONFIG_ESPTOOLPY_FLASHSIZE_2MB=y` to `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`
  - Change `CONFIG_ESPTOOLPY_FLASHSIZE="2MB"` to `CONFIG_ESPTOOLPY_FLASHSIZE="16MB"`
  - Add `CONFIG_PARTITION_TABLE_CUSTOM=y`
  - Add `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"`
  - Remove `CONFIG_PARTITION_TABLE_SINGLE_APP=y`
  - Add `CONFIG_ESP_TASK_WDT_PANIC=y` (stuck task → reboot, not silent hang — critical for field-deployed devices)
  - Add `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y` (post-mortem debugging — core dump saved to flash partition, retrievable via `espcoredump.py`)
  - Add `CONFIG_HEAP_POISONING_LIGHT=y` (catches heap corruption early with ~1% runtime overhead)
  - Add `CONFIG_COMPILER_STACK_CHECK_MODE_NORM=y` (compile-time stack overflow instrumentation, ~2% code size)
0A.8. Create `partitions.csv` in project root — custom partition table for 16MB flash:
  ```
  # Name,    Type,  SubType,  Offset,    Size
  nvs,       data,  nvs,      0x9000,    0x6000,
  phy_init,  data,  phy,      0xf000,    0x1000,
  factory,   app,   factory,  0x10000,   0x300000,
  ota_0,     app,   ota_0,    0x310000,  0x300000,
  coredump,  data,  coredump, 0x610000,  0x10000,
  littlefs,  data,  spiffs,   0x620000,  0x80000,
  storage,   data,  fat,      0x6A0000,  0x960000,
  ```
  - factory app partition: 3MB (firmware + SQLite + NimBLE + MQTT + Lua)
  - ota_0 partition: 3MB (OTA updates)
  - coredump partition: 64KB (post-mortem crash dump — retrievable via `espcoredump.py`; requires `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`)
  - littlefs partition: 512KB (write-ahead buffer for measurement records — stores pending data before flush to SD card SQLite)
  - storage partition: ~9.4MB FAT (internal flash reserve, available for future use)
  - SD card: 64GB FAT32 (external, removable — hosts SQLite DB and bulk data; formatted with 32KB clusters via third-party tool to support >32GB on FAT32)
  - NVS: 24KB (WiFi credentials, device config: channel metadata, MQTT broker URI/client ID)
  - Note: `littlefs` uses SubType `spiffs` (0x82) because ESP-IDF's partition table parser maps both to generic data; LittleFS is mounted programmatically via `esp_littlefs`

0A.9. **Extend `components/hal/i2c_bus/`** with two new functions for hardware robustness:
  - `esp_err_t i2c_bus_check_and_recover(i2c_port_t port)` — reads SDA/SCL pin levels via `gpio_get_level()`. If SDA is stuck low, toggles SCL 9 times via GPIO (standard I2C bus recovery per spec). If still stuck, performs `i2c_driver_delete()` → `i2c_driver_install()` (full reinit). Returns `ESP_OK` if bus is healthy, `ESP_FAIL` if unrecoverable.
  - `esp_err_t i2c_bus_probe_addr(i2c_port_t port, uint8_t addr)` — sends START → addr+W → STOP. Returns `ESP_OK` on ACK (device present), `ESP_ERR_NOT_FOUND` on NACK (absent). Extracted from existing `i2cscan` logic — reusable by both startup checks and CLI `i2cscan` command.
  - Called in `app_main` before sensor init: `i2c_bus_check_and_recover()` first, then `i2c_bus_probe_addr(0x77)` before `bme280_init()` and `i2c_bus_probe_addr(0x53)` before `rtc_init()`. If probe returns `ESP_ERR_NOT_FOUND`, skip init and set `_ready = false` immediately — avoids full register-level init on absent hardware.
  - Also used as retry path: on `ESP_ERR_TIMEOUT` from `bme280_read()` or `rtc_get_time()`, attempt `i2c_bus_check_and_recover()` → re-probe → re-init if recovered.

### Phase 0B: Convert BME280 to Native ESP-IDF I2C (medium effort)

The `I2C_device/` component is an Arduino `Wire`-based C++ shim used **only** by `bme280_arduino_shim.cpp`. Critically, `I2C_device` already uses ESP-IDF `i2c_master_cmd_begin` / `i2c_master_write_read_device` under the hood — the Arduino `TwoWire` reference is stored but **never used for data transfer**. The RTC already demonstrates the correct pattern: direct I2C via `i2c_bus` without Arduino. Removing `I2C_device` also removes the dependency on `framework-arduinoespressif32`.

**Feasibility analysis**: Keep C++ (public API is already `extern "C"`). ~57% of code kept verbatim (all compensation math: `readTemperature`, `readPressure`, `readHumidity`, `readCoefficients`, config bitfields). ~11% rewritten (~70 lines: `begin()`, `write8()`, `read8()`, `read16()`, `read24()`, constructor/destructor). ~24% deleted (Adafruit_Sensor subclasses, SPI paths, TwoWire artifacts). Net result: ~450 lines down from ~896 across all BME280 component files (BME280.cpp: 666, bme280_adafruit_private.hpp: ~180, bme280_arduino_shim.cpp: ~50), zero Arduino dependency. Risk: LOW — the actual I2C transport is already ESP-IDF under the hood.

**Arduino→ESP-IDF replacements:**

| Arduino | ESP-IDF replacement |
|---|---|
| `I2C_device->reg_w(reg, value)` | `i2c_master_write_to_device(port, addr, buf, 2, timeout)` |
| `I2C_device->reg_r(reg)` | `i2c_master_write_read_device(port, addr, &reg, 1, &val, 1, timeout)` |
| `I2C_device->reg_r(reg, buf, n)` | `i2c_master_write_read_device(port, addr, &reg, 1, buf, n, timeout)` |
| `I2C_device->ping()` | Inline `i2c_master_cmd_begin()` with write+stop |
| `delay(ms)` | `vTaskDelay(pdMS_TO_TICKS(ms))` |
| `millis()` | `(uint32_t)(esp_timer_get_time() / 1000)` |
| `byte` | `uint8_t` |
| `TwoWire *theWire, &Wire` | Remove entirely; store `i2c_port_t` + `uint8_t addr` |
| `#include "Arduino.h"` | `#include <cstdint>`, `#include <cmath>`, `#include "i2c_bus.h"` |

0B.1. Refactor `bme280_adafruit_private.hpp`: remove Arduino/I2C_device includes, replace `I2C_device *i2c_dev` member with `i2c_port_t port_` + `uint8_t addr_`, remove `TwoWire` from `begin()` signature, replace `byte` with `uint8_t`, delete `BME280_ENABLE_ADAFRUIT_SENSOR_API` ifdef blocks entirely
0B.2. Refactor `BME280.cpp`: rewrite `begin()` (store port/addr, do ping via ESP-IDF), rewrite `write8()`/`read8()`/`read16()`/`read24()` to use `i2c_master_write_read_device()` with `i2c_bus_lock()`/`unlock()`, replace `delay()` → `vTaskDelay()`, replace `millis()` → `esp_timer_get_time()/1000`, simplify destructor (no `delete i2c_dev`). All compensation math functions (readTemperature, readPressure, readHumidity, readCoefficients, setSampling, etc.) kept verbatim.
  - **I2C timeout**: Define `#define BME280_I2C_TIMEOUT_MS 100` (100ms — BME280 typical transaction is <1ms, 100ms provides ample margin). Pass `pdMS_TO_TICKS(BME280_I2C_TIMEOUT_MS)` to all `i2c_master_write_read_device()` and `i2c_master_write_to_device()` calls. On `ESP_ERR_TIMEOUT`: set `s_bme280_ready = false`, return `ESP_ERR_TIMEOUT` to caller. Caller (`cmd_read_env`) can attempt `i2c_bus_check_and_recover()` once, then re-probe via `i2c_bus_probe_addr()` before re-init.
0B.3. Refactor `bme280_arduino_shim.cpp` → rename to `bme280_driver.cpp`: remove `&Wire` from `begin()` call, pass port from `i2c_bus_get_port()` instead
0B.4. Update `bme280/CMakeLists.txt`: remove `framework-arduinoespressif32` and `I2C_device` from PRIV_REQUIRES (keep `i2c_bus`), rename source file
0B.5. Delete entire `components/I2C_device/` directory
0B.6. Remove `arduino` from `platformio.ini` frameworks and remove Arduino build flags (`build_unflags`, `build_flags` for `ARDUINO_USB_CDC_ON_BOOT`)
0B.7. Verify `pio run` succeeds and BME280 init + read still work

### Phase 0C: Privatize PCF2131 RTC Internals (low effort, *parallel with 0B*)

The 3-class hierarchy (`RTC_NXP` → `PCF2131_base` → `PCF2131_I2C`) is kept intact to preserve all RTC features (alarm, timestamp, periodic interrupt, watchdog). However, the C++ class hierarchy is currently exposed via `include/RTC_NXP.h` — any consumer can include it. The public API should be C-only (`pcf2131tfy_rtc_api.h`).

Note: all three classes are declared in a **single header file** `include/RTC_NXP.h`, not separate headers per class.

0C.1. Move `RTC_NXP.h` from `include/` to the source directory (e.g., `components/pcf2131tfy_rtc/RTC_NXP.h`) — makes the C++ classes private implementation details
0C.2. Update `#include` paths in `RTC_NXP.cpp`, `PCF2131_base.cpp`, `PCF2131_I2C.cpp`, `pcf2131tfy_rtc_api.cpp` to reference the new location (relative include)
0C.3. Verify only `pcf2131tfy_rtc_api.h` remains in `include/` (public API)
0C.4. Verify `pio run` succeeds and all RTC features still work. No source files deleted, no features removed.

### Phase 0D: Miscellaneous (optional, *parallel with 0B/0C*)

0D.1. Remove `#if defined(BME280_ENABLE_ADAFRUIT_SENSOR_API)` ifdef block from `bme280_adafruit_private.hpp` — flag is never set

**Verification for Phase 0:** `pio run` succeeds. Flash and confirm all features (BME280, RTC, LED, WiFi, CLI, Lua) still work.

---

## Phase 1: Domain Layer — Introduce Ports (Header-Only Contracts)

Create a `components/domain/` component containing only header files that define the contracts (ports) each bounded context exposes. No implementation, no ESP-IDF deps.

**Steps:**

1.1. Create `components/domain/` with `CMakeLists.txt` (`REQUIRES esp_common` — needed for `esp_err_t` type; this is purely type definitions, no infrastructure dependency)

1.2. Define **Sensing port** — `components/domain/include/sensing_port.h`:
  - `typedef struct { float temperature_c; float humidity_percent; float pressure_pa; } measurement_t;`
  - `typedef esp_err_t (*sensor_read_fn)(measurement_t *out);`
  - `typedef esp_err_t (*clock_read_fn)(time_t *out);`
  - This replaces direct includes of `bme280.h` and `pcf2131tfy_rtc_api.h` in consumers

1.3. Define **Persistence port** — `components/domain/include/persistence_port.h`:
  - **Flat schema** (single struct, single table). No separate header/value types.
  - `typedef enum { MEASUREMENT_SYNC_PENDING = 0, MEASUREMENT_SYNC_INFLIGHT = 1, MEASUREMENT_SYNC_SYNCED = 2 } measurement_sync_state_t;`
  - `typedef struct { int64_t sensor_id; int64_t measure_id; char measure_type[32]; time_t timestamp; char data_type[32]; float value; measurement_sync_state_t sync_state; } measurement_record_t;`
  - Fixed-size char arrays avoid heap allocation and ownership ambiguity. SQLite callbacks `strncpy` directly into these fields. Query results remain capped at 64 rows max (~6KB).
  - **sensorID**: identifies the physical sensor instance. **measureID**: groups all data points within one logical reading. A BME280 reading produces 3 rows with the same `measure_id` and different `data_type` values ("temperature_c", "humidity_pct", "pressure_pa").
  - **Device-prefixed measureID**: `measure_id` format = `<device_id_prefix> * 1000000 + <sequence>`. E.g., device 1 → IDs 1000001, 1000002, ...; device 2 → 2000001. Allows merging DBs from multiple devices without ID collisions.
  - **sync_state** lives on every row of a logical group (denormalized). All rows for a given `measure_id` always share the same `sync_state`. Transitions (`mark_inflight`, `mark_pending`, `mark_synced`) UPDATE all rows for that `measure_id` atomically.
  - Repository operations:
    - `typedef esp_err_t (*measurement_store_fn)(const measurement_record_t *records, size_t count);` — caller assembles all flat rows for one logical group and passes them together
    - `typedef esp_err_t (*measurement_query_fn)(const char *measure_type, time_t from, time_t to, measurement_record_t *out, size_t max, size_t *count);`
    - `typedef esp_err_t (*measurement_query_by_id_fn)(int64_t measure_id, measurement_record_t *out, size_t max, size_t *count);` — all rows for one `measureID`, ordered by `dataType`
    - `typedef esp_err_t (*measurement_count_fn)(const char *measure_type, size_t *count);` — counts distinct `measureID` groups
    - `typedef esp_err_t (*measurement_next_id_fn)(int64_t *out_id);` — returns next device-prefixed measure_id
    - `typedef esp_err_t (*measurement_query_unsynced_fn)(const char *measure_type, measurement_record_t *out, size_t max, size_t *count);` — rows where `syncState = PENDING`
    - `typedef esp_err_t (*measurement_claim_next_pending_fn)(const char *measure_type, int64_t *out_measure_id);` — atomically claims the oldest `PENDING` group (PENDING→INFLIGHT) and returns its `measureID`; `ESP_ERR_NOT_FOUND` when nothing pending
    - `typedef esp_err_t (*measurement_mark_inflight_fn)(int64_t measure_id);` — `PENDING → INFLIGHT`
    - `typedef esp_err_t (*measurement_mark_pending_fn)(int64_t measure_id);` — `INFLIGHT → PENDING`
    - `typedef esp_err_t (*measurement_mark_synced_fn)(int64_t measure_id);` — `INFLIGHT → SYNCED`
  - This replaces the old text-file approach and supports any sensor type without schema changes

1.4. Define **Device Status port** — `components/domain/include/device_status_port.h`:
  - `typedef esp_err_t (*status_set_fn)(uint8_t r, uint8_t g, uint8_t b);`

1.5. ~~Sensing service~~ — **REMOVED from domain/**: `sensing_ctx_t` and its registration functions are application-layer orchestration, not pure domain. Moved into `device_commands/` (Phase 3.1) where `device_commands_config_t` aggregates all ports directly. `domain/` stays truly header-only (port typedefs only, no .c files).

**Verification:** `domain/` component builds with `REQUIRES esp_common` only (no SRCS), headers compile standalone.

---

## Phase 2: Adapt Infrastructure — Implement Ports

Wrap existing components behind port interfaces without rewriting their internals.

**Steps:**

2.1. **BME280 adapter** — modify `components/bme280/` to add a function that returns a `sensor_read_fn` conforming to the sensing port. The existing `bme280_read()` already matches closely; add a thin adapter in `bme280.c`. Add `REQUIRES domain` to its CMakeLists.txt. (I2C_device already eliminated in Phase 0B)

2.2. **RTC adapter** — modify `components/pcf2131tfy_rtc/` similarly: add a `clock_read_fn` adapter around `pcf2131tfy_rtc_get_time()`. Add `REQUIRES domain`. (Hierarchy already flattened in Phase 0C)

2.3. **SQLite persistence adapter** — Create a new persistence component using SQLite on SD card with a LittleFS write-ahead buffer:

  **Architecture**: Measurements flow through a two-stage pipeline: **LittleFS (internal flash) → SD card (SQLite)**. The caller writes to LittleFS immediately (always available, power-loss safe). A background flush task periodically drains pending records from LittleFS into SQLite on the SD card. This decouples producers from SD card availability — if the SD card is busy, absent, or slow, new measurements are safely buffered on internal flash.

  **Data flow**:
  ```
  sensor read → cmd_store_measurement() → append to /littlefs/pending.bin → return success
                                                      ↓
                              pending_flush_task (every 5s, Core 0)
                                                      ↓
                              read pending records → INSERT into SQLite on SD card
                                                      ↓
                              on success: remove flushed entries from LittleFS
  ```

  **Data lifecycle**: Synchronization state (`syncState`) is stored on every row of a logical group (denormalized flat schema). New measurements start as `PENDING` (0), move to `INFLIGHT` (1) only after a publish is successfully queued, and become `SYNCED` (2) only after broker acknowledgment. On boot, any stale `INFLIGHT` rows are reset to `PENDING` before MQTT starts.

  - **Prerequisites**:
    - Add `joltwallet/littlefs` as ESP Component Registry dependency (or git submodule under `components/littlefs/`)
    - Add LittleFS init to `app_main.c`: mount `/littlefs` partition before any persistence access. **Mount failure recovery**: if `esp_vfs_littlefs_register()` fails, attempt `esp_littlefs_format()` → remount. Log format event to NVS counter (`nvs_set_u32("diag", "lfs_fmt_count", ++n)`) for field diagnostics. If format also fails, set `s_persistence_available = false` — all `cmd_store`/`cmd_query` return `ESP_ERR_NOT_SUPPORTED` rather than crashing. LittleFS is critical — it serves as the write-ahead buffer for all measurement data.
    - Add `sd_card` to `main/CMakeLists.txt` REQUIRES. Add `sdcard_init_default()` + `sdcard_mount()` calls in `app_main.c`. **SD mount failure handling**: if `sdcard_mount()` fails, wait 500ms → retry once (SD cards may need warm-up after power-on). If still fails, log `ESP_LOGW` and set `sd_available = false`. Add `bool sd_card_is_available(void)` to `sd_card.h` public API. Device continues operating — new measurements accumulate in LittleFS pending buffer until SD card becomes available. `cmd_query`/`cmd_count` return `ESP_ERR_NOT_SUPPORTED` (queries require SQLite). Lua’s `device.status()` includes SD state.
  - Add `siara-cc/esp32-idf-sqlite3` as a git submodule under `components/sqlite3/` (pure ESP-IDF, Apache-2.0)
  - **Fix VFS shim**: The `siara-cc` `esp32.c` has a sync no-op (`esp32_Sync()` only calls `fflush`, not `fsync`) and stub-only locking. Fix `esp32_Sync()` to call `fflush()` + `fsync(fileno(fd))`. Without this fix, no journal mode is truly safe.
  - Add a new source file `sqlite_persistence.c` (in a new `components/persistence/` component) that implements all port functions from `persistence_port.h`:
    - On init: `sqlite3_open("/sdcard/measurements.db", &db)`, configure PRAGMAs, create table if not exists
    - **Startup integrity check**: Immediately after `sqlite3_open()`, run `PRAGMA integrity_check;`. If result ≠ `"ok"`: close DB → rename `.db`/`.db-wal`/`.db-shm` to `.corrupt` suffix → reopen (creates fresh DB) → log event to NVS (`nvs_set_u32("diag", "db_corrupt_count", ++n)`). Data is lost but device recovers function.
    - **Schema migration**: `migrate_schema()` runs after `CREATE TABLE` — uses `PRAGMA table_info` to detect the legacy `synced` column name and renames it to `syncState` via `ALTER TABLE RENAME COLUMN`. Safe to call on every boot; no-op if column is already `syncState`.
    - **PRAGMAs** (set immediately after open and integrity check):
      ```
      PRAGMA journal_mode = WAL;
      PRAGMA locking_mode = EXCLUSIVE;
      PRAGMA synchronous = NORMAL;
      PRAGMA page_size = 4096;
      PRAGMA cache_size = -64;
      PRAGMA wal_autocheckpoint = 25;
      PRAGMA journal_size_limit = 65536;
      PRAGMA temp_store = MEMORY;
      ```
    - **Flat table schema** (single table, all columns per row):
      - `CREATE TABLE IF NOT EXISTS measurements (sensorID INTEGER NOT NULL, measureID INTEGER NOT NULL, measureType TEXT NOT NULL, timestamp INTEGER NOT NULL, dataType TEXT NOT NULL, dataValue REAL, syncState INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (measureID, dataType));`
    - Indexes:
      - `CREATE INDEX IF NOT EXISTS idx_type_ts ON measurements (measureType, timestamp);`
      - `CREATE INDEX IF NOT EXISTS idx_sync_type ON measurements (syncState, measureType, timestamp);`
    - On init, run `UPDATE measurements SET syncState = 0 WHERE syncState = 1;` so stale in-flight rows are retried after reboot
    - `measurement_store_fn` → caller passes all flat `measurement_record_t` rows for one logical group; implementation appends them to the LittleFS pending store, returns immediately
    - `measurement_query_fn` → `SELECT … FROM measurements WHERE measureType = ? AND timestamp BETWEEN ? AND ? ORDER BY measureID, dataType`; returns flat `measurement_record_t` rows
    - `measurement_query_by_id_fn` → `SELECT … FROM measurements WHERE measureID = ? ORDER BY dataType`; returns all rows for one logical group
    - `measurement_count_fn` → `SELECT COUNT(DISTINCT measureID) FROM measurements WHERE measureType = ?;`
    - `measurement_next_id_fn` → reads `s_next_measure_id` atomic counter from `pending_store.c` (initialized at startup from `max(SQLite MAX(measureID), pending_meta.max_measure_id) + 1`; incremented on each append)
    - `measurement_query_unsynced_fn` → `SELECT … FROM measurements WHERE measureType = ? AND syncState = 0 ORDER BY measureID, dataType;`
    - `measurement_claim_next_pending_fn` → `BEGIN IMMEDIATE`; select oldest PENDING `measureID`; `UPDATE SET syncState = 1`; `COMMIT`; return `measureID`; returns `ESP_ERR_NOT_FOUND` when nothing pending
    - `measurement_mark_inflight_fn` → `UPDATE measurements SET syncState = 1 WHERE measureID = ? AND syncState = 0;`
    - `measurement_mark_pending_fn` → `UPDATE measurements SET syncState = 0 WHERE measureID = ? AND syncState = 1;`
    - `measurement_mark_synced_fn` → `UPDATE measurements SET syncState = 2 WHERE measureID = ? AND syncState = 1;`
  - **Query result buffers**: Heap-allocated by the caller (not stack). Lua bindings `malloc` the needed buffer, push results to a Lua table, then `free`. This avoids stack overflow in 4KB task stacks.
  - **LittleFS pending store** (`components/persistence/pending_store.c`):
    - Each entry stores one flat `measurement_record_t` with a magic/CRC wrapper:
      ```c
      typedef struct {
          uint32_t magic;              // 0xABCD1234 — validity marker
          measurement_record_t record; // flat row (sync_state always PENDING on write)
          uint32_t crc32;              // integrity check
      } pending_entry_t;
      ```
    - `pending_store_append(const measurement_record_t *records, size_t count)` — appends flat records to `/littlefs/pending.bin`. This is what `measurement_store_fn` calls.
    - `pending_store_read(pending_entry_t *out, size_t max, size_t *count)` — reads the first N valid entries
    - `pending_store_remove(size_t count)` — advances head pointer in `/littlefs/pending_meta.bin` (circular buffer). When head catches tail, truncate file to reclaim space.
    - `pending_store_count()` — returns pending entry count from metadata
    - **Circular buffer metadata**: `/littlefs/pending_meta.bin` stores `{ uint32_t head; uint32_t tail; int64_t max_measure_id; }`. Updated atomically (write + fsync) on every append and remove.
    - **Atomic measureID counter**: `s_next_measure_id` in `pending_store.c` tracks the highest measureID. Initialized at startup as `max(SQLite MAX(measureID), pending_meta.max_measure_id)`. Incremented on each `pending_store_append()`. `measurement_next_id_fn` reads this counter instead of querying SQLite — avoids duplicate IDs when records are still in the pending buffer.
    - **Thread safety**: Protected by `s_pending_mutex` (`SemaphoreHandle_t`). Both `cmd_store_measurement` (producer, any task) and the flush task (consumer, Core 0) take this mutex.
    - **Capacity**: 512KB LittleFS partition holds roughly 1,700 flat three-value measurements at ~300 bytes each — about 28 hours of buffer at 1 reading per minute. If pending store exceeds 80%, log `ESP_LOGW`. If 100% full, `cmd_store_measurement` returns `ESP_ERR_NO_MEM`.
  - **Pending flush task** (`pending_flush_task`, pinned to Core 0):
    - Runs every 5 seconds: checks `pending_store_count()`, skips if 0
    - If `sd_card_is_available()`: reads up to 16 flat entries and inserts them into SQLite inside one transaction
    - On successful SQLite INSERT: calls `pending_store_remove(n)` to clear flushed entries
    - On failure (SD card removed mid-flush): entries remain in LittleFS, retried next cycle
    - **Startup drain**: On boot, after SQLite init, immediately drain all pending LittleFS records to SQLite before starting normal operation.
    - **Read-after-write latency**: `db.store()` followed immediately by `db.query()` won’t see the just-stored record (up to 5s delay). Acceptable for IoT telemetry.
  - Text-file functions (`sdcard_write_line`, `sdcard_append_file`, `sdcard_read_line_at`, etc.) are kept until Phase 3C when both Lua and CLI are rewired to use `device_commands`. After Phase 3C, these functions are removed.
  - `components/persistence/CMakeLists.txt`: `REQUIRES domain sqlite3 sd_card` + `PRIV_REQUIRES esp_littlefs`
  - **Thread safety**: SQLite compiled with `SQLITE_THREADSAFE=2` (multi-thread mode). `s_sqlite_mutex` in `sqlite_persistence.c` serializes all DB access. LittleFS pending store has its own `s_pending_mutex`.

2.4. **Status adapter** — `ambyte_status` already fits. Add a function returning `status_set_fn`.

2.5. *(Moved to Phase 0C)* ~~Hide RTC internals~~

2.6. *(Moved to Phase 0B)* ~~Hide I2C_device~~

**Verification:** All existing code still compiles; no public API removals, only additions.

---

## Phase 3: Shared Command Layer — Direct Call Architecture

CLI and Lua currently implement overlapping operations independently. Instead of maintaining two command implementations, both call a shared `device_commands` layer directly. Thread safety is ensured by per-resource mutexes rather than serializing everything through a single task.

### Threading Design

Both CLI and Lua tasks call `cmd_*` functions directly and independently. Thread safety is provided by existing per-resource mutexes:
- **I2C sensors**: Already protected by `i2c_bus_lock()`/`unlock()` (existing mutex in `i2c_bus.c`)
- **LittleFS pending store**: Protected by `s_pending_mutex` in `pending_store.c` (dedicated `SemaphoreHandle_t`). Both producers (CLI/Lua via `cmd_store_measurement`) and the flush task take this mutex.
- **SQLite**: Protected by `s_sqlite_mutex` in `sqlite_persistence.c` (dedicated `SemaphoreHandle_t`). Only the flush task and query commands access SQLite.
- **Status LED**: GPIO writes are atomic; no mutex needed
- **Lua VM**: Owned exclusively by Lua task — CLI never touches `lua_State`

No FreeRTOS queue or IPC needed between CLI and Lua — they operate independently against `device_commands`. CLI remains responsive even when Lua is running a long script, and Lua is not burdened with CLI dispatch duties.

### Design

**`i2cscan` stays CLI-native only:**
- Requires direct I2C bus lock/probe — not routed through Lua or `device_commands`
- Uses `i2c_bus_lock()` + `i2c_master_cmd_begin()` directly (current implementation preserved)
- Not exposed to Lua (no `device.i2c_scan()`)

**Lua gains all capabilities:**
- `device.set_rgb(r, g, b)` — LED control
- `device.read_rtc()` — RTC time
- `device.status()` — device status (sensors ready, WiFi, etc.)
- `device.read_env()` — BME280 environmental reading
- `db.store(header, values)` — store one logical measurement group to SQLite (takes caller-assembled metadata + values, not sensor reads)
- `db.query(type, from, to)` — query measurements
- `db.count(type)` — measurement count
- `db.next_id()` — get next device-prefixed measure ID
- `db.unsynced(type)` — query un-synced logical measurement headers
- `device.mqtt_publish(topic, payload)` — raw MQTT publish for diagnostics or ad-hoc messages (Phase 6A)
- `device.mqtt_publish_measurement(measure_id)` — publish one logical measurement group as a JSON payload using the fixed telemetry topic contract (Phase 6A)
- `device.mqtt_publish_unsynced(type)` — claim and publish the next unsynced measurement group of the requested type using the fixed telemetry topic contract (Phase 6A)
- `device.mqtt_status()` — MQTT connection status (Phase 6A)
- `device.uart_query(channel, cmd, timeout)` — UART sensor query
- `device.uart_ping(channel)` — UART health check
- `device.uart_status()` — all channel states
- `device.sleep_ms(ms)` — task delay
- `device.log(msg)` — ESP_LOGI logging

### Command Inventory

| Operation | Old CLI | After | Lua binding |
|---|---|---|---|
| I2C scan | `i2cscan` (native) | `i2cscan` stays native CLI-only | — (not in Lua) |
| Set RGB LED | `red 0\|1` | CLI → `cmd_set_rgb()` | `device.set_rgb(r,g,b)` |
| RTC read | `rtc` | CLI → `cmd_read_rtc()` | `device.read_rtc()` |
| Sensor status | `status` | CLI → `cmd_device_status()` | `device.status()` |
| Ping | `ping` | removed | — |
| Exit | `exit` | removed | — |
| BME280 read | — | CLI → `cmd_read_env()` | `device.read_env()` |
| Store measurement | — | CLI → `cmd_store_measurement()` | `db.store(header, values)` |
| Query measurements | — | CLI → `cmd_query_measurements()` | `db.query(type, from, to)` |
| Count measurements | — | CLI → `cmd_measurement_count()` | `db.count(type)` |
| Next measure ID | — | CLI → `cmd_next_measure_id()` | `db.next_id()` |
| Query unsynced groups | — | CLI → `cmd_query_unsynced()` | `db.unsynced(type)` |
| MQTT publish | — | CLI → `cmd_mqtt_publish()` | `device.mqtt_publish(topic, payload)` |
| MQTT publish measurement | — | CLI → `cmd_mqtt_publish_measurement()` | `device.mqtt_publish_measurement(id)` |
| MQTT publish unsynced | — | CLI → `cmd_mqtt_publish_unsynced()` | `device.mqtt_publish_unsynced(type)` |
| MQTT status | — | CLI → `cmd_mqtt_status()` | `device.mqtt_status()` |
| UART query | — | CLI → `cmd_uart_query()` | `device.uart_query(ch, cmd, timeout)` |
| UART ping | — | CLI → `cmd_uart_ping()` | `device.uart_ping(ch)` |

### Command function pattern

```
typedef struct {
    esp_err_t status;       // ESP_OK or error
    char message[256];      // Human-readable result or error text
} cmd_result_t;

cmd_result_t cmd_set_rgb(uint8_t r, uint8_t g, uint8_t b);
cmd_result_t cmd_read_rtc(time_t *out_time);
cmd_result_t cmd_device_status(bool *bme_ready, bool *rtc_ready, time_t *rtc_time);
cmd_result_t cmd_read_env(float *temp, float *hum, float *pres);
cmd_result_t cmd_log(const char *msg);
cmd_result_t cmd_store_measurement(const measurement_header_t *header, const measurement_value_t *values, size_t count);   // stores one logical measurement group to LittleFS pending buffer (flushed to SQLite by background task) — does NOT read sensors
cmd_result_t cmd_query_measurements(const char *measure_type, time_t from, time_t to, measurement_record_t *out, size_t max, size_t *count);   // caller provides heap-allocated buffer
cmd_result_t cmd_measurement_count(const char *measure_type, size_t *count);
cmd_result_t cmd_next_measure_id(int64_t *out_id);
cmd_result_t cmd_sleep_ms(uint32_t ms);
cmd_result_t cmd_query_unsynced(const char *measure_type, measurement_header_t *out, size_t max, size_t *count);   // returns logical measurement groups, not flattened rows
// Phase 6A additions:
cmd_result_t cmd_mqtt_publish(const char *topic, const char *payload);
cmd_result_t cmd_mqtt_publish_measurement(int64_t measure_id);   // fixed-contract topic derived internally
cmd_result_t cmd_mqtt_publish_unsynced(const char *measure_type);   // fixed-contract topic, claims and publishes at most one pending group per call
cmd_result_t cmd_mqtt_status(void);
```

Note: `cmd_store_measurement` receives one fully assembled `measurement_header_t` plus a `measurement_value_t[]` array. The caller (Lua script) is responsible for reading sensors via `device.read_env()`, getting timestamp via `device.read_rtc()`, obtaining a measureID via `db.next_id()`, assembling the header, and filling the value array before calling `db.store(header, values)`.

**CMakeLists.txt:** `REQUIRES domain` only (uses domain ports, not concrete components)

### Adapters

**CLI adapter** (in `CLI.c`):
- Uses `esp_console` for command registration and argument parsing (provides line editing, history, and tab completion for field debugging)
- Uses `esp_console`'s built-in REPL (`esp_console_new_repl_*()` + `esp_console_start_repl()`) rather than a custom `fgets()` loop
- Register commands via `esp_console_cmd_register()`: each command's handler calls the corresponding `cmd_*()` function from `device_commands`
- Native handler: `i2cscan` → registered as `esp_console` command, runs directly in CLI task using `i2c_bus_lock()` + `i2c_master_cmd_begin()`
- All other commands: handler calls `cmd_*()` directly, prints `cmd_result_t.message`
- Thread-safe: sensor/DB mutexes protect shared resources — no coordination with Lua needed
- Zero Lua state access from CLI task

**Lua adapter** (in `lua_runner.c`):
- Owns the `lua_State` exclusively — only the Lua task touches it
- Each Lua C binding function calls the corresponding `cmd_*` function from `device_commands` directly
- On success: pushes result to Lua stack
- On failure (non-critical ops like DB queries): returns `nil, result.message`
- On failure (critical ops like set_rgb): calls `luaL_error(L, "%s", result.message)`
- No CLI queue polling — Lua task runs scripts independently

### Phase 3A: Create `device_commands` Component

3A.1. Create `components/device_commands/` with `cmd_result_t` struct and all command functions listed above. `REQUIRES domain` only — receives all port function pointers via a config struct:
  ```
  typedef struct {
      /* Sensing ports (Phase 2) */
      sensor_read_fn                      read_env;
      clock_read_fn                       read_clock;
      /* Persistence ports (Phase 2) */
      measurement_store_fn                store;
      measurement_query_fn                query;
      measurement_count_fn                count;
      measurement_next_id_fn              next_id;
      measurement_query_unsynced_fn       query_unsynced;
      measurement_mark_synced_fn          mark_synced;
      measurement_mark_inflight_fn        mark_inflight;
      measurement_mark_pending_fn         mark_pending;
      measurement_query_by_id_fn          query_by_id;
      measurement_claim_next_pending_fn   claim_next_pending;
      /* Status port (Phase 2) */
      status_set_fn                       set_status;
      /* Messaging ports (Phase 6A) */
      message_publish_fn                  publish;
      message_is_connected_fn             message_is_connected;
      message_set_publish_ack_handler_fn  set_publish_ack_handler;
      /* Phase 6B adds: message_subscribe_fn subscribe; */
      /* Phase 7 adds:  uart_sensor_query_fn uart_query; */
      /* Phase 8 adds:  power_read_fn read_power;        */
  } device_commands_config_t;
  ```
  `device_commands_init(const device_commands_config_t *cfg)` — registers the publish-ack handler (`on_publish_ack`) via `set_publish_ack_handler` if provided. Each `cmd_*` function checks if its required port(s) are NULL before use (returns `ESP_ERR_NOT_SUPPORTED`). Messaging ports are NULL until Phase 6A wires them up.
  
  **Port requirements per command:**
  - `cmd_set_rgb` → needs `set_status`
  - `cmd_read_rtc` → needs `read_clock`
  - `cmd_read_env` → needs `read_env`
  - `cmd_store_measurement` → needs `store` only (header + values provided by caller)
  - `cmd_query_measurements` → needs `query` only
  - `cmd_next_measure_id` → needs `next_id` only
  - `cmd_query_unsynced` → needs `query_unsynced_headers` only
  - `cmd_mqtt_publish` → needs `publish` only
  - `cmd_mqtt_publish_measurement` → needs `query_header_by_id` + `query_by_id` + `publish` + `mark_inflight` + `mark_pending` + `mark_synced`; also depends on the publish-ack handler being registered during `device_commands_init()`
  - `cmd_mqtt_publish_unsynced` → needs `claim_next_pending_header` + `query_by_id` + `publish` + `mark_pending` + `mark_synced`; also depends on the publish-ack handler being registered during `device_commands_init()`
  - `cmd_mqtt_status` → needs `message_is_connected`
  - `cmd_device_status` → needs `read_env` + `read_clock` (checks readiness)

**Verification:** `pio run` succeeds with `device_commands` component added. No consumers call it yet.

### Phase 3B: Rewire Lua Bindings

3B.1. Update `lua_runner/CMakeLists.txt`:
  - Remove REQUIRES `ambyte_status pcf2131tfy_rtc sd_card`
  - Add REQUIRES: `device_commands`
  - Final REQUIRES: `lua device_commands` (the `lua` dependency must remain for the Lua VM)
3B.2. Rewrite Lua bindings in `lua_runner.c`:
  - **Remove old `status.*` and `sd.*` module registrations entirely** — old bindings using `ambyte_status_set_rgb()`, `sdcard_append_line()`, etc. are deleted (intentional breaking change)
  - Register new `device.*` and `db.*` bindings, each calling the corresponding `cmd_*()` function directly
  - Do **not** expose any manual sync-state mutation API to Lua; sync state is managed by the MQTT publish path, not by scripts
  - Update embedded Lua script (`lua_script_blob.c`) to use new `device.*`/`db.*` API
3B.3. **Lua VM lifecycle**: Keep current one-shot lifecycle (create VM → run script → close VM → `vTaskDelete`). The VM does NOT need to stay alive permanently since CLI calls `cmd_*` directly instead of dispatching through Lua. This saves ~30-50KB heap when no script is running. Future enhancement: persistent VM for continuous automation scripts.
  - Error recovery: If `lua_pcall` fails: log error via `ESP_LOGE`, return error — VM destruction handles cleanup
  - **Sensor liveness via Lua**: Periodic I2C sensor health checks are implemented as a Lua script pattern rather than C code. `device.read_env()` already returns `nil, error_message` on I2C failure. Recommended script practice: if `read_env()` returns nil, log “BME280 lost”, retry after 30s. The `cmd_read_env` implementation surfaces I2C errors through the port — no additional C code needed for liveness detection.

**Verification:** `pio run` succeeds. Lua scripts produce same behavior using new `device.*`/`db.*` API. Old `status.*`/`sd.*` modules no longer exist.

### Phase 3C: Rewire CLI and Clean Up

3C.1. Update `CLI/CMakeLists.txt`:
  - Remove PRIV_REQUIRES `bme280 pcf2131tfy_rtc ambyte_status`
  - Add REQUIRES `device_commands i2c_bus console` + `PRIV_REQUIRES driver`
  - Keep `esp_console` (provides line editing, history, argument parsing)
3C.2. Rewrite `CLI.c`:
  - Use `esp_console_cmd_register()` to register all commands (including `i2cscan`)
  - Use `esp_console_new_repl_*()` plus `esp_console_start_repl()` for the shell lifecycle
  - Each command handler: calls corresponding `cmd_*()` function directly → prints `cmd_result_t.message`
  - Native command: `i2cscan` handler uses `i2c_bus_lock()` + `i2c_master_cmd_begin()` directly
  - No FreeRTOS queue or IPC — direct function calls

3C.3. Remove old text-file functions from `sd_card` (`sdcard_write_line`, `sdcard_append_file`, `sdcard_read_line_at`, etc.) — no longer used by any consumer after Phases 3B and 3C. SD card retains mount/unmount API for bulk file I/O.

3C.4. **Rewire app_main.c** — becomes the **composition root**:
  - Initialize infrastructure components (I2C, sensors, SD, WiFi)
  - Initialize persistence (LittleFS pending store + SQLite on SD card)
  - Drain pending LittleFS records to SQLite (startup flush)
  - Start pending flush task (Core 0, periodic 5s)
  - Create domain port adapters (function pointers)
  - Call `device_commands_init()` with port adapters
  - Start Lua runner (registers bindings, runs default script)
  - Start CLI (`esp_console_start_repl()`, handlers call `cmd_*()` directly)
  - **Task stack sizes** (defined as `#define` constants in respective headers):
    - Lua task: **8192 bytes** (must handle lua_pcall → C binding → cmd_* → SQLite → VFS call chain; 4096 is insufficient). Log `uxTaskGetStackHighWaterMark()` at end of first Lua script execution to validate sizing.
    - CLI task: **4096 bytes** (esp_console + i2cscan + direct cmd_* calls)
    - Pending flush task: **6144 bytes** (LittleFS read + SQLite INSERT via FAT VFS + SDMMC call chain, pinned to Core 0)
    - UART sensor task: **4096 bytes** (uart_write/read + wait)
  - **Heap monitoring**: Log `esp_get_free_heap_size()` after each major init block (I2C, sensors, SD, SQLite, Lua, WiFi). Periodically (every 60s) log `esp_get_minimum_free_heap_size()`. If free heap drops below 32KB, log `ESP_LOGW("HEAP", "Low heap: %lu bytes", esp_get_free_heap_size())`.
  - **Hardware inventory log**: After all `*_init()` calls and I2C probes, call `log_hardware_inventory()` which queries `bme280_is_ready()`, `pcf2131tfy_rtc_is_ready()`, `sd_card_is_available()`, and (Phase 7) UART ping results. Log a single structured line: `ESP_LOGI("BOOT", "I2C=%s BME280=%s RTC=%s SD=%s", ...)`. Also populate a `hw_inventory_t` struct accessible by `device.status()` in Lua.
  - `main/CMakeLists.txt` REQUIRES — explicit list per phase:
    - Current: `i2c_bus wifi_manager nvs_flash ambyte_status lua_runner pcf2131tfy_rtc bme280 CLI`
    - After Phase 3: `i2c_bus wifi_manager nvs_flash ambyte_status lua_runner pcf2131tfy_rtc bme280 CLI device_commands persistence sd_card domain`
    - After Phase 6A: add `mqtt_client certs`
    - After Phase 6B: add `device_config`
    - After Phase 7: add `uart_sensors`
    - After Phase 8: add `mp2731`

**Verification:** `pio run` succeeds. CLI commands work via direct `cmd_*()` calls. `i2cscan` works natively. Lua scripts still work independently. SD card text-file functions removed without breakage.

---

## Phase 4: Enforce Boundaries in Build System

**Steps:**

4.1. Audit all `CMakeLists.txt` after refactoring — verify:
  - `domain/` REQUIRES only `esp_common` (type definitions only — no infra deps)
  - `device_commands/` REQUIRES only `domain`
  - Infra components (`bme280`, `sd_card`, etc.) REQUIRES `domain` + their drivers
  - `CLI/`, `lua_runner/` REQUIRES only `device_commands` (or `domain`)
  - Only `main/` REQUIRES everything (composition root)

4.2. Document the layer rules in a `.github/copilot-instructions.md` or project README section

**Verification:** Intentionally add a wrong dependency (e.g., `domain` REQUIRES `bme280`) and confirm build fails or is caught in review.

---

## Optional (Future): Full Ubiquitous Language Rename

Rename public APIs to use domain language. This is non-essential but improves readability.

| Current | Proposed |
|---------|----------|
| `bme280_read()` | `env_sensor_read()` (or keep behind port) |
| `pcf2131tfy_rtc_get_time()` | `rtc_read_time()` (or keep behind port) |
| `sdcard_write_line()` | `measurement_log_append()` |
| `sensor_report_task()` | `measurement_collection_task()` |
| `ambyte_status_set_rgb()` | `device_status_indicate()` |

**Excluded from plan**: This is cosmetic and can be done incrementally. Ports abstract the names already.

---

## Phase 5: BLE Wi-Fi Provisioning

### Best Practice: ESP-IDF Wi-Fi Provisioning Manager

ESP-IDF provides a **built-in solution** (`wifi_prov_mgr`) that handles exactly this use case. It is the recommended best practice from Espressif:

**How it works:**
1. On first boot (or when `wifi_prov_mgr_is_provisioned()` returns false):
   - Start BLE GATT server with `wifi_prov_scheme_ble`
   - User provisions Wi-Fi credentials over encrypted BLE (X25519 + AES-CTR, Security 1)
   - Device attempts Wi-Fi connection and reports success/failure to the provisioning client
   - On success: credentials are saved to NVS automatically and provisioning stops
2. **BLE memory is freed automatically** using `WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BLE` (note: `FREE_BTDM` is for ESP32 with combined BT+BLE controller; ESP32-S3 has a separate BLE controller and requires `FREE_BLE`)
   - This frees BLE memory (~60-70 KB RAM) when provisioning manager is de-initialized
   - On subsequent boots, BLE is never initialized when stored Wi-Fi credentials are present
3. Credentials **persist in NVS** across reboots — never need to re-provision unless NVS is erased

**Key config**: Use `WIFI_PROV_SECURITY_1` with a proof-of-possession (PoP) string printed on device label or shown on LED pattern.

### Steps

5.1. Extend `components/wifi_manager/` — add provisioning mode (no separate `ble_provisioning/` component):
  - Add `wifi_manager_is_provisioned(bool *out)` — checks the real Wi-Fi provisioning state stored by ESP-IDF / `esp_wifi`, not only a project-local flag
  - Add `wifi_manager_start_provisioning(const char *device_name, const char *pop)` — internally calls `wifi_prov_mgr_init()` with BLE scheme, starts provisioning, waits for `WIFI_PROV_CRED_SUCCESS`, then deinits prov mgr (frees BLE RAM) and connects WiFi
  - BLE-specific code lives entirely *inside* `wifi_manager` — consumers never see BLE types
  - Update `wifi_manager/CMakeLists.txt`: add `PRIV_REQUIRES wifi_provisioning protocomm bt` (private — no BLE leak to consumers)

5.2. Modify startup flow in `app_main.c`:
  ```
  app_main()
    ├─ NVS init
    ├─ wifi_manager_init()
    ├─ Check: wifi_manager_is_provisioned()?
    │   ├─ NO → wifi_manager_start_provisioning("AMBYTE", pop_string)
    │   │       Internally: starts BLE, waits for credentials, connects WiFi
    │   │       On success: deinits prov mgr (frees BLE RAM)
    │   └─ YES → wifi_manager_connect_stored() (uses NVS-stored credentials, never inits BT stack → saves ~60KB RAM)
    ├─ Continue normal startup (I2C, sensors, SD, MQTT, CLI, Lua...)
  ```

5.3. Phase 5A scope ends with Wi-Fi provisioning only. **Do not block Phase 6A on a custom BLE device-config endpoint.**
  - Phase 6A is **compile-time-config only**. It does not read `device_config`.
  - Define the exact 6A config carrier in Kconfig / `sdkconfig.defaults` (or a local `sdkconfig` overlay):
    - `CONFIG_AMBYTE_DEVICE_ID`
    - `CONFIG_AMBYTE_MQTT_URI`
    - `CONFIG_AMBYTE_MQTT_CLIENT_ID`
    - `CONFIG_AMBYTE_MQTT_TOPIC_ROOT`
  - `CONFIG_AMBYTE_DEVICE_ID` is the canonical device identity for Phase 6A. It is used in telemetry payloads/topics and also parsed once at startup as the numeric `measure_id` prefix for persistence. `CONFIG_AMBYTE_MQTT_CLIENT_ID` remains the broker client identifier and may differ.

5.4. Phase 5B / 6B follow-up — add a dedicated `components/device_config/` component (NVS-backed, separate from `wifi_manager`):
  - Owns device configuration stored in NVS namespace `"device_cfg"`
  - First fields: `"mqtt_uri"`, `"mqtt_client_id"`, `"mqtt_topic_root"`
  - Later fields: AMBIT channel metadata (`"ambit1"` ... `"ambit4"`) with string values stored via `nvs_set_str`; the 15-character NVS limit applies to keys and namespaces, not stored string values
  - Optional custom BLE endpoint `device-config` can populate this component later; that work is explicitly deferred to Phase 6B
  - App still sends device-config before Wi-Fi config when that endpoint is introduced, per ESP-IDF provisioning requirements

5.5. Enable BLE in sdkconfig.defaults:
  - `CONFIG_BT_ENABLED=y`
  - `CONFIG_BT_NIMBLE_ENABLED=y` (NimBLE uses less RAM than Bluedroid)
  - `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y` (if PSRAM available)

**Verification:** Flash, observe BLE advertising → provision Wi-Fi credentials → device connects → reboot → device auto-connects without BLE. Device configuration beyond Wi-Fi is deferred to Phase 5B / 6B.

---

## Phase 6A: MQTT Telemetry Publisher

Add outbound MQTT telemetry only. MQTT v5 is already enabled in sdkconfig (`CONFIG_MQTT_PROTOCOL_5=y`). Target broker: AWS IoT Core (mutual TLS). Inbound MQTT commands are deferred to Phase 6B.

### Bounded Context Addition

| Bounded Context | Domain Concepts | Components |
|---|---|---|
| **Messaging** | Message, Topic, PublishResult, ConnectionState | `mqtt_client/` (new) |

### Domain Port

6A.0. Define **Messaging port** in `components/domain/include/messaging_port.h`:
  - `typedef esp_err_t (*message_publish_fn)(const char *topic, const char *payload, size_t len, int *out_msg_id);`
  - `typedef bool (*message_is_connected_fn)(void);`
  - `typedef void (*message_publish_ack_fn)(int msg_id, esp_err_t status, void *ctx);`
  - `typedef esp_err_t (*message_set_publish_ack_handler_fn)(message_publish_ack_fn handler, void *ctx);`
  - Phase 6B later adds:
    - `typedef void (*message_handler_fn)(const char *topic, const char *payload, size_t len);`
    - `typedef esp_err_t (*message_subscribe_fn)(const char *topic, message_handler_fn handler);`

### Steps

6A.1. Create `components/mqtt_client/` — **DONE** — wraps ESP-MQTT (`esp_mqtt_client`):
  - Add `components/mqtt_client/Kconfig.projbuild` and define the 6A compile-time carrier:
    - `CONFIG_AMBYTE_DEVICE_ID`
    - `CONFIG_AMBYTE_MQTT_URI`
    - `CONFIG_AMBYTE_MQTT_CLIENT_ID`
    - `CONFIG_AMBYTE_MQTT_TOPIC_ROOT`
  - `typedef struct { const char *broker_uri; const char *client_id; } mqtt_client_config_t;` — `broker_uri` and `client_id` are the only MQTT-client-level settings; `topic_root` and `device_id` are domain-level and are injected into `device_commands_config_t` by `app_main`, not through `mqtt_client_config_t`
  - `mqtt_client_init(const mqtt_client_config_t *cfg)` takes config built by `app_main` from NVS first, falling back to `CONFIG_AMBYTE_*` Kconfig symbols; `device_config_init()` must be called before `mqtt_client_init()`
  - `mqtt_client_start()` / `mqtt_client_stop()`
  - Implements `message_publish_fn`, `message_is_connected_fn`, and `message_set_publish_ack_handler_fn`
  - Uses QoS 1 for telemetry publishes
  - Registers an ESP-MQTT event handler and translates `MQTT_EVENT_PUBLISHED` into the plan's publish-ack callback
  - `REQUIRES domain certs` + `PRIV_REQUIRES mqtt esp_event`

6A.2. Extend persistence for telemetry upload — **DONE**:
  - Add a group-aware repository API now:
    - `measurement_query_header_by_id_fn`
    - `measurement_query_unsynced_headers_fn`
    - `measurement_claim_next_pending_header_fn`
    - `measurement_mark_inflight_fn`
    - `measurement_mark_pending_fn`
    - `measurement_mark_synced_fn`
  - Keep `measurement_query_by_id_fn` for loading the value rows belonging to one logical measurement group
  - Sync state is persisted in `measurement_headers.syncState`, not inferred from an in-memory table alone
  - On boot, reset any stale `INFLIGHT` headers back to `PENDING` before MQTT publishing starts

6A.3. Add MQTT commands to `device_commands`:
  - Add two string fields to `device_commands_config_t`:
    ```c
    const char *topic_root; /* e.g. "ambyte/prod" — runtime NVS value, Kconfig fallback applied by app_main */
    const char *device_id;  /* e.g. "thing-abc"  — runtime NVS value, Kconfig fallback applied by app_main */
    ```
    `device_commands.c` uses `s_cfg.topic_root` and `s_cfg.device_id` for all topic construction; it does **not** reference `CONFIG_AMBYTE_MQTT_TOPIC_ROOT` or `CONFIG_AMBYTE_DEVICE_ID` directly.
  - **Migration of existing code**: `device_commands.c` currently uses `CONFIG_AMBYTE_MQTT_TOPIC_ROOT` and `CONFIG_AMBYTE_DEVICE_ID` directly in two snprintf calls (`cmd_mqtt_publish_measurement` topic construction and `device_commands_subscribe_inbound` topic construction); when implementing 6A.3, replace all four `CONFIG_AMBYTE_*` references with `s_cfg.topic_root` and `s_cfg.device_id`
  - `cmd_mqtt_publish(const char *topic, const char *payload)` — raw publish for debugging / ad-hoc messages
  - `cmd_mqtt_publish_measurement(int64_t measure_id)` — query the header plus all value rows for `measure_id`, derive the fixed-contract topic internally, mark the header `INFLIGHT`, then publish with QoS 1
  - `cmd_mqtt_publish_unsynced(const char *measure_type)` — atomically claims the next pending measurement header of the requested type, derives the fixed-contract topic internally, and publishes exactly one logical measurement group per call
  - `cmd_mqtt_status()` — return connection state
  - `device_commands` owns a single pending measurement publish slot `msg_id <-> measure_id` for ack correlation only; durable publish state remains in SQLite
  - Raw `cmd_mqtt_publish()` calls never occupy or overwrite the measurement publish slot
  - The publish-ack handler only acts on the tracked measurement slot; unknown `msg_id` values are ignored
  - `device_commands_init()` registers a publish-ack handler through `message_set_publish_ack_handler_fn`
  - Robust + light sync policy with a single-in-flight uploader:
    - Before publish: transition header `PENDING -> INFLIGHT`
    - If publish queueing fails immediately: transition header back `INFLIGHT -> PENDING`
    - On `MQTT_EVENT_PUBLISHED`: transition header `INFLIGHT -> SYNCED`
    - On disconnect or reboot recovery: any remaining `INFLIGHT` header returns to `PENDING`
    - `cmd_mqtt_publish_unsynced()` claims at most one pending header and refuses to queue another measurement publish while one measurement is already in flight
  - **Important**: `cmd_mqtt_publish_measurement()` and `cmd_mqtt_publish_unsynced()` do **not** mark anything synced immediately after `publish()`

6A.4. Define the telemetry wire contract explicitly:
  - One MQTT payload represents one logical measurement group (`measure_id`)
  - JSON payload structure:
    ```json
    {"measure_id":1,"sensor_id":1,"measure_type":"env","timestamp":1700000000,"values":{"temperature":25.1,"humidity":60.0,"pressure":101325}}
    ```
    - `values` is a **JSON object** keyed by `data_type` (not an array)
    - device identifier embedded in the topic, not the payload
  - Topic format: `<s_cfg.topic_root>/<s_cfg.device_id>/<measure_type>/<measure_id>`
  - Measurement-oriented MQTT commands use this topic format only; they do not accept caller-provided topic overrides

6A.5. Wire in `app_main.c` — **TODO** (depends on 6A.3 + 6B.3):
  - `app_main` registers static event handlers directly: `on_got_ip` → `mqtt_client_start()`; `on_wifi_disconnect` → `mqtt_client_stop()` + `device_commands_on_mqtt_disconnect()`
  - No MQTT supervisor task — handlers call MQTT APIs directly; this is simpler and sufficient given the single-threaded event loop
  - `device_config_init()` called after NVS init; MQTT config built from NVS first, `CONFIG_AMBYTE_*` Kconfig fallback
  - All 19 `device_commands_config_t` fields wired: 17 original + `topic_root` + `device_id`; both read from NVS via `device_config_get_mqtt_topic_root()` / `device_config_get_device_id()` with Kconfig fallback (see 6C.4 for the code pattern)
  - `if (wifi_manager_is_connected()) mqtt_client_start()` called after event handler registration to handle the case where WiFi connected during `app_start_wifi()` before handlers were registered
  - `device_commands_subscribe_inbound()` called after `device_commands_init()`

6A.6. Provision certs pragmatically for the first milestone:
  - First implementation uses compile-time embedded PEMs via `components/certs/certs.c`
  - Runtime certificate provisioning is deferred until NVS / flash encryption or secure-element strategy is decided
  - Do not describe plain NVS certificate storage as "more secure" unless the security model is explicitly strengthened

**Verification:** Publish a test message and a real measurement payload; verify reception on AWS IoT console or MQTT test client. Force a disconnect between queue and acknowledgment and confirm the affected measurement header returns to `PENDING`, not `SYNCED`.

---

## Phase 6B: MQTT Inbound Commands And Runtime Device Config

Add inbound MQTT command handling and runtime device configuration only after 6A telemetry is stable.

### Bounded Context Addition

| Bounded Context | Domain Concepts | Components |
|---|---|---|
| **Messaging** | Subscription, CommandMessage, ReassembledPayload | `mqtt_client/`, `device_config/` |

### Steps

6B.1. ~~Extend `components/domain/include/messaging_port.h`~~ — **DONE** (completed in Phase 6A):
  - `message_handler_fn` already present
  - `message_subscribe_fn` already present

6B.2. Extend `components/mqtt_client/` — **DONE**:
  - Subscription table (8 slots, re-subscribes on MQTT_EVENT_CONNECTED)
  - Dynamic heap reassembly of fragmented `MQTT_EVENT_DATA` (malloc on first chunk, free after dispatch)
  - OOM on oversized payloads: skips remaining chunks and logs error — no crash

6B.3. Implement `components/device_config/` — **DONE** (base); extend with `device_id`:
  - NVS namespace `"device_cfg"`: keys `mqtt_uri`, `mqtt_client_id`, `mqtt_topic_root`; add `device_id` (9 chars, fits NVS_KEY_NAME_MAX_SIZE = 16)
  - `device_config_init()` + get/set for all four keys:
    - `device_config_get_device_id(char *buf, size_t len)` / `device_config_set_device_id(const char *val)`
  - `app_main` reads all four from NVS first, falls back to `CONFIG_AMBYTE_*` if key absent

6B.4. Wire inbound command topic handling — **PARTIAL** (subscribe/dispatch done; topic construction uses `CONFIG_AMBYTE_*` placeholder pending 6A.3):
  - `device_commands_subscribe_inbound()` subscribes to `<root>/<device_id>/cmd`
  - `cmd_dispatch_json()` parses JSON `{"cmd":...}` and dispatches to `cmd_*` functions
  - `cJSON` used for parsing (PRIV_REQUIRES in `device_commands/CMakeLists.txt`)
  - Supported commands: `set_rgb`, `read_env`, `status`, `publish_unsynced`, `mqtt_status`, `sleep_ms`, `log`
  - Command parsing lives in `device_commands`, not in `mqtt_client` plumbing
  - **Pending**: topic construction in `device_commands_subscribe_inbound` and `cmd_mqtt_publish_measurement` currently uses `CONFIG_AMBYTE_MQTT_TOPIC_ROOT`/`CONFIG_AMBYTE_DEVICE_ID` directly; corrected when 6A.3 adds `s_cfg.topic_root`/`s_cfg.device_id` and updates both snprintf calls

6B.5. Add BLE `device-config` provisioning endpoint (deferred — **requires 6C.2 and 6B.3 device_id extension first**):
  - **Dependency**: cannot be implemented before 6C.2 extends `wifi_manager_start_provisioning` with the extra-endpoint API (current 2-parameter signature has no mechanism to register custom endpoints) and before 6B.3's `device_id` extension is complete (`device_config_set_device_id()` must exist for the handler to persist the device_id field)
  - Implemented as a `protocomm` endpoint `"dev-cfg"` registered via the extra-endpoint array (see 6C.2 and 6C.4)
  - Accepted JSON payload: `{"mqtt_uri":"…","mqtt_client_id":"…","mqtt_topic_root":"…","device_id":"…"}`
  - Persist each present field to NVS via `device_config_set_*`; absent fields are left unchanged
  - **Timeout: 120 seconds, session-scoped** (shared with Wi-Fi and cert provisioning — see 6C constraints)
  - Reboot is deferred to session end alongside cert and Wi-Fi credential writes — single reboot covers all (see 6C.4)
  - `dev_cfg_prov_handler` is a thin wrapper in `app_main.c`; sets `s_config_written = true` on success via `priv_data`

**Verification:** Publish a fragmented test command payload; verify `mqtt_client` reassembles it once, command dispatch runs once, and OOM on oversize payload logs error without crashing.

---

## Phase 6C: TLS Certificate Provisioning over BLE

Deliver TLS client certificates to the device at runtime over BLE, replacing the compile-time `certs.c` placeholder. Requires 6B.5 (BLE `device-config` endpoint) to be stable first.

### Motivation

Compile-time embedding (`certs.c`) is acceptable for a first milestone but does not scale to field deployment: each device needs unique client certificates, and rebuilding firmware per device is impractical. This phase provisions certificates over the existing BLE connection used for Wi-Fi and device-config.

### Constraints

- **Security**: Certificate data is delivered as a custom `protocomm` endpoint registered via `wifi_prov_mgr_endpoint_create("cert-prov")`. This endpoint runs **inside** the existing SRP6a-encrypted BLE session (`WIFI_PROV_SECURITY_1`), so the private key is never transmitted in plaintext. A raw standalone GATT characteristic must not be used — it would bypass the session encryption entirely. Note: `WIFI_PROV_SECURITY_1` (SRP6a) is functional in ESP-IDF 5.x but `WIFI_PROV_SECURITY_2` (SRP6a + AES-GCM) is the current recommendation; upgrading is a future hardening step and does not block 6C.
- **BLE MTU limit**: `protocomm` endpoint payloads are capped by the negotiated MTU (typically 512 bytes minus framing). A PEM certificate is ~1–2 KB, so multi-chunk transfer is required.
- **NVS value size limit**: NVS string values are limited to 4000 bytes. PEM certificates and RSA-2048 private keys (~1700 bytes) all fit.
- **No flash encryption yet**: Certificates stored in NVS are readable if physical access to flash is obtained. This is an accepted risk until flash encryption is addressed in a later phase.
- **120-second BLE window is session-scoped**: the timeout starts when BLE advertising begins and covers the entire provisioning session — Wi-Fi credentials, device-config, and certificate writes must all complete within that window. The timeout does not reset on each write. 120 s is chosen to accommodate SRP6a key exchange, three endpoint writes, and BLE chunked cert transfer in a realistic mobile-app-driven flow.

### Steps

6C.1. Extend `components/certs/` to support NVS-backed certificate storage:
  - NVS namespace `"certs"`, keys: `"ca_cert"` (7), `"dev_cert"` (8), `"dev_key"` (7) — all ≤ 15 chars (NVS_KEY_NAME_MAX_SIZE = 16 incl. null)
  - `certs_init()` opens an NVS handle into a module-level `static nvs_handle_t s_handle` (same pattern as `device_config.c` — handle stays open for the device lifetime), then reads all three keys into module-level static buffers:
    ```c
    static char s_ca_cert[2048];
    static char s_dev_cert[2048];
    static char s_dev_key[2048];
    ```
    6 KB BSS total, always resident. If a key is absent the buffer holds an empty string and the compiled-in global is used as fallback. If flash cost becomes a concern, replace with heap-allocated `char *` pointers initialised to `NULL` on `certs_init()` and allocated only when the NVS key is found.
  - `certs_get_ca()`, `certs_get_device_cert()`, `certs_get_device_key()` — return `const char *` pointing to the static buffer if `buffer[0] != '\0'`, otherwise return the compiled-in global directly. Pointers are stable for the device lifetime; callers must not free them.
  - `certs_set_ca(const char *pem)`, `certs_set_device_cert(const char *pem)`, `certs_set_device_key(const char *pem)` — write to NVS via `nvs_set_str` + `nvs_commit` using the open `s_handle`; do **not** update the cached static buffer (a reboot is required to reload)
  - `certs_are_provisioned()` — per-cert check; returns true only if every getter returns a non-empty string:
    ```c
    return certs_get_ca()[0]          != '\0'
        && certs_get_device_cert()[0] != '\0'
        && certs_get_device_key()[0]  != '\0';
    ```
    A mix of NVS-loaded and compiled-in certs correctly returns true.
  - `certs_port.h` does not exist and should not be created — certs are infrastructure-to-infrastructure and need no domain port
  - Update `components/certs/CMakeLists.txt`: add `REQUIRES nvs_flash` (needed by `nvs_open`, `nvs_get_str`, `nvs_set_str`)
  - **Call site**: `app_main()` must call `certs_init()` after NVS init (`app_init_nvs()`) and before `mqtt_client_init()`. Without this call, `s_ca_cert`/`s_dev_cert`/`s_dev_key` remain empty, all `certs_get_*()` callers silently fall back to the compiled-in empty stubs, and TLS is disabled with no error reported.
  - Add `certs` to `REQUIRES` in `main/CMakeLists.txt` at this step — `app_main.c` now directly calls `certs_init()` and includes `certs.h`; making the dependency explicit here (rather than relying on the transitive path through `mqtt_client`) prevents a build break when 6C.5 later removes `certs` from `mqtt_client/CMakeLists.txt`

6C.2. Decouple `wifi_manager` from cert logic — extend `wifi_manager_start_provisioning` to accept caller-provided extra endpoints:
  - Add to `wifi_manager.h`:
    ```c
    typedef struct {
        const char              *name;
        protocomm_req_handler_t  handler;
        void                    *ctx;
    } wifi_prov_extra_endpoint_t;

    esp_err_t wifi_manager_start_provisioning(
        const char                       *device_name,
        const char                       *pop,
        const wifi_prov_extra_endpoint_t *extra_endpoints,
        size_t                            num_extra_endpoints);
    ```
  - Inside `wifi_manager_start_provisioning`: call `wifi_prov_mgr_init()` first (line 562 in current `wifi_manager.c`); then iterate `extra_endpoints` and call `wifi_prov_mgr_endpoint_create(ep->name)` for each — this MUST happen after `wifi_prov_mgr_init()` but BEFORE `wifi_prov_mgr_start_provisioning()` (line 576); then call `wifi_prov_mgr_start_provisioning()`; then iterate again and call `wifi_prov_mgr_endpoint_register(ep->name, ep->handler, ep->ctx)` for each after start
  - `wifi_manager` gains no dependency on `certs` or `device_config`; `app_main` owns the endpoint descriptors
  - Update `wifi_manager/CMakeLists.txt`: move `protocomm` from `PRIV_REQUIRES` to `REQUIRES` — `wifi_manager.h` now exposes `protocomm_req_handler_t` in the public `wifi_prov_extra_endpoint_t` struct; consumers (e.g. `app_main`) must be able to see that type at compile time

6C.3. Implement `cert_prov_handler` — chunked transfer inside the encrypted session:
  - `cert_prov_handler` is a `protocomm_req_handler_t` (signature: `esp_err_t handler(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen, uint8_t **outbuf, ssize_t *outlen, void *priv_data)`); responses are written into `*outbuf`/`*outlen` (the protocomm response buffer) — not GATT notifications
  - **CRITICAL**: `*outbuf` MUST be allocated with `malloc` — protocomm calls `free(*outbuf)` after sending the response. Never assign a string literal or stack address to `*outbuf`; always do `*outbuf = malloc(len); memcpy(*outbuf, json_str, len); *outlen = len;`
  - Handler state machine (`IDLE → RECEIVING → DONE`), with `bool *out_written` injected via `priv_data`:

    | State | Write received | Action | Response written to `*outbuf` |
    |---|---|---|---|
    | IDLE | first chunk (≥ 4 bytes) | read big-endian `total_len` from bytes 0–3; if `total_len > 8192` reject immediately; else `malloc(total_len+1)`, copy bytes 4+ | `{"complete":false}` |
    | IDLE | first chunk < 4 bytes | — | `{"ok":false,"error":"bad header"}` |
    | RECEIVING | continuation chunk | append to buffer | `{"complete":false}` |
    | RECEIVING | fill == total_len (last chunk) | null-terminate; parse JSON; call `certs_set_*` for each present field; free buffer; set `*out_written = true` | `{"ok":true}` or `{"ok":false,"error":"…"}` |
    | RECEIVING | oversize / OOM | free buffer; reset to IDLE | `{"ok":false,"error":"overflow"}` |

  - **Single-chunk complete**: after copying the first chunk, check immediately if `fill == total_len`; if so, process the buffer as a completed message (same action as the RECEIVING last-chunk row) and respond `{"ok":true}` instead of `{"complete":false}`. This handles test payloads or any cert small enough to fit in one BLE packet.
  - On BLE disconnect before last chunk: free buffer, reset to IDLE, no NVS write (`priv_data` flag not set)
  - **Cleanup after session end**: expose `void cert_prov_handler_reset(void)` — frees in-flight buffer and resets state to IDLE. `app_main` calls this unconditionally after `wifi_manager_start_provisioning()` returns (before the `prov_ok || any_write` reboot check), covering both the timeout+zero-writes path and the normal session-end path.
  - **Accepted JSON payload** (all fields optional; absent fields leave existing NVS values unchanged):
    ```json
    {"ca_cert":"-----BEGIN CERTIFICATE-----\n…","dev_cert":"…","dev_key":"…"}
    ```
  - The client must wait for each protocomm response before sending the next chunk

6C.4. Refactor `app_start_wifi()` and own write flags in `app_main`:

  **Refactor `app_start_wifi()`** — strip it down to init + start only; remove the provisioning check and both connect paths:
  ```c
  static esp_err_t app_start_wifi(void)
  {
      esp_err_t err = wifi_manager_init();
      if (err != ESP_OK) return err;
      return wifi_manager_start();
  }
  ```

  **`app_main()` owns the full Wi-Fi branching** — after `app_start_wifi()` returns, check provisioning state and drive both paths:
  ```c
  err = app_start_wifi();
  if (err != ESP_OK) {
      ESP_LOGE(APP_TAG, "Wi-Fi startup failed: %s", esp_err_to_name(err));
      return;
  }

  bool provisioned = false;
  wifi_manager_is_provisioned(&provisioned);

  if (!provisioned) {
      static bool s_certs_written  = false;
      static bool s_config_written = false;

      wifi_prov_extra_endpoint_t extra[] = {
          { "cert-prov", cert_prov_handler,    &s_certs_written  },
          { "dev-cfg",   dev_cfg_prov_handler, &s_config_written },
      };
      esp_err_t prov_err = wifi_manager_start_provisioning(
          "AMBYTE", "ambyte123", extra, 2);

      cert_prov_handler_reset(); /* free any in-flight reassembly buffer */
      bool any_write = s_certs_written || s_config_written;
      bool prov_ok   = (prov_err == ESP_OK);
      if (prov_ok || any_write) {
          esp_restart();
      }
      /* else: timed out, zero writes — continue without reboot */
  } else {
      err = wifi_manager_connect_stored();
      if (err == ESP_OK) {
          ESP_LOGI(APP_TAG, "Wi-Fi connected");
      } else if (err == ESP_ERR_TIMEOUT) {
          ESP_LOGW(APP_TAG, "Wi-Fi initial connect timed out; reconnect continues in background");
      } else {
          ESP_LOGW(APP_TAG, "Wi-Fi connect failed: %s", esp_err_to_name(err));
      }
  }
  ```

  The reboot-on-success block (previously 6C.6) is now inline here — 6C.6 is superseded by this structure.

  - `dev_cfg_prov_handler` is a thin wrapper that calls `device_config_set_*` for each present JSON field and sets `*(bool *)priv_data = true` on success
  - Both handlers are defined in `app_main.c` (as static functions above `app_main`); no cert or config logic moves into `wifi_manager`
  - `app_main.c` uses `cJSON` to parse JSON payloads in both handlers → add `json` to `PRIV_REQUIRES` in `main/CMakeLists.txt`
  - `on_got_ip` and `on_wifi_disconnect` are registered after this entire Wi-Fi block, so they cannot fire during the provisioning window
  - After `app_start_wifi()` and the provisioning/connect block, wire `topic_root` and `device_id` into `device_commands_config_t` using NVS reads with Kconfig fallback:
    ```c
    char topic_root[128], device_id[64];
    if (device_config_get_mqtt_topic_root(topic_root, sizeof(topic_root)) != ESP_OK)
        strncpy(topic_root, CONFIG_AMBYTE_MQTT_TOPIC_ROOT, sizeof(topic_root) - 1);
    topic_root[sizeof(topic_root) - 1] = '\0';
    if (device_config_get_device_id(device_id, sizeof(device_id)) != ESP_OK)
        strncpy(device_id, CONFIG_AMBYTE_DEVICE_ID, sizeof(device_id) - 1);
    device_id[sizeof(device_id) - 1] = '\0';

    device_commands_config_t cmd_cfg = {
        /* … all existing fields … */
        .topic_root = topic_root,
        .device_id  = device_id,
    };
    ```
    The `topic_root` and `device_id` buffers are on `app_main`'s stack. They remain valid for the device lifetime because `app_main` never returns — this is safe.

6C.5. Decouple `mqtt_client` from `certs` — pass PEM pointers through config:
  - Extend `mqtt_client_config_t` with three new fields:
    ```c
    const char *ca_cert_pem;     /* NULL → no TLS */
    const char *device_cert_pem; /* NULL → no client auth */
    const char *device_key_pem;  /* NULL → no client auth */
    ```
  - `mqtt_client_init()` reads TLS config from these fields; enable TLS only when `ca_cert_pem != NULL`
  - `mqtt_client` no longer REQUIRES `certs`; remove `certs` from `mqtt_client/CMakeLists.txt`
  - `app_main` gates the PEM fields on `certs_are_provisioned()` — `certs_get_*()` always returns a non-NULL pointer (falls back to compiled-in empty string), so a NULL check on the return value alone would always be true and would pass empty certs to the MQTT stack:
    ```c
    bool certs_ok = certs_are_provisioned();
    mqtt_client_config_t mqtt_cfg = {
        .broker_uri      = mqtt_uri,
        .client_id       = mqtt_client_id,
        .ca_cert_pem     = certs_ok ? certs_get_ca()           : NULL,
        .device_cert_pem = certs_ok ? certs_get_device_cert()  : NULL,
        .device_key_pem  = certs_ok ? certs_get_device_key()   : NULL,
    };
    ```
  - `main/CMakeLists.txt` already has `certs` in `REQUIRES` (added in 6C.1); removing `certs` from `mqtt_client/CMakeLists.txt` here is safe

6C.6. ~~Single reboot at provisioning session end~~ — **superseded by 6C.4**:
  - The `prov_ok || any_write` reboot logic is now inline in `app_main()` immediately after `wifi_manager_start_provisioning()` returns (see 6C.4 code block). No separate step needed.

6C.7. CLI cert status via `device_commands`:
  - Add `certs_status_fn` function pointer to `device_commands_config_t`:
    ```c
    typedef bool (*certs_status_fn)(void); /* wraps certs_are_provisioned() */
    ```
  - Add `cmd_cert_status()` to `device_commands` — calls `s_cfg.certs_status` and returns a `cmd_result_t` describing whether NVS-backed certs are present
  - CLI calls `cmd_cert_status()` — CLI never references `certs` directly
  - `app_main` wires `certs_are_provisioned` into `device_commands_config_t.certs_status`
  - After this step, `device_commands_config_t` has **20 fields** total: 17 original + `topic_root` + `device_id` (added in 6A.3) + `certs_status_fn` (this step)
  - `WIFI_MANAGER_PROV_TIMEOUT_MS` is already `120000` — ✓ DONE

**Verification:** Flash device with empty `certs.c` stubs. Over BLE provisioning (using ESP's provisioning app or `esp_prov` CLI tool), send Wi-Fi credentials to the standard endpoint, device-config JSON to `"dev-cfg"`, and cert JSON to `"cert-prov"`. Confirm single reboot occurs after session. Confirm MQTT connects with TLS to AWS IoT Core on next boot. Run `cert status` CLI command to confirm NVS-backed certs are reported. Test edge case: send only cert JSON then let session time out — confirm single reboot still occurs.

---

## Phase 7: UART Sensor Querying (4 Channels, Non-Blocking)

### Hardware Constraint

ESP32-S3 has **3 hardware UART controllers** (UART0, UART1, UART2). Console uses USB Serial JTAG (not UART), so all 3 hardware UARTs are free. But you need **4 UART channels**. Options:
  - **Option A**: 3 HW UARTs + 1 time-multiplexed (GPIO matrix remap between queries — possible since queries are sequential with 2-min responses)
  - **Option B**: External UART multiplexer IC (e.g., SC16IS752 — 2-channel UART over I2C)
  - **Option C**: Soft UART (bit-banging) for the 4th channel — unreliable at higher baud rates

**Recommendation**: Option A (GPIO matrix remap). Since queries take up to 2 min each, sensors are queried sequentially. One UART controller can be remapped to different pins between queries using `uart_set_pin()`. This needs zero additional hardware.

### Dual-Core Architecture

ESP32-S3 has 2 cores. Pin tasks to specific cores to minimize blocking:

| Core | Tasks | Priority | Rationale |
|---|---|---|---|
| **Core 0** (PRO) | WiFi stack, MQTT client, data management, SQLite writes | High | WiFi requires Core 0; MQTT callbacks run here |
| **Core 1** (APP) | Lua runner, CLI, status LED, UART sensor queries | Mixed | Sensor queries block for up to 2 min; isolated from networking |

### Domain Port

7.0. Define **UART Sensor port** in `components/domain/include/uart_sensor_port.h`:
  - `typedef struct { uint8_t channel; const char *query; size_t query_len; } uart_query_t;`
  - `typedef struct { uint8_t channel; uint8_t *response; size_t response_len; esp_err_t status; } uart_response_t;`
  - `typedef esp_err_t (*uart_sensor_query_fn)(const uart_query_t *query, uart_response_t *response, uint32_t timeout_ms);`

### Steps

7.1. Create `components/uart_sensors/` — manages 4 UART sensor channels:
  - Configure 3 UART controllers + GPIO remap for 4th channel
  - Each query runs as a FreeRTOS task pinned to Core 1 via `xTaskCreatePinnedToCore(..., 1)`
  - Non-blocking pattern: send query → `uart_read_bytes()` with timeout → yield via `vTaskDelay()` between poll attempts
  - Response buffer managed per-channel (heap-allocated, freed after processing)
  - Mutex per UART controller prevents concurrent access
  - `REQUIRES domain driver` + `PRIV_REQUIRES i2c_bus` (if using Option B)

7.2. UART query task architecture (non-blocking):
  ```
  uart_sensor_task (pinned to Core 1):
    for each scheduled query:
      if channel needs GPIO remap: uart_set_pin(uart_num, new_tx, new_rx, ...)
      uart_write_bytes(uart_num, query, len)
      while (elapsed < timeout_ms):
        bytes = uart_read_bytes(uart_num, buf, len, pdMS_TO_TICKS(100))
        if (response_complete(buf)) break
        // Core 1 yields here — Core 0 (WiFi/MQTT) unaffected
      store result → notify data management task on Core 0
  ```
  - **Task watchdog**: The UART sensor task is **not registered** with the ESP task watchdog (`esp_task_wdt`). It is designed to block for up to 2 minutes per query — WDT monitoring is counterproductive. If the UART task truly hangs, the caller (Lua) detects it via response timeout. Other tasks (Lua, CLI, WiFi) remain WDT-monitored.
  - `cmd_uart_query(uint8_t channel, const char *query, uint32_t timeout_ms)` — send query, wait for response
  - `cmd_uart_ping(uint8_t channel)` — send a short probe (e.g., `"PING\n"`), expect response within ~2s. Returns connected/disconnected. Used by Lua before attempting a full measurement query.
  - `cmd_uart_status()` — show all 4 channels: connected/disconnected/busy/disabled (from `channel_config`)

7.3a. **Ping/health-check design**:
  - Each channel defines a lightweight probe: send a known short command, expect any response within a short timeout (e.g., 2000ms)
  - `uart_sensor_ping()` internally: flush RX buffer → `uart_write_bytes(ping_cmd)` → `uart_read_bytes(buf, 64, pdMS_TO_TICKS(2000))`
  - If bytes received > 0 → `SENSOR_CONNECTED`; if timeout → `SENSOR_DISCONNECTED`
  - Result cached per-channel with a TTL (e.g., 10s) so repeated checks don't hammer the bus
  - Lua usage: `if device.uart_ping(1) then device.uart_query(1, "MEASURE") end`
  - CLI usage: `ping_uart 1` → "AMBIT1: connected (MultispeQ v2.0)" or "AMBIT1: disconnected"
  - Status is also checked automatically before `cmd_uart_query()` — if ping fails, return error immediately instead of blocking for 2 min

7.4. Wire into `app_main.c`:
  - Init UART controllers with pin assignments
  - Register `uart_sensor_query_fn` port adapter
  - Pin sensor tasks to Core 1, networking tasks to Core 0
  - **Auto-ping at boot**: After UART init + channel config load from NVS, call `uart_sensor_ping(channel)` for each channel where `sensor_name` is non-empty. 2s timeout per channel, sequential (up to 8s total for 4 channels). Results populate `hw_inventory_t` (Phase 3.5) and are logged as part of hardware inventory. Channels marked `SENSOR_DISCONNECTED` remain queryable (sensor may come online later) — just flagged at boot.

7.5. GPIO pin assignment for 4 UART channels:

  | Channel | RX Pin | TX Pin | HW UART |
  |---|---|---|---|
  | AMBIT1 | GPIO 3 | GPIO 46 | UART1 |
  | AMBIT2 | GPIO 17 | GPIO 18 | UART2 |
  | AMBIT3 | GPIO 47 | GPIO 48 | UART0 (remapped for query, then back) |
  | AMBIT4 | GPIO 40 | GPIO 41 | UART0 (remapped — time-shared with AMBIT3) |

  - AMBIT1 and AMBIT2 get dedicated HW UARTs (UART1, UART2) — can run concurrently
  - AMBIT3 and AMBIT4 share UART0 via GPIO matrix remap between queries
  - Console uses USB Serial JTAG (not UART0), so UART0 is free
  - AMBIT3/4 pins (47/48, 40/41) are NOT UART0 default pins (43/44) — GPIO matrix remap via `uart_set_pin()` maps UART0 to these custom pins. No conflict with default UART0 GPIOs.
  - No pin conflicts with: I2C (38, 39), SDMMC (9-14), USB JTAG (19, 20), status LED (45)
  - **Note**: GPIO 46 (AMBIT1 TX) is a boot strapping pin on ESP32-S3. Ensure the AMBIT1 sensor does not drive this pin during reset (e.g., series resistor, or ensure sensor powers on after ESP32 boot completes).

**Verification:** Send a test query to a sensor, verify response received. Monitor Core 0 WiFi connectivity during a 2-min query on Core 1 — no disconnections.

---

## Revised Bounded Context Map

| Bounded Context | Domain Concepts | Components |
|---|---|---|
| **Sensing** | Measurement, SensorReading, SensorId | `bme280/`, `pcf2131tfy_rtc/`, `uart_sensors/` (new) |
| **Connectivity** | Connection, NetworkCredentials, Provisioning | `wifi_manager/` (incl. BLE provisioning), `certs/` |
| **Messaging** | Message, Topic, PublishResult, Subscription | `mqtt_client/` (new), `device_config/` |
| **Persistence** | MeasurementLog, MeasurementHeader, MeasurementRecord | `persistence/` (new, LittleFS WAL + SQLite on SD card), `sd_card/` (FAT I/O), `sqlite3` |
| **Automation** | Script, MeasurementJob | `lua_runner/`, `lua/` |
| **Device Management** | DeviceStatus, Command | `CLI/`, `ambyte_status/` |

---

## Relevant Files

- [main/app_main.c](main/app_main.c) — Refactor into composition root (Phase 3C.4)
- [main/CMakeLists.txt](main/CMakeLists.txt) — Adjust REQUIRES after refactoring
- [components/CLI/CMakeLists.txt](components/CLI/CMakeLists.txt) — Remove direct sensor PRIV_REQUIRES (Phase 3C)
- [components/CLI/CLI.c](components/CLI/CLI.c) — Rewrite to call `cmd_*()` directly, keep only `i2cscan` native (Phase 3C)
- [components/lua_runner/CMakeLists.txt](components/lua_runner/CMakeLists.txt) — Remove direct component REQUIRES (Phase 3B)
- [components/lua_runner/lua_runner.c](components/lua_runner/lua_runner.c) — Remove old sd.*/status.* bindings, add device.*/db.* bindings via cmd_*(), and expose no manual sync-state mutation API (Phase 3B)
- [components/bme280/CMakeLists.txt](components/bme280/CMakeLists.txt) — Remove I2C_device + Arduino deps (Phase 0B), add `REQUIRES domain` (Phase 2.1)
- [components/bme280/bme280_arduino_shim.cpp](components/bme280/bme280_arduino_shim.cpp) — Rename to `bme280_driver.cpp`, refactor to use i2c_bus directly (Phase 0B)
- [components/bme280/BME280.cpp](components/bme280/BME280.cpp) — Rewrite I2C primitives (write8/read8/read16/read24/begin), replace Arduino functions (Phase 0B)
- [components/bme280/bme280_adafruit_private.hpp](components/bme280/bme280_adafruit_private.hpp) — Remove Arduino/I2C_device includes, replace member types (Phase 0B)
- [components/pcf2131tfy_rtc/CMakeLists.txt](components/pcf2131tfy_rtc/CMakeLists.txt) — Add `REQUIRES domain` (Phase 2.2)
- [components/pcf2131tfy_rtc/include/RTC_NXP.h](components/pcf2131tfy_rtc/include/RTC_NXP.h) — Move to source directory (Phase 0C)
- [components/I2C_device/](components/I2C_device/) — DELETE entire component (Phase 0B)
- [components/wifi_manager/](components/wifi_manager/) — Extend with BLE Wi-Fi provisioning only (Phase 5A), add `PRIV_REQUIRES wifi_provisioning protocomm bt`
- `components/device_config/` — NVS-backed runtime MQTT and channel metadata store (Phase 5B / 6B)
- `components/mqtt_client/Kconfig.projbuild` — compile-time MQTT config carrier for Phase 6A (`CONFIG_AMBYTE_DEVICE_ID`, `CONFIG_AMBYTE_MQTT_URI`, `CONFIG_AMBYTE_MQTT_CLIENT_ID`, `CONFIG_AMBYTE_MQTT_TOPIC_ROOT`)
- [components/sd_card/sd_card.c](components/sd_card/sd_card.c) — Text-file functions removed in Phase 3C.3 (after Lua/CLI rewired); SQLite DB lives on SD card

**New files to create:**
- `partitions.csv` — Custom partition table for 16MB flash with OTA + LittleFS (Phase 0A.8)
- `components/domain/CMakeLists.txt` — Header-only component, `REQUIRES esp_common`
- `components/domain/include/sensing_port.h` — Sensing contracts
- `components/domain/include/persistence_port.h` — Persistence contracts (SQLite repository pattern, fixed-size char[32] fields)
- `components/domain/include/device_status_port.h` — Status contracts
- `components/domain/include/messaging_port.h` — MQTT publish / ack / status contracts (Phase 6A), subscribe contracts added in Phase 6B
- `components/domain/include/uart_sensor_port.h` — UART query/response contracts (Phase 7)
- `components/sqlite3/` — Git submodule of `siara-cc/esp32-idf-sqlite3` (Phase 2.3) — with VFS shim fix for `esp32_Sync()`
- `components/littlefs/` — `joltwallet/littlefs` from ESP Component Registry (Phase 2.3)
- `components/persistence/CMakeLists.txt` — SQLite persistence component, `REQUIRES domain sqlite3 sd_card` + `PRIV_REQUIRES esp_littlefs`
- `components/persistence/sqlite_persistence.c` — SQLite-based persistence port implementation with dedicated `s_sqlite_mutex`
- `components/persistence/pending_store.c` — LittleFS write-ahead buffer for measurement records with `s_pending_mutex`
- `components/persistence/include/sqlite_persistence.h` — Init + port adapter functions
- `components/persistence/include/pending_store.h` — Pending store append/read/remove/count API
- `components/device_commands/CMakeLists.txt` — Shared command layer, REQUIRES `domain` only
- `components/device_commands/device_commands.c` — All `cmd_*` implementations + `device_commands_config_t` struct aggregating all ports
- `components/device_commands/include/device_commands.h` — `cmd_result_t` + `device_commands_config_t` + all command signatures
- `components/device_config/CMakeLists.txt` — NVS-backed runtime config component (Phase 5B / 6B)
- `components/device_config/device_config.c` — reads/writes MQTT runtime config and later channel metadata in NVS (Phase 5B / 6B)
- `components/device_config/include/device_config.h` — device config API (Phase 5B / 6B)
- `components/mqtt_client/Kconfig.projbuild` — compile-time MQTT config carrier for 6A
- `components/mqtt_client/` — ESP-MQTT wrapper with TLS, publish ack translation, and later subscribe support (Phase 6A / 6B)
- `components/uart_sensors/` — 4-channel UART sensor manager, Core 1 tasks (Phase 7)
- `components/mp2731/` — MP2731 battery charger I2C driver (Phase 8)

---

## Heap Budget

ESP32-S3 has ~512KB total SRAM, of which ~320KB is available after ESP-IDF + WiFi + MQTT stacks.

| Consumer | Heap (approx.) | Phase | Lifetime |
|---|---|---|---|
| Lua VM | 30-50 KB | 3 | Transient (one-shot lifecycle — freed after script completes) |
| SQLite (DB open, WAL buffers) | 80-120 KB | 2 | Permanent |
| BLE provisioning (NimBLE) | 60-70 KB | 5 | Temporary (first boot only) |
| UART RX buffers (×4 channels) | 4-16 KB | 7 | Permanent |
| FreeRTOS mutexes (SQLite, I2C, pending) | ~1.5 KB | 2/3 | Permanent |
| Measurement query buffer | ~5.9 KB | 2 | Transient (malloc/free per query) |
| Task stacks (Lua 8K + CLI 4K + Flush 6K + UART 4K) | ~22 KB | 3/7 | Permanent |

**Worst case (all phases, no BLE, Lua running)**: ~145-210KB permanent → ~110-175KB free.
**With Lua idle (one-shot completed)**: subtract ~30-50KB → ~140-225KB free.
**During BLE provisioning**: add ~65KB temporary → ~55-115KB free.
**Minimum target**: >32KB free heap at all times. Monitored via `esp_get_minimum_free_heap_size()`.

---

## Verification

1. After each phase, run `pio run` — firmware must build successfully
2. After Phase 2: flash and confirm BME280, RTC, SD card, LED all still work
3. After Phase 3: test CLI commands (`status`, `read_env`, `i2cscan`) still produce correct output via direct `cmd_*()` calls
4. After Phase 3: test Lua scripts still execute and produce same behavior
5. After Phase 4: attempt to add `REQUIRES bme280` to `domain/CMakeLists.txt` — should violate the layering rule (caught by code review / CI)
6. Inspect `components/domain/CMakeLists.txt` has only `REQUIRES esp_common` throughout all phases (no infra deps)
7. **Robustness checks**: Pull SDA low with jumper wire → device should recover via `i2c_bus_check_and_recover()` and log, not hang
8. Remove SD card during operation → new measurements continue accumulating in LittleFS pending buffer, `cmd_query` returns `ESP_ERR_NOT_SUPPORTED`, device continues operating. Re-insert SD card → flush task drains pending records to SQLite within 5s.
9. Disconnect BME280 mid-operation → `device.read_env()` returns error, device continues operating
10. After Phase 6A: publish one measurement group and verify the header transitions `PENDING -> INFLIGHT -> SYNCED`, never directly to `SYNCED` before MQTT acknowledgment
11. After Phase 6A: disconnect Wi-Fi after queueing a publish but before ack and verify the measurement header returns to `PENDING` for retry
12. After Phase 6A: call `cmd_mqtt_publish_unsynced()` twice before the first publish is acknowledged and verify the second call does not claim or queue another measurement
13. After Phase 6B: send a fragmented inbound MQTT payload and verify it is reassembled into a single logical command callback
14. Monitor `esp_get_minimum_free_heap_size()` across a full boot+provision+measure cycle → stays above 32KB
15. Verify `uxTaskGetStackHighWaterMark()` for Lua task reports >1KB remaining after first script execution

---

## Decisions

- **No runtime abstraction overhead**: Ports use function pointers (single indirection), not vtables or heap allocation. This is acceptable for an embedded system.
- **ESP-IDF component model preserved**: DDD layers are enforced through `REQUIRES`/`PRIV_REQUIRES`, not filesystem reorganization into `domain/`, `infrastructure/` top-level folders. Components stay in `components/`.
- **Incremental migration**: Each phase is independently shippable. Phase 1-2 are additive (no breaking changes). Phase 3 is the first refactor.
- **Arduino dependency eliminated in Phase 0B**: Refactoring BME280 to use native ESP-IDF I2C removes `framework-arduinoespressif32` and the `I2C_device` component entirely.
- **SQLite for persistence**: Using `siara-cc/esp32-idf-sqlite3` (pure ESP-IDF, Apache-2.0). DB file lives on **64GB SD card** (FAT32) at `/sdcard/measurements.db`. New measurements are buffered in LittleFS on internal flash first, then flushed to SQLite by a background task. If SD card is absent, data accumulates safely in LittleFS. VFS shim `esp32_Sync()` must be fixed to call `fsync()`.
- **SQLite PRAGMAs**: `journal_mode=WAL`, `locking_mode=EXCLUSIVE`, `synchronous=NORMAL`, `page_size=4096`, `cache_size=-64`, `wal_autocheckpoint=25`, `journal_size_limit=65536`, `temp_store=MEMORY`. `wal_autocheckpoint=25` limits WAL growth to 100KB max (25 pages × 4KB) — keeps heap and SD card write amplification bounded.
- **Dual filesystem**: LittleFS (512KB, internal flash) as write-ahead buffer for measurement records; FAT32 (64GB SD card, 32KB clusters) for SQLite DB and bulk data/logs.
- **All structured persistence through SQLite on SD card**: Text-file API (`sdcard_write_line`, etc.) removed in Phase 3C after Lua/CLI are rewired. SD card retains mount/unmount API.
- **Arduino framework fully removed**: After BME280 is converted to native ESP-IDF I2C (Phase 0B), `arduino` is removed from `platformio.ini` frameworks. The RTC already uses native ESP-IDF.
- **Board config updated**: Switch `platformio.ini` from `adafruit_feather_esp32s3` (2MB) to `esp32-s3-devkitm-1` (16MB) to match actual ESP32-S3-WROOM-1 hardware. Custom `partitions.csv` with 3MB factory app + 3MB OTA + 64KB coredump + 512KB LittleFS + ~9.4MB FAT storage + NVS.
- **Lua standard libs kept**: All Lua standard library modules remain (user may use them in future scripts).
- **Device-prefixed measureID**: `measureID` includes a device ID prefix for future multi-device support.
- **Misc files moved to `docs/`**: `AI prompts.txt`, `MQTT TLS test client.py`, `overall_architecture.txt` moved to `docs/` folder.
- **Excluded**: Event-driven pub/sub between bounded contexts. Can be layered on later.
- **MQTT rollout split into 6A and 6B**: 6A is outbound telemetry only. 6B adds subscribe, inbound command handling, and optional custom BLE-backed runtime device configuration. This keeps the first MQTT milestone shippable and avoids blocking on command parsing or custom provisioning.
- **MQTT v5**: Already enabled in sdkconfig. Uses ESP-MQTT with mutual TLS to AWS IoT Core.
- **BLE provisioning inside wifi_manager**: No separate `ble_provisioning/` component. `wifi_manager` gains `wifi_manager_is_provisioned()` and `wifi_manager_start_provisioning()` — BLE code is private (`PRIV_REQUIRES bt`). Consumers never see BLE types. Phase 5A covers Wi-Fi credentials only.
- **Dual-core split**: Core 0 = WiFi/MQTT/data. Core 1 = Lua/CLI/UART sensors. Prevents 2-min UART queries from blocking networking.
- **4 UART channels on 3 HW UARTs**: GPIO matrix remap for 4th channel (sequential queries allow time-sharing). No extra hardware needed.
- **domain/ REQUIRES esp_common only**: Needed for `esp_err_t` type. This is purely type definitions — no infrastructure dependency. Domain stays header-only (no .c files).
- **Flat persistence schema (single table)**: A single `measurements` table holds all columns per row — no split `measurement_headers`/`measurement_values` tables. `measurement_record_t` is the only struct; `sync_state` is stored on every row of a logical group (denormalized). `mark_inflight`/`mark_pending`/`mark_synced` UPDATE all rows for a given `measureID`. Trade-off: group metadata is duplicated across rows of the same measurement, but this eliminates JOIN complexity and simplifies both the write path and the pending store.
- **CLI and Lua both call `device_commands` directly**: Both CLI and Lua tasks call `cmd_*` functions directly. Thread safety provided by per-resource mutexes (I2C bus lock, SQLite mutex). No FreeRTOS queue or IPC between CLI and Lua — they operate independently. CLI remains responsive during Lua script execution. `i2cscan` stays native in CLI, not in device_commands or Lua.
- **Query result buffers heap-allocated**: Caller `malloc`s the buffer, passes to `cmd_query_measurements`, then `free`s. Avoids stack overflow in 4KB task stacks. Default max 64 records → ~5.9KB heap allocation.
- **device_commands_init() takes a config struct**: `device_commands_config_t` aggregates port function pointers. Struct grows incrementally per phase: Phase 3 has Phase 2 ports only; Phase 6A adds `message_publish_fn`, `message_is_connected_fn`, and `message_set_publish_ack_handler_fn`; Phase 6B adds `message_subscribe_fn`; Phase 7 adds `uart_sensor_query_fn`; Phase 8 adds `power_read_fn`. NULL fields mean "not available" → `ESP_ERR_NOT_SUPPORTED`. Port requirements per command documented (e.g., `cmd_store_measurement` needs only `store`, not `read_env` or `read_clock`).
- **cmd_store_measurement takes flat records**: Receives `const measurement_record_t *records, size_t count` — does NOT read sensors internally. Caller responsible for assembling all rows from `device.read_env()` + `device.read_rtc()` + `db.next_id()` and filling `data_type`/`value` per row. Enables any sensor type to use the same store function.
- **SQLite access serialized by dedicated mutex**: `s_sqlite_mutex` in `sqlite_persistence.c`, separate from SD mount mutex and LittleFS pending mutex. Only the flush task and query commands access SQLite. Prevents Core 0 / Core 1 concurrent DB corruption.
- **sensing_ctx_t folded into device_commands_config_t**: No `sensing_service.h` in domain/. Port aggregation is application-layer concern, lives in `device_commands/`.
- **SD card required for persistence (64GB, FAT32)**: `sdcard_init_default()` + `sdcard_mount()` called in `app_main`. SQLite DB at `/sdcard/measurements.db`. If SD absent, new measurements accumulate in LittleFS pending buffer (up to ~28 hours at typical rates). Queries require SD card. `sd_card_is_available()` public API for Lua scripts.
- **PCF2131 RTC hierarchy preserved**: All features (alarm, timestamp, periodic interrupt, watchdog) kept. Only change: `RTC_NXP.h` moved from `include/` to source directory (privatized). No source files deleted.
- **Lua `sd.*` API explicitly removed in Phase 3B**: Old `status.*` and `sd.*` module registrations deleted. Replaced by `device.*`/`db.*` API. Embedded script updated. Breaking change for user scripts using old API.
- **No public manual sync-state mutation command**: Neither Lua nor CLI exposes `mark_synced`. Scripts can inspect pending groups and request publish, but MQTT acknowledgment remains the only normal path to `SYNCED`.
- **Lua task lifecycle stays one-shot**: VM is created, runs script, then destroyed. Does NOT stay alive permanently since CLI calls `cmd_*` directly (no queue dispatch needed). This saves ~30-50KB heap when no script is running. Error recovery: `lua_pcall` failure → log + return error; VM destruction handles cleanup.
- **sdkconfig.defaults tracked in git**: Removed from `.gitignore` — defines project build defaults that must be version-controlled.
- **BLE deinit uses `FREE_BLE` not `FREE_BTDM`**: ESP32-S3 has a separate BLE controller (not combined BTDM like ESP32). `WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BLE` is the correct handler.
- **GPIO 46 boot strapping pin**: AMBIT1 TX uses GPIO 46, which is a strapping pin. Series resistor added to prevent sensor from interfering with boot.
- **OTA partition reserved**: 3MB OTA slot in partition table for future firmware updates.
- **MP2731 register values documented**: Init comments specify target battery spec. Values confirmed correct (board config change is cosmetic, same physical circuit).
- **I2C bus recovery**: `i2c_bus_check_and_recover()` performs SCL clock-stretching recovery (9 toggles) if SDA stuck low, then full driver reinit if still stuck. Called before sensor init and as retry path on `ESP_ERR_TIMEOUT`. `i2c_bus_probe_addr()` does raw address probe (START → addr+W → STOP) before full sensor init — avoids register-level init on absent hardware.
- **LittleFS as write-ahead buffer**: Measurement records are appended to `/littlefs/pending.bin` immediately (always available, power-loss safe). Background flush task drains LittleFS → SQLite on SD card every 5s. If SD card is absent, data accumulates safely in LittleFS (~28 hours at typical rates). LittleFS mount failure is critical — persistence unavailable without it.
- **SQLite corruption recovery**: `PRAGMA integrity_check;` at startup. If damaged: rename `.db`/`.db-wal`/`.db-shm` to `.corrupt` → recreate fresh DB → log to NVS. Trades data loss for continued operation. Pending records in LittleFS are not affected.
- **Task stack sizes**: Lua task 8192 bytes (SQLite + Lua call chain requires it), CLI 4096 (esp_console + cmd_*), Flush 6144 (SQLite + FAT VFS + SDMMC call chain), UART 4096. Validated via `uxTaskGetStackHighWaterMark()`.
- **Heap monitoring**: `esp_get_free_heap_size()` logged after each init block; `esp_get_minimum_free_heap_size()` logged every 60s. Alert at <32KB.
- **Hardware inventory at boot**: Single structured log line after all inits (`[BOOT] I2C=OK BME280=OK RTC=OK SD=ABSENT LFS=OK`). `hw_inventory_t` struct accessible via `device.status()`.
- **SD card mount retry**: Retry once after 500ms on mount failure (warm-up). `sd_card_is_available()` public API. If SD unavailable, new measurements continue accumulating in LittleFS pending buffer. Queries return `ESP_ERR_NOT_SUPPORTED` until SD card is available.
- **UART sensor task not WDT-registered**: Designed to block up to 2 minutes; WDT monitoring counterproductive. Caller detects hangs via response timeout.
- **UART auto-ping at boot**: Each configured channel pinged (2s timeout) at startup; results in hardware inventory.
- **Data sync via `measurements.syncState` (three-state enum)**: `MEASUREMENT_SYNC_PENDING=0`, `MEASUREMENT_SYNC_INFLIGHT=1`, `MEASUREMENT_SYNC_SYNCED=2`. Rows start as `PENDING`, move to `INFLIGHT` when a publish is successfully queued, and become `SYNCED` only after `MQTT_EVENT_PUBLISHED`. `device_commands` keeps a single in-flight publish slot (`s_inflight_msg_id` ↔ `s_inflight_measure_id`); raw `cmd_mqtt_publish` calls never occupy this slot, and unknown `msg_id` acks are ignored. Durable state lives in SQLite. On disconnect or reboot recovery, stale `INFLIGHT` rows are reset to `PENDING`. This is the basis for at-least-once delivery without requeue storms.
- **Measurement-oriented MQTT commands are fixed-contract**: `cmd_mqtt_publish_measurement()` and `cmd_mqtt_publish_unsynced()` derive topics internally from `CONFIG_AMBYTE_MQTT_TOPIC_ROOT`, `CONFIG_AMBYTE_DEVICE_ID`, `measure_type`, and `measure_id`. Only `cmd_mqtt_publish(topic, payload)` remains free-form.
- **SD card text-file functions removed in Phase 3C**: Deferred until after both Lua (3B) and CLI (3C) are rewired to `device_commands`. Prevents breakage of `sd.append()` Lua binding during transition.
- **Phase 6A uses a compile-time MQTT config carrier**: `CONFIG_AMBYTE_DEVICE_ID`, `CONFIG_AMBYTE_MQTT_URI`, `CONFIG_AMBYTE_MQTT_CLIENT_ID`, and `CONFIG_AMBYTE_MQTT_TOPIC_ROOT` define the entire 6A MQTT identity/config surface. Runtime `device_config` is deferred to Phase 6B.
- **`app_main` owns the MQTT supervisor**: It subscribes directly to `IP_EVENT_STA_GOT_IP` and `WIFI_EVENT_STA_DISCONNECTED`, but those handlers only notify a dedicated supervisor task. The supervisor task alone initializes, starts, and stops MQTT, and it also reverts stale in-flight publishes when Wi-Fi drops.
- **Sensor liveness via Lua scripts**: `device.read_env()` returns nil+error on I2C failure. Periodic health checks are a Lua script concern, not additional C code.
- **sdkconfig safety for field deployment**: `CONFIG_ESP_TASK_WDT_PANIC=y` (reboot on hang), `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y` (post-mortem), `CONFIG_HEAP_POISONING_LIGHT=y` (corruption detection), `CONFIG_COMPILER_STACK_CHECK_MODE_NORM=y` (stack instrumentation). 64KB coredump partition in `partitions.csv`.
- **CLI uses `esp_console` with the built-in REPL**: Commands are registered via `esp_console_cmd_register()`, and shell lifecycle is handled by `esp_console_new_repl_*()` plus `esp_console_start_repl()`. This preserves line editing, history, and tab completion while keeping each command handler as a direct `cmd_*()` call.
- **Device configuration stored in NVS via `device_config`**: MQTT broker config and later AMBIT channel sensor names live in NVS namespace `"device_cfg"` (internal flash), not in SQLite on SD card. This survives SD card removal. The custom BLE endpoint that writes these values is deferred to Phase 6B. Phase 6A does not consult `device_config`.
- **One MQTT payload per logical measurement**: Telemetry is serialized per `measure_id`, not per row. Each payload contains one logical measurement group with an array of `{ data_type, value }`.
- **Inbound MQTT payload reassembly lives in `mqtt_client`**: ESP-MQTT may emit multiple `MQTT_EVENT_DATA` chunks for one message. `mqtt_client` reassembles them before calling higher-level handlers in Phase 6B.
- **cmd_result_t message[256]**: Human-readable result or error text. Used by both CLI (prints directly) and Lua (returns as string).

---

## Files to Delete (Phase 0)

- `src/` — empty directory
- `dom_ludo_prototype_ambyte_thing_certs/` — leftover cert archive
- `components/I2C_device/` — entire component (after BME280 refactored in 0B)
- `main/app_main.c` lines ~31–85 — dead `sensor_report_task()` function
- `components/bme280/bme280_adafruit_private.hpp` — `BME280_ENABLE_ADAFRUIT_SENSOR_API` ifdef blocks deleted (0B)

Note: PCF2131 RTC files are **NOT deleted** — the class hierarchy is preserved (Phase 0C only moves `RTC_NXP.h` to source directory).

---

## Phase 8: MP2731 Battery Charger I2C Driver

Port user's Arduino-based MP2731 driver to pure C ESP-IDF component using the shared `i2c_bus` kernel, following the PCF2131 RTC driver pattern.

### Phase 8A: Create `components/mp2731/` (no dependencies on Phases 1–7)

8A.1. Create `components/mp2731/include/mp2731.h` — public C API:
  - `#define MP2731_ADDR 0x4B`, register defines REG00–REG17
  - Structs: `mp2731_status_t`, `mp2731_fault_t`, `mp2731_adc_t`, `mp2731_data_t`
  - Functions: `mp2731_init()`, `mp2731_is_ready()`, `mp2731_read_status()`, `mp2731_read_fault()`, `mp2731_run_adc()`, `mp2731_read_adc()`, `mp2731_read_all()`, `mp2731_get_sensors_averaged()`, `mp2731_set_vin_limit()`, `mp2731_set_charge_current()`, `mp2731_read_register()`, `mp2731_dump_registers()` — all return `esp_err_t`

8A.2. Create `components/mp2731/mp2731.c` — implementation:
  - Static I2C helpers: `mp2731_i2c_read_reg()` / `mp2731_i2c_write_reg()` using i2c_bus_lock → cmd link → i2c_master_cmd_begin → unlock (same as PCF2131_I2C.cpp `_reg_r`/`_reg_w`)
  - Port Arduino functions: Wire → i2c_bus, delay → vTaskDelay, Serial.printf → ESP_LOGI
  - Fix bugs: `mp2731_adcInputCurrent()` → REG13 (not REG11); `mp2731_ICC()` → write `setting` to REG05 (not `data`)
  - Static `s_mp2731_ready` flag
  - Add comments in `mp2731_init()` documenting target battery spec: "Configured for single-cell Li-Ion 4.2V, ICC=640mA, IIN_LIM=current setting, VIN_MIN=4.3V" — same physical battery/charger circuit, board config change is cosmetic

8A.3. Create `components/mp2731/CMakeLists.txt`: `PRIV_REQUIRES driver i2c_bus`

8A.4. Wire `mp2731_init()` in `main/app_main.c` after `i2c_bus_init()`

### Phase 8B: Domain Integration (depends on Phases 1, 3)

8B.1. Add `power_port.h` to `components/domain/include/` — `power_adc_t`, `power_read_fn`
8B.2. Add `cmd_read_battery()`, `cmd_set_charge_current()`, `cmd_set_vin_limit()` to `device_commands`
8B.3. Add Lua bindings: `device.battery()`, `device.set_charge_current(ma)`, `device.set_vin_limit(v)`

### Verification
- `pio run` succeeds
- `mp2731_dump_registers()` prints 24 register values
- `mp2731_read_all()` returns plausible ADC values
- Interleaved I2C reads (MP2731 + BME280 + RTC) cause no bus corruption

### Relevant files
- [components/hal/i2c_bus/include/i2c_bus.h](components/hal/i2c_bus/include/i2c_bus.h) — shared kernel API
- [components/pcf2131tfy_rtc/PCF2131_I2C.cpp](components/pcf2131tfy_rtc/PCF2131_I2C.cpp) — I2C pattern template (`_reg_r`/`_reg_w`)
- New: `components/mp2731/mp2731.c`, `components/mp2731/include/mp2731.h`, `components/mp2731/CMakeLists.txt`
