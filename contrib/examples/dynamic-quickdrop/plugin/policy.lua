local P = {}
local function has(name, words)
    for _, word in ipairs(words) do if string.find(name, word, 1, true) then return true end end
    return false
end
function P.surface(item)
    if string.sub(item, 1, 12) == "own_bkrails_" or string.sub(item, 1, 13) == "own_bkledges_" then return true end
    if string.sub(item, 1, 13) ~= "own_bkskate3_" then return false end
    local name = string.sub(item, 14)
    for _, word in ipairs({"rail", "roundrai", "fatroundrail", "square_rail", "squarerail", "squarefence", "ledge", "downledge", "hubba",
        "big_mega_rail", "guard_rail", "industrial_guard_rail", "flat_bar_", "kinked_tbar_", "spillway_legdes"}) do
        if string.sub(name, 1, #word) == word then return true end
    end
    return false
end
function P.mass(item, authored)
    if authored and authored > .05 and math.abs(authored-1) > .01 then return math.clamp(authored, .1, 20000) end
    local size = has(item, {"_s_", "small", "short"}) and .6 or (has(item, {"_l_", "large", "long"}) and 1.7 or 1)
    if has(item, {"bottlecrate", "wood_crate", "wooden_palette"}) then return 15 end
    if has(item, {"bottle", "sodacan", "drinkcan"}) then return .5 end
    if has(item, {"trash_lid", "trashcanlid", "binlid"}) then return 2 end
    if has(item, {"beach_ball", "balloon"}) then return .3 end
    if has(item, {"soccer_ball", "card_board", "cardboard", "garbage_bag"}) then return 1 end
    if has(item, {"dumpster"}) then return 300*size end
    if has(item, {"container_", "tanker", "tractor", "vehicle"}) then return 2500*size end
    if has(item, {"fridge", "vending_machine"}) then return 90 end
    if has(item, {"cone_", "pylon_"}) and not has(item, {"drum", "pole"}) then return 2 end
    if has(item, {"barrier_plastic", "construction_barrier"}) then return 12 end
    if has(item, {"barrier"}) then return 180*size end
    if has(item, {"garbage_bin", "recycl", "tin_trash", "garbage_can", "garbagecan"}) then return 12*size end
    if has(item, {"oil_drum", "barrel", "metal_keg"}) then return 25 end
    if has(item, {"planter", "ledge", "hubba", "legdes"}) then return 350*size end
    if P.surface(item) then return 35*size end
    if has(item, {"bench", "table", "couch"}) then return 55*size end
    if has(item, {"ramp", "pipe", "stairs", "funbox", "_pad_", "banked", "wavedeck"}) then return 350*size end
    if has(item, {"tree", "lampost", "lampole", "billboard"}) then return 180*size end
    return 25*size
end
function P.delta(mass, closing, dt, impact)
    if mass <= 0 or dt <= 0 then return 0 end
    closing = math.clamp(closing, 0, 45)
    if impact then return math.min(35, 1.05*80/(80+mass)*closing) end
    return math.min(closing, 180/mass*math.clamp(dt, 0, .1))
end
function P.hold(state, available, device, down, candidate, now)
    if not available or state.device ~= device then
        state.armed, state.fired, state.target, state.began, state.device = false, false, nil, nil, device
    end
    if not available then return nil end
    if not down then state.armed, state.fired, state.target, state.began = true, false, nil, nil; return nil end
    if not state.armed then return nil end
    if not state.target then
        if not candidate then state.armed = false; return nil end
        state.target, state.began = candidate, now
    end
    if candidate ~= state.target or now < state.began then state.armed, state.target = false, nil; return nil end
    if not state.fired and now-state.began >= 600 then state.fired = true; return state.target end
    return nil
end
return P
