#include "Extension/Multiplayer/Steam/steam_lobbies.h"
#include "Engine/Game/Build/supported_build.h"
#include <iostream>
#include <map>
#include <stdexcept>
#include <set>

using namespace dingosdk;
using namespace dingosdk::multiplayer;
namespace {
void check(bool condition, const char *why) {
    if (!condition)
        throw std::runtime_error(why);
}
constexpr std::uint64_t host_id = 76561198000000123ULL, guest_id = 76561198000000456ULL;
const std::string map_name = "levels/Game/Beach/Beach";
struct Fake final : LobbyApi {
    std::uint64_t next{}, create_call{}, list_call{}, join_call{};
    int at_calls{};
    bool fail_write{};
    std::map<std::uint64_t, LobbyResult> results;
    std::map<std::uint64_t, std::map<std::string, std::string>> metadata;
    std::map<std::uint64_t, std::uint64_t> owners;
    std::vector<std::uint64_t> ids, left;
    std::set<std::uint64_t> exposed;
    void open() override {}
    std::uint64_t create(unsigned) override { return create_call = ++next; }
    std::uint64_t search() override { return list_call = ++next; }
    std::uint64_t join(std::uint64_t) override { return join_call = ++next; }
    std::optional<LobbyResult> poll(std::uint64_t id, LobbyCall) override {
        const auto found = results.find(id);
        if (found == results.end())
            return {};
        const auto value = found->second;
        results.erase(found);
        return value;
    }
    void leave(std::uint64_t id) override {
        left.push_back(id);
        exposed.erase(id);
    }
    std::uint64_t at(int i) override {
        ++at_calls;
        return ids.at(i % ids.size());
    }
    std::uint64_t owner(std::uint64_t id) override { return owners[id]; }
    std::string data(std::uint64_t id, const char *key) override { return metadata[id][key]; }
    bool data(std::uint64_t id, const char *key, const std::string &v) override {
        if (fail_write)
            return false;
        metadata[id][key] = v;
        return true;
    }
    bool joinable(std::uint64_t id, bool value) override {
        if (value) {
            check(read_lobby(id, owner(id), [&](const char *k) { return data(id, k); }).has_value(),
                  "A public lobby was exposed before all metadata was valid");
            exposed.insert(id);
        } else
            exposed.erase(id);
        return true;
    }
    bool visibility(std::uint64_t, bool) override { return true; }
    std::string name() override { return "Test host"; }
    void record(std::uint64_t id = 9001) {
        owners[id] = host_id;
        metadata[id] = {{"rs_kind", "reskate-co-skate"},
                        {"rs_protocol", std::to_string(protocol_version)},
                        {"rs_build", std::string(supported_build::game_sha256)},
                        {"rs_name", "Test host"},
                        {"rs_code", format_invite({host_id, 123})},
                        {"rs_map", map_name},
                        {"rs_players", "1"},
                        {"rs_capacity", "8"},
                        {"rs_password", "0"},
                        {"rs_open", "1"}};
    }
};
struct Fixture {
    Fake *api = new Fake;
    SteamLobbies lobbies{std::unique_ptr<LobbyApi>(api)};
    void list() {
        api->record();
        api->ids = {9001};
        lobbies.refresh(1000000);
        api->results[api->list_call] = {true, 0, 1, 1};
        lobbies.tick(1100000);
        check(lobbies.status().rows.size() == 1, "Valid public lobby not listed");
    }
    void host() {
        lobbies.host(format_invite({host_id, 123}));
        lobbies.update_host(true, 1, map_name, 1000000);
        api->owners[9001] = host_id;
        api->results[api->create_call] = {true, 9001, 0, 1};
        lobbies.tick(1100000);
        check(!lobbies.status().listed, "Lobby published before readiness update");
        lobbies.update_host(true, 1, map_name, 1100000);
    }
};
void metadata_tests() {
    Fake f;
    f.record();
    auto get = [&](const char *key) { return f.data(9001, key); };
    check(read_lobby(9001, host_id, get).has_value(), "Valid lobby rejected");
    check(read_lobby(9001, 0, get).has_value(), "Non-member browser discarded advertised host");
    check(!read_lobby(9001, guest_id, get), "Changed owner accepted");
    // A lobby's custom mode (rs_mode): absent or invalid reads as none, and never refuses the lobby.
    check(read_lobby(9001, host_id, get)->mode.empty(), "A lobby without rs_mode read as having a mode");
    f.metadata[9001]["rs_mode"] = "domination";
    check(read_lobby(9001, host_id, get)->mode == "domination", "A lobby's custom mode was not read");
    f.metadata[9001]["rs_mode"] = "DOMINATION!";
    const auto odd = read_lobby(9001, host_id, get);
    check(odd && odd->mode.empty(), "An invalid rs_mode was read, or refused the lobby");
    f.metadata[9001].erase("rs_mode");
    for (const auto &[key, value] : std::map<std::string, std::string>{{"rs_kind", "another-game"},
                                                                       {"rs_protocol", "999"},
                                                                       {"rs_build", "other-build"},
                                                                       {"rs_code", "malformed"},
                                                                       {"rs_players", "8"},
                                                                       {"rs_open", "0"},
                                                                       {"rs_name", "bad\nname"},
                                                                       {"rs_map", std::string(257, 'x')}}) {
        f.record();
        f.metadata[9001][key] = value;
        check(!read_lobby(9001, host_id, get), "Incompatible or malformed metadata accepted");
    }
}
void host_tests() {
    Fixture f;
    f.host();
    check(f.lobbies.status().listed && f.api->exposed.contains(9001), "Host not publicly listed");
    f.lobbies.update_host(true, multiplayer_lobby_player_limit, map_name, 1200000);
    check(!f.lobbies.status().listed && f.api->metadata[9001]["rs_players"] == std::to_string(multiplayer_lobby_player_limit),
          "Occupied host stayed open");
    f.lobbies.update_host(true, 1, map_name, 1300000);
    check(f.lobbies.status().listed, "Guest departure did not reopen listing");
    f.lobbies.update_host(false, 2, "", 1400000);
    check(!f.lobbies.status().listed && f.api->metadata[9001]["rs_open"] == "0" && f.api->left.empty(),
          "Host travel left a stale public listing or destroyed the lobby");
    const auto old_code = f.api->metadata[9001]["rs_code"];
    f.lobbies.update_host(true, 2, "Levels/Root|Levels/NewMap", 1500000);
    check(f.lobbies.status().listed && f.api->metadata[9001]["rs_map"] == "Levels/Root|Levels/NewMap" &&
              f.api->metadata[9001]["rs_code"] == old_code, "Host travel did not reopen the same lobby on the new map");
    f.lobbies.stop();
    check(!f.lobbies.status().listed && f.api->left == std::vector<std::uint64_t>{9001},
          "Host stop leaked lobby");
    Fixture late;
    late.lobbies.host(format_invite({host_id, 123}));
    late.lobbies.update_host(true, 1, map_name, 1000000);
    late.lobbies.stop();
    late.api->results[late.api->create_call] = {true, 9002, 0, 1};
    late.lobbies.tick(2000000);
    check(late.api->left == std::vector<std::uint64_t>{9002} && !late.lobbies.status().listed,
          "Late create after cancellation leaked a lobby");
    Fixture failure;
    failure.api->fail_write = true;
    failure.host();
    check(!failure.lobbies.status().listed && failure.api->left.size() == 1,
          "Failed publication left a partial public lobby");
}
void join_tests() {
    Fixture f;
    f.list();
    bool rejected{};
    try {
        f.lobbies.join(9001, host_id, 1200000);
    } catch (...) {
        rejected = true;
    }
    check(rejected && !f.api->join_call, "Self join reached Steam");
    // Discovery no longer needs a locally loaded map; the host supplies it over P2P.
    f.lobbies.join(9001, guest_id, 1300000);
    f.api->results[f.api->join_call] = {true, 9001, 0, 1};
    f.lobbies.tick(1400000);
    const auto joined = f.lobbies.take_join();
    check(joined && joined->owner == host_id && f.api->left.size() == 1,
          "Browser join failed or retained directory membership");
    check(!f.lobbies.take_join(), "Browser join was delivered twice");
    Fixture stale;
    stale.list();
    stale.lobbies.join(9001, guest_id, 1300000);
    stale.api->metadata[9001]["rs_open"] = "0";
    stale.api->results[stale.api->join_call] = {true, 9001, 0, 1};
    stale.lobbies.tick(1400000);
    check(!stale.lobbies.take_join() && stale.api->left.size() == 1, "Stale browser record reached P2P");
    Fixture cancelled;
    cancelled.list();
    cancelled.lobbies.join(9001, guest_id, 1300000);
    cancelled.lobbies.stop();
    cancelled.api->results[cancelled.api->join_call] = {true, 9001, 0, 1};
    cancelled.lobbies.tick(1400000);
    check(!cancelled.lobbies.take_join() && cancelled.api->left.size() == 1,
          "Cancelled join was delivered or leaked");
    Fixture timeout;
    timeout.list();
    timeout.lobbies.join(9001, guest_id, 1300000);
    timeout.lobbies.tick(30000000);
    check(!timeout.lobbies.status().joining, "Timed-out join remained busy");
    timeout.api->results[timeout.api->join_call] = {true, 9001, 0, 1};
    timeout.lobbies.tick(31000000);
    check(!timeout.lobbies.take_join() && timeout.api->left.size() == 1,
          "Late timed-out join leaked membership");
    Fixture bounded;
    bounded.api->record();
    bounded.api->ids = {9001};
    bounded.lobbies.refresh(1000000);
    bounded.api->results[bounded.api->list_call] = {true, 0, 1000000, 1};
    bounded.lobbies.tick(1100000);
    check(bounded.api->at_calls == 50 && bounded.lobbies.status().rows.size() == 1,
          "Search result bounds or duplicate handling failed");
}
void friend_join_tests() {
    Fixture f;
    f.api->record(); // Friend joining does not require opening the browser first.
    f.lobbies.join_friend(9001, guest_id, 1000000);
    f.api->results[f.api->join_call] = {true, 9001, 0, 1};
    f.lobbies.tick(1100000);
    const auto joined = f.lobbies.take_join();
    check(joined && joined->owner == host_id && f.api->left.size() == 1,
          "Friend join must verify the host and release temporary Steam membership");
    for (const auto& [key, value] : std::map<std::string, std::string>{
            {"rs_password", "1"}, {"rs_open", "0"}, {"rs_build", "other-build"},
            {"rs_protocol", "999"}, {"rs_players", "8"}, {"rs_code", "malformed"}}) {
        Fixture rejected;
        rejected.api->record(); rejected.api->metadata[9001][key] = value;
        rejected.lobbies.join_friend(9001, guest_id, 1000000);
        rejected.api->results[rejected.api->join_call] = {true, 9001, 0, 1};
        rejected.lobbies.tick(1100000);
        check(!rejected.lobbies.take_join() && rejected.api->left.size() == 1,
              "Incompatible, closed, full or password-protected friend lobby must not start P2P");
    }
    Fixture wrong_owner;
    wrong_owner.api->record(); wrong_owner.api->owners[9001] = guest_id + 1;
    wrong_owner.lobbies.join_friend(9001, guest_id, 1000000);
    wrong_owner.api->results[wrong_owner.api->join_call] = {true, 9001, 0, 1};
    wrong_owner.lobbies.tick(1100000);
    check(!wrong_owner.lobbies.take_join(), "Friend join cannot use a forged advertised host");
    Fixture cancelled;
    cancelled.api->record(); cancelled.lobbies.join_friend(9001, guest_id, 1000000);
    cancelled.lobbies.stop();
    cancelled.api->results[cancelled.api->join_call] = {true, 9001, 0, 1};
    cancelled.lobbies.tick(1100000);
    check(!cancelled.lobbies.take_join() && cancelled.api->left.size() == 1,
          "Cancelled friend joins must release late membership");
}
} // namespace
int main() {
    try {
        metadata_tests();
        host_tests();
        join_tests();
        friend_join_tests();
        std::cout << "Steam lobby lifecycle regressions passed.\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
