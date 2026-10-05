#pragma once
#include "Engine/Game/Multiplayer/distance_settings.h"
#include "Engine/Game/Multiplayer/object_placement.h"
#include "Engine/Game/Multiplayer/session_model.h"
#include "Engine/Game/Multiplayer/tick_settings.h"
#include "Engine/Game/Multiplayer/voice_settings.h"
#include "Engine/Game/World/park_rotation.h"
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::server {
// One kind of player vote: whether players may start it, and the share of connected players
// (percent, 1-100) whose yes passes it.
struct VoteSetting {
    bool enabled{};
    unsigned percent = 60;
};
struct VoteSettings {
    VoteSetting map, kick, time{false, 50};
    unsigned seconds = 30;  // how long a vote runs
    unsigned cooldown = 60; // seconds before the same player may start another
};
// ReSkateServer.json. Every setting an admin or the console changes is saved
// back, so a restart keeps it.
struct ServerConfig {
    std::filesystem::path file;
    std::string name = "ReSkate server";
    // The map everyone skates, named like the game's `load` command: "San Vansterdam",
    // "Isle of Grom", or a custom map in Mods\ such as "bbcity".
    std::string map = "San Vansterdam";
    std::vector<std::string> map_pool; // maps for votes and the rotation, in order; empty: every map
    unsigned map_rotation = 0;         // minutes per map before the next pool map (0: off)
    unsigned max_players = 16; // players; the server itself is not one
    std::string password;      // empty: anyone may join
    std::string welcome;       // sent to each player as they join
    bool listed = true;        // shown in the in-game server browser
    bool auto_update = true;   // install new releases when nobody is on
    bool global_bans = true;   // turn away players the ReSkate team has banned (global_bans.h)
    bool activity_log = true;  // console lines for throwdowns, objects and loading
    bool announce_throwdowns = true; // tell everyone in chat when a throwdown drop is placed
    // Players form parties (/party, the game's Social menu): 2-8 players each (the game's Party
    // panel has eight rows). Off, nobody can be in one.
    bool parties = true;
    unsigned party_size = 8;
    // Players whose game runs fast (a speedhack; Server/speed_check.h): "warn" takes them out of
    // throwdowns and coop challenges and tells the admins, "kick" also removes them, "off" does not check.
    std::string speed_check = "warn";
    // Players whose mods change how tricks score (Engine/Vfs/mod_scoring.h): "warn" takes them out
    // of throwdowns and coop challenges (the server stops relaying theirs) and tells everyone,
    // "kick" removes them, "off" does not check.
    std::string score_check = "warn";
    // Scoring fingerprints accepted besides the game's own (a server that runs on a scoring mod
    // everyone installs). Each player's fingerprint is in the console when they are flagged.
    std::vector<std::uint64_t> score_allow;
    std::uint16_t port = 27015, query_port = 27016;
    unsigned tps = multiplayer_default_tps;
    bool voice_chat = true;
    float voice_range = default_voice_range;
    MultiplayerDistances distances;
    // everyone, admins (only the admins below may build) or nobody.
    ObjectPlacement object_placement = ObjectPlacement::everyone;
    // Whether players may use noclip (and teleport) / No Bail / the boosts (admins always may).
    bool noclip = true, no_bail = true, boosts = true;
    // Players skate with the game's own physics tuning, not copies they edited.
    bool enforce_tuning = true;
    VoteSettings votes; // all off until the owner turns them on
    ParkChoices parks{"skatepark_01", "megapark_05", "flumppark_08"};
    // Forced on every player while world_layer_sync is on: layer key -> mode.
    // Needs world-layers.json (the players' catalog) next to the server.
    bool world_layer_sync{};
    std::map<std::string, std::string> layers;
    std::vector<std::uint64_t> admins;
    std::vector<MultiplayerBan> bans;
};
// Reads `file`, writing a default one first when it does not exist. Settings this version
// has that the file lacks (added by an update) are written back with their defaults, and
// their names go to `added` ("votes.seconds" for a nested one).
ServerConfig load_config(const std::filesystem::path &file, std::vector<std::string> *added = nullptr);
void save_config(const ServerConfig &config);
// Why `config` cannot run, or empty.
std::string config_error(const ServerConfig &config);
// A scoring fingerprint as the config and console write it (16 hex digits), and read back
// (nothing for text that is not one, or for 0: the game's own scoring needs no entry).
std::string scoring_text(std::uint64_t fingerprint);
std::optional<std::uint64_t> parse_scoring(std::string_view text);

// Maps. The game always has its root level (DingoLevel_Root) loaded, and a map
// is the level loaded into it, named as the game's own `load` command names it:
// "San Vansterdam", "Isle of Grom", a custom map's "bbcity"...
struct ServerLevel {
    std::string asset, name;
};
// The retail maps, and the custom maps in `mods`\<mod>\reskate-levels.json
// (players' map mods, copied next to the server). Returns why a mod was skipped.
std::vector<std::string> load_levels(const std::filesystem::path &mods);
const std::vector<ServerLevel> &levels();
// Like the game's `load`: a level path, a name or short name, or the unique start of one.
const ServerLevel *find_level(std::string_view map);
// What players load for a map, as the protocol carries it ("<root>|<level>").
// Empty when the map is unknown (a full level path is always accepted).
std::string map_destination(std::string_view map);
// The name to store for a map, a level path or a destination (an in-game admin
// sends destinations).
std::string map_setting(std::string_view map);
// A map's name for people: "San Vansterdam".
std::string map_label(std::string_view map);
std::vector<const ServerLevel *> pool_levels(const ServerConfig &config); // known pool maps once each; all when empty
bool in_map_pool(const ServerConfig &config, std::string_view map);
const ServerLevel *next_pool_map(const ServerConfig &config, std::string_view map); // after `map`; null if no other
} // namespace dingosdk::server
