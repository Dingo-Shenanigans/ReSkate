#include "Server/server_words.h"
#include "Engine/Core/Text/word_filter.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace dingosdk::server;
using namespace dingosdk::text;
namespace {
void check(bool ok, const std::string &message) {
    if (ok) return;
    std::cerr << message << '\n';
    std::exit(1);
}
} // namespace

int main() {
    const auto folder = std::filesystem::temp_directory_path() / "reskate-server-words-test";
    std::filesystem::remove_all(folder);
    ServerConfig config;
    config.file = folder / "ReSkateServer.json";

    // The ReSkate team's lists, as a server has them once the backend answers.
    const std::vector<std::string> filtered{"darn"}, forbidden{"zorblat", "snarf", "snarfing"};
    set_word_lists(filtered, forbidden);

    // A new server: both files are made, empty.
    auto words = use_own_words(config);
    check(words.problem.empty() && !words.filtered && !words.blocked && words.sent.empty() &&
              std::filesystem::exists(filtered_words_file(config)) && std::filesystem::file_size(filtered_words_file(config)) == 0 &&
              std::filesystem::exists(blocked_words_file(config)) && std::filesystem::file_size(blocked_words_file(config)) == 0,
          "The word files were not made empty");
    check(contains_forbidden_words("you zorblat") && contains_forbidden_words("snarfing") && !contains_forbidden_words("florp"),
          "A server without words of its own does not hold the team's lists");

    // Its own words: filtered ones are starred and still allowed, blocked ones are refused.
    std::ofstream(filtered_words_file(config), std::ios::binary | std::ios::app)
        << "\xEF\xBB\xBFSmeg   # a comment\r\n\r\nflibber\n" << std::string(40, 'a') << "\n";
    std::ofstream(blocked_words_file(config), std::ios::binary | std::ios::app) << "florp\n  # nothing\nGl0rp\n";
    words = use_own_words(config);
    check(words.filtered == 3 && words.blocked == 2, "The word files were not read a word to a line");
    check(words.sent == std::vector<std::string>{"Smeg", "flibber"}, "The words sent to players are not the filtered ones that fit a packet");
    check(contains_bad_words("you smeg") && !contains_forbidden_words("you smeg") && mask_bad_words("flibber off") == "******* off",
          "A filtered word of the server's own is not filtered, or is refused outright");
    check(contains_forbidden_words("florp") && contains_forbidden_words("you glorp") && contains_bad_words("florp") &&
              contains_bad_words("darn") && contains_forbidden_words("zorblat"),
          "The server's own blocked words are not refused beside the team's");
    check(own_words_text(config, words).find("filters 3") != std::string::npos, "The log line does not count the words");

    // Without the team's lists only its own words are filtered and refused.
    config.global_words = false;
    words = use_own_words(config);
    check(!contains_forbidden_words("zorblat") && !contains_bad_words("darn") && contains_forbidden_words("florp") &&
              contains_bad_words("smeg"),
          "use_global_word_filter off still holds the team's lists, or dropped the server's own");
    // The team's lists arriving again (every ten minutes) do not bring them back.
    const std::vector<std::string> later{"zorblat", "blarg"};
    set_word_lists(filtered, later);
    check(!contains_forbidden_words("blarg") && contains_forbidden_words("florp"), "A later read of the team's lists turned them back on");
    config.global_words = true;
    use_own_words(config);
    check(contains_forbidden_words("blarg") && contains_forbidden_words("florp"), "use_global_word_filter on did not bring the team's lists back");

    set_own_words({}, {});
    reset_word_lists();
    std::filesystem::remove_all(folder);
    std::cout << "server words: ok\n";
}
