#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>
#include <optional>
#include <set>
#include <string_view>
#include "Engine/Core/Json/json.h"
#include "utf8_text.h"

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
    // The pop it plays on, as the pack that has it on says (its Pack rules); empty when no pack says. "" any.
    std::optional<std::string> pack_pop;
    // Made on another PC (its maker's key is not this one's): it plays, tests and previews, but is not opened to edit,
    // copied or shared again.
    bool locked{};
    // Where it stands in the playlist when the playlist plays (select_playlist): the order it takes its turn in.
    int order{-1};
    // Switched to PLAY on its own (MY TRICKS): it plays in game whichever packs are on (where none of theirs has its
    // trick, pop and stance). A trick saved or put in a pack starts switched off.
    bool playing{};
    // The foot it leads with, as the studio reads its file (authored_goofy and its switch label, which a save and the
    // upgrade keep true to its clip). Empty: from its stance and switch labels.
    std::optional<bool> leads_right;
};
// The foot a saved trick leads with: what decides which takeoffs it plays on, as the game mirrors by it. A goofy normal
// and a regular switch recording both lead with the right foot.
inline bool preset_leads_right(const Preset& p) {
    return p.leads_right ? *p.leads_right : (p.stance == "goofy") != p.switch_stance.value_or(false);
}
// Which of a trick's takeoffs an animation is for, past its trick and pop: the foot it leads with, how it went in (off
// the ground or a rail) and fakie. Two animations with the same are for the same takeoffs: one of them plays.
inline std::string stance_part(const Preset& p) {
    return std::string(preset_leads_right(p) ? "|right" : "|left") + "|" + p.entry + (p.fakie.value_or(false) ? "|fakie" : "");
}
// What to do with a trick and pop that more than one pack that is on has: "*" (or nothing) play any of them at random,
// attempt by attempt; a pack's name: that pack's plays it. By slot (preset_slot).
using ConflictChoices = std::map<std::string, std::string>;
// A trick and pop more than one pack that is on has, each with its own animation of it.
struct PackConflict {
    std::string slot, trick, pop;
    std::vector<std::pair<std::string, std::vector<std::string>>> packs; // each pack and its animations of it
    std::string choice; // as ConflictChoices says: a pack's name, "*" at random, "" not decided yet (at random)
};
// A pack: saved animations grouped under a name, filed by trick and pop ("slots"). One is used at a time.
// Where a pack has more than one of a trick and pop, that slot is Fixed - its first one plays - or
// Alternate - one of the ones linked into its cycle plays each attempt, at random but never the same one twice
// running. `alternate` is what a slot is
// when it has not been set on its own (packs from before had one setting for the whole pack).
struct PresetPack {
    std::string name;
    std::string made_by; // who made the pack, so a pack passed on says whose it is
    bool alternate{};
    std::map<std::string, std::vector<std::string>> slots;
    std::map<std::string, bool> slot_alternate; // slot -> Alternate (true) or Fixed, where set on its own
    std::set<std::string> unlinked;             // members left out of their slot's cycle
    // Pack rules: the pop every trick in it plays on, whatever each was saved with. "" each its own, "any" every
    // pop, or "low", "mid" or "high" only.
    std::string pop_rule;
    std::string maker_key;     // the key of the PC it was made on (export_pack), so its tricks stay its maker's
    std::set<std::string> off; // members switched off: kept in the pack, but they do not play
    // Its maker lets others open its tricks to edit and copy them. Off (the default), they play for others, but are locked.
    bool editable{};
    std::string description;       // what its maker says of it, shown wherever it is shared (MARKET, an import)
    std::string version{"1.0.0"};  // its version on Thunderstore: the next published is this, or one past it
    std::string published;         // the version last published from this PC, "" never
    std::string published_as;      // the Thunderstore package it went up as (Team-Name): an update goes to the same one
    std::string cover;             // how its cover was made in the cover designer (JSON), "" none
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
// Which stance version a saved trick is (0 own stance, 1 fakie, 2 switch, 3 switch fakie: make_stance_version), for the skater's own stance.
inline int stance_version(const std::string& stance, std::optional<bool> switch_stance, std::optional<bool> fakie, bool own_lead_right) {
    const bool lead_right = (stance == "goofy") != switch_stance.value_or(false);
    return (lead_right == own_lead_right ? 0 : 2) + (fakie.value_or(false) ? 1 : 0);
}
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
// (one of them at random each attempt, never the same twice running), or its first when none is linked. Where two
// packs that are on have the same trick and pop, either plays at random unless one was chosen (pack_conflicts).
// Exact-pop precedence remains the runtime's job.
inline std::string effective_pop(const PresetPack& pack, const std::string& own) {
    return pack.pop_rule.empty() ? own : pack.pop_rule == "any" ? std::string{} : pack.pop_rule;
}
// The tricks and pops more than one of `packs` (all on) has, each with an animation of its own in each for the same
// takeoffs (stance_part): a regular kickflip in one pack and a goofy one in another both play, each in its stance. A
// trick on any pop does not clash with one on a set pop either: the set pop's plays on its pop, the other on the rest.
inline std::vector<PackConflict> pack_conflicts(std::span<const Preset> presets, std::span<const PresetPack> packs,
                                                const ConflictChoices& choices = {}) {
    std::map<std::string, PackConflict> found;
    std::map<std::string, std::map<std::string, std::set<std::pair<std::string, std::string>>>> takeoffs; // slot -> stance -> pack, animation
    for (const auto& pack : packs)
        for (const auto& [filed, names] : pack.slots) {
            std::vector<std::string> mine;
            std::string trick, pop;
            for (const auto& name : names) {
                if (pack.off.contains(name)) continue;
                const auto p = std::find_if(presets.begin(), presets.end(), [&](const Preset& x) { return x.name == name; });
                if (p == presets.end()) continue;
                mine.push_back(name);
                trick = p->trick;
                pop = effective_pop(pack, p->pop);
                takeoffs[preset_slot(trick, pop)][stance_part(*p)].emplace(pack.name, name);
            }
            if (mine.empty()) continue;
            const auto key = preset_slot(trick, pop);
            auto& c = found[key];
            c.slot = key;
            c.trick = trick;
            c.pop = pop;
            c.packs.emplace_back(pack.name, std::move(mine));
        }
    std::vector<PackConflict> out;
    for (auto& [key, c] : found) {
        if (c.packs.size() < 2) continue;
        // Two packs with different animations for the same takeoffs. The same animation in two packs is not a choice to make.
        bool clash = false;
        for (const auto& [stance, entries] : takeoffs[key]) {
            std::set<std::string> in_packs, distinct;
            for (const auto& [pack, name] : entries) {
                in_packs.insert(pack);
                distinct.insert(name);
            }
            clash |= in_packs.size() > 1 && distinct.size() > 1;
        }
        if (!clash) continue;
        if (const auto chosen = choices.find(key); chosen != choices.end()) c.choice = chosen->second;
        out.push_back(std::move(c));
    }
    return out;
}
// `playing`: the tricks switched to PLAY on their own (MY TRICKS); with it, only those play when no pack is on, and they
// play beside the packs that are on. Without it, every saved trick plays when no pack is on (as before PLAY).
inline void select_presets(std::span<Preset> presets, std::span<const PresetPack> packs, const ConflictChoices* choices = nullptr,
                           const std::set<std::string>* playing = nullptr) {
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
    const auto compatible = [](const Preset& p, const std::string& pop) { return preset_slot(p.trick, pop) + stance_part(p); };
    for (auto& p : presets) {
        p.active = p.included = false;
        p.pack_pop.reset();
        const auto slot = preset_slot(p.trick, p.pop);
        const auto compatible_slot = compatible(p, p.pop);
        if (packs.empty()) {
            if (playing && !playing->contains(p.name)) continue; // not switched to PLAY
            p.included = true;
            auto& [picked, place] = selected[compatible_slot];
            if (!picked || p.saved_at > picked->saved_at || (p.saved_at == picked->saved_at && p.name < picked->name)) picked = &p;
            continue;
        }
        for (std::size_t k = 0; k < packs.size(); ++k) {
            const auto place = pack_place(packs[k], slot, p.name);
            if (!place || packs[k].off.contains(p.name)) continue;
            p.included = true;
            // Its pack's rules say what pop it plays on (the first pack that has it on that says).
            const auto pop = effective_pop(packs[k], p.pop);
            if (!packs[k].pop_rule.empty() && !p.pack_pop) p.pack_pop = pop;
            const auto key = std::to_string(k) + "|" + compatible(p, pop);
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
    // A trick more than one pack has, where one pack was chosen to play it: the others' animations of it stand down.
    if (choices && packs.size() > 1)
        for (const auto& conflict : pack_conflicts(std::span<const Preset>(presets.data(), presets.size()), packs, *choices)) {
            if (conflict.choice.empty()) continue;
            const auto winner = std::find_if(conflict.packs.begin(), conflict.packs.end(), [&](const auto& e) { return e.first == conflict.choice; });
            if (winner == conflict.packs.end()) continue;
            // Only for the takeoffs the chosen pack has an animation for: the others' animations for other stances play on.
            std::set<std::string> covered;
            for (const auto& name : winner->second)
                for (const auto& p : presets) if (p.name == name) covered.insert(stance_part(p));
            for (const auto& [pack, names] : conflict.packs) {
                if (pack == conflict.choice) continue;
                for (const auto& name : names) {
                    if (std::find(winner->second.begin(), winner->second.end(), name) != winner->second.end()) continue;
                    for (auto& p : presets) if (p.name == name && covered.contains(stance_part(p))) p.active = false;
                }
            }
        }
    // Switched to PLAY on their own: they play beside the packs, where none of the packs' plays that trick, pop and stance
    // (the newest of them, where there is more than one).
    if (playing && !packs.empty()) {
        std::set<std::string> taken;
        for (const auto& p : presets)
            if (p.active) taken.insert(compatible(p, p.pack_pop ? *p.pack_pop : p.pop));
        std::map<std::string, Preset*> loose;
        for (auto& p : presets) {
            if (p.active || !playing->contains(p.name)) continue;
            const auto key = compatible(p, p.pop);
            if (taken.contains(key)) continue;
            auto& pick = loose[key];
            if (!pick || p.saved_at > pick->saved_at || (p.saved_at == pick->saved_at && p.name < pick->name)) pick = &p;
        }
        for (auto& [key, pick] : loose) pick->active = pick->included = true;
    }
}
// ---- the playlist ----------------------------------------------------------------------------------
// What plays in game when the playlist is on, instead of the packs that are on: whole packs and single tricks, in the
// order they are listed. Where more than one of them is a trick and pop for the same stance, Fixed plays the first,
// In turns plays them one after another in the playlist's order, attempt by attempt, and Shuffle any of them at random
// (never the same twice running).
struct PlaylistEntry {
    std::string pack;      // the pack it is, or the pack the trick is from ("" a trick of your own)
    std::string animation; // "" the whole pack
    bool operator==(const PlaylistEntry&) const = default;
};
enum class PlaylistMode { fixed, turns, shuffle };
inline const char* playlist_mode_name(PlaylistMode mode) { return mode == PlaylistMode::turns ? "turns" : mode == PlaylistMode::shuffle ? "shuffle" : "fixed"; }
inline PlaylistMode playlist_mode_named(std::string_view name) {
    return name == "turns" ? PlaylistMode::turns : name == "shuffle" ? PlaylistMode::shuffle : PlaylistMode::fixed;
}
inline void select_playlist(std::span<Preset> presets, std::span<const PresetPack> packs, std::span<const PlaylistEntry> playlist, PlaylistMode mode) {
    for (auto& p : presets) {
        p.active = p.included = false;
        p.pack_pop.reset();
        p.order = -1;
    }
    const auto find_pack = [&](const std::string& name) -> const PresetPack* {
        for (const auto& pack : packs) if (pack.name == name) return &pack;
        return nullptr;
    };
    int order = 0;
    const auto take = [&](const std::string& name, const PresetPack* pack) {
        for (auto& p : presets) {
            if (p.name != name || p.included) continue;
            p.included = true;
            p.order = order++;
            if (pack && !pack->pop_rule.empty()) p.pack_pop = effective_pop(*pack, p.pop);
        }
    };
    for (const auto& entry : playlist) {
        const auto* pack = entry.pack.empty() ? nullptr : find_pack(entry.pack);
        if (!entry.animation.empty()) {
            if (pack && pack->off.contains(entry.animation)) continue;
            take(entry.animation, pack);
            continue;
        }
        if (!pack) continue;
        for (const auto& [slot, names] : pack->slots)
            for (const auto& name : names)
                if (!pack->off.contains(name)) take(name, pack);
    }
    // One trick and pop, in one stance, off one kind of takeoff: Fixed plays its first; the others take turns.
    std::map<std::string, Preset*> first;
    for (auto& p : presets) {
        if (!p.included) continue;
        const auto pop = p.pack_pop ? *p.pack_pop : p.pop;
        const auto key = preset_slot(p.trick, pop) + stance_part(p);
        if (mode != PlaylistMode::fixed) {
            p.active = true;
            continue;
        }
        auto& picked = first[key];
        if (!picked || p.order < picked->order) picked = &p;
    }
    for (auto& [key, p] : first) p->active = true;
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
    if (!pack.pop_rule.empty()) out["pop_rule"] = pack.pop_rule;
    if (!pack.maker_key.empty()) out["maker_key"] = pack.maker_key;
    if (!pack.off.empty()) out["off"] = Json(std::vector<std::string>(pack.off.begin(), pack.off.end()));
    if (pack.editable) out["editable"] = true;
    if (!pack.description.empty()) out["description"] = pack.description;
    if (pack.version != "1.0.0") out["release"] = pack.version; // "version" is the file format
    if (!pack.published.empty()) out["published"] = pack.published;
    if (!pack.published_as.empty()) out["published_as"] = pack.published_as;
    if (!pack.cover.empty()) out["cover"] = pack.cover;
    return out;
}
inline PresetPack read_pack_json(const Json& root) {
    if (!root.is_object() || root.value("format", "") != "animator-preset-pack" || root.value("version", 0) != 1 ||
        !root.contains("slots") || !root.at("slots").is_object() || root.at("slots").size() > 2000)
        throw std::runtime_error("Invalid animation preset pack");
    PresetPack out;
    out.name = root.value("name", "Pack");
    out.made_by = utf8_prefix(root.value("made_by", ""), 48);
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
    if (const auto rule = root.value("pop_rule", ""); rule == "any" || rule == "low" || rule == "mid" || rule == "high") out.pop_rule = rule;
    out.maker_key = root.value("maker_key", "").substr(0, 64);
    out.editable = root.value("editable", false);
    out.description = utf8_prefix(root.value("description", ""), 2000);
    out.version = root.value("release", "1.0.0").substr(0, 32);
    out.published = root.value("published", "").substr(0, 32);
    out.published_as = utf8_prefix(root.value("published_as", ""), 260);
    out.cover = utf8_prefix(root.value("cover", ""), 4000);
    if (root.contains("off") && root.at("off").is_array() && root.at("off").size() <= 4000)
        for (const auto& name : root.at("off"))
            if (name.is_string() && !name.string().empty()) out.off.insert(name.string());
    return out;
}
} // namespace dingosdk::anim_studio
