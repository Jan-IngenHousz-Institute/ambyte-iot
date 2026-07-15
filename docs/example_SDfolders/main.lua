
local NUM_CHANNELS = 4                              -- AMBIT channels to scan (0..N-1)

-- ── Protocols (segment tables passed to ambit.run) ────────────────────────
local SS = {                                        -- steady-state probe (1 line)
    { pulses = 40, freq =1, actinic = 0 },
}

local MPF = {                                       -- multi-phase saturating flash
    { pulses = 40, freq = 10,  actinic = 0    },    -- dark baseline
    { pulses = 70, freq = 100, actinic = -250 },    -- saturating phases (raw DAC)
    { pulses = 10, freq = 100, actinic = -200 },
    { pulses = 10, freq = 100, actinic = -160 },
    { pulses = 10, freq = 100, actinic = -250 },
    { pulses = 40, freq = 10,  actinic = 0    },    -- relaxation
}

-- ── Helpers ───────────────────────────────────────────────────────────────

local function record_spectra()
    local stored = 0
    for ch = 0, NUM_CHANNELS - 1 do
        if ambit.ping(ch) then
            local s, err = ambit.spec(ch)
            if s then
                stored = stored + (s.id and 1 or 0)
                device.log(string.format("spectra ch%d: PAR=%.2f%s",
                           ch, s.par, s.id and (" id=" .. s.id) or " store failed"))
            else
                device.log(string.format("spectra ch%d: read failed: %s", ch, err or "?"))
            end
        end
    end
    if stored == 0 then device.log("spectra: no AMBIT responded") end
end

-- ── Parallel run tuning ─────────────────────────────────────────────────────
local POLL_INTERVAL_MS    = 500     -- gap between poll sweeps
local POLL_START_FRAC     = 0.9     -- don't poll a channel until 90% of its estimate elapsed
local DEADLINE_MARGIN_MS  = 15000   -- est + this without a result ⇒ ambit considered broken
local SEG_OVERHEAD_MS     = 300     -- per-segment config/light-sleep slack in the estimate

-- Approximate a trace's run time (ms): each segment ≈ pulses/freq seconds, plus a
-- fixed per-segment overhead. Used to schedule polling and bound a broken ambit.
local function estimate_ms(trace)
    local total = 0
    for _, seg in ipairs(trace) do
        local pulses = seg.pulses or seg[1] or 0
        local freq   = seg.freq   or seg[2] or 1
        if freq < 1 then freq = 1 end
        total = total + (pulses / freq) * 1000 + SEG_OVERHEAD_MS
    end
    return math.floor(total)
end


local function run_trace(tag, trace, hold_window)
    if hold_window then device.measurement_window(true) end

    local est     = estimate_ms(trace)
    local pending = {}                 -- ch -> t0 (uptime_ms at trigger)
    local count   = 0
    for ch = 0, NUM_CHANNELS - 1 do
        if ambit.ping(ch) then
            local ok, err = ambit.trigger(ch, trace, { interrupt = false })
            if ok then
                pending[ch] = device.uptime_ms()
                count = count + 1
            else
                device.log(string.format("%s ch%d: trigger failed: %s", tag, ch, err or "?"))
            end
        end
    end

    
    while count > 0 do
        device.sleep_ms(POLL_INTERVAL_MS)
        local now = device.uptime_ms()
        for ch = 0, NUM_CHANNELS - 1 do
            local t0 = pending[ch]
            if t0 then
                local elapsed = now - t0
                if elapsed >= est * POLL_START_FRAC then
                    local st = ambit.poll(ch)
                    if st == "done" then
                        local r, err = ambit.fetch(ch, { store = true })
                        if r then
                            device.log(string.format("%s ch%d: %d points, %.1fC, stored %d",
                                       tag, ch, r.points, r.leaf_temp or 0, r.stored or 0))
                        else
                            device.log(string.format("%s ch%d: fetch failed: %s", tag, ch, err or "?"))
                        end
                        pending[ch] = nil; count = count - 1
                    elseif st == "error" then
                        device.log(string.format("%s ch%d: ambit reported run error", tag, ch))
                        pending[ch] = nil; count = count - 1
                    elseif elapsed > est + DEADLINE_MARGIN_MS then
                        device.log(string.format("%s ch%d: no result after %dms — ambit broken?",
                                   tag, ch, elapsed))
                        pending[ch] = nil; count = count - 1
                    end
                    -- else "busy"/"idle": keep waiting
                end
            end
        end
    end

    if hold_window then device.measurement_window(false) end
end

-- ── Job bodies ────────────────────────────────────────────────────────────
-- SS passes hold_window=false → the publisher drains concurrently during the
-- poll-loop gaps. MPF/edge are large arrun traces → hold the window (true).
local function ss_round()   run_trace("SS",   SS,  false) end
local function mpf_round()  run_trace("MPF",  MPF, true)  end
local function edge_round() run_trace("edge", MPF, true)  end

-- ── Boot banner ───────────────────────────────────────────────────────────
do
    local sr, ss = sync.sun_today()
    device.log(string.format("schedule started; sunrise=%s sunset=%s", sr, ss))
end

-- ── Schedule ──────────────────────────────────────────────────────────────

sched.every("1m",   ss_round)                          -- steady-state probe, every 1 minute
sched.every("5m", record_spectra, { when = "day" })  -- spectrum + PAR, daytime
sched.every("10m", mpf_round,      { when = "day" })  -- saturating flash, daytime
sched.sun("sunset",   30 * 60, edge_round)           -- dark-edge trace, 30 min after sunset
sched.sun("sunrise", -30 * 60, edge_round)           -- dark-edge trace, 30 min before sunrise
-- (status heartbeat + liveness LED are firmware-owned — see header)

sched.run()                                          -- blocking merge loop


for ch = 0, NUM_CHANNELS - 1 do
    if ambit.run(ch) then
        device.log(string.format("ch%d: %s", ch, ambit.query(ch)))
    end
end