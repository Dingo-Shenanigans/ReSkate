#include "Extension/Boot/discord_presence.h"
#include "Engine/Core/Json/json.h"
#include "Extension/Profile/local_profile_runtime.h"
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>

// The profile is not part of this test: the switch is simply on.
namespace dingosdk::profile_runtime {
std::optional<bool> local_preference(std::string_view) noexcept { return std::nullopt; }
void set_local_preference(std::string_view, bool) noexcept {}
} // namespace dingosdk::profile_runtime

namespace {
int failures{};
void check(bool ok, const char *what) {
    if (ok) return;
    std::cout << "FAIL: " << what << '\n';
    ++failures;
}
} // namespace

int main() {
    using namespace dingosdk;
    using namespace dingosdk::discord_presence;
    // A message is its opcode and length, little endian, then the JSON.
    const auto bytes = frame(1, "{}");
    std::uint32_t header[2]{};
    std::memcpy(header, bytes.data(), sizeof(header));
    check(bytes.size() == 10 && header[0] == 1 && header[1] == 2 && bytes.substr(8) == "{}", "A frame is not opcode, length and JSON");

    // Alone: the map and how long, and nothing to join.
    Presence solo{"On San Vansterdam", "Solo skating"};
    auto sent = Json::parse(activity_command(&solo, 4321, 1790000000, 7));
    const auto &alone = sent.at("args").at("activity");
    check(sent.value("cmd", "") == "SET_ACTIVITY" && sent.value("nonce", "") == "7" && sent.at("args").at("pid").get<std::uint64_t>() == 4321,
          "The command does not name this process");
    check(alone.value("details", "") == "On San Vansterdam" && alone.value("state", "") == "Solo skating" &&
              alone.at("timestamps").at("start").get<std::int64_t>() == 1790000000 && !alone.contains("party") && !alone.contains("secrets"),
          "Skating alone does not show the map and the time alone");

    // On a server with room: its name, how many, and the code behind the Join button.
    Presence server{"On DHS", "Server: EU 1", 12, 32, "85568392924040001", "85568392924040001-00000000deadbeef"};
    sent = Json::parse(activity_command(&server, 1, 1790000000, 8));
    const auto &busy = sent.at("args").at("activity");
    check(busy.at("party").value("id", "") == "85568392924040001" && busy.at("party").at("size").at(0).get<int>() == 12 &&
              busy.at("party").at("size").at(1).get<int>() == 32 && busy.at("party").at("privacy").get<int>() == 1 &&
              busy.at("secrets").value("join", "") == server.join,
          "A server with room does not show its players and a way in");
    // Full, or with no code that is one: no Join button.
    auto full = server;
    full.party_size = 32;
    check(!Json::parse(activity_command(&full, 1, 0, 9)).at("args").at("activity").contains("secrets"), "A full server offers a way in");
    auto closed = server;
    closed.join.clear();
    check(!Json::parse(activity_command(&closed, 1, 0, 9)).at("args").at("activity").contains("secrets"), "A session without a code offers a way in");
    closed.join = "\"}],\"evil\":1";
    check(!Json::parse(activity_command(&closed, 1, 0, 9)).at("args").at("activity").contains("secrets"), "Text that is not a join code was sent as one");
    // A long name is cut to what Discord takes, and nothing clears the status.
    Presence longer{std::string(200, 'x'), "y"};
    const auto cut = Json::parse(activity_command(&longer, 1, 0, 1)).at("args").at("activity");
    check(cut.value("details", "").size() == 128 && cut.value("state", "") == "y ", "Lines are not kept within Discord's lengths");
    check(Json::parse(activity_command(nullptr, 1, 0, 1)).at("args").at("activity").is_null(), "Clearing the status does not send an empty one");

    // Discord asking to join: only its own event, and only a code that is one.
    check(join_request(R"({"cmd":"DISPATCH","evt":"ACTIVITY_JOIN","data":{"secret":"76561198084159190-00000000deadbeef"}})") ==
              "76561198084159190-00000000deadbeef",
          "A request to join was not read");
    for (const auto *wrong : {R"({"cmd":"DISPATCH","evt":"ACTIVITY_JOIN","data":{"secret":"mp stop; quit"}})",
                              R"({"cmd":"DISPATCH","evt":"ACTIVITY_JOIN","data":{"secret":"76561198084159190-00000000DEADBEEF"}})",
                              R"({"cmd":"DISPATCH","evt":"ACTIVITY_SPECTATE","data":{"secret":"76561198084159190-00000000deadbeef"}})",
                              R"({"cmd":"SET_ACTIVITY","evt":null,"data":{"secret":"76561198084159190-00000000deadbeef"}})",
                              R"({"cmd":"DISPATCH","evt":"ACTIVITY_JOIN"})", "not json"})
        check(join_request(wrong).empty(), wrong);
    // Someone asking to join: who, and only from Discord's own event.
    check(join_asker(R"({"cmd":"DISPATCH","evt":"ACTIVITY_JOIN_REQUEST","data":{"user":{"id":"53908232506183680","username":"Mason"}}})") ==
              "53908232506183680",
          "Who asked to join was not read");
    for (const auto *wrong : {R"({"cmd":"DISPATCH","evt":"ACTIVITY_JOIN_REQUEST","data":{"user":{"id":"1\"},\"x\":\""}}})",
                              R"({"cmd":"DISPATCH","evt":"ACTIVITY_JOIN_REQUEST","data":{"user":{"id":53908232506183680}}})",
                              R"({"cmd":"DISPATCH","evt":"ACTIVITY_JOIN","data":{"user":{"id":"53908232506183680"}}})",
                              R"({"cmd":"DISPATCH","evt":"ACTIVITY_JOIN_REQUEST","data":{}})"})
        check(join_asker(wrong).empty(), wrong);
    if (!failures) std::cout << "discord presence tests passed\n";
    return failures ? 1 : 0;
}
