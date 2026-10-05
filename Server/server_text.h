#pragma once
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// Parsing helpers for console lines, admin requests and chat commands.
namespace dingosdk::server {
inline std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}
// The first word and the rest, both trimmed.
inline std::pair<std::string_view, std::string_view> split(std::string_view text) {
    text = trim(text);
    const auto space = text.find(' ');
    if (space == std::string_view::npos) return {text, {}};
    return {text.substr(0, space), trim(text.substr(space + 1))};
}
inline std::optional<std::uint64_t> number(std::string_view text) {
    std::uint64_t value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return {};
    return value;
}
inline std::optional<bool> on_off(std::string_view text) {
    if (text == "on" || text == "true" || text == "yes") return true;
    if (text == "off" || text == "false" || text == "no") return false;
    return {};
}
inline std::string lower(std::string_view text) {
    std::string result(text);
    for (auto &c : result)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return result;
}
// Cuts text to at most `limit` bytes, never through a UTF-8 character.
inline void cut_text(std::string &text, std::size_t limit) {
    if (text.size() <= limit) return;
    auto cut = limit;
    while (cut && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    text.resize(cut);
}
// A direct message as the recipient reads it: "[DM from <from>] text", or "[DM from <from> to <scope>] text" for
// a group. The text is cut so the whole line fits `limit` bytes and the marker is never lost.
inline std::string dm_line(std::string_view from, std::string_view scope, std::string_view text, std::size_t limit) {
    std::string head = "[DM from " + std::string(from);
    if (!scope.empty()) head += " to " + std::string(scope);
    head += "] ";
    std::string body(text);
    cut_text(body, limit > head.size() ? limit - head.size() : 0);
    return head + body;
}
// A command as the log shows it: the log is plain text, so a new password is left out.
inline std::string loggable(std::string_view command) {
    const auto [verb, argument] = split(command);
    if (lower(verb) != "password" || argument.empty() || argument == "off") return std::string(command);
    return std::string(verb) + " <hidden>";
}
} // namespace dingosdk::server
