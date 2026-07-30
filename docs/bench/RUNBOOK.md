# Ticket 07 root-cause bench runbook

This kit drives one stored QoS1 event per `measure_id` at 10 messages/s while a
low-priority task records network and memory diagnostics once per minute. Run it
on one desk unit against the normal development endpoint or an isolated local
broker. Do not use a production device identity against a shared broker.

## Prepare and flash

1. Update submodules if this checkout has not built before:

   ```sh
   git submodule update --init --recursive components/littlefs
   ```

2. Put the accelerated script at the root of the unit's SD card, named exactly
   `main.lua`:

   ```sh
   cp docs/bench/main_bench.lua /path/to/mounted-sd/main.lua
   ```

   Eject the card cleanly, insert it in the powered-off unit, then power the unit.
   Existing `/sdcard/events` data changes the starting point and time-to-drain;
   archive and clear it before the first run if it is not part of the test.

3. Build, flash, and monitor the instrumented baseline:

   ```sh
   pio run -e bench -t upload
   pio device monitor -b 115200
   ```

4. Confirm these boot/runtime markers before timing a run:

   - `BENCH diagnostics enabled: interval=60s`
   - `BENCH repro started rate=10/s`
   - `BENCH sample begin` once per minute
   - MQTT connects and PUBACK progress begins

Keep the serial monitor captured to a file. The firmware SD logger retains the
WARN summary lines under `/sdcard/logs/`, but the full raw `stats_display()` and
Wi-Fi driver text dumps are console output; a serial capture is the complete
record.

## What to record every minute

Each diagnostic frame starts with `BENCH sample begin` and ends with
`BENCH wifi_buffer_dump=... sample end`. Copy the complete frame and track:

| Area | Counters to plot | Warning trend |
|---|---|---|
| Internal heap | `heap=INTERNAL free`, `largest`, `min_free` | free or largest steadily falls |
| DMA heap | `heap=DMA free`, `largest`, `min_free` | free or largest steadily falls |
| PSRAM | `heap=SPIRAM free`, `largest`, `min_free` | confirms whether allocations migrate to PSRAM |
| lwIP sockets | `sockets_open` versus `sockets_max` | open count rises and does not return after reconnect |
| lwIP | TCP/link `drop`, `memerr`, `err`; SYS `sem`, `mbox`; full `stats_display()` | used approaches its limit, or allocation/error counters rise at the stall |
| Wi-Fi driver | the `WIFI_STATIS_BUFFER` text between the frame markers | a free/available TX or RX buffer count monotonically approaches zero |

IDF 5.5 has a public `esp_wifi_statis_dump(WIFI_STATIS_BUFFER)` API, but it only
prints text and provides no public structured buffer-stat getter. Preserve the
serial log verbatim so the Wi-Fi lines remain available for comparison.

## Message count and expected failure

`db.store_event` allocates one monotonic `measure_id` per event. The script logs
progress every 100 successful stores. Use `last measure_id - first measure_id + 1`
for the generated message count; also record the publisher's acknowledged-id
progress (STATUS `last_acked_id`, downstream data, or the relevant publish/PUBACK
logs). Count the messages that actually reached the broker when assigning the
failure threshold; stored backlog alone is not proof of TX work.

The known signature is:

- after roughly 120,000-150,000 published messages, TCP writes stop making
  progress and a network poll times out with `errno=0`;
- MQTT disconnect/reconnect attempts may continue and new TLS connections fail;
- Wi-Fi remains associated and heap telemetry may still look healthy;
- stored `measure_id` continues advancing while acknowledged/downstream progress
  stops.

Stop a successful run only after it passes 150,000 broker-observed messages with
no sustained publish gap. Save serial and SD logs with environment name, start
and end `measure_id`, acknowledged count, elapsed time, and outcome.

## A/B/C config bisect

Use the same unit, SD card, endpoint, payload, rate, credentials, and starting
backlog. Power-cycle between runs. Run the baseline twice, then each single-variable
variant twice. Never combine variants in one result.

| Run | PlatformIO environment | Only change from baseline | Confirmation |
|---|---|---|---|
| Baseline | `bench` | none | `LWIP_STATS=y`, diagnostics marker present |
| A | `bench-spiram-internal` | `SPIRAM_TRY_ALLOCATE_WIFI_LWIP=n` | inspect generated `sdkconfig.bench-spiram-internal` |
| B | `bench-wifi-ps-none` | runtime `WIFI_PS_MIN_MODEM` -> `WIFI_PS_NONE` | boot log says `Wi-Fi power save override: WIFI_PS_NONE (ESP_OK)` |
| C | `bench-pm-none` | `CONFIG_PM_ENABLE=n` (DFS off) | inspect generated `sdkconfig.bench-pm-none`; app PM setup reports unsupported/disabled |

For each row, substitute the environment name in:

```sh
pio run -e ENVIRONMENT -t upload
pio device monitor -b 115200
```

A variant names the trigger only when both of its runs pass 150,000+ published
messages cleanly and both baseline runs reproduce the stall. A later failure is
not a pass; record its count and counter state.

## Finding "the counter that walks to zero"

Compare the final healthy frames with the stall frame. "Walks to zero" means a
resource's available capacity falls monotonically across samples and does not
recover after MQTT reconnects:

- Wi-Fi subsystem: a free/available TX or RX buffer counter in
  `WIFI_STATIS_BUFFER` reaches zero while association remains up.
- lwIP subsystem: socket headroom (`sockets_max - sockets_open`) reaches zero, or
  a full lwIP dump shows exhausted capacity accompanied by `memerr`/`err` growth.
  ESP-IDF's lwIP configuration allocates several objects from heap rather than
  fixed `memp` pools, so heap-capability loss can be the corresponding signal.
- Internal/DMA heap subsystem: `free` or especially `largest` approaches the
  allocation size required by the network path while PSRAM stays healthy.

A cumulative traffic counter increasing is not depletion. Name the resource only
when its available/headroom counter decreases toward zero at stall onset and the
two reproductions show the same behavior.

## Count-correlation fallback

If the 10/s run fails, edit only the first setting in `main_bench.lua` to
`local RATE_HZ = 2`, replace `/sdcard/main.lua`, and repeat the baseline twice.
At 2/s, 150,000 messages take about 20.8 hours instead of about 4.2 hours. Failure
near the same published-message count supports traffic/work correlation; failure
after similar wall time instead supports a time/uptime mechanism. If 10/s cannot
sustain publishing and only grows the SD backlog, lower the rate until broker
throughput is continuous and report the actual rate.
