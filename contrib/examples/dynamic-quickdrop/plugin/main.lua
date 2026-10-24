-- Draft API port. Not a replacement for the author's working combined 2.1.2 mod.
local enabled = false -- Explicit opt-in on every load until native parity is tested.
local held, pins, objects, favorites = {}, {}, {}, {}
local status, restore, retry_at = "Experimental physics disabled. Read the contribution notes first.", {}, {}
local elapsed = 0
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
    held, retry_at, restore = {}, {}, {}
end
local function excluded(item)
    -- Moving grind support has not been ported. Do not silently offer it.
    item = string.lower(item)
    for _, word in ipairs({"rail", "ledge", "hubba", "ramp", "stairs", "funbox", "quarterpipe", "halfpipe"}) do
        if string.find(item, word, 1, true) then return true end
    end
    return false
end
local function snapshot(id, object)
    local motion = reskate.objects.motion(id)
    return {item = object.item, included = true, position = motion and motion.position or object.position,
        rotation = motion and motion.rotation or object.rotation, generation = motion and motion.generation}
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
        if not excluded(object.item) and count < 32 then
            count += 1
            if not reskate.objects.motion(id) and elapsed >= (retry_at[id] or 0) then
                local ok, why = reskate.objects.physics(id, {dynamic = true, mass = 25})
                retry_at[id] = elapsed + 250
                if ok then held[id] = true else status = tostring(why) end
            end
            local motion = reskate.objects.motion(id)
            if held[id] and motion and (not pins[id] or pins[id].generation ~= motion.generation) then
                pins[id] = snapshot(id, object); restore[id] = nil
            end
        end
    end
    if events and not overflow then
        for _, event in ipairs(events) do
            if event == "saved" then
                for id, pin in pairs(pins) do
                    if pin.included and held[id] and live[id] then pins[id] = snapshot(id, live[id]) end
                end
                status = "Saved the included objects at their current physics poses."
            elseif event == "returned" then
                for id, pin in pairs(pins) do
                    if pin.included and held[id] then restore[id] = {position=pin.position, rotation=pin.rotation} end
                end
            end
        end
    end
    for id, wanted in pairs(restore) do
        if not live[id] or not held[id] then restore[id] = nil
        else
            local ok, why = reskate.objects.pose(id, wanted.position, wanted.rotation)
            if ok then restore[id] = nil; status = "Restored marker-linked object poses."
            else status = "Marker restore pending: " .. tostring(why) end
        end
    end
end)
reskate.menu.page("QuickDrop (draft)", function(ui)
    ui:note("Experimental offline API port. Moving grinds and native-dropper parity are not implemented.")
    enabled = ui:toggle("enabled", "Enable experimental ordinary-prop physics", enabled)
    ui:note(status)
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
