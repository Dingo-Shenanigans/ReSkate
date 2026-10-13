#include "Server/server_text.h"
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using dingosdk::server::dm_line;
using dingosdk::server::max_teleport_movers;
using dingosdk::server::parse_teleport;
using dingosdk::server::TeleportRequest;
namespace {
void check(bool ok, const std::string &message) {
    if (ok) return;
    std::cerr << message << '\n';
    std::exit(1);
}
} // namespace

int main() {
    check(dm_line("Server", {}, "hi", 100) == "[DM from Server] hi", "plain DM");
    check(dm_line("Player", "party", "hi", 100) == "[DM from Player to party] hi", "party DM");
    check(dm_line("Player", "admins", "hi", 100) == "[DM from Player to admins] hi", "admins DM");
    const auto cut = dm_line("Server", {}, std::string(500, 'a'), 40);
    check(cut.size() == 40 && cut.starts_with("[DM from Server] "), "text is cut, marker kept");
    // Never cut through a UTF-8 character (\xC3\xA9 is one letter).
    const auto utf8 = dm_line("S", {}, "\xC3\xA9\xC3\xA9\xC3\xA9", 12);
    check(utf8 == "[DM from S] \xC3\xA9" || utf8 == "[DM from S] ", "UTF-8 boundary");
    check(dm_line("Server", {}, "hi", 3) == "[DM from Server] ", "tiny limit keeps no text");

    // teleport <players> <target>
    TeleportRequest r;
    check(parse_teleport("76561198000000001 Kush", r).empty() && r.ids == std::vector<std::uint64_t>{76561198000000001} &&
              r.name.empty() && r.player == "Kush" && !r.point,
          "one SteamID64 to a player");
    check(parse_teleport("ku -247 2172 -259", r).empty() && r.ids.empty() && r.name == "ku" && r.point &&
              (*r.point)[0] == -247.f && (*r.point)[1] == 2172.f && (*r.point)[2] == -259.f && r.player.empty(),
          "a name to x y z, in order");
    check(parse_teleport("  1,2,3   0.5 -1.25 1e3 ", r).empty() && r.ids == std::vector<std::uint64_t>{1, 2, 3} &&
              r.point && (*r.point)[0] == .5f && (*r.point)[1] == -1.25f && (*r.point)[2] == 1000.f,
          "a list to decimals, extra spaces");
    check(parse_teleport("5,5,6 Bob", r).empty() && r.ids == std::vector<std::uint64_t>{5, 6}, "duplicates dropped");
    check(parse_teleport("1 Big Bob", r).empty() && r.player == "Big Bob" && !r.point, "a target name with a space");
    check(parse_teleport("1 1 2", r).empty() && r.player == "1 2" && !r.point, "two numbers are not a place");
    check(parse_teleport("1 1 2 3 4", r).empty() && r.player == "1 2 3 4" && !r.point, "four numbers are not a place");
    check(parse_teleport("1 3 dogs x", r).empty() && r.player == "3 dogs x", "a name starting with a number");
    check(!parse_teleport("", r).empty(), "empty refused");
    check(!parse_teleport("76561198000000001", r).empty(), "no target refused");
    check(!parse_teleport("1,,2 Bob", r).empty(), "empty list entry refused");
    check(!parse_teleport("1, Bob", r).empty(), "trailing comma refused");
    check(!parse_teleport(",1 Bob", r).empty(), "leading comma refused");
    check(!parse_teleport("1,Bob Bob", r).empty(), "a name in a list refused");
    check(!parse_teleport("0,1 Bob", r).empty(), "id 0 refused");
    check(!parse_teleport("1,-2 Bob", r).empty(), "a negative id refused");
    check(!parse_teleport("1 nan 0 0", r).empty(), "NaN refused");
    check(!parse_teleport("1 0 inf 0", r).empty(), "inf refused");
    check(!parse_teleport("1 0 0 -infinity", r).empty(), "-inf refused");
    check(!parse_teleport("1 1e9 0 0", r).empty(), "an absurd coordinate refused");
    check(!parse_teleport("1 0 1e99 0", r).empty(), "an out-of-range coordinate refused");
    check(parse_teleport("1 100000 -100000 0", r).empty() && r.point, "the limit itself accepted");
    std::string list = "1";
    for (std::uint64_t i = 2; i <= max_teleport_movers; ++i) list += "," + std::to_string(i);
    check(parse_teleport(list + " 0 0 0", r).empty() && r.ids.size() == max_teleport_movers, "64 movers accepted");
    check(!parse_teleport(list + ",65 0 0 0", r).empty(), "65 movers refused");
    check(parse_teleport(list + ",1 0 0 0", r).empty(), "a repeat does not count twice");
    return 0;
}
