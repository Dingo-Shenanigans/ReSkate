#pragma once
#include "world_layer_catalog.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dingosdk {

// Layers ReSkate keeps switched off and does not offer: the seasons' content on San Vansterdam
// (seasons 5 and 6) and the Isle of Grom (season 5). They keep their rows in the catalog, which
// players and servers share by position, so nothing about a session's layer list changes; the
// game just treats each as "off" whatever is saved or sent for it (local_world_layers.cpp),
// and no menu or console command names it.
inline constexpr std::array<std::string_view, 8> world_layers_kept_off{
    "bam_dtn_season_05_content", "bam_dtn_season_05_inactive", "bam_dtn_season_05_permacontent",
    "bam_dev_seasonal_content_season_06", "bam_dtn_season_06_content", "bam_dtn_season_06_inactive",
    "bam_dtn_season_06_permacontent", "grom_season_05_content"};
inline bool world_layer_kept_off(std::string_view key) noexcept {
    for (const auto kept : world_layers_kept_off)
        if (kept == key) return true;
    return false;
}
// A layer's name as menus show it: its label without the developers' own marks, the words
// DEV and DTN ("DTN Season 04 Content" is "Season 04 Content").
inline std::string world_layer_title(std::string_view label) {
    std::string title;
    for (std::size_t at = 0; at < label.size();) {
        const auto end = std::min(label.find(' ', at), label.size());
        const auto word = label.substr(at, end - at);
        at = end + 1;
        const bool mark = word.size() == 3 && (word[0] == 'D' || word[0] == 'd') &&
            (((word[1] == 'E' || word[1] == 'e') && (word[2] == 'V' || word[2] == 'v')) ||
             ((word[1] == 'T' || word[1] == 't') && (word[2] == 'N' || word[2] == 'n')));
        if (mark || word.empty()) continue;
        if (!title.empty()) title += ' ';
        title += word;
    }
    return title.empty() ? std::string(label) : title;
}

inline constexpr std::array<std::string_view, 3> world_layer_modes{"default", "on", "off"};
// One choice per catalog row (world_layers()), in catalog order.
using WorldLayerChoices = std::vector<std::string>;
inline WorldLayerChoices default_world_layers() { return WorldLayerChoices(world_layers().size(), "default"); }
inline bool valid_world_layer_mode(std::string_view mode) {
    return mode == "default" || mode == "on" || mode == "off";
}
using WorldLayerState = std::vector<std::uint8_t>;
inline WorldLayerState pack_world_layers(const WorldLayerChoices &choices) {
    if (choices.size() != world_layers().size()) throw std::invalid_argument("World layer choices do not match the catalog");
    WorldLayerState result(choices.size());
    for (std::size_t i = 0; i < result.size(); ++i) {
        if (!valid_world_layer_mode(choices[i])) throw std::invalid_argument("Invalid world layer choice");
        result[i] = choices[i] == "on" ? 1 : choices[i] == "off" ? 2 : 0;
    }
    return result;
}
inline WorldLayerChoices unpack_world_layers(const WorldLayerState &state) {
    if (state.size() != world_layers().size()) throw std::invalid_argument("World layer state does not match the catalog");
    WorldLayerChoices result(state.size());
    for (std::size_t i = 0; i < result.size(); ++i) {
        if (state[i] >= world_layer_modes.size()) throw std::invalid_argument("Invalid world layer mode");
        result[i] = world_layer_modes[state[i]];
    }
    return result;
}
struct WorldLayersModel {
    bool available{}, ready{};
    bool controlled_by_host{};
    WorldMap map{WorldMap::none};
    WorldLayerChoices choices{default_world_layers()};
    // A row is supported when the active level exposes the root controller
    // needed to reach it. Standalone custom levels intentionally carry only a
    // subset of their donor's controllers, so map family and row availability
    // cannot be inferred from one canonical retail root alone.
    std::vector<bool> supported = std::vector<bool>(world_layers().size());
    // Observed requested/loaded state, including ancestors. Unknown while the
    // reference is unavailable; never invent a live state from AutoLoad alone.
    std::vector<std::optional<bool>> enabled = std::vector<std::optional<bool>>(world_layers().size());
    std::vector<std::string> status = std::vector<std::string>(world_layers().size());
    std::string feedback;
    // Clear the per-row live state, sized to the catalog.
    void clear_live() {
        supported.assign(world_layers().size(), false);
        enabled.assign(world_layers().size(), std::nullopt);
        status.assign(world_layers().size(), {});
    }
};

// An override is session-only. Repeated rosters must never replace the saved
// local choices with a previous host snapshot, including across map changes.
struct WorldLayerOverride {
    std::optional<WorldLayerChoices> local;
    bool apply(WorldLayersModel &model, bool forced, const WorldLayerChoices &choices) {
        if (forced) {
            if (choices.size() != world_layers().size()) return false;
            for (const auto &choice : choices) if (!valid_world_layer_mode(choice)) return false;
            if (!local) local = model.choices;
            model.choices = choices;
        } else if (local) {
            model.choices = std::move(*local);
            local.reset();
        }
        model.controlled_by_host = forced;
        return true;
    }
};

struct WorldLayerMapSelection {
    WorldMap map{WorldMap::none};
    std::size_t root{world_layer_nodes().size()};
    bool canonical{};
    bool ambiguous{};
};

inline std::size_t world_layer_root_slot(std::size_t slot) noexcept {
    if (slot >= world_layer_nodes().size()) return world_layer_nodes().size();
    while (world_layer_nodes()[slot].parent >= 0)
        slot = static_cast<std::size_t>(world_layer_nodes()[slot].parent);
    return slot;
}

// Select the one map family represented by live SubWorldReference controllers.
// Retail maps expose their generated-catalog anchor. A standalone Studio level
// can deliberately omit that anchor while retaining a smaller, self-contained
// set such as BAM lighting/VFX/core; in that case a stable top-level controller
// is the fallback anchor. Mixed map families are a transition, never a choice.
inline WorldLayerMapSelection select_world_layer_map(
        const std::vector<bool>& present,
        WorldMap fallback_map = WorldMap::none) noexcept {
    WorldLayerMapSelection result;
    // Preserve the native rule: one canonical retail anchor is authoritative,
    // even if a departing world's non-anchor references have not died yet.
    std::size_t native_root = world_layer_nodes().size();
    for (const auto slot : world_map_anchors())
        if (slot < present.size() && present[slot]) {
            if (native_root != world_layer_nodes().size())
                return {WorldMap::none, world_layer_nodes().size(), false, true};
            native_root = slot;
        }
    if (native_root != world_layer_nodes().size())
        return {world_layer_nodes()[native_root].map, native_root, true, false};

    // No native anchor: fallback is authorized only by the active custom-level
    // manifest. Require at least one matching live controller and reject a
    // mixed family rather than guessing during a transition.
    if (fallback_map == WorldMap::none) return {};
    std::size_t fallback = world_layer_nodes().size();
    std::size_t any = world_layer_nodes().size();
    for (std::size_t slot = 0; slot < present.size(); ++slot) {
        if (!present[slot]) continue;
        if (world_layer_nodes()[slot].map != fallback_map)
            return {WorldMap::none, world_layer_nodes().size(), false, true};
        if (any == world_layer_nodes().size()) any = slot;
        if (world_layer_nodes()[slot].parent < 0 && fallback == world_layer_nodes().size())
            fallback = slot;
    }
    if (fallback == world_layer_nodes().size()) fallback = any;
    if (fallback == world_layer_nodes().size()) return {};
    result.map = fallback_map;
    result.root = fallback;
    return result;
}

inline bool world_layer_supported(
        std::size_t choice,
        const std::vector<bool>& present) noexcept {
    if (choice >= world_layers().size()) return false;
    const auto leaf_root = world_layer_root_slot(world_layers()[choice].leaf);
    const auto switch_root = world_layer_root_slot(world_layers()[choice].switch_slot);
    return leaf_root < present.size() && switch_root < present.size() &&
        present[leaf_root] && present[switch_root];
}
}
