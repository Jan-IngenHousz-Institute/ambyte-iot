-- legacy_1Hz_spec.lua — 1 Hz field acquisition on the *legacy* spec command.
--
-- Same duty cycle as 1Hz_spec.lua: one spectrum per second from one hour
-- before sunrise to one hour after sunset, one every ten minutes overnight.
-- The difference is the opcode: this script uses the ordinary spectrum read
-- that every Ambit has always had, not the new cmd 35 raw-spectrum call.
--
-- OPCODE NOTE — the legacy spec command is **cmd 31** (AMBIT_CMD_GET_SPEC,
-- ASCII "get_par"), not 34. Cmd 34 is AMBIT_CMD_GET_TEMP_RAW, the extended
-- temperature/MLX-register diagnostic; it returns 14 bytes of temperature and
-- no spectrum at all. See components/device_commands/include/ambit_protocol.h.
-- Nothing here writes the opcode by hand anyway: `ambit.spec()` is the binding
-- for cmd 31, and it is the whole reason this script is shorter than its cmd 35
-- sibling.
--
-- What cmd 31 buys over cmd 35, and what it costs:
--   + universal — no firmware gate. Cmd 31 predates every image in the fleet,
--     so the try_gate()/ambit-fw>=1.2.0 dance in 1Hz_spec.lua is simply gone.
--   + self-storing — `ambit.spec()` is a *fused* call: it queries, decodes and
--     appends one MEASUREMENT event with the correct schema, device name and
--     config metadata. No CMD table, no string.unpack format, no decode(), no
--     db.store_event for samples. That is ~70 lines of the cmd 35 script that
--     this file does not need to contain, and cannot get wrong.
--   - 10 counts + PAR only. Cmd 31 reports no atime/astep/gain/flags, so the
--     exposure that produced a reading is not recoverable from the sample.
--     If the experiment needs exposure provenance, it needs cmd 35.
--
-- This is an opt-in released fleet script. It is packaged beside `main.lua`
-- under the same lua-vX.Y.Z release and deployed deliberately to a selected
-- subset of Ambytes as their /sdcard/main.lua. Those devices therefore:
--   * stop producing SS / MPF / arrun traces — a 1 Hz cadence has no room for
--     one, and this script runs nothing else;
--   * report this asset's versioned SHA from legacy_1Hz_spec.lua.manifest.json,
--     distinct from the default main.lua asset in the same release.
--
-- Topology assumed: ONE Ambit on one Ambyte. Cmd 31 is synchronous and holds
-- the channel for the whole wake→command→response exchange, so a second board
-- would serialise behind the first and break the cadence. This script only ever
-- touches CFG.channel; it does not sweep channels 0-3 the way main.lua does.

local CFG = {
    channel     = 0,            -- Ambit UART channel, 0-3 (the only one used)
    deployment  = "CHANGE_ME",  -- site/experiment tag, stamped on the status event

    -- Solar geometry. time_sync's compiled default is volatile and not held in
    -- NVS, so a site away from the compiled default must set it here. Left at
    -- 0,0 the call is SKIPPED, not sent: writing 0,0 would move the sun path to
    -- the Gulf of Guinea, which is worse than the firmware default. There is no
    -- tz knob: every sync.* call re-applies the device's IANA zone offset
    -- (timezone_localize → time_sync_set_utc_offset_seconds), so anything a
    -- script sets here is overwritten before the first sunrise is computed.
    lat         = 0.0,
    lon         = 0.0,

    fast_ms     = 1000,         -- cadence inside the solar window
    slow_ms     = 600000,       -- cadence outside it
    sunrise_lead_s = -3600,     -- fast phase starts at sunrise - 1 h
    sunset_trail_s =  3600,     -- ... and ends at sunset + 1 h

    solar_check_every_ms = 60000,
    status_every_ms      = 3600000,

    -- Failure-log throttle: log the first failure of a streak, then one line
    -- per this many, then the recovery. device.log is ESP_LOGI (console only),
    -- so this is about a readable console, not the card.
    log_every_fail = 300,
}

-- ── why 1 Hz is safe here, and what protects it ──────────────────────────
--
-- The success path is cheap: wake-ack, 9 bytes out, 24 bytes back at 115200,
-- then an SD append — tens of milliseconds against a 1000 ms budget. With one
-- Ambit there is no channel contention, so the fast phase has ample margin.
--
-- The danger is the FAILURE path. `ambit.spec()` takes no timeout argument:
-- cmd_ambit_get_spec() hardcodes 5000 ms, and a wake that never acks burns
-- 25 retries × 50 ms before the deadline stops it. One call to a missing or
-- wedged Ambit therefore blocks this task for up to five seconds and eats five
-- 1 Hz slots. That is precisely why 1Hz_spec.lua carries its own circuit
-- breaker with a bounded ambit.query() timeout.
--
-- Here the breaker is `ambit.ping()`, and it is the firmware's, not ours:
-- do_ping() caches its verdict — 10 s when the sensor answered, and **5 min
-- when it did not** (PING_FAIL_CACHE_TTL_US, added for exactly this reason).
-- So gating each acquisition on a ping means:
--   * present Ambit  → 9 of every 10 pings are a cached bool, ~free at 1 Hz;
--   * absent Ambit   → one real 1.25 s wake burst per five minutes, and the
--                      5 s spec stall is never entered at all;
--   * dies mid-window→ at worst ~10 s of stalls until the OK cache expires and
--                      the negative cache takes over.
-- It also suppresses the log flood: an unplugged Ambit polled at 1 Hz would
-- otherwise write a firmware WARN ("wake failed") per attempt, and sd_logger
-- tees WARN to /sdcard/logs — ~86 k lines a day, the 2026-07 536 K-line flood.
-- The ping guard is both the cadence protection and the flood fix, which is why
-- this script has no fail_backoff_after/backoff_ms of its own: reimplementing
-- in Lua a backoff the firmware already applies would only make it slower.

-- ── uptime arithmetic (device.uptime_ms wraps at 2^32 ≈ 49.7 days) ────────
-- A field unit runs past the wrap, so every comparison goes through these.

local U32, I32_HALF = 4294967296, 2147483648

local function u32(v) return v % U32 end

local function delta_ms(lhs, rhs)
    local d = (lhs - rhs) % U32
    if d >= I32_HALF then d = d - U32 end
    return d
end

-- device.uptime_ms() is esp_log_timestamp(), already a uint32, so no masking.
local function now_ms() return device.uptime_ms() end

local function log(fmt, ...) device.log("LSPEC " .. string.format(fmt, ...)) end

-- ── state ────────────────────────────────────────────────────────────────

local C = { seq = 0, spec_err = 0, absent = 0, missed = 0, fail_run = 0 }

-- Throttled failure logging: every failure is counted, only a few are printed.
-- `fail_run` is the current consecutive-failure streak, so the log shows the
-- onset, a periodic reminder while it persists, and the recovery.
local function log_fail(fmt, ...)
    C.fail_run = C.fail_run + 1
    if C.fail_run == 1 or C.fail_run % CFG.log_every_fail == 0 then
        log("%s (consecutive=%d)", string.format(fmt, ...), C.fail_run)
    end
end

local function log_ok()
    if C.fail_run > 0 then
        log("recovered after %d consecutive failures", C.fail_run)
        C.fail_run = 0
    end
end

local boot_epoch = device.read_rtc() or 0   -- makes `seq` orderable across reboots
local phase = nil                           -- resolved on the first solar check
local period_ms = CFG.slow_ms               -- fail closed until then
local next_due_ms = now_ms()
local next_solar_ms = now_ms()
local next_status_ms = u32(now_ms() + CFG.status_every_ms)

-- ── solar phase ──────────────────────────────────────────────────────────

local function check_solar()
    next_solar_ms = u32(now_ms() + CFG.solar_check_every_ms)
    local to_fast = sync.until_sun("sunrise", CFG.sunrise_lead_s)
    local to_slow = sync.until_sun("sunset", CFG.sunset_trail_s)
    -- Whichever edge is nearer tells us which side of the window we are on: if
    -- the closing edge arrives first we are already inside it. A missing sun
    -- event (polar, no fix) fails closed to the slow cadence rather than
    -- burning the link and the SD card at 1 Hz all night.
    local want = "slow"
    if type(to_fast) == "number" and type(to_slow) == "number" then
        want = (to_slow < to_fast) and "fast" or "slow"
    end
    if want ~= phase then
        phase = want
        period_ms = (phase == "fast") and CFG.fast_ms or CFG.slow_ms
        next_due_ms = now_ms()        -- sample immediately on entering a phase
        log("phase=%s period_ms=%d", phase, period_ms)
    end
end

-- ── acquisition ──────────────────────────────────────────────────────────
-- One sample = one event, appended by `ambit.spec()` itself. This script never
-- publishes and never formats a payload; the firmware publisher drains the SD
-- event log under its own power and connectivity gates.

local function acquire()
    -- Cheap and mostly cached; see the 1 Hz note above. Skipping on a negative
    -- ping is what keeps a missing Ambit from stalling the deadline grid.
    if not ambit.ping(CFG.channel) then
        C.absent = C.absent + 1
        log_fail("ambit ch%d not answering", CFG.channel)
        return
    end
    local s, err = ambit.spec(CFG.channel)
    if not s then
        C.spec_err = C.spec_err + 1
        log_fail("spec failed: %s", tostring(err))
        return
    end
    -- `id` is the measure_id of the stored event. Absent means the acquisition
    -- worked but the SD append did not, which is data loss and must not read as
    -- a success — the firmware has already logged the store failure.
    if not s.id then
        C.spec_err = C.spec_err + 1
        log_fail("spec stored no event (sd?)")
        return
    end
    -- Counted only once durable, so stored sequences stay contiguous within a
    -- boot session and a gap always means data loss.
    C.seq = C.seq + 1
    log_ok()
end

-- ── status ───────────────────────────────────────────────────────────────
-- The fused sample store has no room for experiment provenance, so the hourly
-- status event carries the deployment tag and the counters for the whole hour.

local function store_status()
    next_status_ms = u32(now_ms() + CFG.status_every_ms)
    local loc = sync.location()
    db.store_event{
        channel = CFG.channel,
        data = {
            kind = "legacy_spec_status",
            dep = CFG.deployment,
            boot = boot_epoch,
            up_ms = now_ms(),
            phase = phase or "unknown",
            seq = C.seq,
            spec_err = C.spec_err,
            absent = C.absent,
            missed = C.missed,
            -- Current consecutive-failure streak: the console log is throttled,
            -- so this is how a stuck channel reaches the platform.
            fail_run = C.fail_run,
            sd = device.sd_ready() and 1 or 0,
        },
        metadata = {
            script = "legacy_1Hz_spec",
            cmd = "get_par",        -- cmd 31, the legacy spectrum read
            -- sync.location() always answers; these are the values the sun path
            -- actually used, not the ones CFG asked for.
            lat = loc.lat, lon = loc.lon, tz = loc.tz,
        },
    }
    log("status seq=%d spec_err=%d absent=%d missed=%d fail_run=%d",
        C.seq, C.spec_err, C.absent, C.missed, C.fail_run)
end

-- ── boot ─────────────────────────────────────────────────────────────────

do
    -- Unset (0,0) means "keep whatever the firmware was built with" — see CFG.
    if CFG.lat ~= 0.0 or CFG.lon ~= 0.0 then
        sync.set_location(CFG.lat, CFG.lon)
    end
    -- sun_today() first: it goes through sync_now(), which re-applies the exact
    -- (DST-aware, possibly half-hour) zone offset that set_location's hours-only
    -- argument would otherwise have truncated.
    local sunrise, sunset = sync.sun_today()
    local loc = sync.location()
    log("start dep=%s ch=%d boot=%d lat=%.4f lon=%.4f sunrise=%s sunset=%s",
        CFG.deployment, CFG.channel, boot_epoch, loc.lat, loc.lon,
        tostring(sunrise), tostring(sunset))
    -- Warn, never refuse: an unconfigured tag makes the data hard to attribute,
    -- but a unit that will not measure is worse and needs a site visit.
    if CFG.deployment == "CHANGE_ME" then
        log("WARNING deployment tag not set; samples are unattributed")
    end
    if CFG.lat == 0.0 and CFG.lon == 0.0 then
        log("WARNING location not set; using the firmware default above")
    end
end
check_solar()

-- ── main loop ────────────────────────────────────────────────────────────
-- An absolute deadline grid, not a sleep-per-cycle: blocking time is never
-- added to the next deadline, and a deadline already in the past is counted
-- and skipped rather than fired as a catch-up burst.

while true do
    if delta_ms(now_ms(), next_solar_ms) >= 0 then check_solar() end
    if delta_ms(now_ms(), next_status_ms) >= 0 then store_status() end

    local wait_ms = delta_ms(next_due_ms, now_ms())
    if wait_ms > 0 then
        -- Wake for whichever housekeeping deadline lands first, so a ten-minute
        -- night cadence still checks the sun and reports status on time.
        -- Written out rather than looped over a table: this runs every cycle
        -- for months, and the loop should not allocate.
        local d = delta_ms(next_solar_ms, now_ms())
        if d > 0 and d < wait_ms then wait_ms = d end
        d = delta_ms(next_status_ms, now_ms())
        if d > 0 and d < wait_ms then wait_ms = d end
        device.sleep_ms(wait_ms)
    else
        local due = next_due_ms
        acquire()
        next_due_ms = u32(due + period_ms)
        -- A slot lost to a 5 s stall (or an SD hiccup) is counted and skipped,
        -- never replayed as a burst: the grid stays aligned to real time.
        local behind = delta_ms(now_ms(), next_due_ms)
        if behind > 0 then
            local skipped = math.floor(behind / period_ms) + 1
            C.missed = C.missed + skipped
            next_due_ms = u32(next_due_ms + skipped * period_ms)
        end
    end
end
