#include "server_config.h"
#include <mutex>
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Platform/path_text.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include "Engine/Game/World/world_names.h"
#include <algorithm>
#include <initializer_list>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace dingosdk::server {
namespace {
std::string placement_text(ObjectPlacement policy) {
    return policy == ObjectPlacement::everyone ? "everyone" : policy == ObjectPlacement::host_only ? "admins" : "nobody";
}
// The config file as it is written: sections, and the settings in each, in the order an owner
// reads them (a Json object alone would put them in alphabetical order).
struct Layout {
    std::vector<std::string> names;
    std::vector<Layout> members;
    Json value;
    bool leaf{};
    Layout &section(std::string name) {
        names.push_back(std::move(name));
        return members.emplace_back();
    }
    void set(std::string name, Json json) {
        names.push_back(std::move(name));
        auto &member = members.emplace_back();
        member.value = std::move(json);
        member.leaf = true;
    }
};
void write(std::string &out, const Layout &layout, std::size_t depth) {
    const std::string inner(2 * (depth + 1), ' ');
    out += "{\n";
    for (std::size_t i = 0; i < layout.names.size(); ++i) {
        out += inner + Json(layout.names[i]).dump() + ": ";
        if (layout.members[i].leaf) {
            // A list or an object of its own spans lines: each goes in by this setting's depth.
            for (const char c : layout.members[i].value.dump(2)) {
                out += c;
                if (c == '\n') out += inner;
            }
        } else {
            write(out, layout.members[i], depth + 1);
        }
        out += i + 1 < layout.names.size() ? ",\n" : "\n";
    }
    out += std::string(2 * depth, ' ') + "}";
}
Layout layout(const ServerConfig &c) {
    // SteamID64s are written as strings: JSON readers often lose 64-bit precision.
    const auto ids = [](const std::vector<std::uint64_t> &list) {
        auto out = Json::array();
        for (const auto id : list) out.push_back(std::to_string(id));
        return out;
    };
    Layout root;
    auto &server = root.section("server");
    server.set("name", c.name);
    server.set("password", c.password);
    server.set("welcome_message", c.welcome);
    server.set("chat_color", c.chat_color);
    server.set("chat_text_color", c.chat_text_color);
    server.set("listed", c.listed);
    server.set("max_players", c.max_players);
    server.set("port", static_cast<unsigned>(c.port));
    server.set("query_port", static_cast<unsigned>(c.query_port));
    server.set("steam_token", c.steam_token);
    server.set("auto_update", c.auto_update);
    server.set("autostart", c.autostart);
    server.set("activity_log", c.activity_log);

    auto &access = root.section("access");
    access.set("reserved_players_slots", ids(c.reserved));
    access.set("use_global_bans", c.global_bans);

    auto &maps = root.section("maps");
    maps.set("map", c.map);
    auto pool = Json::array();
    for (const auto &map : c.map_pool) pool.push_back(map);
    maps.set("pool", std::move(pool));
    maps.set("rotation_minutes", c.map_rotation);
    maps.set("rotation_vote", c.rotation_vote);
    maps.set("rotation_vote_seconds", c.rotation_vote_seconds);
    maps.set("rotation_extend_minutes", c.rotation_extend);
    maps.set("rotation_announce", c.rotation_announce);
    maps.set("latest_versions", c.latest_versions);
    maps.set("latest_check_minutes", c.latest_check_minutes);
    auto parks = Json::object();
    for (unsigned lot = 0; lot < park_lots.size(); ++lot) parks[park_lots[lot].key] = c.parks[lot];
    maps.set("parks", std::move(parks));
    maps.set("world_layer_sync", c.world_layer_sync);
    auto layers = Json::object();
    for (const auto &[key, mode] : c.layers) layers[key] = mode;
    maps.set("layers", std::move(layers));
    auto park_mods = Json::array();
    for (const auto &park : c.park_mods) {
        auto item = Json::object();
        item["name"] = park.name;
        item["folder"] = park.folder;
        item["map"] = park.map;
        item["key"] = park.key;
        park_mods.push_back(std::move(item));
    }
    maps.set("park_mods", std::move(park_mods));
    auto park_limits = Json::object();
    park_limits["parks"] = c.park_limits.parks;
    park_limits["objects"] = c.park_limits.objects;
    maps.set("park_limits", std::move(park_limits));

    auto &players = root.section("players");
    players.set("allow_boosts", c.boosts);
    players.set("allow_no_bail", c.no_bail);
    players.set("allow_noclip", c.noclip);
    players.set("allow_parties", c.parties);
    players.set("party_size", c.party_size);
    players.set("afk_kick_minutes", c.afk_kick);
    players.set("kick_minutes", c.kick_minutes);
    players.set("word_warnings", c.word_warnings);
    players.set("use_global_word_filter", c.global_words);
    players.set("allow_voice_chat", c.voice_chat);
    players.set("voice_range", static_cast<double>(c.voice_range));
    players.set("object_placement", placement_text(c.object_placement));
    players.set("object_limit", c.object_limit);
    players.set("allow_object_scaling", c.object_scaling);
    players.set("sync_effects", c.sync_effects);
    players.set("sync_drones", c.sync_drones);
    players.set("announce_throwdowns", c.announce_throwdowns);
    auto locks = Json::array();
    for (const auto bit : {lock_noclip, lock_nobail, lock_boosts, lock_tuning})
        if (c.throwdown_locks & bit) locks.push_back(lock_tools_text(bit));
    players.set("throwdown_locks", std::move(locks));

    auto &anti_cheat = root.section("anti_cheat");
    anti_cheat.set("speed_hack", c.speed_check);
    anti_cheat.set("pinned_board", c.board_check);
    anti_cheat.set("modified_scoring", c.score_check);
    auto allowed = Json::array();
    for (const auto fingerprint : c.score_allow) allowed.push_back(scoring_text(fingerprint));
    anti_cheat.set("allowed_scoring_mods", std::move(allowed));
    anti_cheat.set("enforce_tuning", c.enforce_tuning);
    anti_cheat.set("bone_scale_limit", c.bone_scale_limit);
    anti_cheat.set("bone_reach_limit", c.bone_reach_limit);

    auto &network = root.section("network");
    network.set("use_steam_relay", c.use_steam_relay);
    network.set("send_rate", c.send_rate);
    network.set("crowd_budget", c.crowd_budget);
    network.set("pack_ms", c.pack_ms);
    network.set("threads", c.threads);
    network.set("finger_distance", c.finger_distance);
    auto &distances = network.section("distances");
    distances.set("full_rate_return", c.distances.full_rate_return);
    distances.set("half_rate_start", c.distances.half_rate_start);
    distances.set("half_rate_return", c.distances.half_rate_return);
    distances.set("low_rate_start", c.distances.low_rate_start);
    network.set("steam_debug", c.steam_debug);

    auto &votes = root.section("votes");
    const auto vote = [&](const char *name, const VoteSetting &v) {
        auto &item = votes.section(name);
        item.set("enabled", v.enabled);
        item.set("percent", v.percent);
        item.set("seconds", v.seconds);
        item.set("cooldown_seconds", v.cooldown);
        item.set("min_players", v.min_players);
    };
    vote("map", c.votes.map);
    vote("kick", c.votes.kick);
    vote("time_of_day", c.votes.time);
    vote("extend_map", c.votes.extend);
    votes.set("seconds", c.votes.seconds);
    votes.set("cooldown_seconds", c.votes.cooldown);
    votes.set("kick_minutes", c.votes.kick_minutes);
    votes.set("starter_votes_yes", c.votes.starter_votes_yes);
    votes.set("polls", c.votes.polls);
    votes.set("poll_seconds", c.votes.poll_seconds);
    auto custom = Json::array();
    for (const auto &v : c.votes.custom) {
        auto item = Json::object();
        item["name"] = v.name;
        item["description"] = v.description;
        item["command"] = v.command;
        auto choices = Json::array();
        for (const auto &choice : v.choices) choices.push_back(choice);
        item["choices"] = std::move(choices);
        item["enabled"] = v.setting.enabled;
        item["percent"] = v.setting.percent;
        item["seconds"] = v.setting.seconds;
        item["cooldown_seconds"] = v.setting.cooldown;
        item["min_players"] = v.setting.min_players;
        custom.push_back(std::move(item));
    }
    votes.set("custom", std::move(custom));

    auto &announcements = root.section("announcements");
    auto messages = Json::array();
    for (const auto &message : c.announcements.messages) messages.push_back(message);
    announcements.set("messages", std::move(messages));
    announcements.set("interval_minutes", c.announcements.interval);

    auto commands = Json::array();
    for (const auto &command : c.commands) {
        auto item = Json::object();
        item["name"] = command.name;
        item["reply"] = command.reply;
        auto runs = Json::array();
        for (const auto &run : command.commands) runs.push_back(run);
        item["command"] = std::move(runs);
        item["admin"] = command.admin;
        commands.push_back(std::move(item));
    }
    root.set("commands", std::move(commands));
    return root;
}
// Bans have a file of their own, beside the config.
Json bans_json(const ServerConfig &c) {
    auto bans = Json::array();
    for (const auto &ban : c.bans) {
        auto row = Json::object();
        row["id"] = std::to_string(ban.id);
        row["name"] = ban.name;
        row["added"] = ban.added;
        bans.push_back(std::move(row));
    }
    return bans;
}
void write_file(const std::filesystem::path &file, const std::string &text) {
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path());
    const auto temporary = std::filesystem::path(file).concat(".tmp");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << text << '\n';
        if (!out) throw std::runtime_error("Cannot write " + path_utf8(temporary));
    }
    std::filesystem::rename(temporary, file);
}
std::uint64_t steam_id(const Json &value) {
    if (value.is_string()) return std::stoull(value.string());
    return value.get<std::uint64_t>();
}
// The roles in data/roles.json, or the ones a server starts with when there is no such file.
Roles roles_on_disk(const ServerConfig &c, bool &fresh) {
    const auto roles = roles_file(c);
    fresh = !std::filesystem::exists(roles);
    if (fresh) return default_roles();
    std::stringstream held;
    {
        std::ifstream in(roles, std::ios::binary);
        held << in.rdbuf();
    }
    try {
        return read_roles(held.str());
    } catch (const std::exception &e) {
        // Never started with nobody in charge because the file has a typo in it.
        throw std::runtime_error("Cannot read " + path_utf8(roles) + ": " + e.what());
    }
}
} // namespace
ServerConfig load_config(const std::filesystem::path &file, std::vector<std::string> *added) {
    ServerConfig c;
    c.file = file;
    if (!std::filesystem::exists(file)) {
        bool fresh{};
        c.roles = roles_on_disk(c, fresh); // a config written anew keeps the roles there are
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
    // A setting is in its section under its name. A config from before the sections has it at the
    // top, under that name or an older one: it is read from there, and the file is written back
    // as it is laid out now. A setting found nowhere is new, and the owner is told of it.
    std::vector<std::string> missing;
    bool moved{};
    using Old = std::initializer_list<std::string_view>;
    const auto find = [&](std::string_view section, std::string_view key, Old old = {}) -> const Json * {
        if (root.contains(section) && root.at(section).is_object() && root.at(section).contains(key)) return &root.at(section).at(key);
        if (root.contains(key) && !root.at(key).is_null() && key != "votes") {
            moved = true;
            return &root.at(key);
        }
        for (const auto name : old)
            if (root.contains(name)) {
                moved = true;
                return &root.at(name);
            }
        missing.push_back(std::string(section) + "." + std::string(key));
        return nullptr;
    };
    const auto get = [&]<typename T>(std::string_view section, std::string_view key, T fallback, Old old = {}) {
        const auto *found = find(section, key, old);
        return found ? found->template get<T>() : fallback;
    };
    c.name = get("server", "name", c.name);
    c.password = get("server", "password", c.password);
    c.welcome = get("server", "welcome_message", c.welcome, {"welcome"});
    c.chat_color = get("server", "chat_color", c.chat_color);
    c.chat_text_color = get("server", "chat_text_color", c.chat_text_color);
    c.listed = get("server", "listed", c.listed);
    c.max_players = get("server", "max_players", c.max_players);
    // Checked here, not in config_error: once narrowed, 70000 is just port 4464, and the
    // next save would write that over the owner's typo.
    const auto read_port = [&](const char *key, std::uint16_t fallback) {
        const auto value = get("server", key, static_cast<unsigned>(fallback));
        if (value < 1 || value > 65535) throw std::runtime_error(std::string(key) + " must be 1 to 65535.");
        return static_cast<std::uint16_t>(value);
    };
    c.port = read_port("port", c.port);
    c.query_port = read_port("query_port", c.query_port);
    c.steam_token = get("server", "steam_token", c.steam_token);
    c.auto_update = get("server", "auto_update", c.auto_update);
    c.autostart = get("server", "autostart", c.autostart);
    c.activity_log = get("server", "activity_log", c.activity_log);

    // (Admins were listed here before there were roles: they are moved into the admin role below.)
    // Looked for where either layout kept them, and not as a setting: there is none to add.
    std::vector<std::uint64_t> listed_admins;
    const Json *admins_listed = root.contains("access") && root.at("access").is_object() && root.at("access").contains("admins")
                                    ? &root.at("access").at("admins") : root.contains("admins") ? &root.at("admins") : nullptr;
    if (admins_listed && admins_listed->is_array())
        for (const auto &id : *admins_listed) listed_admins.push_back(steam_id(id));
    if (const auto *reserved = find("access", "reserved_players_slots", {"reserved"}); reserved && reserved->is_array())
        for (const auto &id : *reserved) c.reserved.push_back(steam_id(id));
    c.global_bans = get("access", "use_global_bans", c.global_bans, {"global_bans"});

    c.map = get("maps", "map", c.map);
    if (const auto *pool = find("maps", "pool", {"map_pool"}); pool && pool->is_array())
        for (const auto &map : *pool)
            if (map.is_string() && !map.string().empty()) c.map_pool.push_back(map.string());
    c.map_rotation = std::min(get("maps", "rotation_minutes", c.map_rotation, {"map_rotation_minutes"}), max_map_rotation);
    c.rotation_vote = get("maps", "rotation_vote", c.rotation_vote);
    c.rotation_vote_seconds = std::clamp(get("maps", "rotation_vote_seconds", c.rotation_vote_seconds), 10U, 120U);
    c.rotation_extend = std::min(get("maps", "rotation_extend_minutes", c.rotation_extend), 240U);
    c.rotation_announce = get("maps", "rotation_announce", c.rotation_announce);
    c.latest_versions = get("maps", "latest_versions", c.latest_versions);
    c.latest_check_minutes = std::clamp(get("maps", "latest_check_minutes", c.latest_check_minutes), 5U, 1440U);
    if (const auto *parks = find("maps", "parks"); parks && parks->is_object())
        for (unsigned lot = 0; lot < park_lots.size(); ++lot) c.parks[lot] = parks->value(park_lots[lot].key, c.parks[lot]);
    c.world_layer_sync = get("maps", "world_layer_sync", c.world_layer_sync);
    if (const auto *layers = find("maps", "layers"); layers && layers->is_object())
        for (const auto &[key, mode] : layers->items())
            if (mode.is_string()) c.layers[key] = mode.string();
    if (const auto *parks = find("maps", "park_mods"); parks && !parks->is_null()) {
        if (!parks->is_array()) throw std::runtime_error("park_mods must be a list of parks, each a JSON object.");
        for (const auto &item : *parks) {
            if (!item.is_object()) throw std::runtime_error("park_mods must be a list of parks, each a JSON object.");
            c.park_mods.push_back({item.value("name", std::string{}), item.value("folder", std::string{}),
                                   item.value("map", std::string{}), item.value("key", std::string{})});
        }
    }
    if (const auto *limits = find("maps", "park_limits"); limits && limits->is_object()) {
        c.park_limits.parks = limits->value("parks", c.park_limits.parks);
        c.park_limits.objects = limits->value("objects", c.park_limits.objects);
    }

    c.boosts = get("players", "allow_boosts", c.boosts, {"boosts"});
    c.no_bail = get("players", "allow_no_bail", c.no_bail, {"no_bail"});
    c.noclip = get("players", "allow_noclip", c.noclip, {"noclip"});
    c.parties = get("players", "allow_parties", c.parties, {"parties"});
    c.party_size = std::clamp(get("players", "party_size", c.party_size), 2U, 8U);
    c.afk_kick = get("players", "afk_kick_minutes", c.afk_kick);
    c.kick_minutes = get("players", "kick_minutes", c.kick_minutes);
    c.word_warnings = get("players", "word_warnings", c.word_warnings);
    c.global_words = get("players", "use_global_word_filter", c.global_words);
    c.voice_chat = get("players", "allow_voice_chat", c.voice_chat, {"voice_chat"});
    c.voice_range = get("players", "voice_range", c.voice_range);
    const auto placement = get("players", "object_placement", placement_text(c.object_placement));
    // On a dedicated server the protocol's "host only" means its admins.
    c.object_placement = placement == "nobody" ? ObjectPlacement::nobody
                       : placement == "admins" || placement == "host" ? ObjectPlacement::host_only : ObjectPlacement::everyone;
    c.object_limit = get("players", "object_limit", c.object_limit);
    c.object_scaling = get("players", "allow_object_scaling", c.object_scaling);
    c.sync_effects = get("players", "sync_effects", c.sync_effects);
    c.sync_drones = get("players", "sync_drones", c.sync_drones);
    c.announce_throwdowns = get("players", "announce_throwdowns", c.announce_throwdowns);
    if (const auto *locks = find("players", "throwdown_locks", {}); locks && locks->is_array())
        for (const auto &value : *locks)
            if (value.is_string())
                if (const auto tools = parse_lock_tools(value.string())) c.throwdown_locks |= *tools;

    c.speed_check = get("anti_cheat", "speed_hack", c.speed_check, {"speed_check"});
    if (c.speed_check != "off" && c.speed_check != "warn" && c.speed_check != "kick") c.speed_check = "warn";
    c.board_check = get("anti_cheat", "pinned_board", c.board_check);
    if (c.board_check != "off" && c.board_check != "warn" && c.board_check != "kick") c.board_check = "warn";
    c.score_check = get("anti_cheat", "modified_scoring", c.score_check, {"score_check"});
    if (c.score_check != "off" && c.score_check != "warn" && c.score_check != "kick") c.score_check = "warn";
    if (const auto *allowed = find("anti_cheat", "allowed_scoring_mods", {"score_allow"}); allowed && allowed->is_array())
        for (const auto &value : *allowed)
            if (value.is_string())
                if (const auto fingerprint = parse_scoring(value.string())) c.score_allow.push_back(*fingerprint);
    c.enforce_tuning = get("anti_cheat", "enforce_tuning", c.enforce_tuning);
    c.bone_scale_limit = get("anti_cheat", "bone_scale_limit", c.bone_scale_limit);
    c.bone_reach_limit = get("anti_cheat", "bone_reach_limit", c.bone_reach_limit);

    c.use_steam_relay = get("network", "use_steam_relay", c.use_steam_relay);
    c.send_rate = get("network", "send_rate", c.send_rate);
    c.crowd_budget = get("network", "crowd_budget", c.crowd_budget);
    c.pack_ms = get("network", "pack_ms", c.pack_ms);
    c.threads = get("network", "threads", c.threads);
    c.finger_distance = get("network", "finger_distance", c.finger_distance);
    if (const auto *d = find("network", "distances"); d && d->is_object()) {
        c.distances.full_rate_return = d->value("full_rate_return", c.distances.full_rate_return);
        c.distances.half_rate_start = d->value("half_rate_start", c.distances.half_rate_start);
        c.distances.half_rate_return = d->value("half_rate_return", c.distances.half_rate_return);
        c.distances.low_rate_start = d->value("low_rate_start", c.distances.low_rate_start);
    }
    c.steam_debug = get("network", "steam_debug", c.steam_debug);

    // Votes were a section from the start.
    const Json no_votes = Json::object();
    const auto &votes = root.contains("votes") && root.at("votes").is_object() ? root.at("votes") : no_votes;
    // A vote's own run time, cooldown and player minimum; 0 for the first two is "the votes'".
    const auto vote_limits = [](const Json &item, VoteSetting &v) {
        v.seconds = item.value("seconds", v.seconds);
        if (v.seconds) v.seconds = std::clamp(v.seconds, 10U, 300U);
        v.cooldown = std::min(item.value("cooldown_seconds", v.cooldown), 3600U);
        v.min_players = std::clamp(item.value("min_players", v.min_players), 1U, static_cast<unsigned>(multiplayer::max_players - 1));
    };
    const auto read_vote = [&](const char *key, VoteSetting &v) {
        if (!votes.contains(key) || !votes.at(key).is_object()) return missing.push_back(std::string("votes.") + key);
        const auto &item = votes.at(key);
        for (const char *added : {"seconds", "cooldown_seconds", "min_players"})
            if (!item.contains(added)) missing.push_back(std::string("votes.") + key + "." + added);
        v.enabled = item.value("enabled", v.enabled);
        v.percent = std::clamp(item.value("percent", v.percent), 1U, 100U);
        vote_limits(item, v);
    };
    read_vote("map", c.votes.map);
    read_vote("kick", c.votes.kick);
    read_vote("time_of_day", c.votes.time);
    read_vote("extend_map", c.votes.extend);
    for (const char *key : {"seconds", "cooldown_seconds", "kick_minutes", "starter_votes_yes", "polls", "poll_seconds", "custom"})
        if (!votes.contains(key)) missing.push_back(std::string("votes.") + key);
    c.votes.seconds = std::clamp(votes.value("seconds", c.votes.seconds), 10U, 300U);
    c.votes.cooldown = std::clamp(votes.value("cooldown_seconds", c.votes.cooldown), 0U, 3600U);
    c.votes.kick_minutes = votes.value("kick_minutes", c.votes.kick_minutes);
    c.votes.starter_votes_yes = votes.value("starter_votes_yes", c.votes.starter_votes_yes);
    c.votes.polls = votes.value("polls", c.votes.polls);
    if (c.votes.polls != "off" && c.votes.polls != "admins" && c.votes.polls != "everyone") c.votes.polls = "admins";
    c.votes.poll_seconds = std::clamp(votes.value("poll_seconds", c.votes.poll_seconds), 10U, 600U);
    if (votes.contains("custom") && votes.at("custom").is_array())
        for (const auto &item : votes.at("custom")) {
            if (!item.is_object()) throw std::runtime_error("votes.custom must be a list of votes, each a JSON object.");
            // A vote the owner writes in needs no more than its name and command.
            CustomVote v;
            v.name = item.value("name", std::string{});
            v.description = item.value("description", std::string{});
            v.command = item.value("command", std::string{});
            if (item.contains("choices") && item.at("choices").is_array())
                for (const auto &choice : item.at("choices"))
                    if (choice.is_string()) v.choices.push_back(choice.string());
            v.setting.enabled = item.value("enabled", v.setting.enabled);
            v.setting.percent = std::clamp(item.value("percent", v.setting.percent), 1U, 100U);
            vote_limits(item, v.setting);
            c.votes.custom.push_back(std::move(v));
        }

    // Announcements came after the sections.
    const Json no_announcements = Json::object();
    const auto &announcements = root.contains("announcements") && root.at("announcements").is_object()
                                    ? root.at("announcements") : no_announcements;
    for (const char *key : {"messages", "interval_minutes"})
        if (!announcements.contains(key)) missing.push_back(std::string("announcements.") + key);
    if (announcements.contains("messages") && announcements.at("messages").is_array())
        for (const auto &message : announcements.at("messages"))
            if (message.is_string() && !message.string().empty()) c.announcements.messages.push_back(message.string());
    c.announcements.interval = std::min(announcements.value("interval_minutes", c.announcements.interval), max_announcement_interval);

    // Chat commands came after announcements: a command is a string or a list of them.
    if (!root.contains("commands")) missing.push_back("commands");
    else if (!root.at("commands").is_array()) throw std::runtime_error("commands must be a list of commands, each a JSON object.");
    else
        for (const auto &item : root.at("commands")) {
            if (!item.is_object()) throw std::runtime_error("commands must be a list of commands, each a JSON object.");
            CustomCommand command;
            command.name = item.value("name", std::string{});
            command.reply = item.value("reply", std::string{});
            command.admin = item.value("admin", command.admin);
            if (item.contains("command") && item.at("command").is_string()) command.commands.push_back(item.at("command").string());
            else if (item.contains("command") && item.at("command").is_array())
                for (const auto &run : item.at("command"))
                    if (run.is_string() && !run.string().empty()) command.commands.push_back(run.string());
            c.commands.push_back(std::move(command));
        }

    const auto read_bans = [&](const Json &rows) {
        if (!rows.is_array()) throw std::runtime_error("The bans must be a JSON list.");
        for (const auto &row : rows) {
            const auto id = steam_id(row.at("id"));
            if (std::none_of(c.bans.begin(), c.bans.end(), [&](const auto &ban) { return ban.id == id; }))
                c.bans.push_back({id, row.value("name", std::string{}), row.value("added", std::int64_t{})});
        }
    };
    if (const auto bans = bans_file(c); std::filesystem::exists(bans)) {
        std::stringstream held;
        {
            std::ifstream in(bans, std::ios::binary);
            held << in.rdbuf();
        }
        try {
            read_bans(Json::parse(held.str()));
        } catch (const std::exception &e) {
            // Never started without its bans because their file has a typo in it.
            throw std::runtime_error("Cannot read " + path_utf8(bans) + ": " + e.what());
        }
    }
    // A config from an older version holds the bans itself: they are moved to their own file.
    const bool bans_moved = root.contains("bans");
    if (bans_moved) read_bans(root.at("bans"));
    bool roles_new{};
    c.roles = roles_on_disk(c, roles_new);
    for (const auto id : listed_admins)
        if (c.roles.find("admin") && !c.roles.players.contains(id)) c.roles.players[id] = "admin";
    // New settings, an older layout, or settings that are no longer any: the file is written
    // as it is now, so owners see what there is.
    const bool gone = root.contains("tps") || root.contains("reserved_slots") || root.contains("relay_everything") ||
                      (root.contains("network") && root.at("network").contains("relay_everything"));
    const bool admins_moved = admins_listed != nullptr;
    if (!missing.empty() || moved || bans_moved || gone || roles_new || admins_moved) {
        save_config(c);
        if (added) *added = std::move(missing);
    }
    return c;
}
std::filesystem::path bans_file(const ServerConfig &c) { return c.file.parent_path() / "data" / "bans.json"; }
std::filesystem::path roles_file(const ServerConfig &c) { return c.file.parent_path() / "data" / "roles.json"; }
Roles default_roles() {
    using namespace multiplayer;
    Roles roles;
    const auto add = [&](std::string name, std::string tag, std::string color, int rank, bool all, std::vector<std::string> commands,
                         std::uint32_t permissions, bool inherit = true) {
        Role role;
        role.name = std::move(name);
        role.tag = std::move(tag);
        role.color = std::move(color);
        role.rank = rank;
        role.all_commands = all;
        role.commands = std::move(commands);
        role.permissions = permissions;
        role.inherit = inherit;
        roles.list.push_back(std::move(role));
    };
    // What everybody may do, then the roles above it; and one below it, for a player who is to keep quiet.
    add("player", "", "#FFFFFF", 0, false, {}, role_chat | role_votes | role_parties | role_whisper);
    add("vip", "VIP", "#FFD24A", 10, false, {}, role_reserved_slot);
    add("moderator", "Mod", "#5EC8FF", 50, false,
        {"kick", "ban", "unban", "bans", "players", "msg", "msg-party", "msg-admins", "tphere", "vote-cancel", "announce-to", "role"},
        role_reserved_slot | role_afk | role_vote_immune | role_alerts | role_polls);
    add("admin", "Admin", "#FF74C6", 100, true, {"role", "reserved"}, role_permission_bits);
    add("muted", "", "#FFFFFF", 0, false, {}, role_votes | role_parties, false);
    roles.default_role = "player";
    roles.settle();
    return roles;
}
std::string roles_error(const Roles &roles) {
    using namespace multiplayer;
    if (roles.list.size() > max_roles) return "at most " + std::to_string(max_roles) + " roles.";
    if (roles.players.size() > max_role_players) return "at most " + std::to_string(max_role_players) + " players with a role.";
    const auto plain = [](std::string_view text) {
        std::string out;
        for (const char c : text)
            if (c != ' ') out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c);
        return out;
    };
    for (std::size_t i = 0; i < roles.list.size(); ++i) {
        const auto &role = roles.list[i];
        const auto where = "role \"" + role.name + "\": ";
        if (!valid_role_word(role.name, max_role_name)) return "a role's name is 1 to 24 of a-z, 0-9, - and _ (\"" + role.name + "\").";
        if (role.name == "none") return "a role cannot be called none.";
        for (std::size_t j = 0; j < i; ++j)
            if (roles.list[j].name == role.name) return where + "listed twice.";
        if (!role.tag.empty() && !valid_role_tag(role.tag)) return where + "its tag is at most 16 bytes of plain text.";
        // What the game itself calls the server, a lobby's host, friends and the ReSkate team.
        for (const std::string_view taken : {"server", "reskate", "reskateadmin", "dev", "developer", "contentcreator", "centrix", "staff", "homie", "host", "friend"})
            if (plain(role.tag) == taken) return where + "the tag \"" + role.tag + "\" is one the game uses itself.";
        if (!parse_colour(role.color)) return where + "color must be a colour like #FF74C6.";
        if (role.rank < 0 || role.rank > 1000) return where + "rank is 0 to 1000.";
        if (role.commands.size() > max_role_commands) return where + "too many commands.";
        for (const auto &command : role.commands)
            if (!valid_role_word(command, max_role_command)) return where + "\"" + command + "\" is not a command's name.";
        if (role.permissions & ~role_permission_bits) return where + "unknown permission.";
    }
    if (!roles.default_role.empty() && !roles.find(roles.default_role))
        return "default_role is \"" + roles.default_role + "\", which is not one of the roles.";
    for (const auto &[id, name] : roles.players) {
        if (!individual_steam_id(id)) return "players are SteamID64s (17 digits starting 7656119).";
        if (!roles.find(name)) return "player " + std::to_string(id) + " has the role \"" + name + "\", which is not one of the roles.";
    }
    return {};
}
Roles read_roles(std::string_view text) {
    const auto root = Json::parse(text);
    if (!root.is_object() || !root.contains("roles") || !root.at("roles").is_array())
        throw std::runtime_error("it must be a JSON object with a \"roles\" list.");
    Roles roles;
    for (const auto &item : root.at("roles")) {
        if (!item.is_object()) throw std::runtime_error("each role is a JSON object.");
        Role role;
        role.name = item.value("name", std::string{});
        role.tag = item.value("tag", std::string{});
        role.color = item.value("color", role.color);
        role.rank = static_cast<int>(std::clamp<std::int64_t>(item.value("rank", std::int64_t{}), -1, 1001));
        role.inherit = item.value("inherit", role.inherit);
        if (item.contains("commands")) {
            if (!item.at("commands").is_array()) throw std::runtime_error("role \"" + role.name + "\": commands is a list.");
            for (const auto &command : item.at("commands")) {
                if (!command.is_string()) throw std::runtime_error("role \"" + role.name + "\": commands is a list of names.");
                if (command.string() == "*") role.all_commands = true;
                else if (const std::string name(canonical_command(command.string()));
                         std::find(role.commands.begin(), role.commands.end(), name) == role.commands.end())
                    role.commands.push_back(name);
            }
        }
        if (item.contains("permissions")) {
            if (!item.at("permissions").is_array()) throw std::runtime_error("role \"" + role.name + "\": permissions is a list.");
            for (const auto &permission : item.at("permissions")) {
                const auto known = std::find_if(role_permissions.begin(), role_permissions.end(), [&](const auto &entry) {
                    return permission.is_string() && permission.string() == entry.first;
                });
                if (known == role_permissions.end())
                    throw std::runtime_error("role \"" + role.name + "\": \"" + (permission.is_string() ? permission.string() : std::string("?")) +
                                             "\" is not a permission.");
                role.permissions |= known->second;
            }
        }
        roles.list.push_back(std::move(role));
    }
    roles.default_role = root.value("default_role", std::string{});
    if (root.contains("players")) {
        if (!root.at("players").is_object()) throw std::runtime_error("players is a JSON object of SteamID64: role.");
        for (const auto &[id, name] : root.at("players").items()) {
            if (!name.is_string()) throw std::runtime_error("players is a JSON object of SteamID64: role.");
            roles.players[steam_id(Json(id))] = name.string();
        }
    }
    if (const auto error = roles_error(roles); !error.empty()) throw std::runtime_error(error);
    roles.settle();
    return roles;
}
std::string roles_text(const Roles &roles) {
    auto list = Json::array();
    for (const auto &role : roles.list) {
        auto item = Json::object();
        item["name"] = role.name;
        item["tag"] = role.tag;
        item["color"] = role.color;
        item["rank"] = static_cast<std::int64_t>(role.rank);
        item["inherit"] = role.inherit;
        auto commands = Json::array();
        if (role.all_commands) commands.push_back("*");
        for (const auto &command : role.commands) commands.push_back(command);
        item["commands"] = std::move(commands);
        auto permissions = Json::array();
        for (const auto &[name, bit] : role_permissions)
            if (role.permissions & bit) permissions.push_back(std::string(name));
        item["permissions"] = std::move(permissions);
        list.push_back(std::move(item));
    }
    auto players = Json::object();
    for (const auto &[id, name] : roles.players) players[std::to_string(id)] = name;
    auto root = Json::object();
    root["default_role"] = roles.default_role;
    root["roles"] = std::move(list);
    root["players"] = std::move(players);
    return root.dump(2);
}
void save_config(const ServerConfig &c) {
    // The bans first: a config that held them is only rewritten without them once they are safe.
    write_file(bans_file(c), bans_json(c).dump(2));
    // And the roles: a config that listed the admins is only rewritten without them once they are.
    write_file(roles_file(c), roles_text(c.roles));
    std::string text;
    write(text, layout(c), 0);
    write_file(c.file, text);
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
std::optional<std::uint8_t> parse_lock_tools(std::string_view text) {
    std::uint8_t tools{};
    for (bool more = true; more;) {
        const auto comma = text.find(',');
        more = comma != std::string_view::npos;
        auto name = text.substr(0, comma);
        text = more ? text.substr(comma + 1) : std::string_view{};
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) name.remove_prefix(1);
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.remove_suffix(1);
        std::string lower(name);
        for (auto &c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "noclip" || lower == "teleport") tools |= lock_noclip;
        else if (lower == "nobail") tools |= lock_nobail;
        else if (lower == "boosts") tools |= lock_boosts;
        else if (lower == "tuning") tools |= lock_tuning;
        else if (lower == "all") tools |= lock_all;
        else return std::nullopt;
    }
    if (!tools) return std::nullopt;
    return tools;
}
std::string lock_tools_text(std::uint8_t tools) {
    std::string text;
    for (const auto &[bit, name] : {std::pair{lock_noclip, "noclip"}, std::pair{lock_nobail, "nobail"},
                                    std::pair{lock_boosts, "boosts"}, std::pair{lock_tuning, "tuning"}})
        if (tools & bit) text += (text.empty() ? "" : ", ") + std::string(name);
    return text.empty() ? "none" : text;
}
std::size_t extra_slots(const ServerConfig &config) noexcept {
    // (A player with such a role who is also listed has one slot, not two.)
    std::size_t slots = config.reserved.size();
    for (const auto &[id, name] : config.roles.players)
        if (const auto *role = config.roles.find(name); role && (role->granted & multiplayer::role_reserved_slot) &&
            std::find(config.reserved.begin(), config.reserved.end(), id) == config.reserved.end()) ++slots;
    // Never more players than a session can hold, the server itself being one of them.
    const std::size_t room = multiplayer::max_players - 1;
    return config.max_players >= room ? 0 : std::min<std::size_t>(slots, room - config.max_players);
}
bool may_join(const ServerConfig &config, std::uint64_t id, std::size_t on) noexcept {
    if (on < config.max_players) return true;
    const auto listed = [&](const std::vector<std::uint64_t> &ids) { return std::find(ids.begin(), ids.end(), id) != ids.end(); };
    // (The role they were given: a slot is kept for a player, not for everybody.)
    const auto *role = config.roles.given(id);
    return (listed(config.reserved) || (role && (role->granted & multiplayer::role_reserved_slot))) &&
           on < config.max_players + extra_slots(config);
}
std::optional<std::uint32_t> parse_colour(std::string_view text) noexcept {
    if (!text.empty() && text.front() == '#') text.remove_prefix(1);
    if (text.size() != 6) return std::nullopt;
    std::uint32_t rgb{};
    for (const char c : text) {
        const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0) return std::nullopt;
        rgb = rgb << 4 | static_cast<std::uint32_t>(digit);
    }
    // Written red first; held with red lowest and opaque.
    return 0xff000000U | (rgb & 0xff) << 16 | (rgb & 0xff00) | rgb >> 16;
}
bool custom_vote_name_free(std::string_view name) noexcept {
    // What "/vote <word>" already means.
    for (const std::string_view taken : {"map", "kick", "tod", "time", "extend", "yes", "y", "no", "n", "poll", "list"})
        if (name == taken) return false;
    // A number is an answer to a poll ("/vote 2").
    return !std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; });
}
std::string custom_votes_error(const std::vector<CustomVote> &votes) {
    using namespace multiplayer;
    if (votes.size() > server_custom_vote_limit)
        return "votes.custom: at most " + std::to_string(server_custom_vote_limit) + " votes.";
    for (std::size_t i = 0; i < votes.size(); ++i) {
        const auto &v = votes[i];
        const auto where = "votes.custom \"" + v.name + "\": ";
        if (!valid_server_vote_name(v.name))
            return "votes.custom: each vote needs a name of 1 to 16 lowercase letters, digits, - or _ (\"" + v.name + "\" is not).";
        if (!custom_vote_name_free(v.name)) return where + "that name is one of the server's own votes.";
        for (std::size_t j = 0; j < i; ++j)
            if (votes[j].name == v.name) return where + "two votes have that name.";
        if (!v.description.empty() && (v.description.size() > server_vote_description_bytes || !valid_chat_text(v.description)))
            return where + "description must be one chat line of at most " + std::to_string(server_vote_description_bytes) + " bytes.";
        if (v.command.empty() || !valid_admin_text(v.command)) return where + "command must be one server command, e.g. \"map {map}\".";
        if (v.choices.size() > server_vote_max_choices)
            return where + "at most " + std::to_string(server_vote_max_choices) + " choices.";
        for (const auto &choice : v.choices)
            if (!valid_server_vote_name(choice))
                return where + "each choice is 1 to 16 lowercase letters, digits, - or _ (\"" + choice + "\" is not).";
        const bool takes_argument = v.command.find("{arg}") != std::string::npos;
        if (takes_argument && v.choices.empty()) return where + "a command with {arg} needs a list of choices.";
        if (!takes_argument && !v.choices.empty()) return where + "choices need {arg} in the command, where the choice goes.";
    }
    return {};
}
bool custom_command_name_free(std::string_view name) noexcept {
    // The chat's own commands, then the server commands admins type as /<command>.
    for (const std::string_view taken :
         {"help", "party", "p", "w", "whisper", "tell", "poll", "vote", "yes", "y", "no", "n", "tp",
          "activity-log", "admin", "admins", "afk-kick", "kick-time", "role", "roles", "announce", "announce-throwdowns", "announce-to", "announcements",
          "ban", "bans", "board-check", "bone-scale", "boosts", "boosts-allow", "chat-color", "chat-colour", "clear-objects", "crowd",
          "distances", "drones", "effects", "kick", "layer", "layer-sync", "layers", "listed", "map", "map-pool", "maps", "msg",
          "msg-admins", "msg-party", "name", "net", "nobail", "nobail-allow", "noclip", "noclip-allow", "object-limit",
          "object-placement", "object-scaling", "objects", "park", "park-mod", "parties", "party-size", "password", "placement",
          "players", "rate", "reserved", "rotation", "say", "score-allow", "score-check", "speed-check", "status", "teleport",
          "tod", "tpall", "tphere", "tps", "tuning", "tuning-enforce", "unban", "voice", "voice-allow", "voice-range",
          "vote-cancel", "votes", "welcome", "world-layer-sync", "quit", "exit", "stop", "update"})
        if (name == taken) return false;
    // A number is an answer to a poll ("/2").
    return !std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; });
}
std::string custom_commands_error(const std::vector<CustomCommand> &commands) {
    using namespace multiplayer;
    if (commands.size() > max_custom_commands) return "commands: at most " + std::to_string(max_custom_commands) + " commands.";
    for (std::size_t i = 0; i < commands.size(); ++i) {
        const auto &c = commands[i];
        if (!valid_server_vote_name(c.name))
            return "commands: each needs a name of 1 to 16 lowercase letters, digits, - or _ (\"" + c.name + "\" is not).";
        const auto where = "commands \"" + c.name + "\": ";
        if (!custom_command_name_free(c.name)) return where + "that name is one of the server's own commands.";
        for (std::size_t j = 0; j < i; ++j)
            if (commands[j].name == c.name) return where + "two commands have that name.";
        if (c.reply.empty() && c.commands.empty()) return where + "needs a reply, a command, or both.";
        if (!c.reply.empty() && !valid_chat_text(c.reply)) return where + "reply must be one chat line (at most 200 bytes).";
        if (c.commands.size() > max_custom_command_runs)
            return where + "at most " + std::to_string(max_custom_command_runs) + " server commands.";
        for (const auto &run : c.commands)
            if (!valid_admin_text(run)) return where + "each command must be one server command, e.g. \"announce-to {player} Hi\".";
    }
    return {};
}
bool valid_mod_folder(std::string_view folder) noexcept {
    // What the game's mod list takes (mods::valid_mod_name), without blanks at either end.
    if (folder.empty() || folder.size() > 64 || folder.front() == '.' || folder.front() == ' ' || folder.back() == ' ') return false;
    return std::all_of(folder.begin(), folder.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ' ';
    });
}
bool valid_park_key(std::string_view key) noexcept {
    return !key.empty() && key.size() <= 32 &&
           std::all_of(key.begin(), key.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}
std::string park_mods_error(const ServerConfig &c) {
    using namespace multiplayer;
    if (c.park_limits.parks < 1 || c.park_limits.parks > max_spawned_parks)
        return "park_limits.parks must be 1 to " + std::to_string(max_spawned_parks) + ".";
    if (c.park_limits.objects < 1 || c.park_limits.objects > max_object_limit)
        return "park_limits.objects must be 1 to " + std::to_string(max_object_limit) + ".";
    if (c.park_mods.size() > max_park_mods) return "park_mods: at most " + std::to_string(max_park_mods) + " parks.";
    for (std::size_t i = 0; i < c.park_mods.size(); ++i) {
        const auto &park = c.park_mods[i];
        if (!valid_server_vote_name(park.name))
            return "park_mods: each park needs a name of 1 to 16 lowercase letters, digits, - or _ (\"" + park.name + "\" is not).";
        const auto where = "park_mods \"" + park.name + "\": ";
        for (std::size_t j = 0; j < i; ++j)
            if (c.park_mods[j].name == park.name) return where + "two parks have that name.";
        if (!valid_mod_folder(park.folder))
            return where + "folder must be the name of a mod folder in Mods (letters, digits, spaces, _ - and ., no path).";
        if (!find_level(park.map) || !valid_map_destination(map_destination(park.map)))
            return where + "map \"" + park.map + "\" is not a single known map. Use the map's name, e.g. \"San Vansterdam\".";
        if (!park.key.empty() && !valid_park_key(park.key))
            return where + "key must be a base map's key such as bam, grom or stadium_1, or empty.";
    }
    return {};
}
std::string config_error(const ServerConfig &c) {
    using namespace multiplayer;
    if (!parse_colour(c.chat_color)) return "chat_color must be a colour like #8E5CFF.";
    if (c.afk_kick > 1440) return "afk_kick_minutes must be 0 (never) to 1440.";
    if (c.kick_minutes > 1440) return "kick_minutes must be 0 (until the server restarts) to 1440.";
    if (c.votes.kick_minutes > 1440) return "votes.kick_minutes must be 0 (until the server restarts) to 1440.";
    if (c.word_warnings > 10) return "word_warnings must be 0 (words are not checked) to 10.";
    if (!parse_colour(c.chat_text_color)) return "chat_text_color must be a colour like #D9C8FF.";
    if (!valid_server_name(c.name)) return std::string("name must be ") + server_name_rule + ".";
    for (const auto id : c.reserved)
        if (!individual_steam_id(id)) return "reserved_players_slots must be SteamID64s (17 digits starting 7656119).";
    if (c.reserved.size() > 1024) return "reserved_players_slots holds at most 1024 players.";
    if (c.send_rate < 128 || c.send_rate > 16384) return "send_rate must be 128 to 16384 (KB/s for each player).";
    if (c.bone_scale_limit != 0 && !(c.bone_scale_limit >= 1.f && c.bone_scale_limit <= 8.f))
        return "bone_scale_limit must be 0 (no limit) or 1 to 8 (1: no resized body parts at all).";
    if (c.bone_reach_limit != 0 && !(c.bone_reach_limit >= .5f && c.bone_reach_limit <= 20.f))
        return "bone_reach_limit must be 0 (no limit) or 0.5 to 20 metres.";
    if (c.pack_ms > 50) return "pack_ms must be 0 (off) to 50.";
    if (c.threads > 32) return "threads must be 0 (one for each processor but one) to 32.";
    if (c.finger_distance > 10000) return "finger_distance must be 0 (fingers always sent) to 10000.";
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
    if (auto error = custom_votes_error(c.votes.custom); !error.empty()) return error;
    if (auto error = custom_commands_error(c.commands); !error.empty()) return error;
    if (c.announcements.messages.size() > max_announcements)
        return "announcements.messages: at most " + std::to_string(max_announcements) + " messages.";
    for (const auto &message : c.announcements.messages)
        if (!valid_chat_text(message)) return "announcements.messages: each must be one chat line (at most 200 bytes).";
    if (c.tps != dedicated_tps) return "tps is " + std::to_string(dedicated_tps) + " on dedicated servers.";
    if (!valid_voice_range(c.voice_range)) return "voice_range must be 50 to 1000.";
    if (!valid_object_limit(c.object_limit)) return "object_limit must be 0 (no limit) to " + std::to_string(max_object_limit) + ".";
    if (!c.distances.valid()) return "distances must be ordered: full_rate_return < half_rate_start <= half_rate_return < low_rate_start <= 10000.";
    for (unsigned lot = 0; lot < park_lots.size(); ++lot)
        if (c.parks[lot].empty() || !valid_park(lot, c.parks[lot]))
            return "parks." + std::string(park_lots[lot].key) + " is not a park layout (e.g. skatepark_01, or empty).";
    if (auto error = park_mods_error(c); !error.empty()) return error;
    if (!c.port || !c.query_port || c.port == c.query_port) return "port and query_port must differ.";
    if (const auto error = roles_error(c.roles); !error.empty()) return "roles: " + error;
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
// The list is the server thread's; the server window reads a copy of it (level_snapshot), and
// this is held while one is taken or the list is put in place.
std::mutex level_mutex;
} // namespace
std::vector<std::string> load_levels(const std::filesystem::path &mods) {
    // Built aside and put in place at the end: "maps reload" reads the folder while the server runs.
    std::vector<ServerLevel> list;
    struct Publish {
        std::vector<ServerLevel> &fresh;
        ~Publish() {
            std::lock_guard lock(level_mutex);
            level_list() = std::move(fresh);
        }
    } publish{list};
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
            // The mod's version, when the whole folder was copied here and not only its level list.
            std::string version;
            try {
                std::ifstream about(entry.path() / "manifest.json", std::ios::binary);
                std::stringstream about_text;
                about_text << about.rdbuf();
                if (about) version = Json::parse(about_text.str()).value("version_number", "");
            } catch (const std::exception &) {}
            const auto package = multiplayer::map_package_name(entry.path().filename().string(), version);
            for (const auto &level : root.at("levels")) {
                const auto asset = level.at("asset").string();
                const auto name = level.value("displayName", world_level_name(asset));
                if (std::none_of(list.begin(), list.end(), [&](const auto &l) { return same(l.asset, asset); }))
                    list.push_back({asset, name.empty() ? world_level_name(asset) : name, package});
            }
        } catch (const std::exception &e) {
            problems.push_back(entry.path().filename().string() + ": " + e.what());
        }
    }
    return problems;
}
const std::vector<ServerLevel> &levels() { return level_list(); }
std::vector<ServerLevel> level_snapshot() {
    std::lock_guard lock(level_mutex);
    return level_list();
}
const ServerLevel *find_level(std::string_view map) {
    if (map.empty()) return nullptr;
    for (const auto &level : level_list())
        if (same(level.asset, map)) return &level;
    map = world_level_typed_name(map);
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
std::uint32_t direct_ipv4(std::string_view text) noexcept {
    std::uint32_t address{};
    unsigned parts{};
    while (!text.empty() && parts < 4) {
        const auto dot = text.find('.');
        const auto part = text.substr(0, dot);
        unsigned value{};
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), value);
        if (part.empty() || part.size() > 3 || parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() || value > 255) return 0;
        address = (address << 8) | value;
        ++parts;
        text = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
        if (dot != std::string_view::npos && text.empty()) return 0; // a trailing dot
    }
    return parts == 4 && text.empty() ? address : 0;
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
std::string map_package(std::string_view map) {
    const auto *level = find_level(map);
    return level ? level->package : std::string();
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
