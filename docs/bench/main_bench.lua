-- Accelerated root-cause repro: continuously create realistic ~1.2 KB events.
-- Copy this file to /sdcard/main.lua on the bench unit.

local RATE_HZ = 10                 -- fallback correlation run: change to 2
local REPORT_EVERY = 100           -- console progress cadence, in stored events

assert(RATE_HZ > 0 and RATE_HZ <= 100, "RATE_HZ must be in (0, 100]")
local PERIOD_MS = math.floor(1000 / RATE_HZ)

-- A compact stand-in for a real ADC trace. Together with the scalar readings
-- and metadata this serializes to approximately 1.2 KB per event.
local ADC_CHUNK = "1842,1847,1851,1859,1866,1870,1868,1861,1854,1848,1844,1841,"
local ADC_TRACE = string.rep(ADC_CHUNK, 15)

local stored = 0
local failures = 0
local first_id
local last_id
local next_due_ms = device.uptime_ms()

device.log(string.format(
    "BENCH repro started rate=%d/s period=%dms payload_trace_bytes=%d",
    RATE_HZ, PERIOD_MS, #ADC_TRACE))

while true do
    local seq = stored + failures + 1
    local measure_id, err = db.store_event{
        data = {
            protocol = "ambit_bench_trace_v1",
            sequence = seq,
            leaf_temp_c = 23.71 + (seq % 17) * 0.01,
            air_temp_c = 22.48 + (seq % 13) * 0.01,
            relative_humidity_pct = 61.2 + (seq % 9) * 0.1,
            par_umol_m2_s = 418 + (seq % 31),
            fluorescence = { f0 = 812, fm = 2364, fv_fm = 0.656 },
            adc_trace = ADC_TRACE,
        },
        metadata = {
            source = "ticket_07_root_cause_bench",
            rate_hz = RATE_HZ,
            payload_profile = "realistic_1k2",
        },
        channel = 0,
    }

    if measure_id then
        stored = stored + 1
        first_id = first_id or measure_id
        last_id = measure_id
        if stored % REPORT_EVERY == 0 then
            device.log(string.format(
                "BENCH stored=%d failures=%d measure_id=%d delta=%d",
                stored, failures, last_id, last_id - first_id))
        end
    else
        failures = failures + 1
        device.log(string.format("BENCH store failed count=%d: %s",
                                 failures, tostring(err)))
    end

    -- Deadline-based pacing avoids accumulating db.store_event execution time.
    next_due_ms = next_due_ms + PERIOD_MS
    local now_ms = device.uptime_ms()
    local sleep_ms = next_due_ms - now_ms
    if sleep_ms > 0 then
        device.sleep_ms(sleep_ms)
    elseif sleep_ms < -PERIOD_MS * 10 then
        -- If SD stalls for over ten periods, resume from now instead of bursting.
        next_due_ms = now_ms
    end
end
