-- Geometry only. The companion replay UI still reads the recorded controller.
local title = "Live Controller Layout"
local defaults = {left = 100, top = 165, width = 620}
local function copy(v) return {left = v.left, top = v.top, width = v.width} end
local function valid(v)
    if type(v) ~= "table" then return false end
    for _, name in ipairs({"left", "top", "width"}) do
        local n = v[name]
        if type(n) ~= "number" or n ~= n or math.abs(n) > 10000 then return false end
    end
    return v.width >= 160 and v.width <= 1200 and v.left >= 0 and v.top >= 0
        and v.left + v.width <= 1920 and v.top + v.width * .625 <= 1080
end
local saved = copy(defaults)
local text = reskate.files.read("layout.json")
if text then
    local ok, value = pcall(reskate.json.decode, text)
    if ok and valid(value) then saved = copy(value) end
end
local draft, editing, ready, armed, previous = copy(saved), false, false, false, {}
local enter_was_down = false
local status = "Waiting for the companion UI float models."
local function publish()
    local ok, why = reskate.ui.values(editing and draft or saved)
    ready = ok
    status = ok and "Live layout ready. Companion replay assets must use this plugin's symbols." or tostring(why)
end
local function finish(save)
    if save then
        local ok, why = reskate.files.write("layout.json", reskate.json.encode(draft))
        if not ok then reskate.ui.toast("Layout save failed: " .. tostring(why), "warn"); return end
        saved = copy(draft)
    end
    draft, editing, armed, previous = copy(saved), false, false, {}
    reskate.ui.close()
    publish()
end
local function begin()
    if editing then finish(false); return end
    if not ready or reskate.level.current() == "" then return end
    draft, armed, previous, editing = copy(saved), false, {}, true
    enter_was_down = reskate.input.key("Enter")
    reskate.ui.open(title)
end
reskate.ui.screen(title, function() end, {panel = false})
reskate.input.bind("edit_layout", "F8")
reskate.on("action", function(name) if name == "edit_layout" then begin() end end)
reskate.on("screen_closed", function(name)
    if name == title then draft, editing, armed, previous = copy(saved), false, false, {}; publish() end
end)
reskate.on("level_unloading", function() if editing then finish(false) end end)
reskate.menu.page(title, function(ui)
    ui:label("F8: move and resize the recorded-input overlay, without rebuilding.")
    ui:note(status)
    if ui:button("edit", "Edit layout", ready) then begin() end
end)
local function axis(v)
    return math.abs(v) <= .18 and 0 or math.sign(v) * (math.abs(v) - .18) / .82
end
reskate.on("tick", function(dt)
    if editing then
        if not reskate.input.focus().focused then finish(false); return end
        local enter = reskate.input.key("Enter")
        if enter and not enter_was_down then finish(true); return end
        enter_was_down = enter
        local pad = reskate.input.gamepad()
        if not pad then armed, previous = false, {}
        else
            local b = pad.buttons
            local function pressed(k) return b[k] and not previous[k] end
            if not armed then armed = not (b.a or b.b or b.x)
            elseif pressed("b") then finish(false); return
            elseif pressed("a") then finish(true); return
            else
                if pressed("x") then draft = copy(defaults) end
                local seconds = math.clamp(dt, 0, 50) / 1000
                local speed = b.lb and 60 or 420
                local width = math.clamp(draft.width + axis(pad.right_y) * speed * seconds, 160, 1200)
                draft.left = math.clamp(draft.left + (draft.width - width)/2 + axis(pad.left_x)*speed*seconds, 0, 1920-width)
                draft.top = math.clamp(draft.top + (draft.width - width)*.625/2 - axis(pad.left_y)*speed*seconds, 0, 1080-width*.625)
                draft.width = width
            end
            previous = b
        end
    end
    publish()
    if editing and not ready then finish(false) end
end)
reskate.on("draw", function(d)
    if not editing then return end
    local size = d:size()
    d:rect(vector.create(20, size.y-90, 0), vector.create(size.x-20, size.y-20, 0), {fill=true, colour="#15202bee"})
    d:text(vector.create(35, size.y-80, 0), "LS: move | RS: resize | LB/L1: fine | A/Enter: save | B/Esc/F8: cancel | X: reset")
end)
