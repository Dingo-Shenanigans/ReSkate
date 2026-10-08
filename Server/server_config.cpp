#include "server_config.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Platform/path_text.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include "Engine/Game/World/world_names.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace dingosdk::server {
namespace {
std::string placement_text(ObjectPlacement policy) {
    return policy == ObjectPlacement::everyone ? "everyone" : policy == ObjectPlacement::host_only ? "admins" : "nobody";
}
Json to_json(const ServerConfig &c) {
    auto root = Json::object();
    root["name"] = c.name;
    root["map"] = c.map;
    auto pool = Json::array();
    for (const auto &map : c.map_pool) pool.push_back(map);
    root["map_pool"] = std::move(pool);
    root["map_rotation_minutes"] = c.map_rotation;
    root["max_players"] = c.max_players;
    root["reserved_slots"] = c.reserved_slots;
    root["crowd_budget"] = c.crowd_budget;
    root["bone_scale_limit"] = c.bone_scale_limit;
    root["send_rate"] = c.send_rate;
    auto reserved = Json::array();
    for (const auto id : c.reserved) reserved.push_back(std::to_string(id)); // as strings, like the admins
    root["reserved"] = std::move(reserved);
    root["password"] = c.password;
    root["welcome"] = c.welcome;
    root["listed"] = c.listed;
    root["steam_token"] = c.steam_token;
    root["auto_update"] = c.auto_update;
    root["global_bans"] = c.global_bans;
    root["activity_log"] = c.activity_log;
    root["announce_throwdowns"] = c.announce_throwdowns;
    root["parties"] = c.parties;
    root["party_size"] = c.party_size;
    root["speed_check"] = c.speed_check;
    root["score_check"] = c.score_check;
    auto allowed = Json::array();
    for (const auto fingerprint : c.score_allow) allowed.push_back(scoring_text(fingerprint));
    root["score_allow"] = std::move(allowed);
    root["port"] = static_cast<unsigned>(c.port);
    root["query_port"] = static_cast<unsigned>(c.query_port);
    root["tps"] = c.tps;
    root["voice_chat"] = c.voice_chat;
    root["voice_range"] = static_cast<double>(c.voice_range);
    auto distances = Json::object();
    distances["full_rate_return"] = c.distances.full_rate_return;
    distances["half_rate_start"] = c.distances.half_rate_start;
    distances["half_rate_return"] = c.distances.half_rate_return;
    distances["low_rate_start"] = c.distances.low_rate_start;
    root["distances"] = std::move(distances);
    root["object_placement"] = placement_text(c.object_placement);
    root["object_limit"] = c.object_limit;
    root["noclip"] = c.noclip;
    root["no_bail"] = c.no_bail;
    root["boosts"] = c.boosts;
    root["enforce_tuning"] = c.enforce_tuning;
    auto votes = Json::object();
    const auto vote = [](const VoteSetting &v) {
        auto item = Json::object();
        item["enabled"] = v.enabled;
        item["percent"] = v.percent;
        return item;
    };
    votes["map"] = vote(c.votes.map);
    votes["kick"] = vote(c.votes.kick);
    votes["time_of_day"] = vote(c.votes.time);
    votes["seconds"] = c.votes.seconds;
    votes["cooldown_seconds"] = c.votes.cooldown;
    root["votes"] = std::move(votes);
    auto parks = Json::object();
    for (unsigned lot = 0; lot < park_lots.size(); ++lot) parks[park_lots[lot].key] = c.parks[lot];
    root["parks"] = std::move(parks);
    root["world_layer_sync"] = c.world_layer_sync;
    auto layers = Json::object();
    for (const auto &[key, mode] : c.layers) layers[key] = mode;
    root["layers"] = std::move(layers);
    // SteamID64s are written as strings: JSON readers often lose 64-bit precision.
    auto admins = Json::array();
    for (const auto id : c.admins) admins.push_back(std::to_string(id));
    root["admins"] = std::move(admins);
    auto bans = Json::array();
    for (const auto &ban : c.bans) {
        auto row = Json::object();
        row["id"] = std::to_string(ban.id);
        row["name"] = ban.name;
        row["added"] = ban.added;
        bans.push_back(std::move(row));
    }
    root["bans"] = std::move(bans);
    return root;
}
std::uint64_t steam_id(const Json &value) {
    if (value.is_string()) return std::stoull(value.string());
    return value.get<std::uint64_t>();
}
// The settings `expected` has that `file` lacks, nested objects included.
void missing_settings(const Json &file, const Json &expected, const std::string &prefix, std::vector<std::string> &out) {
    for (const auto &[key, value] : expected.items()) {
        if (!file.contains(key)) out.push_back(prefix + key);
        else if (value.is_object() && file.at(key).is_object()) missing_settings(file.at(key), value, prefix + key + ".", out);
    }
}
} // namespace
ServerConfig load_config(const std::filesystem::path &file, std::vector<std::string> *added) {
    ServerConfig c;
    c.file = file;
    if (!std::filesystem::exists(file)) {
        save_config(c);
        return c;
    }
    std::stringstream text;
    {
        // Closed before any write-back: Windows cannot replace a file that is still open.
        std::ifstream in(file, std::ios::binary);
        text << in.rdbuf();
    }
    const auto root = Json::parse(text.str());
    if (!root.is_object()) throw std::runtime_error("The server config must be a JSON object.");
    c.name = root.value("name", c.name);
    c.map = root.value("map", c.map);
    if (root.contains("map_pool") && root.at("map_pool").is_array())
        for (const auto &map : root.at("map_pool"))
            if (map.is_string() && !map.string().empty()) c.map_pool.push_back(map.string());
    c.map_rotation = std::min(root.value("map_rotation_minutes", c.map_rotation), max_map_rotation);
    c.max_players = root.value("max_players", c.max_players);
    c.reserved_slots = root.value("reserved_slots", c.reserved_slots);
    c.crowd_budget = root.value("crowd_budget", c.crowd_budget);
    c.bone_scale_limit = root.value("bone_scale_limit", c.bone_scale_limit);
    c.send_rate = root.value("send_rate", c.send_rate);
    if (root.contains("reserved") && root.at("reserved").is_array())
        for (const auto &id : root.at("reserved")) c.reserved.push_back(steam_id(id));
    c.password = root.value("password", c.password);
    c.welcome = root.value("welcome", c.welcome);
    c.listed = root.value("listed", c.listed);
    c.steam_token = root.value("steam_token", c.steam_token);
    c.auto_update = root.value("auto_update", c.auto_update);
    c.global_bans = root.value("global_bans", c.global_bans);
    c.activity_log = root.value("activity_log", c.activity_log);
    c.announce_throwdowns = root.value("announce_throwdowns", c.announce_throwdowns);
    c.parties = root.value("parties", c.parties);
    c.party_size = std::clamp(root.value("party_size", c.party_size), 2U, 8U);
    c.speed_check = root.value("speed_check", c.speed_check);
    if (c.speed_check != "off" && c.speed_check != "warn" && c.speed_check != "kick") c.speed_check = "warn";
    c.score_check = root.value("score_check", c.score_check);
    if (c.score_check != "off" && c.score_check != "warn" && c.score_check != "kick") c.score_check = "warn";
    if (root.contains("score_allow") && root.at("score_allow").is_array())
        for (const auto &value : root.at("score_allow"))
            if (value.is_string())
                if (const auto fingerprint = parse_scoring(value.string()))
                    c.score_allow.push_back(*fingerprint);
    // Checked here, not in config_error: once narrowed, 70000 is just port 4464, and the
    // next save would write that over the owner's typo.
    const auto read_port = [&](const char *key, std::uint16_t fallback) {
        const auto value = root.value(key, static_cast<unsigned>(fallback));
        if (value < 1 || value > 65535) throw std::runtime_error(std::string(key) + " must be 1 to 65535.");
        return static_cast<std::uint16_t>(value);
    };
    c.port = read_port("port", c.port);
    c.query_port = read_port("query_port", c.query_port);
    c.tps = dedicated_tps; // not the file's (see ServerConfig::tps)
    c.voice_chat = root.value("voice_chat", c.voice_chat);
    c.voice_range = root.value("voice_range", c.voice_range);
    if (root.contains("distances")) {
        const auto &d = root.at("distances");
        c.distances.full_rate_return = d.value("full_rate_return", c.distances.full_rate_return);
        c.distances.half_rate_start = d.value("half_rate_start", c.distances.half_rate_start);
        c.distances.half_rate_return = d.value("half_rate_return", c.distances.half_rate_return);
        c.distances.low_rate_start = d.value("low_rate_start", c.distances.low_rate_start);
    }
    c.noclip = root.value("noclip", c.noclip);
    c.no_bail = root.value("no_bail", c.no_bail);
    c.boosts = root.value("boosts", c.boosts);
    c.enforce_tuning = root.value("enforce_tuning", c.enforce_tuning);
    if (root.contains("votes") && root.at("votes").is_object()) {
        const auto &votes = root.at("votes");
        const auto read_vote = [&](const char *key, VoteSetting &v) {
            if (!votes.contains(key) || !votes.at(key).is_object()) return;
            const auto &item = votes.at(key);
            v.enabled = item.value("enabled", v.enabled);
            v.percent = std::clamp(item.value("percent", v.percent), 1U, 100U);
        };
        read_vote("map", c.votes.map);
        read_vote("kick", c.votes.kick);
        read_vote("time_of_day", c.votes.time);
        c.votes.seconds = std::clamp(votes.value("seconds", c.votes.seconds), 10U, 300U);
        c.votes.cooldown = std::clamp(votes.value("cooldown_seconds", c.votes.cooldown), 0U, 3600U);
    }
    c.object_limit = root.value("object_limit", c.object_limit);
    const auto placement = root.value("object_placement", placement_text(c.object_placement));
    // On a dedicated server the protocol's "host only" means its admins.
    c.object_placement = placement == "nobody" ? ObjectPlacement::nobody
                       : placement == "admins" || placement == "host" ? ObjectPlacement::host_only : ObjectPlacement::everyone;
    if (root.contains("parks"))
        for (unsigned lot = 0; lot < park_lots.size(); ++lot)
            c.parks[lot] = root.at("parks").value(park_lots[lot].key, c.parks[lot]);
    c.world_layer_sync = root.value("world_layer_sync", c.world_layer_sync);
    if (root.contains("layers") && root.at("layers").is_object())
        for (const auto &[key, mode] : root.at("layers").items())
            if (mode.is_string()) c.layers[key] = mode.string();
    if (root.contains("admins"))
        for (const auto &id : root.at("admins")) c.admins.push_back(steam_id(id));
    if (root.contains("bans"))
        for (const auto &row : root.at("bans"))
            c.bans.push_back({steam_id(row.at("id")), row.value("name", std::string{}),
                              row.value("added", std::int64_t{})});
    // A config from an older version: write the new settings into it so owners can see them.
    std::vector<std::string> missing;
    missing_settings(root, to_json(c), {}, missing);
    if (!missing.empty()) {
        save_config(c);
        if (added) *added = std::move(missing);
    }
    return c;
}
void save_config(const ServerConfig &c) {
    if (c.file.has_parent_path()) std::filesystem::create_directories(c.file.parent_path());
    const auto temporary = std::filesystem::path(c.file).concat(".tmp");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << to_json(c).dump(2) << '\n';
        if (!out) throw std::runtime_error("Cannot write " + path_utf8(temporary));
    }
    std::filesystem::rename(temporary, c.file);
}
std::string scoring_text(std::uint64_t fingerprint) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text(16, '0');
    for (int i = 15; i >= 0; --i, fingerprint >>= 4) text[static_cast<std::size_t>(i)] = digits[fingerprint & 15];
    return text;
}
std::optional<std::uint64_t> parse_scoring(std::string_view text) {
    if (text.empty() || text.size() > 16) return std::nullopt;
    std::uint64_t value{};
    for (const auto c : text) {
        const auto digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0) return std::nullopt;
        value = value << 4 | static_cast<std::uint64_t>(digit);
    }
    if (!value) return std::nullopt;
    return value;
}
bool may_join(const ServerConfig &config, std::uint64_t id, std::size_t on) noexcept {
    if (on >= config.max_players) return false;
    const auto listed = [&](const std::vector<std::uint64_t> &ids) { return std::find(ids.begin(), ids.end(), id) != ids.end(); };
    return on + config.reserved_slots < config.max_players || listed(config.reserved) || listed(config.admins);
}
std::string config_error(const ServerConfig &c) {
    using namespace multiplayer;
    if (!valid_server_name(c.name)) return std::string("name must be ") + server_name_rule + ".";
    for (const auto id : c.reserved)
        if (!individual_steam_id(id)) return "reserved must be SteamID64s (17 digits starting 7656119).";
    if (c.reserved_slots >= c.max_players) return "reserved_slots must be less than max_players, so that anyone can join at all.";
    if (c.reserved.size() > 1024) return "reserved holds at most 1024 players.";
    if (c.send_rate < 128 || c.send_rate > 16384) return "send_rate must be 128 to 16384 (KB/s for each player).";
    if (c.bone_scale_limit != 0 && !(c.bone_scale_limit >= 1.f && c.bone_scale_limit <= 8.f))
        return "bone_scale_limit must be 0 (no limit) or 1 to 8 (1: no resized body parts at all).";
    if (!valid_crowd_budget(c.crowd_budget))
        return "crowd_budget must be 0 (no limit) or " + std::to_string(min_crowd_budget) + " to " +
               std::to_string(max_crowd_budget) + ".";
    if (c.steam_token.size() > 64 || !std::all_of(c.steam_token.begin(), c.steam_token.end(), [](unsigned char ch) { return std::isalnum(ch); }))
        return "steam_token must be a game server login token (letters and digits), or empty to sign in anonymously.";
    if (c.map.empty() || !valid_map_destination(map_destination(c.map)) || !installed_map(c.map))
        return "map \"" + c.map + "\" is not a map this server has. Use a name like \"San Vansterdam\", or put the map's mod "
               "folder in Mods next to the server.";
    for (const auto &map : c.map_pool)
        if (!find_level(map) || !valid_map_destination(map_destination(map)))
            return "map_pool: \"" + map + "\" is not a single known map. Use names like \"Isle of Grom\", or put the "
                   "map's mod folder in Mods next to the server.";
    if (c.max_players < 1 || c.max_players + 1 > max_players)
        return "max_players must be 1 to " + std::to_string(max_players - 1) + ".";
    if (c.password.size() > 64) return "password must be at most 64 characters.";
    if (!c.welcome.empty() && !valid_chat_text(c.welcome)) return "welcome must be one chat line (at most 200 bytes).";
    if (c.tps != dedicated_tps) return "tps is " + std::to_string(dedicated_tps) + " on dedicated servers.";
    if (!valid_voice_range(c.voice_range)) return "voice_range must be 50 to 1000.";
    if (!valid_object_limit(c.object_limit)) return "object_limit must be 0 (no limit) to " + std::to_string(max_object_limit) + ".";
    if (!c.distances.valid()) return "distances must be ordered: full_rate_return < half_rate_start <= half_rate_return < low_rate_start <= 10000.";
    for (unsigned lot = 0; lot < park_lots.size(); ++lot)
        if (c.parks[lot].empty() || !valid_park(lot, c.parks[lot]))
            return "parks." + std::string(park_lots[lot].key) + " is not a park layout (e.g. skatepark_01, or empty).";
    if (!c.port || !c.query_port || c.port == c.query_port) return "port and query_port must differ.";
    for (const auto id : c.admins)
        if (!individual_steam_id(id)) return "admins must be SteamID64s (17 digits starting 7656119).";
    return {};
}
namespace {
constexpr std::string_view root_level = "Levels/Game/DingoLevel_Root/DingoLevel_Root";
std::string folded(std::string_view text) {
    std::string result(text);
    for (auto &c : result) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        if (c == '\\') c = '/';
    }
    return result;
}
bool same(std::string_view a, std::string_view b) { return folded(a) == folded(b); }
bool starts(std::string_view text, std::string_view prefix) { return folded(text).starts_with(folded(prefix)); }
std::vector<ServerLevel> &level_list() {
    static std::vector<ServerLevel> value;
    return value;
}
} // namespace
std::vector<std::string> load_levels(const std::filesystem::path &mods) {
    auto &list = level_list();
    list.clear();
    // The retail maps players can skate together; the game names them the same way.
    for (const char *asset : {"Levels/Game/BAM_LevelRoot/BAM_LevelRoot",
                              "Levels/Game/DingoLevel_Isle_of_Grom/DingoLevel_Isle_of_Grom",
                              "Levels/Game/DingoLevel_MPR/DingoLevel_MPR",
                              "Levels/Game/DingoLevel_FTUE_Island/DingoLevel_FTUE_Island",
                              "Levels/Game/DingoLevel_SDM/DingoLevel_SDM_Int_001/DingoLevel_SDM_Int_001",
                              "Levels/Game/DingoLevel_SDM/DingoLevel_SDM_Int_002/DingoLevel_SDM_Int_002"})
        list.push_back({asset, world_level_name(asset)});
    std::vector<std::string> problems;
    std::error_code error;
    if (!std::filesystem::is_directory(mods, error)) return problems;
    for (const auto &entry : std::filesystem::directory_iterator(mods, error)) {
        const auto manifest = entry.path() / "reskate-levels.json";
        if (!entry.is_directory() || !std::filesystem::exists(manifest)) continue;
        try {
            std::ifstream in(manifest, std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            const auto root = Json::parse(text.str());
            for (const auto &level : root.at("levels")) {
                const auto asset = level.at("asset").string();
                const auto name = level.value("displayName", world_level_name(asset));
                if (std::none_of(list.begin(), list.end(), [&](const auto &l) { return same(l.asset, asset); }))
                    list.push_back({asset, name.empty() ? world_level_name(asset) : name});
            }
        } catch (const std::exception &e) {
            problems.push_back(entry.path().filename().string() + ": " + e.what());
        }
    }
    return problems;
}
const std::vector<ServerLevel> &levels() { return level_list(); }
const ServerLevel *find_level(std::string_view map) {
    if (map.empty()) return nullptr;
    for (const auto &level : level_list())
        if (same(level.asset, map)) return &level;
    for (const bool exact : {true, false}) {
        const ServerLevel *result{};
        for (const auto &level : level_list()) {
            const auto alias = world_level_short_name(level.asset);
            if (exact ? (same(level.name, map) || same(alias, map))
                      : (starts(level.name, map) || starts(alias, map) || starts(level.asset, map))) {
                if (result) return nullptr; // ambiguous
                result = &level;
            }
        }
        if (result) return result;
    }
    return nullptr;
}
bool installed_map(std::string_view map) {
    const auto destination = map_destination(map);
    const auto asset = world_destination_asset(destination);
    return !asset.empty() && std::any_of(level_list().begin(), level_list().end(), [&](const auto &level) { return same(level.asset, asset); });
}
std::string map_destination(std::string_view map) {
    if (map.find('|') != std::string_view::npos) return std::string(map);
    if (const auto *level = find_level(map)) return std::string(root_level) + "|" + level->asset;
    // A level path the server has no manifest for: players with that map can still load it.
    if (map.find('/') != std::string_view::npos) return std::string(root_level) + "|" + std::string(map);
    return {};
}
std::string map_setting(std::string_view map) {
    std::string_view level = map;
    if (const auto split = map.find('|'); split != std::string_view::npos) {
        const auto root = map.substr(0, split), detached = map.substr(split + 1);
        // Anything not loaded into the usual root level keeps its full destination.
        if (!same(root, root_level) || detached.empty()) return std::string(map);
        level = detached;
    }
    if (const auto *known = find_level(level)) return known->name;
    return std::string(level);
}
std::string map_label(std::string_view map) {
    if (const auto *level = find_level(map)) return level->name;
    return world_level_name(world_destination_asset(map_destination(map)));
}
std::vector<const ServerLevel *> pool_levels(const ServerConfig &config) {
    std::vector<const ServerLevel *> pool;
    const auto add = [&](const ServerLevel *level) {
        if (level && multiplayer::valid_map_destination(map_destination(level->asset)) &&
            std::find(pool.begin(), pool.end(), level) == pool.end())
            pool.push_back(level);
    };
    if (config.map_pool.empty())
        for (const auto &level : level_list()) add(&level);
    for (const auto &map : config.map_pool) add(find_level(map));
    return pool;
}
bool in_map_pool(const ServerConfig &config, std::string_view map) {
    if (config.map_pool.empty()) return true;
    const auto pool = pool_levels(config);
    const auto *level = find_level(map);
    return level && std::find(pool.begin(), pool.end(), level) != pool.end();
}
const ServerLevel *next_pool_map(const ServerConfig &config, std::string_view map) {
    const auto pool = pool_levels(config);
    const auto *current = find_level(map);
    const auto at = static_cast<std::size_t>(std::find(pool.begin(), pool.end(), current) - pool.begin());
    for (std::size_t step = 1; step <= pool.size(); ++step) {
        const auto *next = at == pool.size() ? pool[step - 1] : pool[(at + step) % pool.size()];
        if (next != current) return next;
    }
    return nullptr;
}
} // namespace dingosdk::server
