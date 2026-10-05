#include "Server/server_text.h"
#include <cstdlib>
#include <iostream>
#include <string>

using dingosdk::server::dm_line;
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
    return 0;
}
