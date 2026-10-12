#pragma once
#include "server_config.h"
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace dingosdk::server {
// A server's own chat words, beside the ReSkate team's lists (Extension/Multiplayer/word_lists.h):
// data/words_filtered.txt and data/words_blocked.txt, one word to a line, with "#" starting a
// comment. A filtered word shows as stars to players who have their chat filter on (the server
// sends them its list); a blocked one is not allowed at all, as the team's are (config
// word_warnings). With "use_global_word_filter": false the team's lists are left out here, and
// only the blocked file is refused.
std::filesystem::path filtered_words_file(const ServerConfig &config);
std::filesystem::path blocked_words_file(const ServerConfig &config);
struct OwnWords {
    std::size_t filtered{}, blocked{};
    // The filtered words that go to players: as many as a packet carries (max_server_words),
    // without the ones too long for it.
    std::vector<std::string> sent;
    std::string problem; // a file that could not be read or written; the words read still apply
};
// Reads both files (making each, empty, when it is not there) and puts
// them, and the config's use_global_word_filter, in use.
OwnWords use_own_words(const ServerConfig &config);
// What that did, for the log.
std::string own_words_text(const ServerConfig &config, const OwnWords &words);
} // namespace dingosdk::server
