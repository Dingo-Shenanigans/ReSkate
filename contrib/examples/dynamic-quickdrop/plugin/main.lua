local P = require("./policy")
local enabled = false -- Deliberate opt-in; never change a saved layout on plugin load.
local held, pins, objects, favorites = {}, {}, {}, {}
local status, restore, retry_at = "Experimental physics disabled. Read the contribution notes first.", {}, {}
local elapsed, candidate_at, candidate = 0, 0, nil
local hold, contacts = {}, {}
local marker_link, moving_surfaces = true, true
local progress, prompt_until = 0, 0
local function save_favorites()
    local ok, why = reskate.files.write("favorites.json", reskate.json.encode(favorites))
    if not ok then status = "Favorites save failed: " .. tostring(why) end
end
local text = reskate.files.read("favorites.json")
if text then
    local ok, values = pcall(reskate.json.decode, text)
    if ok and type(values) == "table" then
        local count = 0
        for key, value in pairs(values) do
            if type(key) == "string" and #key <= 200 and value == true and count < 32 then
                favorites[key] = true; count += 1
            end
        end
    end
end
local function release_all()
    for id in pairs(held) do reskate.objects.physics(id, nil) end
    held, retry_at, restore, hold, contacts, candidate = {}, {}, {}, {}, {}, nil
end
local function snapshot(id, object)
    local motion = reskate.objects.motion(id)
    return {item = object.item, included = true, position = motion and motion.position or object.position,
        rotation = motion and motion.rotation or object.rotation, generation = motion and motion.generation,
        edit_revision = motion and motion.edit_revision}
end
reskate.on("level_unloading", function() release_all(); pins, objects = {}, {} end)
reskate.on("unload", release_all)
reskate.on("tick", function(dt)
    elapsed += math.clamp(dt, 0, 1000)
    local events, overflow = reskate.marker.events()
    if overflow == true then restore = {}; status = "Marker events overflowed; no stale restore was replayed." end
    objects = reskate.objects.list()
    local live = {}
    for _, object in ipairs(objects) do live[object.id] = object end
    for id in pairs(pins) do if not live[id] or pins[id].item ~= live[id].item then pins[id], restore[id] = nil, nil end end
    for id in pairs(held) do if not live[id] then reskate.objects.physics(id, nil); held[id] = nil end end
    if not enabled then if next(held) then release_all() end; return end
    local count = 0
    for _, object in ipairs(objects) do
        local id = object.id
        if count < 32 then
            count += 1
            local previous_motion=reskate.objects.motion(id)
            if held[id] and not previous_motion then
                reskate.objects.physics(id,nil); held[id]=nil
            end
            if not previous_motion and elapsed >= (retry_at[id] or 0) then
                local ok, why = reskate.objects.physics(id, {dynamic = true, mass = P.mass(object.item), surface = moving_surfaces})
                retry_at[id] = elapsed + 250
                if ok then held[id] = true else status = tostring(why) end
            end
            local motion = reskate.objects.motion(id)
            if held[id] and motion and (not pins[id] or pins[id].generation ~= motion.generation) then
                pins[id] = snapshot(id, object); restore[id] = nil
            end
            if motion then
                local mass = P.mass(object.item, motion.authored_mass)
                if motion.mass ~= mass or motion.surface ~= moving_surfaces then
                    reskate.objects.physics(id, {dynamic=true, mass=mass, surface=moving_surfaces})
                end
                local pin = pins[id]
                if pin and motion.edit_revision ~= pin.edit_revision then
                    local included = pin.included
                    pins[id] = snapshot(id, object); pins[id].included = included; restore[id] = nil
                end
            end
        end
    end
    if events and not overflow and marker_link then
        for _, event in ipairs(events) do
            if event == "saved" then
                for id, pin in pairs(pins) do
                    if pin.included and held[id] and live[id] then pins[id] = snapshot(id, live[id]) end
                end
                status = "Saved the included objects at their current physics poses."
            elseif event == "returned" then
                for id, pin in pairs(pins) do
                    if pin.included and held[id] then restore[id] = {position=pin.position, rotation=pin.rotation, expires=elapsed+3000} end
                end
            end
        end
    end
    for id, wanted in pairs(restore) do
        if not live[id] or not held[id] or elapsed>wanted.expires then restore[id] = nil
        else
            local ok, why = reskate.objects.pose(id, wanted.position, wanted.rotation)
            if ok then restore[id] = nil; status = "Restored marker-linked object poses."
            else status = "Marker restore pending: " .. tostring(why) end
        end
    end
    local actor, skater, pad = reskate.objects.actor(), reskate.skater.get(), reskate.input.gamepad()
    local active = actor and actor.active and skater and dt>0 and dt<=250
    if active and elapsed>=candidate_at then
        candidate_at, candidate = elapsed+75, nil
        local best = math.huge
        local heading, at = skater.heading, skater.position
        local function facing(side, height, forward)
            return vector.create(math.cos(heading)*side+math.sin(heading)*forward,height,-math.sin(heading)*side+math.cos(heading)*forward)
        end
        local function search(from,to)
            local hit=reskate.objects.contact(from,to)
            if not hit or not pins[hit.id] then return end
            local delta=hit.position-at
            local forward=delta.x*math.sin(heading)+delta.z*math.cos(heading)
            local side=delta.x*math.cos(heading)-delta.z*math.sin(heading)
            local distance=vector.dot(delta,delta)
            if forward<-.4 or forward>2.6 or math.abs(side)>2.6 or distance>2.6*2.6+1.2 then return end
            local score=distance+math.abs(side)*.6+math.abs(delta.y-.25)*.8
            if score<best then best,candidate=score,hit.id end
        end
        -- 57 rays per scan, leaving room within the shared 64/tick sandbox budget.
        for _,angle in ipairs({0,-.61,.61,-1.31,1.31}) do
            for _,height in ipairs({.75,.5,.25,0,-.25,-.5,-.7,-.9,-1.05}) do
                search(at+vector.create(0,height,0),at+facing(math.sin(angle)*2.6,height,math.cos(angle)*2.6))
            end
        end
        for _,side in ipairs({0,-.45,.45}) do for _,forward in ipairs({.65,1.25,1.9,2.6}) do
            search(at+vector.create(0,.3,0),at+facing(side,-1.1,forward))
        end end
    elseif not active then candidate=nil end
    local only_rb=true
    if pad then for button,down in pairs(pad.buttons) do if down and button~="rb" then only_rb=false end end end
    local target=P.hold(hold, active and pad~=nil and only_rb, actor and actor.device or 0,
        pad and pad.buttons.rb or false, candidate, elapsed)
    progress=hold.armed and hold.target and hold.began and math.clamp((elapsed-hold.began)/600,0,1) or 0
    if target and pins[target] then
        local pin=pins[target]; pin.included=not pin.included
        if pin.included and live[target] then pins[target]=snapshot(target,live[target]) end
        restore[target]=nil; prompt_until=elapsed+1200
    end
    -- Transfer momentum only after an unobstructed native body contact. Lua owns
    -- impact/pressure policy; the native solver still owns gravity and tumbling.
    local transfer=active and (actor.grounded or (not actor.on_foot and actor.physics_state>=100 and actor.physics_state<200))
    local touching={}
    if transfer then
        local velocity=actor.grounded and actor.walking_velocity or actor.velocity
        local horizontal=vector.create(velocity.x,0,velocity.z)
        if actor.grounded and vector.magnitude(actor.wish)>vector.magnitude(horizontal) then horizontal=actor.wish end
        local speed=vector.magnitude(horizontal)
        if speed>.12 then
            local direction=horizontal/speed
            for _,height in ipairs({-1.05,-.9,-.65,-.4,-.1,.2}) do
                local from=skater.position+vector.create(0,height,0)
                local hit=reskate.objects.contact(from,from+direction*.95)
                if hit and held[hit.id] then
                    local motion=reskate.objects.motion(hit.id)
                    if motion and not motion.editing then
                        touching[hit.id]=true
                        local previous=contacts[hit.id]
                        local impact=not previous or elapsed-previous>150
                        local closing=speed-vector.dot(motion.velocity,direction)
                        local delta=P.delta(motion.mass,closing,dt/1000,impact)
                        local wanted=motion.velocity+direction*delta
                        local magnitude=vector.magnitude(wanted)
                        if magnitude>45 then wanted=wanted*(45/magnitude) end
                        if delta>0 then reskate.objects.velocity(hit.id,wanted) end
                        break
                    end
                end
            end
        end
    end
    for id in pairs(touching) do contacts[id]=elapsed end
    for id,at in pairs(contacts) do if elapsed-at>500 then contacts[id]=nil end end
end)
reskate.on("draw", function(draw)
    if not enabled or not candidate or not pins[candidate] then return end
    local included=pins[candidate].included
    local text=(included and "Marker linked" or "Marker excluded") .. " | Hold RB to toggle"
    if progress>0 and progress<1 then text ..= " " .. math.floor(progress*100) .. "%" end
    if elapsed<prompt_until then text=included and "Added to marker recall" or "Removed from marker recall" end
    draw:text(vector.create(36,120,0),text,{size=20,colour="#ffffff"})
end)
reskate.menu.page("Dynamic QuickDrop", function(ui)
    ui:note("Offline dynamic objects, native editing, moving surfaces and marker recall.")
    enabled = ui:toggle("enabled", "Enable Dynamic QuickDrop", enabled)
    marker_link = ui:toggle("marker_link", "Link included objects to the native marker", marker_link)
    local support=ui:toggle("moving_surfaces", "Moving skateable surfaces", moving_surfaces)
    if support~=moving_surfaces then
        moving_surfaces=support
        for id in pairs(held) do local motion=reskate.objects.motion(id)
            if motion then reskate.objects.physics(id,{dynamic=true,mass=motion.mass,surface=support}) end
        end
    end
    ui:note(status)
    if #objects>32 then ui:note("Sandbox limit: only the first 32 live placements are controlled.") end
    ui:label("Marker pins are session-local. Favorite item names are saved separately from the game profile.")
    local catalog = reskate.objects.catalog()
    local known = {}
    for _, item in ipairs(catalog) do known[item.key] = true end
    local shown = {}
    for _, object in ipairs(objects) do
        local pin = pins[object.id]
        if pin then pin.included = ui:toggle("pin_" .. object.id, "Marker: " .. object.item, pin.included) end
        if known[object.item] and not shown[object.item] and ui:button("fav_" .. object.id, (favorites[object.item] and "Unfavorite " or "Favorite ") .. object.item) then
            if favorites[object.item] then favorites[object.item] = nil
            else
                local count = 0; for _ in pairs(favorites) do count += 1 end
                if count < 32 then favorites[object.item] = true else status = "Favorites limit: 32 items." end
            end
            save_favorites()
        end
        shown[object.item] = true
    end
    local ordered = {}; for key in pairs(favorites) do table.insert(ordered, key) end; table.sort(ordered)
    for index, key in ipairs(ordered) do
        if known[key] and ui:button("place_" .. index, "Place favorite: " .. key) then
            local at, heading = reskate.skater.position(), reskate.skater.heading()
            if not at or not heading then status = "Load the local skater before placing a favorite."
            else
                local ok, why = reskate.objects.place(key, at + vector.create(math.sin(heading)*2, .2, math.cos(heading)*2), heading)
                status = ok and "Favorite placement queued through the normal object API." or tostring(why)
            end
        end
    end
end)
