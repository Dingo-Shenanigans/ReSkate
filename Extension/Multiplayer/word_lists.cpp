#include "word_lists.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Text/word_filter.h"
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dingosdk::multiplayer {
namespace {
// A list is at most this many words of this many bytes: what the panel takes, and a bound on
// what an answer can make the filter hold.
constexpr std::size_t most_words = 5000, longest_word = 64;

std::vector<std::string> read(const Json &answer, const char *name) {
    std::vector<std::string> words;
    if (!answer.contains(name)) return words;
    const auto &listed = answer.at(name);
    if (!listed.is_array()) throw std::runtime_error("a word list is not a list");
    for (const auto &entry : listed) {
        if (!entry.is_string()) throw std::runtime_error("a word is not text");
        if (words.size() == most_words) break;
        if (const auto &word = entry.string(); !word.empty() && word.size() <= longest_word) words.push_back(word);
    }
    return words;
}
} // namespace

WordListsRead use_word_lists(std::string_view json) {
    const auto answer = Json::parse(json);
    WordListsRead result;
    if (!answer.is_object() || (!answer.contains("filtered_words") && !answer.contains("forbidden_words"))) return result;
    auto lists = std::pair(read(answer, "filtered_words"), read(answer, "forbidden_words"));
    result.given = true;
    result.filtered = lists.first.size();
    result.forbidden = lists.second.size();
    static std::mutex mutex;
    static std::optional<std::pair<std::vector<std::string>, std::vector<std::string>>> last;
    std::lock_guard lock(mutex);
    if (last == lists) return result;
    text::set_word_lists(lists.first, lists.second);
    last = std::move(lists);
    result.changed = true;
    return result;
}

std::string word_warning(unsigned count, unsigned warnings) {
    return "Your message was not sent: it has a word that is not allowed here. Warning " + std::to_string(count) + " of " +
           std::to_string(warnings) + (count >= warnings ? ". The next one gets you kicked." : ".");
}
} // namespace dingosdk::multiplayer
