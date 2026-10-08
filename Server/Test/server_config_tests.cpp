// A config written by an older server gains the settings added since, keeping its own values.
#include "Server/server_config.h"
#include "Extension/Multiplayer/Session/peer_slots.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {
int failures{};
void check(bool ok, const char *what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}
std::string text(const std::filesystem::path &file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}
} // namespace

int run() {
    using namespace dingosdk::server;
    using dingosdk::valid_server_name;
    const auto folder = std::filesystem::temp_directory_path() / "reskate_server_config_tests";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    const auto file = folder / "ReSkateServer.json";
    // No enforce_tuning, and a votes object without "seconds".
    std::ofstream(file, std::ios::binary) << R"({"name": "Old Server", "tps": 60, "boosts": false,
        "votes": {"map": {"enabled": true, "percent": 60}}})";

    std::vector<std::string> added;
    const auto config = load_config(file, &added);
    const auto has = [&](std::string_view name) { return std::ranges::find(added, name) != added.end(); };
    check(has("anti_cheat.enforce_tuning") && has("votes.seconds") && has("votes.kick") && has("anti_cheat.modified_scoring") && has("anti_cheat.allowed_scoring_mods") &&
              has("access.use_global_bans") && has("maps.pool") && has("maps.rotation_minutes"),
          "New settings not reported");
    // A setting under its old name is read, written back under its new one, and not called new.
    check(!has("players.allow_boosts") && has("players.allow_noclip"), "A renamed setting was reported as new, or a new one was not");
    check(!has("server.name") && !has("tps") && !has("votes.map"), "Settings the file had reported as new");
    const auto written = text(file);
    check(written.find("\"enforce_tuning\"") != std::string::npos && written.find("\"seconds\"") != std::string::npos,
          "New settings not written into the file");
    check(config.name == "Old Server" && config.tps == dedicated_tps /* fixed: the file's 60 is not kept */ && !config.boosts && config.votes.map.enabled &&
              config.votes.map.percent == 60 && config.enforce_tuning && config.global_bans,
          "The file's own values or the new defaults were lost");
    const auto reloaded = load_config(file);
    check(reloaded.name == "Old Server" && !reloaded.boosts && reloaded.votes.map.percent == 60, "Values lost on rewrite");

    // steam_token: empty (anonymous) unless set, kept on a rewrite, and only letters and digits.
    // (The maps are not loaded yet, so config_error has another complaint: look for this one.)
    const auto token_refused = [](const ServerConfig &c) { return config_error(c).find("steam_token") != std::string::npos; };
    check(has("server.steam_token") && config.steam_token.empty() && !token_refused(config), "steam_token is not a new, empty setting");
    auto tokened = reloaded;
    tokened.steam_token = "0123456789ABCDEF0123456789ABCDEF";
    save_config(tokened);
    check(load_config(file).steam_token == tokened.steam_token && !token_refused(tokened), "steam_token was not kept");
    tokened.steam_token = "not a token";
    check(token_refused(tokened), "A steam_token with other characters was accepted");
    tokened.steam_token = std::string(65, 'A');
    check(token_refused(tokened), "An overlong steam_token was accepted");
    {
        check(has("network.crowd_budget") && config.crowd_budget == 600, "crowd_budget is not a new setting of 600");
        check(has("network.send_rate") && config.send_rate == 900, "send_rate is not a new setting of 900");
        ServerConfig rate;
        rate.send_rate = 64;
        check(config_error(rate).find("send_rate") != std::string::npos, "A send rate too low to play with was accepted");
        check(has("anti_cheat.bone_scale_limit") && config.bone_scale_limit == 2, "bone_scale_limit is not a new setting of 2");
        ServerConfig scaled;
        scaled.bone_scale_limit = 0.5f;
        check(config_error(scaled).find("bone_scale_limit") != std::string::npos, "A bone scale limit under 1 was accepted");
        check(has("network.pack_ms") && config.pack_ms == 10, "pack_ms is not a new setting of 10");
        check(has("network.finger_distance") && config.finger_distance == 25, "finger_distance is not a new setting of 25");
        check(has("network.use_steam_relay") && config.use_steam_relay, "use_steam_relay is not a new setting of true");
        ServerConfig crowd;
        crowd.crowd_budget = 50;
        check(config_error(crowd).find("crowd_budget") != std::string::npos, "A crowd budget too small to play with was accepted");
    }
    // Reserved slots: beyond max_players, for the listed players and the admins only.
    {
        check(has("access.reserved_players_slots") && config.reserved.empty(), "Reserved slots are not a new setting that reserves nothing");
        ServerConfig slots;
        slots.max_players = 4;
        const std::uint64_t vip = 76561198000000001ULL, admin = 76561198000000002ULL, anyone = 76561198000000003ULL;
        check(may_join(slots, anyone, 3) && !may_join(slots, anyone, 4) && !may_join(slots, vip, 4) && extra_slots(slots) == 0,
              "A server with nothing reserved did not fill up plainly");
        // Nothing is held back from anyone; the listed players and the admins have a slot each past the last.
        const std::uint64_t vip2 = 76561198000000004ULL;
        slots.reserved = {vip, vip2};
        slots.admins = {admin, vip}; // listed twice: one slot
        check(extra_slots(slots) == 3, "Extra slots were not one for each reserved player and admin");
        check(may_join(slots, anyone, 3) && !may_join(slots, anyone, 4) && !may_join(slots, anyone, 6), "An extra slot was given to anyone, or a slot was held back");
        check(may_join(slots, vip, 4) && may_join(slots, admin, 5) && may_join(slots, vip2, 6), "A reserved player or an admin was kept out of a full server");
        check(!may_join(slots, vip, 7) && !may_join(slots, admin, 7), "More joined than there are extra slots");
        ServerConfig brim;
        brim.max_players = static_cast<unsigned>(dingosdk::multiplayer::max_players) - 2;
        brim.reserved = {vip, vip2};
        check(extra_slots(brim) == 1 && may_join(brim, vip, brim.max_players) && !may_join(brim, vip2, brim.max_players + 1),
              "Extra slots went past what a session can hold");
        const auto refused = [](const ServerConfig &c) { return config_error(c).find("reserved") != std::string::npos; };
        check(!refused(slots), "Valid reserved slots were refused");
        slots.reserved = {vip, 42};
        check(refused(slots), "A reserved entry that is not a player was accepted");
        auto kept = reloaded;
        kept.reserved = {vip};
        save_config(kept);
        const auto back = load_config(file);
        check(back.reserved == std::vector<std::uint64_t>{vip} && text(file).find("\"reserved_slots\"") == std::string::npos, "Reserved slots were not kept");
        save_config(reloaded);
    }
    // object_limit: 100 unless set (0 is no limit), and kept on a rewrite.
    check(has("players.object_limit") && config.object_limit == 100, "object_limit is not a new setting of 100");
    auto limited = reloaded;
    limited.object_limit = 50;
    save_config(limited);
    check(load_config(file).object_limit == 50, "object_limit was not kept");
    limited.object_limit = 0;
    save_config(limited);
    check(load_config(file).object_limit == 0, "No object limit became the default again");
    save_config(reloaded);

    // Server names: letters, digits, spaces and - _ / [ ] ( ) only.
    check(valid_server_name("Old Server") && valid_server_name("[EU] Skate_Park-2 (24x7)") && valid_server_name("a") &&
              valid_server_name("EU/West 24/7") && !valid_server_name("///") && !valid_server_name("a\\b"),
          "A plain server name was refused");
    check(!valid_server_name("") && !valid_server_name(std::string(65, 'a')) && !valid_server_name("Best! Server") &&
              !valid_server_name("caf\xC3\xA9") && !valid_server_name("a.b") && !valid_server_name("<b>x</b>") &&
              !valid_server_name(" padded") && !valid_server_name("padded ") && !valid_server_name("[]--()") &&
              !valid_server_name("two\nlines"),
          "A server name with other characters was accepted");

    // An up-to-date file is left alone.
    const auto before = std::filesystem::last_write_time(file);
    added.clear();
    static_cast<void>(load_config(file, &added));
    check(added.empty() && std::filesystem::last_write_time(file) == before, "A complete config was rewritten");

    // The scoring check: its mode and the accepted fingerprints survive a save, and bad entries are dropped.
    check(reloaded.score_check == "warn" && reloaded.score_allow.empty(), "Scoring check defaults wrong");
    auto scoring = reloaded;
    scoring.score_check = "kick";
    scoring.score_allow = {0x00c0ffee12345678ULL, 0xffffffffffffffffULL};
    save_config(scoring);
    const auto saved = load_config(file);
    check(saved.score_check == "kick" && saved.score_allow == scoring.score_allow, "Scoring check settings lost");
    check(scoring_text(0x00c0ffee12345678ULL) == "00c0ffee12345678", "Fingerprint not written as 16 hex digits");
    check(parse_scoring("00C0FFEE12345678") == 0x00c0ffee12345678ULL && !parse_scoring("0") && !parse_scoring("xyz") &&
              !parse_scoring("123456789abcdef01"),
          "Fingerprint text not read back, or a bad one accepted");
    std::ofstream(file, std::ios::binary) << R"({"score_check": "ban", "score_allow": ["nothex", "0", "abc"]})";
    const auto odd = load_config(file);
    check(odd.score_check == "warn" && odd.score_allow == std::vector<std::uint64_t>{0xabc}, "Bad scoring settings not cleaned");

    // A config from before the sections: every setting is read from where it was, under the name
    // it had, and the file is written back in sections, in reading order, with nothing called new
    // that was only moved.
    {
        std::ofstream(file, std::ios::binary) << R"({"name": "Flat", "no_bail": false, "score_check": "kick", "score_allow": ["abc"],
            "speed_check": "off", "map": "Isle of Grom", "map_pool": ["Isle of Grom"], "map_rotation_minutes": 30, "welcome": "Hi",
            "admins": ["76561198000000002"], "reserved": ["76561198000000001"], "reserved_slots": 2, "tps": 30, "global_bans": false,
            "voice_chat": false, "send_rate": 700, "distances": {"half_rate_start": 70}, "parks": {"financial": "empty"},
            "votes": {"map": {"enabled": false, "percent": 40}, "kick": {"enabled": true, "percent": 60},
                      "time_of_day": {"enabled": true, "percent": 50}, "seconds": 30, "cooldown_seconds": 60}})";
        std::vector<std::string> fresh;
        const auto flat = load_config(file, &fresh);
        check(flat.name == "Flat" && !flat.no_bail && flat.score_check == "kick" && flat.score_allow == std::vector<std::uint64_t>{0xabc} &&
                  flat.speed_check == "off" && flat.map == "Isle of Grom" && flat.map_pool.size() == 1 && flat.map_rotation == 30 && flat.welcome == "Hi" &&
                  flat.admins.size() == 1 && flat.reserved.size() == 1 && !flat.global_bans && !flat.voice_chat && flat.send_rate == 700 &&
                  flat.distances.half_rate_start == 70 && !flat.votes.map.enabled && flat.votes.map.percent == 40,
              "A config from before the sections lost a setting");
        const auto fresh_has = [&](std::string_view name) { return std::ranges::find(fresh, name) != fresh.end(); };
        check(!fresh_has("players.allow_no_bail") && !fresh_has("anti_cheat.modified_scoring") && !fresh_has("maps.pool") && !fresh_has("server.name") &&
                  fresh_has("network.pack_ms"),
              "A moved setting was called new, or a new one was not");
        const auto now = text(file);
        const auto at = [&](std::string_view what) { return now.find(what); };
        check(at("\"server\"") < at("\"access\"") && at("\"access\"") < at("\"maps\"") && at("\"maps\"") < at("\"players\"") &&
                  at("\"players\"") < at("\"anti_cheat\"") && at("\"anti_cheat\"") < at("\"network\"") && at("\"network\"") < at("\"votes\"") &&
                  at("\"votes\"") != std::string::npos && at("\"name\"") < at("\"password\"") && at("\"password\"") < at("\"welcome_message\""),
              "The config was not written in sections, in reading order");
        check(at("\"no_bail\"") == std::string::npos && at("\"tps\"") == std::string::npos && at("\"score_check\"") == std::string::npos &&
                  at("\"reserved_slots\"") == std::string::npos && at("\"modified_scoring\": \"kick\"") != std::string::npos,
              "Old names were left in the config");
        std::vector<std::string> again;
        const auto back = load_config(file, &again);
        check(again.empty() && back.name == "Flat" && !back.no_bail && back.score_check == "kick" && back.map_pool.size() == 1 && back.reserved.size() == 1 &&
                  back.distances.half_rate_start == 70 && !back.votes.map.enabled && text(file) == now,
              "A config in sections did not read back as it was written");
        std::filesystem::remove_all(folder / "data");
    }
    // Bans have a file of their own; a config that still holds them has them moved there.
    {
        std::filesystem::remove_all(folder / "data");
        std::ofstream(file, std::ios::binary) << R"({"name": "Old", "bans": [{"id": "76561198000000001", "name": "A", "added": 5}]})";
        auto old = load_config(file);
        check(old.bans.size() == 1 && old.bans[0].id == 76561198000000001ULL && old.bans[0].name == "A", "An old config's bans were not read");
        std::stringstream now;
        now << std::ifstream(file, std::ios::binary).rdbuf();
        check(std::filesystem::exists(bans_file(old)) && now.str().find("\"bans\"") == std::string::npos, "Bans were not moved to data/bans.json");
        old.bans.push_back({76561198000000002ULL, "B", 6});
        save_config(old);
        const auto again = load_config(file);
        check(again.bans.size() == 2 && again.bans[1].name == "B" && again.bans[1].added == 6, "Bans were not kept in their own file");
        std::ofstream(bans_file(old), std::ios::binary) << "{not a list";
        bool refused_bans{};
        try {
            static_cast<void>(load_config(file));
        } catch (const std::exception &) {
            refused_bans = true;
        }
        check(refused_bans, "A server started without its bans");
        std::filesystem::remove_all(folder / "data");
    }
    // A port outside 1-65535 is refused, not wrapped to another port, and the file keeps the typo.
    const auto refused = [&](const char *json) {
        std::ofstream(file, std::ios::binary) << json;
        try {
            static_cast<void>(load_config(file));
        } catch (const std::exception &) {
            return text(file) == json;
        }
        return false;
    };
    check(refused(R"({"query_port": 70000})") && refused(R"({"port": 65536})") && refused(R"({"port": 0})"),
          "An out-of-range port was accepted or the file rewritten");
    std::ofstream(file, std::ios::binary) << R"({"port": 65535, "query_port": 1})";
    const auto edges = load_config(file);
    check(edges.port == 65535 && edges.query_port == 1, "Ports at the ends of the range refused");

    static_cast<void>(load_levels(folder / "Mods")); // no Mods folder: the retail maps only
    {
        // Only maps the server has: the game's own always, a custom one once its mod is in Mods.
        ServerConfig elsewhere;
        elsewhere.map = "Levels/Game/NotHere/NotHere";
        check(config_error(elsewhere).find("not a map this server has") != std::string::npos && !installed_map(elsewhere.map),
              "A map the server does not have was accepted");
        check(installed_map("Isle of Grom") && installed_map("Levels/Game/DingoLevel_MPR/DingoLevel_MPR"), "One of the game's own maps was refused");
    }
    ServerConfig pool;
    check(pool_levels(pool).size() == levels().size() && in_map_pool(pool, "Stadium 2"),
          "An empty pool does not allow every map");
    pool.map_pool = {"Isle", "Isle of Grom", "San Vansterdam", "Stadium 1"};
    check(pool_levels(pool).size() == 3, "Pool maps not resolved once each");
    check(in_map_pool(pool, "San Vansterdam") && in_map_pool(pool, "Levels/Game/DingoLevel_SDM/DingoLevel_SDM_Int_001/DingoLevel_SDM_Int_001") &&
              !in_map_pool(pool, "Super Ultra Mega Resort") && !in_map_pool(pool, "Nowhere"),
          "Pool membership wrong");
    const auto next = [&](std::string_view map) {
        const auto *level = next_pool_map(pool, map);
        return level ? level->name : std::string{};
    };
    check(next("Isle of Grom") == "San Vansterdam" && next("Stadium 1") == "Isle of Grom" &&
              next("Super Ultra Mega Resort") == "Isle of Grom",
          "Rotation does not follow the pool's order");
    pool.map_pool = {"Isle of Grom"};
    check(next("Isle of Grom").empty() && next("San Vansterdam") == "Isle of Grom", "A one-map pool rotated to itself");
    check(config_error(pool).empty(), "A valid pool refused");
    pool.map_pool = {"Isle of Grom", "Nowhere"};
    check(!config_error(pool).empty(), "An unknown pool map accepted");
    pool.map_pool = {"Isle of Grom", "Stadium 1"};
    pool.map_rotation = 15;
    pool.file = file;
    save_config(pool);
    const auto rotating = load_config(file);
    check(rotating.map_pool == pool.map_pool && rotating.map_rotation == 15, "Map pool or rotation lost on save");
    std::ofstream(file, std::ios::binary) << R"({"map_pool": ["Isle of Grom", 7, ""], "map_rotation_minutes": 5000})";
    const auto odd_pool = load_config(file);
    check(odd_pool.map_pool == std::vector<std::string>{"Isle of Grom"} && odd_pool.map_rotation == 1440,
          "Bad pool entries kept, or rotation not capped");

    std::filesystem::remove_all(folder);
    if (failures) return 1;
    std::cout << "server config: ok\n";
    return 0;
}
int main() {
    try {
        return run();
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
