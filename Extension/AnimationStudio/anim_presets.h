#pragma once
#include <algorithm>
#include <map>
#include <span>
#include <string>
#include <vector>
#include <optional>
#include "Engine/Core/Json/json.h"

namespace dingosdk::anim_studio {
struct Preset {
    std::string name, trick, pop;
    bool active{}, included{};
    std::string stance;
    std::optional<bool> switch_stance;
    std::string entry{"ground"};
};
struct PresetPack {
    std::string name;
    bool alternate{};
    std::map<std::string, std::vector<std::string>> slots;
};
inline std::string preset_slot(std::string trick, const std::string& pop) {
    for (char& c : trick) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return trick + "|" + pop;
}
// Fixed packs use their first available variant per trick/pop/stance; alternating packs enable every
// included variant. With no pack, choose alphabetically, independent of directory order.
// Exact-pop precedence remains the runtime's job. An absent pack slot leaves that trick native.
inline void select_presets(std::span<Preset> presets, const std::optional<PresetPack>& pack) {
    std::map<std::string, Preset*> selected;
    for (auto& p : presets) {
        p.active = p.included = false;
        const auto slot = preset_slot(p.trick, p.pop);
        const auto compatible_slot = slot + (p.stance == "goofy" ? "|goofy" : "|regular") +
                                     (p.switch_stance.value_or(false) ? "|switch" : "|normal") + "|" + p.entry;
        if (pack) {
            const auto entry = pack->slots.find(slot);
            if (entry == pack->slots.end()) continue;
            const auto found = std::find(entry->second.begin(), entry->second.end(), p.name);
            if (found == entry->second.end()) continue;
            p.included = true;
            if (pack->alternate) { p.active = true; continue; }
            auto& picked = selected[compatible_slot];
            if (!picked || found < std::find(entry->second.begin(), entry->second.end(), picked->name)) picked = &p;
            continue;
        }
        p.included = true;
        auto& picked = selected[compatible_slot];
        if (!picked || p.name < picked->name) picked = &p;
    }
    for (const auto& [slot, picked] : selected) picked->active = true;
}
inline Json pack_json(const PresetPack& pack) {
    Json out = Json::object();
    out["format"] = "animator-preset-pack";
    out["version"] = 1;
    out["name"] = pack.name;
    out["alternate"] = pack.alternate;
    out["slots"] = Json::object();
    for (const auto& [slot, names] : pack.slots) out["slots"][slot] = Json(names);
    return out;
}
inline PresetPack read_pack_json(const Json& root) {
    if (!root.is_object() || root.value("format", "") != "animator-preset-pack" || root.value("version", 0) != 1 ||
        !root.contains("slots") || !root.at("slots").is_object() || root.at("slots").size() > 2000)
        throw std::runtime_error("Invalid animation preset pack");
    PresetPack out;
    out.name = root.value("name", "Pack");
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
    return out;
}
} // namespace dingosdk::anim_studio
