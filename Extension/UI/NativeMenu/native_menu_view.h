#pragma once
#include "Engine/Game/World/world_names.h"
#include "Extension/UI/Overlay/overlay.h"
#include <algorithm>
#include <cctype>
#include <span>
#include <string_view>

namespace dingosdk::multiplayer::menu_view {
enum class Sort { name, players, map };
// The busiest servers first, until the player picks another order.
struct BrowserOptions { std::string query; bool same_map{}; Sort sort = Sort::players; };

// Owned pages: Multiplayer and Mod Options (needs the overlay's tool callbacks).
inline constexpr unsigned page_count = 2, tools_page = 1;
// Return missing owned slots in canonical order. Existing native stack indices
// remain untouched, including their active focus bindings. Offline mode has
// no Multiplayer tab.
inline std::vector<unsigned> missing_tabs(const std::vector<std::uint32_t>& keys,
                                          std::uint32_t multiplayer_key, bool has_tools,
                                          bool has_multiplayer = true) {
    std::vector<unsigned> result;
    for (unsigned slot = 0; slot < page_count; ++slot)
        if ((has_tools || slot != tools_page) && (has_multiplayer || slot != 0) &&
            std::find(keys.begin(), keys.end(), multiplayer_key + slot * 0x100) == keys.end()) result.push_back(slot);
    return result;
}

inline std::string fold(std::string value) {
    for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
inline std::string map_name(const std::string& path, std::span<const overlay::Level> levels = {}) {
    return world_destination_name(path, levels);
}
// Keep network-provided names to one bounded line without splitting UTF-8.
inline std::string caption(std::string_view text, std::size_t limit = 48) {
    std::string result(text);
    for (auto& c : result) if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
    if (result.size() <= limit) return result;
    auto end = limit;
    while (end && (static_cast<unsigned char>(result[end]) & 0xc0) == 0x80) --end;
    result.resize(end);
    return result + "...";
}
struct BrowserResults {
    std::vector<const MultiplayerLobby*> lobbies;
    std::size_t total{};
};
inline bool can_join(const MultiplayerModel& model, const MultiplayerLobby& lobby) {
    // From a session they joined a player can hop straight to another; a host ends theirs first.
    const bool free = !model.active || (!model.hosting && !model.echo && lobby.id != model.public_lobby);
    return free && !model.lobby_joining && lobby.players < lobby.capacity && lobby.owner != model.local_id;
}
inline std::string player_identity(const MultiplayerPlayer& player) {
    return std::to_string(player.id) + " " + std::to_string(player.epoch);
}
inline const MultiplayerPlayer* selected_player(const MultiplayerModel& model, const std::string& identity) {
    const auto found = std::find_if(model.roster.begin(), model.roster.end(), [&](const auto& player) {
        return player.connected && player.id != model.local_id && player_identity(player) == identity;
    });
    return model.active && found != model.roster.end() ? &*found : nullptr;
}
inline BrowserResults browse(const MultiplayerModel& model, const BrowserOptions& options,
                             std::span<const overlay::Level> levels = {}) {
    BrowserResults result;
    const auto query = fold(options.query);
    for (const auto& lobby : model.lobbies) {
        // Servers advertise their map's name rather than its destination.
        if (options.same_map && lobby.map != model.map &&
            !(lobby.dedicated && map_name(lobby.map, levels) == map_name(model.map, levels))) continue;
        auto text = lobby.name + " " + map_name(lobby.map, levels) + " " + lobby.map;
        for (const auto& name : lobby.friends) text += " " + name;
        if (!query.empty() && fold(text).find(query) == std::string::npos) continue;
        result.lobbies.push_back(&lobby);
    }
    std::sort(result.lobbies.begin(), result.lobbies.end(), [&](const auto* a, const auto* b) {
        // The ReSkate team's own servers lead the list, however the rest is sorted.
        if (a->official != b->official) return a->official;
        // Then where friends are.
        if (a->friends.empty() != b->friends.empty()) return !a->friends.empty();
        if (options.sort == Sort::players && a->players != b->players) return a->players > b->players;
        if (options.sort == Sort::map) {
            const auto an = fold(map_name(a->map, levels)), bn = fold(map_name(b->map, levels));
            if (an != bn) return an < bn;
        }
        const auto an = fold(a->name), bn = fold(b->name);
        return an != bn ? an < bn : a->id < b->id;
    });
    result.total = result.lobbies.size();
    return result;
}
} // namespace dingosdk::multiplayer::menu_view
