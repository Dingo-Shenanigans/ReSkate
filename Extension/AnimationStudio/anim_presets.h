#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>
#include <optional>
#include <set>
#include "Engine/Core/Json/json.h"

namespace dingosdk::anim_studio {
struct Preset {
    std::string name, trick, pop;
    bool active{}, included{};
    std::string stance;
    std::optional<bool> switch_stance;
    std::string entry{"ground"};
    std::optional<bool> fakie; // made rolling fakie: its own slot, so a fixed pack keeps one of each
    std::string made_by;       // who made it (the animation's author)
    std::string label;         // the name it is shown by (MY TRICKS); empty: its name
    std::int64_t saved_at{};   // when its file was last written: with no pack on, the newest of a kind plays
};
// A pack: saved animations grouped under a name, filed by trick and pop ("slots"). One is used at a time.
// Where a pack has more than one of a trick and pop, that slot is Fixed - its first one plays - or
// Alternate - the ones linked into its cycle take turns, attempt by attempt. `alternate` is what a slot is
// when it has not been set on its own (packs from before had one setting for the whole pack).
struct PresetPack {
    std::string name;
    std::string made_by; // who made the pack, so a pack passed on says whose it is
    bool alternate{};
    std::map<std::string, std::vector<std::string>> slots;
    std::map<std::string, bool> slot_alternate; // slot -> Alternate (true) or Fixed, where set on its own
    std::set<std::string> unlinked;             // members left out of their slot's cycle
    bool alternates(const std::string& slot) const {
        const auto found = slot_alternate.find(slot);
        return found != slot_alternate.end() ? found->second : alternate;
    }
    // The slot a member is filed under; empty when it is not in the pack.
    std::string slot_of(const std::string& animation) const {
        for (const auto& [slot, names] : slots)
            if (std::find(names.begin(), names.end(), animation) != names.end()) return slot;
        return {};
    }
    bool has(const std::string& animation) const {
        return std::any_of(slots.begin(), slots.end(), [&](const auto& slot) {
            return std::find(slot.second.begin(), slot.second.end(), animation) != slot.second.end();
        });
    }
    std::vector<std::string> members() const {
        std::vector<std::string> out;
        for (const auto& [slot, names] : slots)
            for (const auto& member : names)
                if (std::find(out.begin(), out.end(), member) == out.end()) out.push_back(member);
        return out;
    }
};
inline std::string preset_slot(std::string trick, const std::string& pop) {
    for (char& c : trick) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return trick + "|" + pop;
}
// Where an animation sits in a pack: its place under its own trick and pop, or after those when it is filed
// elsewhere (its pop was changed after it went in). Empty when it is not in the pack.
inline std::optional<std::size_t> pack_place(const PresetPack& pack, const std::string& slot, const std::string& name) {
    if (const auto own = pack.slots.find(slot); own != pack.slots.end())
        if (const auto at = std::find(own->second.begin(), own->second.end(), name); at != own->second.end())
            return static_cast<std::size_t>(at - own->second.begin());
    for (const auto& [other, names] : pack.slots)
        if (const auto at = std::find(names.begin(), names.end(), name); at != names.end())
            return 100000 + static_cast<std::size_t>(at - names.begin());
    return std::nullopt;
}
// With no pack on, every saved animation plays: one per trick, pop and stance, the one saved last (so what
// was just saved is what plays), alphabetically when two were saved at once. With a pack on, only its animations
// play. A Fixed slot plays its first variant per stance; an Alternate slot every variant linked into its cycle
// (they take turns), or its first when none is linked. Were two packs on, both would play and take turns.
// Exact-pop precedence remains the runtime's job.
inline void select_presets(std::span<Preset> presets, std::span<const PresetPack> packs) {
    std::map<std::string, std::pair<Preset*, std::size_t>> selected; // pack and stance slot -> the pick, its place
    std::set<std::string> cycling; // pack and stance slots with at least one variant in their cycle
    // Pack and trick slots with anything linked into their cycle: an unlinked variant of one never plays, in
    // any stance (one saved leading with the other foot is a stance slot of its own, and was played there).
    std::set<std::string> linked;
    for (const auto& p : presets)
        for (std::size_t k = 0; k < packs.size(); ++k) {
            const auto filed = packs[k].slot_of(p.name);
            if (!filed.empty() && packs[k].alternates(filed) && !packs[k].unlinked.contains(p.name))
                linked.insert(std::to_string(k) + "|" + filed);
        }
    for (auto& p : presets) {
        p.active = p.included = false;
        const auto slot = preset_slot(p.trick, p.pop);
        const auto compatible_slot = slot + (p.stance == "goofy" ? "|goofy" : "|regular") +
                                     (p.switch_stance.value_or(false) ? "|switch" : "|normal") + "|" + p.entry +
                                     (p.fakie.value_or(false) ? "|fakie" : "");
        if (packs.empty()) {
            p.included = true;
            auto& [picked, place] = selected[compatible_slot];
            if (!picked || p.saved_at > picked->saved_at || (p.saved_at == picked->saved_at && p.name < picked->name)) picked = &p;
            continue;
        }
        for (std::size_t k = 0; k < packs.size(); ++k) {
            const auto place = pack_place(packs[k], slot, p.name);
            if (!place) continue;
            p.included = true;
            const auto key = std::to_string(k) + "|" + compatible_slot;
            const auto filed = packs[k].slot_of(p.name);
            if (packs[k].alternates(filed)) {
                if (!packs[k].unlinked.contains(p.name)) {
                    p.active = true;
                    cycling.insert(key);
                    continue;
                }
                if (linked.contains(std::to_string(k) + "|" + filed)) continue; // unlinked, and others are in the cycle
            }
            auto& [picked, at] = selected[key];
            if (!picked || *place < at) { picked = &p; at = *place; }
        }
    }
    // An Alternate slot plays its cycle; one with nothing linked plays its first, as Fixed does.
    for (const auto& [key, pick] : selected)
        if (!cycling.contains(key)) pick.first->active = true;
}
inline void select_presets(std::span<Preset> presets, const std::optional<PresetPack>& pack) {
    if (pack) select_presets(presets, std::span<const PresetPack>(&*pack, 1));
    else select_presets(presets, std::span<const PresetPack>{});
}
inline Json pack_json(const PresetPack& pack) {
    Json out = Json::object();
    out["format"] = "animator-preset-pack";
    out["version"] = 1;
    out["name"] = pack.name;
    if (!pack.made_by.empty()) out["made_by"] = pack.made_by;
    out["alternate"] = pack.alternate;
    out["slots"] = Json::object();
    for (const auto& [slot, names] : pack.slots) out["slots"][slot] = Json(names);
    if (!pack.slot_alternate.empty()) {
        out["modes"] = Json::object();
        for (const auto& [slot, alternating] : pack.slot_alternate) out["modes"][slot] = alternating ? "alternate" : "fixed";
    }
    if (!pack.unlinked.empty()) out["unlinked"] = Json(std::vector<std::string>(pack.unlinked.begin(), pack.unlinked.end()));
    return out;
}
inline PresetPack read_pack_json(const Json& root) {
    if (!root.is_object() || root.value("format", "") != "animator-preset-pack" || root.value("version", 0) != 1 ||
        !root.contains("slots") || !root.at("slots").is_object() || root.at("slots").size() > 2000)
        throw std::runtime_error("Invalid animation preset pack");
    PresetPack out;
    out.name = root.value("name", "Pack");
    out.made_by = root.value("made_by", "").substr(0, 48);
    out.alternate = root.value("alternate", false);
    std::size_t count = 0;
    for (const auto& [slot, names] : root.at("slots").items()) {
        if (!names.is_array() || (count += names.size()) > 4000) throw std::runtime_error("Too many pack variants");
        for (const auto& name : names) {
            if (!name.is_string() || name.string().empty()) throw std::runtime_error("Invalid pack variant");
            auto& list = out.slots[slot];
            if (std::find(list.begin(), list.end(), name.string()) == list.end()) list.push_back(name.string());
        }
    }
    if (root.contains("modes") && root.at("modes").is_object() && root.at("modes").size() <= 2000)
        for (const auto& [slot, mode] : root.at("modes").items())
            if (mode.is_string() && (mode.string() == "alternate" || mode.string() == "fixed")) out.slot_alternate[slot] = mode.string() == "alternate";
    if (root.contains("unlinked") && root.at("unlinked").is_array() && root.at("unlinked").size() <= 4000)
        for (const auto& name : root.at("unlinked"))
            if (name.is_string() && !name.string().empty()) out.unlinked.insert(name.string());
    return out;
}
} // namespace dingosdk::anim_studio
