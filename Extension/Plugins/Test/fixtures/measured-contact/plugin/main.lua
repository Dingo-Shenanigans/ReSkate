-- One explicitly armed measured-contact diagnostic; no force or control requests.
local P, V = reskate.world.physics, vector.create
local body, sample, origin, right
local phase, detail = "idle", "Start explicitly, then close the menu."
local armed, used, retry_clock, finished, queried = false, false, false, false, false
local unreturned_allocation_unknown = false
local session_ms, session_ticks, body_ms, body_ticks, retries = 0, 0, 0, 0, 0
local creates, queries = {}, {}
local actor_before, actor_after
local key = "measured_contact_trial_v1"
if reskate.storage.get(key) ~= nil then used, finished, phase = true, true, "previous trial" end
local function finite(n) return type(n) == "number" and n == n and math.abs(n) < math.huge end
local function vec(v) return type(v) == "vector" and finite(v.x) and finite(v.y) and finite(v.z) end
local function xyz(v) return {v.x, v.y, v.z} end
local function snapshot(s)
    if type(s) ~= "table" then return nil end
    return {serial = s.serial, time = s.time, seconds = s.seconds, mass = s.mass,
        inertia = vec(s.inertia) and xyz(s.inertia) or nil, position = vec(s.position) and xyz(s.position) or nil,
        velocity = vec(s.velocity) and xyz(s.velocity) or nil, angular_velocity = vec(s.angular_velocity) and xyz(s.angular_velocity) or nil,
        rotation = type(s.rotation) == "table" and {s.rotation[1], s.rotation[2], s.rotation[3], s.rotation[4]} or nil}
end
local function valid(s)
    if type(s) ~= "table" or type(s.serial) ~= "string" or not s.serial:match("^[1-9][0-9]*$") or not s.epoch
        or not finite(s.time) or not finite(s.seconds) or math.abs(s.seconds - 1 / 60) >= 1e-7 or s.mass ~= 12
        or not vec(s.position) or not vec(s.velocity) or not vec(s.angular_velocity) or not vec(s.inertia)
        or type(s.rotation) ~= "table" or #s.rotation ~= 4 then return false end
    for i = 1, 4 do if not finite(s.rotation[i]) then return false end end
    return true
end
local function same(a, b)
    if not valid(a) or not valid(b) or a.serial ~= b.serial then return false end
    local function number(x, y) return x == y and (x ~= 0 or 1 / x == 1 / y) end
    for _, field in {"time", "seconds", "mass"} do if not number(a[field], b[field]) then return false end end
    for _, field in {"position", "velocity", "angular_velocity", "inertia"} do
        for _, axis in {"x", "y", "z"} do if not number(a[field][axis], b[field][axis]) then return false end end
    end
    for i = 1, 4 do if not number(a.rotation[i], b.rotation[i]) then return false end end
    return true
end
local function finish(reason, passed)
    if finished then return end
    finished, armed, used, retry_clock = true, false, true, false
    local released, why, release_attempted, release_call_ok = false, "no body handle; no release attempted", false, nil
    if unreturned_allocation_unknown then released, why = false, "create returned no handle; native allocation state unknown" end
    if body then
        release_attempted = true
        local ok, accepted, message = pcall(P.release, body)
        release_call_ok, released = ok, ok and accepted == true
        why = ok and message or tostring(accepted)
        if released then why = "SDK release accepted; retained parked slot, not destroyed" end
        body = nil -- This diagnostic attempts release once, including on refusal or exception.
    end
    phase = passed and released and "passed" or "stopped"
    detail = reason .. "; release=" .. tostring(released) .. "; " .. tostring(why)
    local report = {reason = reason, passed = passed == true and released == true, released = released,
        release_attempted = release_attempted, release_call_ok = release_call_ok, release_detail = why,
        creates = creates, readiness_retries = retries, queries = queries, sample = snapshot(sample),
        session_ms = session_ms, session_ticks = session_ticks, body_ms = body_ms, body_ticks = body_ticks,
        actor_before = actor_before, actor_after = actor_after, submitted = 0,
        actor_exclusion_requested = false, actor_exclusion_supported = false, contact_measurement_requested = true,
        unreturned_allocation_unknown = unreturned_allocation_unknown,
        purpose = "three measured contacts at one capsule placement; no static-world or actor-exclusion qualification"}
    local ok, error = pcall(reskate.storage.set, key, report)
    pcall(reskate.log, "measured-contact-trial-v1 finish " .. reskate.json.encode({phase = phase, reason = reason,
        released = released, storage_queued = ok, storage_detail = tostring(error), query_count = #queries, submitted = 0}))
end
local function query(rays, before)
    local row = {exclude_local_actor = false, require_contact = true, before = snapshot(before), requests = {}, response = {rays = {}}}
    queries[#queries + 1] = row
    for i, ray in rays do row.requests[i] = {from = xyz(ray.from), to = xyz(ray.to)} end
    local ok, batch = pcall(P.probe_batch, body, before.epoch, rays, {exclude_local_actor = false, require_contact = true})
    row.call_ok = ok
    if not ok or type(batch) ~= "table" then row.error = tostring(batch); return nil, row end
    row.response.status, row.response.reason, row.response.native_casts, row.response.budget_exhausted =
        batch.status, batch.reason, batch.native_casts, batch.budget_exhausted
    row.response.epoch_matches = batch.epoch == before.epoch
    row.response.local_actor_excluded = batch.local_actor_excluded
    row.response.owned_bodies_excluded = batch.owned_bodies_excluded
    if type(batch.rays) == "table" then
        for i, ray in batch.rays do
            row.response.rays[i] = {status = ray.status, position = vec(ray.position) and xyz(ray.position) or nil,
                fraction = ray.fraction, motion = ray.motion,
                normal_available = ray.normal_available, surface_velocity_available = ray.surface_velocity_available,
                normal = vec(ray.normal) and xyz(ray.normal) or nil,
                surface_velocity = vec(ray.surface_velocity) and xyz(ray.surface_velocity) or nil}
        end
    end
    return batch, row
end
local function clear_of_actor(rays, actor)
    for _, ray in rays do
        local dx, dz = ray.from.x - actor.x, ray.from.z - actor.z
        if dx * dx + dz * dz < 1 then return false end
    end
    return true -- Endpoint placement check only; no continuous actor-exclusion guarantee.
end
reskate.menu.page("Measured contacts", function(ui)
    ui:note("One 12 kg capsule; three measured rays. No force commands.")
    ui:note("Actor exclusion is false. Clearance is checked only at the recorded endpoints.")
    if not finished and (not used or retry_clock) and not body then
        if ui:button("start", retry_clock and "Retry readiness once" or "Start measured contacts", true) then
            if retry_clock then retries += 1 end
            armed, used, retry_clock = true, true, false
            phase, detail = "armed", "Close menu within two minutes, with the game focused."
            reskate.log("measured-contact-trial-v1 armed; explicit page action")
        end
    end
    if ui:button("stop", "Release / stop") and used and not finished then finish("explicit page action", false) end
    ui:info("Phase", phase)
    ui:info("Status", detail)
    ui:info("Query count", tostring(#queries))
end)
local function tick(dt_ms)
    if finished or (not armed and not body and not retry_clock) then return end
    if not finite(dt_ms) or dt_ms < 0 then finish("invalid wall clock", false); return end
    session_ms += dt_ms
    session_ticks += 1
    if session_ms >= 120000 or session_ticks >= 36000 then finish("session watchdog", false); return end
    if body then
        body_ms += dt_ms
        body_ticks += 1
        if body_ms >= 6000 or body_ticks >= 1800 then finish("allocated body watchdog", false); return end
    end
    local focus = reskate.input.focus()
    if not focus or not focus.focused or not focus.gameplay or focus.interface_open then
        if body then finish("focus/gameplay/interface changed", false) end
        return
    end
    if armed then
        armed = false
        if #creates >= 2 or retries > 1 then finish("create attempt bound", false); return end
        local player = reskate.skater.get()
        if not player or not vec(player.position) or not finite(player.heading) then finish("no finite local actor placement", false); return end
        origin = player.position + V(math.cos(player.heading) * 2, 1.5, -math.sin(player.heading) * 2)
        right = V(math.sin(player.heading), 0, math.cos(player.heading))
        unreturned_allocation_unknown = true
        local ok, created, why = pcall(P.create, {mass = 12, inertia = V(.4, .1, .4), position = origin, rotation = {0, 0, 0, 1},
            capsules = {{a = V(0, -.25, 0), b = V(0, .25, 0), radius = .12}}})
        creates[#creates + 1] = {call_ok = ok, accepted = ok and created ~= nil, detail = ok and tostring(why) or tostring(created)}
        if not ok then finish("create raised: " .. tostring(created), false); return end
        body = created
        if not body then
            local known = why == "waiting for a fresh qualified 60 Hz physics step"
            unreturned_allocation_unknown = not known
            retry_clock = known and #creates == 1 and retries == 0
            phase, detail = "refused", tostring(why)
            if not retry_clock then finish("create refused: " .. tostring(why), false) end
            return
        end
        unreturned_allocation_unknown = false
        body_ms, body_ticks = 0, 0
        phase, detail = "waiting sample", "No command is published."
    end
    if not body then return end
    local ok, observed, why = pcall(P.read, body)
    if not ok then finish("sample read raised: " .. tostring(observed), false); return end
    if not observed then detail = tostring(why); return end
    sample = observed
    if not valid(sample) then finish("invalid native sample", false); return end
    local offset = sample.position - origin
    if offset.x ^ 2 + offset.y ^ 2 + offset.z ^ 2 > .35 ^ 2 then finish("placement left bounded trial", false); return end
    local down = V(0, 4, 0)
    local rays = {{from = sample.position, to = sample.position - down},
        {from = sample.position + V(0, .8, 0), to = sample.position - down},
        {from = sample.position + right * .6, to = sample.position + right * .6 - down}}
    local actor = reskate.skater.get()
    if not actor or not vec(actor.position) then finish("local actor placement unavailable", false); return end
    actor_before = xyz(actor.position)
    if not clear_of_actor(rays, actor.position) then finish("ray entered one-metre actor clearance", false); return end
    if queried then finish("single query already spent", false); return end
    queried = true
    local before = sample -- Keep this table and its opaque epoch; never replace it with the after-read.
    local batch, row = query(rays, before)
    local read_ok, after, read_why = pcall(P.read, body)
    row.after_read_ok = read_ok
    row.after = snapshot(read_ok and after or nil)
    row.after_read_detail = read_ok and tostring(read_why) or tostring(after)
    row.sample_exact_equal = read_ok and valid(after) and same(before, after) -- Includes zero signs; JSON numeric lanes do not preserve them.
    if not row.sample_exact_equal then finish("postquery sample changed or unavailable; no retry", false); return end
    actor = reskate.skater.get()
    if not actor or not vec(actor.position) then finish("postquery actor placement unavailable", false); return end
    actor_after = xyz(actor.position)
    if not clear_of_actor(rays, actor.position) then finish("postquery actor clearance changed", false); return end
    if not batch or batch.status ~= "available" or batch.native_casts ~= 3 or batch.budget_exhausted ~= false
        or batch.reason ~= nil or batch.epoch ~= before.epoch or batch.local_actor_excluded ~= false or batch.owned_bodies_excluded ~= true
        or type(batch.rays) ~= "table" or #batch.rays ~= 3 then
        finish("measured contacts unavailable: " .. tostring(batch and batch.reason), false)
        return
    end
    for i, ray in batch.rays do
        if ray.status ~= "hit" or not vec(ray.position) or not finite(ray.fraction) or ray.fraction < 0 or ray.fraction > 1
            or ray.motion ~= "measured" or ray.normal_available ~= true or ray.surface_velocity_available ~= true
            or not vec(ray.normal) or not vec(ray.surface_velocity) then finish("actual measured contact required", false); return end
        local normal = ray.normal
        local norm = normal.x ^ 2 + normal.y ^ 2 + normal.z ^ 2
        local segment = rays[i].to - rays[i].from
        local facing = segment.x * normal.x + segment.y * normal.y + segment.z * normal.z
        if norm < .98 or norm > 1.02 or facing > 1e-4 * math.sqrt(segment.x ^ 2 + segment.y ^ 2 + segment.z ^ 2) then
            finish("invalid measured normal", false)
            return
        end
        local delta = ray.position - (rays[i].from + segment * ray.fraction)
        if math.abs(delta.x) > .025 or math.abs(delta.y) > .025 or math.abs(delta.z) > .025 then finish("hit left its actual segment", false); return end
        -- Every capsule point is within .25+.12=.37 m of its centre, for every rotation.
        if ray.position.y >= before.position.y - .39 then finish("hit did not pass below capsule bound", false); return end
    end
    local matched = batch.rays[1].position - batch.rays[2].position
    if math.abs(matched.x) > .025 or math.abs(matched.y) > .025 or math.abs(matched.z) > .025 then finish("inside and crossing rays reached different surfaces", false); return end
    if math.abs(batch.rays[1].position.y - batch.rays[3].position.y) > .15 then finish("adjacent reference did not match bounded placement", false); return end
    finish("three actual measured normals and point velocities; one placement, no commands or actor exclusion", true)
end
reskate.on("tick", function(dt_ms)
    local ok, why = pcall(tick, dt_ms)
    if not ok then finish("tick callback raised: " .. tostring(why), false) end
end)
reskate.on("unload", function() if not finished and (body or armed or retry_clock) then finish("plugin unload", false) end end)
