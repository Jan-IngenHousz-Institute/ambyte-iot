# Ambyte IoT — Architecture Layer Rules

## Dependency Layers (enforced via CMakeLists.txt REQUIRES)

```
┌─────────────────────────────────────────────────┐
│  main/app_main.c  (composition root)            │
│  REQUIRES: everything — wires ports to adapters │
├─────────────────────────────────────────────────┤
│  Adapters: CLI, sched_runner                    │
│  REQUIRES: device_commands (+ i2c_bus for CLI,  │
│            sched_spec for sched_runner)         │
│  MUST NOT depend on: bme280, pcf2131tfy_rtc,    │
│            ambyte_status, persistence, sd_card  │
├─────────────────────────────────────────────────┤
│  device_commands/  (shared command layer)       │
│  REQUIRES: domain only                          │
│  MUST NOT depend on any infrastructure component│
├─────────────────────────────────────────────────┤
│  Infrastructure: bme280, pcf2131tfy_rtc,        │
│  ambyte_status, persistence, sd_card, i2c_bus   │
│  REQUIRES: domain + their HW drivers            │
│  MUST NOT depend on device_commands or adapters │
├─────────────────────────────────────────────────┤
│  domain/  (port typedefs only, no .c files)     │
│  REQUIRES: esp_common only                      │
│  MUST NOT depend on ANY other project component │
└─────────────────────────────────────────────────┘
```

## Rules

1. **domain/** is header-only. It defines port typedefs (`sensor_read_fn`, `clock_read_fn`, `measurement_store_fn`, etc.) and value types (`measurement_t`, `measurement_record_t`). No implementation files. REQUIRES only `esp_common`.

2. **device_commands/** implements the `cmd_*()` API using domain port function pointers received via `device_commands_init()`. It never includes infrastructure headers directly. REQUIRES only `domain`.

3. **Infrastructure components** (bme280, pcf2131tfy_rtc, ambyte_status, persistence, sd_card) implement domain ports. They depend on `domain` for the port typedefs and on their hardware drivers (e.g., `i2c_bus`, `driver`, `fatfs`). They must not depend on `device_commands`, `CLI`, or `sched_runner`.

4. **Adapter components** (CLI, sched_runner) depend on `device_commands` to call `cmd_*()` functions. They must not include any infrastructure component header directly. Exception: CLI uses `i2c_bus` for native `i2cscan` command; sched_runner uses the compiled schedule model and fused AMBIT trace adapter.

5. **main/app_main.c** is the composition root. It is the only place that depends on all components, creates port adapter function pointers from infrastructure, and passes them to `device_commands_init()`.

## Verification

Adding a forbidden dependency (e.g., `domain REQUIRES bme280` or `device_commands REQUIRES persistence`) must be caught in code review. The build system does not enforce layering direction — only code review does.
