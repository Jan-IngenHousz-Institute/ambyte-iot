-- sched.lua — declarative RTC job scheduler. Bundled into the firmware and
-- loaded before /sdcard/main.lua, so the `sched` global is always available.
-- Built entirely on the `sync` global (components/time_sync).
--
-- Register jobs, then call sched.run() (which never returns):
--   sched.every(period, fn [, opts])   -- every `period` (seconds or "5m"/"30s"/"1h"),
--                                          clock-aligned (:00/:05/… for 5m, etc.)
--   sched.cron(minutes, fn [, opts])   -- at given minutes-of-hour, e.g. {0,15,30,45}
--   sched.sun(event, offset, fn)       -- once per day at "sunrise"/"sunset" ± offset s
--   sched.run()                        -- blocking merge loop
--
-- opts.when = "day" | "night" | nil    -- gate every()/cron() jobs on sun state
--                                          (sun() jobs always run).
-- When several jobs fall due at the same instant (e.g. a 5-min and a 15-min mark
-- at :15), all of them fire.

local sched = { _jobs = {} }

-- "5m" / "30s" / "2h" / number(seconds) -> seconds
local function secs(v)
    if type(v) == "number" then return v end
    local n, u = tostring(v):match("^(%d+)([smh]?)$")
    n = tonumber(n) or error("sched: bad duration '" .. tostring(v) .. "'")
    return n * (u == "h" and 3600 or u == "m" and 60 or 1)
end

local function gate_ok(when)
    if when == "day"   then return sync.is_daytime() end
    if when == "night" then return not sync.is_daytime() end
    return true
end

local function add(job) sched._jobs[#sched._jobs + 1] = job end

function sched.every(period, fn, opts)
    period = secs(period)
    add{ fn = fn, when = opts and opts.when,
         next_in = function() return sync.until_interval(period, 0) end }
end

function sched.cron(minutes, fn, opts)
    add{ fn = fn, when = opts and opts.when, next_in = function()
        local into = (3600 - sync.until_interval(3600, 0)) % 3600  -- seconds into the hour
        local best
        for _, m in ipairs(minutes) do
            local d = (m * 60 - into) % 3600
            if d == 0 then d = 3600 end                            -- strictly future
            if not best or d < best then best = d end
        end
        return best
    end }
end

function sched.sun(event, offset, fn)
    add{ fn = fn, when = nil,                                       -- sun jobs always run
         next_in = function() return sync.until_sun(event, offset or 0) end }
end

function sched.run()
    while true do
        local waits, wmin = {}, math.huge
        for i, j in ipairs(sched._jobs) do
            local w = j.next_in()
            waits[i] = w
            if w and w >= 0 and w < wmin then wmin = w end
        end
        if wmin == math.huge then
            device.sleep_ms(60000)                                 -- nothing schedulable
        else
            sync.wait(wmin)
            for i, j in ipairs(sched._jobs) do
                if waits[i] == wmin and gate_ok(j.when) then
                    local ok, err = pcall(j.fn)
                    if not ok then device.log("sched job error: " .. tostring(err)) end
                end
            end
        end
    end
end

return sched
