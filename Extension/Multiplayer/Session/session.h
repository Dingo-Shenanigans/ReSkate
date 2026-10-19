#pragma once
#include "Engine/Game/Multiplayer/session_model.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dingosdk::multiplayer {
// Commands and tick execute on the game's client thread. model() is thread-safe.
std::string command(std::string_view action, std::string_view argument = {}, std::string_view password = {});
// A dedicated server's vote is running and this player may answer it (queue_command "vote" yes|no).
bool server_vote_open() noexcept;
// A dedicated server's poll is running and this player may answer it: how many answers it has
// (queue_command "vote" 1 to that many). 0: no poll.
unsigned server_poll_answers() noexcept;
// Private menu queue: passwords never pass through console history/logging.
bool queue_command(std::string_view action, std::string_view argument, std::string_view password);
// Called just before the host's validated native load is submitted, on the client thread.
void host_map_change(std::string_view destination);
enum class MapLoadResult { waiting, queued, failed, missing }; // missing: the map is not installed
// Runs on the client thread; only catalog-registered destinations may be queued.
using MapLoader = MapLoadResult (*)(std::string_view map, bool submitted, std::string &detail);
void tick(std::uintptr_t base, std::uintptr_t client, bool gameplay_ready, std::string_view map,
          MapLoader loader = nullptr);
MultiplayerModel model();
std::string take_leave_notice(); // why the session just ended over a missing map, once; client thread
// What the host or server said as it put the player out of the session (kicked, banned, away too
// long, a mod or a cheat it does not allow), or refused to let them in for it; once. Client thread.
std::string take_removed_notice();
// The same map when the host said which Thunderstore package it is from, so it can be offered
// for download instead (Extension/Assets/map_download.h): the session does not end. It tells
// the host the map is being fetched, which is given much longer than a load, and goes on
// trying the map, so it loads as soon as it is installed. Once per map; client thread.
// The downloader ends its own attempt (queue_command "stop-map-fetch", with its deadline)
// when the player says no or the map
// cannot be had. Should the session end meanwhile for another reason,
// queue_command("rejoin", "", "") goes back to it, with the password it was joined with.
struct MapNeed {
    std::string package; // protocol.h: valid_map_package, never empty
    std::string map;     // its name for people
    bool server{};       // a dedicated server's map, not a player's lobby
    bool moved{};        // the session changed to this map with the player in it (else: joining)
    // The version of it installed here, when that is another than the host's: the host's is
    // fetched in its place. Empty: the map is not installed.
    std::string have;
    // Local steady-clock deadline, shared with the downloader, never transmitted.
    std::uint64_t deadline_us{};
};
std::optional<MapNeed> take_map_need();
// Whether a session is waiting on a map being fetched right now. Thread-safe.
bool fetching_map() noexcept;
std::uint64_t map_fetch_deadline() noexcept;
// Held for the named package while it is being downloaded, installed or applied: unrelated
// joins are not held. The session waits as it does for a missing map. A map that is being
// replaced by another version is one the game already knows, so without this it would load as
// soon as the new files are in place, before the mods are applied again. Thread-safe.
void hold_map_load(bool hold, std::string_view package) noexcept;
// How a host learns the package of the map it is on, to tell its guests: the level's asset in,
// the package (or nothing) out. Called on the client thread, often; it should be quick.
using MapPackageLookup = std::string (*)(std::string_view asset);
// The maps a dedicated server's vote for the next map will offer, as it said about a minute
// before the vote (protocol.h: next_maps): each one's name in the vote and its Thunderstore
// package (may be empty). None outside such a server, and after the map changed. Thread-safe;
// the serial changes whenever the list does.
struct NextMapWanted {
    std::string name, package;
};
std::vector<NextMapWanted> next_maps();
unsigned next_maps_serial() noexcept;
void set_map_package_lookup(MapPackageLookup lookup) noexcept;
// The session's chat lines for the overlay. Thread-safe and cheap: callable every frame.
MultiplayerChat chat();
} // namespace dingosdk::multiplayer
