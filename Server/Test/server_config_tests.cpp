// A config written by an older server gains the settings added since, keeping its own values.
#include "Server/server_config.h"
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
    check(has("enforce_tuning") && has("votes.seconds") && has("votes.kick") && has("score_check") && has("score_allow") &&
              has("global_bans") && has("map_pool") && has("map_rotation_minutes"),
          "New settings not reported");
    check(!has("name") && !has("tps") && !has("votes.map"), "Settings the file had reported as new");
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
    check(has("steam_token") && config.steam_token.empty() && !token_refused(config), "steam_token is not a new, empty setting");
    auto tokened = reloaded;
    tokened.steam_token = "0123456789ABCDEF0123456789ABCDEF";
    save_config(tokened);
    check(load_config(file).steam_token == tokened.steam_token && !token_refused(tokened), "steam_token was not kept");
    tokened.steam_token = "not a token";
    check(token_refused(tokened), "A steam_token with other characters was accepted");
    tokened.steam_token = std::string(65, 'A');
    check(token_refused(tokened), "An overlong steam_token was accepted");
    {
        check(has("crowd_budget") && config.crowd_budget == 600, "crowd_budget is not a new setting of 600");
        check(has("send_rate") && config.send_rate == 900, "send_rate is not a new setting of 900");
        ServerConfig rate;
        rate.send_rate = 64;
        check(config_error(rate).find("send_rate") != std::string::npos, "A send rate too low to play with was accepted");
        ServerConfig crowd;
        crowd.crowd_budget = 50;
        check(config_error(crowd).find("crowd_budget") != std::string::npos, "A crowd budget too small to play with was accepted");
    }
    // Reserved slots: none unless set; the last of them only the listed players and admins take.
    {
        check(has("reserved_slots") && has("reserved") && config.reserved_slots == 0 && config.reserved.empty(),
              "Reserved slots are not new settings that reserve nothing");
        ServerConfig slots;
        slots.max_players = 4;
        const std::uint64_t vip = 76561198000000001ULL, admin = 76561198000000002ULL, anyone = 76561198000000003ULL;
        check(may_join(slots, anyone, 3) && !may_join(slots, anyone, 4) && !may_join(slots, vip, 4), "A server with nothing reserved did not fill up plainly");
        slots.reserved_slots = 2;
        slots.reserved = {vip};
        slots.admins = {admin};
        check(may_join(slots, anyone, 1) && !may_join(slots, anyone, 2) && !may_join(slots, anyone, 3), "Reserved slots were given to anyone");
        check(may_join(slots, vip, 2) && may_join(slots, vip, 3) && may_join(slots, admin, 3), "A reserved player or an admin was kept out of a reserved slot");
        check(!may_join(slots, vip, 4) && !may_join(slots, admin, 4), "A full server let someone in");
        const auto refused = [](const ServerConfig &c) { return config_error(c).find("reserved") != std::string::npos; };
        check(!refused(slots), "Valid reserved slots were refused");
        slots.reserved_slots = 4;
        check(refused(slots), "A server with every slot reserved was accepted");
        slots.reserved_slots = 2;
        slots.reserved.push_back(42);
        check(refused(slots), "A reserved entry that is not a player was accepted");
        auto kept = reloaded;
        kept.reserved_slots = 3;
        kept.reserved = {vip};
        save_config(kept);
        const auto back = load_config(file);
        check(back.reserved_slots == 3 && back.reserved == std::vector<std::uint64_t>{vip}, "Reserved slots were not kept");
        save_config(reloaded);
    }
    // object_limit: 100 unless set (0 is no limit), and kept on a rewrite.
    check(has("object_limit") && config.object_limit == 100, "object_limit is not a new setting of 100");
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
