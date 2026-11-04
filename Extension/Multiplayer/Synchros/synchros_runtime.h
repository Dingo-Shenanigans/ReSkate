#pragma once
#include "sharing_rules.h"
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace dingosdk::multiplayer {
class SteamTransport;
}

// Skate Synchros in the running game: the settings, the cache, the cutter over the enabled mods
// and one Exchange per session, driven by the session's update (session_synchros.cpp). Everything
// here runs on the session's thread unless it says otherwise.
namespace dingosdk::multiplayer::synchros {

// The session, as Skate Synchros sees it each update.
struct SessionView {
    bool playing{};                 // hosting or joined, not a dedicated server's own process
    std::uint64_t session{}, me{};
    struct Player {
        std::uint64_t id{};
        std::uint64_t outfit_revision{};   // changes whenever their outfit packet does
        std::set<std::string> worn;        // item keys of their outfit packet
    };
    std::vector<Player> players;    // handshaken players in the session, not this one
    std::uint64_t own_revision{};
    std::set<std::string> wearing;  // this player's outfit packet's item keys
};

// Called by the session's update, and when it stops.
void tick(SteamTransport& transport, const SessionView& view);
void stop(SteamTransport& transport);

// The settings, for the settings page and console commands. Any thread.
[[nodiscard]] Settings settings();
// Changes them and saves. `change` returns what to tell the player (empty: nothing).
std::string change(const std::function<std::string(Settings&)>& change);
// How it is going with each player in the session now, one line each.
[[nodiscard]] std::vector<std::string> report();
// Blocks a player: neither sent to nor shown, and what was received from them is removed.
void block_player(std::uint64_t player);
// Empties the cache and every received outfit (they show again as stand-ins after the next load).
void empty_cache();
[[nodiscard]] std::uint64_t cache_bytes();

// The "synchros" console command: status, share on|off, show on|off, code, new-code, add <code>,
// remove <code>, codes, group create <name> | join <code> [name] | leave <n> | new-code <n>, groups,
// paired, unpair <SteamID>, block <SteamID>, unblock <SteamID>, cache, empty-cache.
[[nodiscard]] std::string console_command(std::string_view text);

} // namespace dingosdk::multiplayer::synchros
