#include "server_words.h"
#include "Engine/Core/Text/word_filter.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

namespace dingosdk::server {
namespace {
// As many words of as many bytes as one of the team's lists holds.
constexpr std::size_t most_words = 5000, longest_word = 64;

// The words in `file`, which is made empty when it is not there, so an owner finds it to fill in.
std::vector<std::string> read(const std::filesystem::path &file, std::string &problem) {
    std::vector<std::string> words;
    std::error_code error;
    if (!std::filesystem::exists(file, error)) {
        std::filesystem::create_directories(file.parent_path(), error);
        if (!std::ofstream(file, std::ios::binary)) problem = "could not write " + file.filename().string();
        return words;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        problem = "could not read " + file.filename().string();
        return words;
    }
    for (std::string line; std::getline(in, line) && words.size() < most_words;) {
        if (const auto comment = line.find('#'); comment != std::string::npos) line.erase(comment);
        const auto first = line.find_first_not_of(" \t\r\xEF\xBB\xBF"), last = line.find_last_not_of(" \t\r");
        if (first == std::string::npos) continue;
        line = line.substr(first, last - first + 1);
        if (line.size() <= longest_word) words.push_back(std::move(line));
    }
    return words;
}
} // namespace

std::filesystem::path filtered_words_file(const ServerConfig &c) { return c.file.parent_path() / "data" / "words_filtered.txt"; }
std::filesystem::path blocked_words_file(const ServerConfig &c) { return c.file.parent_path() / "data" / "words_blocked.txt"; }

OwnWords use_own_words(const ServerConfig &config) {
    OwnWords result;
    const auto filtered = read(filtered_words_file(config), result.problem);
    const auto blocked = read(blocked_words_file(config), result.problem);
    result.filtered = filtered.size();
    result.blocked = blocked.size();
    for (const auto &word : filtered)
        if (result.sent.size() < multiplayer::max_server_words && multiplayer::valid_server_word(word)) result.sent.push_back(word);
    text::set_own_words(filtered, blocked, config.global_words);
    return result;
}

std::string own_words_text(const ServerConfig &config, const OwnWords &words) {
    std::ostringstream out;
    out << "Chat words: " << (config.global_words ? "the ReSkate team's lists are used"
                                                : "the ReSkate team's lists are off (\"use_global_word_filter\": false)")
        << "; this server filters " << words.filtered << " word(s) of its own (data/words_filtered.txt, " << words.sent.size()
        << " sent to players) and blocks " << words.blocked << " (data/words_blocked.txt).";
    if (!words.problem.empty()) out << " Problem: " << words.problem << ".";
    return out.str();
}
} // namespace dingosdk::server
