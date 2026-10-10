#include "Server/global_bans.h"
#include "Extension/Multiplayer/developer_identity.h"
#include "Extension/Multiplayer/word_lists.h"
#include "Engine/Core/Text/word_filter.h"
#include <cstdlib>
#include <iostream>
#include <string>

using namespace dingosdk::server;
using dingosdk::multiplayer::reskate_banned;
namespace {
void check(bool ok, const std::string &message) {
    if (ok) return;
    std::cerr << message << '\n';
    std::exit(1);
}
constexpr std::uint64_t griefer = 76561198000000009ULL, cheater = 76561198000000010ULL, player = 76561198000000011ULL;

void lists() {
    check(!reskate_banned(griefer), "A player was banned before any list was read");
    // The first list read counts as news even when it bans nobody, so the server can say it has it.
    auto read = use_ban_list(R"({"categories":{"dev":["76561198000000011"]},"banned":[]})");
    check(read.ok && read.changed && read.banned == 0 && read.problem.empty() && !reskate_banned(griefer),
          "A list with nobody on it was not taken as read");
    read = use_ban_list(R"({"categories":{},"banned":["76561198000000010","76561198000000009"]})");
    check(read.ok && read.changed && read.banned == 2 && reskate_banned(griefer) && reskate_banned(cheater) && !reskate_banned(player),
          "The ban list did not ban the players on it");
    // The other lists changing is no news about bans.
    read = use_ban_list(R"({"categories":{"homie":["76561198000000011"]},"banned":["76561198000000009","76561198000000010"]})");
    check(read.ok && !read.changed && read.banned == 2, "The same ban list counted as a change");
    // An answer that is not the lists (the backend down, a proxy's error page) lifts no ban.
    for (const std::string_view wrong : {"", "<html>502 Bad Gateway</html>", R"({"banned":["76561198000000009"]})",
                                         R"({"categories":{},"banned":["everyone"]})"}) {
        read = use_ban_list(wrong);
        check(!read.ok && !read.problem.empty() && reskate_banned(griefer) && reskate_banned(cheater),
              "A bad answer was accepted, or lifted the bans: " + std::string(wrong));
    }
    read = use_ban_list(R"({"categories":{},"banned":["76561198000000010"]})");
    check(read.ok && read.changed && read.banned == 1 && !reskate_banned(griefer) && reskate_banned(cheater), "A lifted ban stayed");
    // A backend from before it had bans bans nobody.
    read = use_ban_list(R"({"categories":{"dev":[]}})");
    check(read.ok && read.changed && read.banned == 0 && !reskate_banned(cheater), "A list without bans kept one");
    // The chat word lists ride in the same answer: none changes nothing, some replace the
    // built-in list, and the same again is not a change.
    check(!read.words_changed && !dingosdk::text::contains_forbidden_words("zorblat") && dingosdk::text::contains_bad_words("shit"),
          "An answer without word lists changed the words");
    const auto with_words = R"({"categories":{},"filtered_words":["darn"],"forbidden_words":["zorblat",""]})";
    read = use_ban_list(with_words);
    check(read.ok && read.words_changed && read.filtered_words == 1 && read.forbidden_words == 1 &&
              dingosdk::text::contains_forbidden_words("Z0rblat!") && dingosdk::text::contains_bad_words("darn") &&
              !dingosdk::text::contains_bad_words("shit"),
          "The backend's word lists were not put in use");
    read = use_ban_list(with_words);
    check(read.ok && !read.words_changed && read.forbidden_words == 1, "The same word lists counted as a change");
    read = use_ban_list(R"({"categories":{},"forbidden_words":"zorblat"})");
    check(!read.ok && dingosdk::text::contains_forbidden_words("zorblat"), "Word lists of the wrong shape were accepted, or cleared the ones in use");
    read = use_ban_list(R"({"categories":{}})");
    check(read.ok && !read.words_changed && dingosdk::text::contains_forbidden_words("zorblat"), "An answer without word lists cleared them");
    using dingosdk::multiplayer::word_warning;
    check(word_warning(1, 3).find("Warning 1 of 3.") != std::string::npos && word_warning(3, 3).find("next one gets you kicked") != std::string::npos &&
              word_warning(2, 3).find("kicked") == std::string::npos,
          "The warnings do not count up to the kick");
    dingosdk::text::reset_word_lists();
}

// --live: the deployed backend, read the way the server reads it. Not part of the
// test run, which stays off the network.
int live() {
    const auto read = read_global_bans();
    if (read.ok) std::cout << "the backend's ban list was read: " << read.banned << " banned\n";
    else std::cout << "the backend's ban list could not be read: " << read.problem << '\n';
    return read.ok ? 0 : 1;
}
} // namespace

int main(int count, char **arguments) {
    if (count == 2 && std::string_view(arguments[1]) == "--live") return live();
    lists();
    std::cout << "global ban tests passed\n";
}
