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
    unsigned seconds{};       // how long it runs; 0: VoteSettings::seconds
    unsigned cooldown{};      // seconds before its starter may start another vote; 0: VoteSettings::cooldown
    unsigned min_players = 1; // players on before anyone may start it
};
// A vote the server's owner defines: "/vote <name> [choice]" runs `command` (any server
// command, as the console) when it passes. In the command {map} is the current map and {arg}
// the choice the starter picked, one of `choices`; without choices the vote takes no argument.
struct CustomVote {
    std::string name;        // 1-16 of a-z 0-9 - _
    std::string description; // shown in /help and the "/" menu
    std::string command;     // "map {map}", "noclip {arg}"...
    std::vector<std::string> choices;
    VoteSetting setting{true, 60};
};
struct VoteSettings {
    VoteSetting map, kick, time{false, 50};
    std::vector<CustomVote> custom;
    unsigned seconds = 30;  // how long a vote runs
    unsigned cooldown = 60; // seconds before the same player may start another
    bool starter_votes_yes = true; // whoever starts a vote has voted yes
    // Polls: questions with up to six answers that run nothing. "off", "admins" or "everyone".
    std::string polls = "admins";
    unsigned poll_seconds = 60;
};
inline constexpr std::size_t max_announcements = 32;
inline constexpr unsigned max_announcement_interval = 1440; // minutes
// Messages the server posts by itself, one every `interval` minutes in turn while players are on.
struct Announcements {
    std::vector<std::string> messages;
    unsigned interval{}; // minutes; 0: off
};
inline constexpr std::size_t max_custom_commands = 32;
inline constexpr std::size_t max_custom_command_runs = 8;
// The owner's own chat commands ("/discord"), never listed in /help. Each answers whoever typed
// it with `reply`, and/or runs `commands` as the console: {player} is their SteamID64, {map}
// the current map, {arg} the rest of what they typed.
struct CustomCommand {
    std::string name; // 1-16 of a-z 0-9 - _
    std::string reply;
    std::vector<std::string> commands;
    bool admin{}; // only admins may use it
};
// ReSkateServer.json. Every setting an admin or the console changes is saved
// back, so a restart keeps it.
inline constexpr unsigned dedicated_tps = 20;
struct ServerConfig {
    std::filesystem::path file;
    std::string name = "ReSkate server";
    // The map everyone skates, named like the game's `load` command: "San Vansterdam",
    // "Isle of Grom", or a custom map in Mods\ such as "bbcity".
    std::string map = "San Vansterdam";
    std::vector<std::string> map_pool; // maps for votes and the rotation, in order; empty: every map
    unsigned map_rotation = 0;         // minutes per map before the next pool map (0: off)
    unsigned max_players = 16; // players; the server itself is not one
    // The most poses a second one player is sent (crowd_limits); 0: no limit.
    unsigned crowd_budget = crowd_pose_budget;
    // The most a mod may resize part of a skater for the other players, as a factor (and its
    // inverse the least). The game's own skater height is a scale as well, so 1 shows every
    // skater at one height and build; 2, the default, leaves height alone. 0 is no limit.
    float bone_scale_limit = 2;
    // How far (metres) a bone of a skater's body or board may be from the one it hangs from
    // for the other players (limit_bone_reach). 0: no limit.
    float bone_reach_limit = 1;
    // How players reach the server: true, through Steam's relay network only; false, straight
    // to `port` (UDP). A direct server still answers through the relays, for a player the port
    // does not reach, one who has turned direct connections off, or an older game.
    bool use_steam_relay = true;
    // Logs what Steam's networking says it is doing, to find out why connections fail.
    bool steam_debug = false;
    // How long a message to a player may wait to share a packet with the next ones, in
    // milliseconds (0: each goes at once in a packet of its own). Fewer, fuller packets:
    // less sent for the same updates, and less work sending it.
    unsigned pack_ms = 10;
    // How many threads share the sending of each pass, this one included (1: the one thread,
    // as before 2.0.2). 0: one for each of the machine's processors but one, up to 8.
    unsigned threads = 0;
    // Past this many metres a player's fingers are not sent moving (0: always). A skater's
    // forty finger bones turn in nearly every pose and are half of what a pose carries.
    unsigned finger_distance = 25;
    // What the server may send each player, in KB/s (128-16384).
    unsigned send_rate = 900;
    // The players with a reserved slot: they can join a full server, as the admins can.
    std::vector<std::uint64_t> reserved;
    std::string password;      // empty: anyone may join
    std::string welcome;       // sent to each player as they join
    // The colours of the server's own lines in chat, as "#RRGGBB": its badge and name, and the
    // text after them.
    std::string chat_color = "#8E5CFF", chat_text_color = "#D9C8FF";
    bool listed = true;        // shown in the in-game server browser
    // A Steam game server login token (steamcommunity.com/dev/managegameservers, app 3354750).
    // With one the server signs in to its own account and keeps the same Steam ID every start,
    // which is how the ReSkate team's list of official servers knows it. Empty: anonymous.
    std::string steam_token;
    bool auto_update = true;   // install new releases when nobody is on
    bool global_bans = true;   // turn away players the ReSkate team has banned (global_bans.h)
    bool activity_log = true;  // console lines for throwdowns, objects and loading
    bool announce_throwdowns = true; // tell everyone in chat when a throwdown drop is placed
    // Players form parties (/party, the game's Social menu): 2-8 players each (the game's Party
    // panel has eight rows). Off, nobody can be in one.
    bool parties = true;
    unsigned party_size = 8;
    // Minutes a player may be away (not moving, talking, typing or building) before the server
    // removes them, 1 to 1440; 0: never. Admins are never removed for it.
    unsigned afk_kick = 0;
    // A chat message with a word the ReSkate team does not allow at all (word_lists.h) is never
    // passed on. Its player is warned; after this many warnings, 1 to 10, the next gets them
    // kicked. 0: nobody is warned or kicked, and the message is still not passed on.
    unsigned word_warnings = 3;
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
    // Fixed for dedicated servers for now (dedicated_tps): a busy one's traffic, in and out,
    // is its players' poses, and at 20 a second that is a third less than at 30. Whatever
    // the file says is read as this; lobbies keep their own choice.
    unsigned tps = dedicated_tps;
    bool voice_chat = true;
    float voice_range = default_voice_range;
    MultiplayerDistances distances;
    // everyone, admins (only the admins below may build) or nobody.
    ObjectPlacement object_placement = ObjectPlacement::everyone;
    // Objects each player may have placed (object_placement.h); 0: no limit. Admins are not held to it.
    unsigned object_limit = default_object_limit;
    // Players may place objects at another size than their own. Off: every player's objects
    // are shared at their own size; admins may still resize theirs.
    bool object_scaling = true;
    // Players see each other's skater effects: sparks and dust where a skater touches the world,
    // and the trails and fire of costumes and skateboards. Off: nothing of them is relayed and
    // players' games show each other without them.
    bool sync_effects = true;
    // Whether players may use noclip (and teleport) / No Bail / the boosts (admins always may).
    bool noclip = true, no_bail = true, boosts = true;
    // Players skate with the game's own physics tuning, not copies they edited.
    bool enforce_tuning = true;
    VoteSettings votes; // all off until the owner turns them on
    Announcements announcements;
    std::vector<CustomCommand> commands;
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
// Where the bans are kept: data/bans.json, beside the config.
std::filesystem::path bans_file(const ServerConfig &config);
// How many players may be on beyond max_players: one for each reserved player and each admin.
std::size_t extra_slots(const ServerConfig &config) noexcept;
// Whether a player who is not yet on may join a server with `on` players on it. Anyone, until
// max_players are on; the reserved players and the admins after that too, in the extra slots
// (a full server of 32 shows 33/32 with one of them on).
bool may_join(const ServerConfig &config, std::uint64_t id, std::size_t on) noexcept;
// "#RRGGBB" (or "RRGGBB") as a colour in the layout the protocol and the overlay use, or nothing.
std::optional<std::uint32_t> parse_colour(std::string_view text) noexcept;
// Why `config` cannot run, or empty.
std::string config_error(const ServerConfig &config);
// Why the owner's custom votes cannot run, or empty; and whether a custom vote may be called
// `name` (not a word "/vote" already takes: map, kick, tod, yes, poll...).
std::string custom_votes_error(const std::vector<CustomVote> &votes);
// Why the owner's custom chat commands cannot run, or empty; and whether a chat command may be
// called `name` (not one players or admins already type: help, party, vote, kick, map...).
bool custom_command_name_free(std::string_view name) noexcept;
std::string custom_commands_error(const std::vector<CustomCommand> &commands);
bool custom_vote_name_free(std::string_view name) noexcept;
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
// Whether the server has this map (a name, level path or destination): one of the game's own,
// or one a mod folder in its Mods lists. It only moves players to a map it has itself.
bool installed_map(std::string_view map);
// "a.b.c.d" as a number (a the highest byte), or 0 when it is not an IPv4 address.
std::uint32_t direct_ipv4(std::string_view text) noexcept;
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
