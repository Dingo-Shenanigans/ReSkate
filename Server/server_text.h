#pragma once
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
// teleport <players> <target>: who goes, and where.
//   players: one player (a SteamID64, or the start of a name) or SteamID64s joined by commas, no spaces.
//   target:  a player the same way, or "x y z" in world units as the log prints them (y is up).
struct TeleportRequest {
    std::vector<std::uint64_t> ids;            // a comma list, or one SteamID64
    std::string_view name;                     // one player by name (the movers' text, when not a number)
    std::optional<std::array<float, 3>> point; // the target as x y z
    std::string_view player;                   // the target as a player
};
constexpr std::size_t max_teleport_movers = 64;
constexpr float max_teleport_coordinate = 100000.f; // far beyond any map, short of nonsense
constexpr std::string_view teleport_usage = "teleport <player|SteamID64,SteamID64,...> <player|x y z>";
// The request, or why there is none.
inline std::string parse_teleport(std::string_view argument, TeleportRequest &out) {
    out = {};
    const auto [movers, target] = split(argument);
    if (movers.empty() || target.empty()) return std::string(teleport_usage);
    if (movers.find(',') != std::string_view::npos) {
        for (auto rest = movers;;) {
            const auto comma = rest.find(',');
            const auto part = rest.substr(0, comma);
            const auto id = number(part);
            if (!id || !*id) return "\"" + std::string(part) + "\" is not a SteamID64. A list is SteamID64s joined by commas, no spaces.";
            if (std::find(out.ids.begin(), out.ids.end(), *id) == out.ids.end()) out.ids.push_back(*id);
            if (out.ids.size() > max_teleport_movers)
                return "At most " + std::to_string(max_teleport_movers) + " players per teleport.";
            if (comma == std::string_view::npos) break;
            rest = rest.substr(comma + 1);
        }
    } else if (const auto id = number(movers)) {
        out.ids.push_back(*id);
    } else {
        out.name = movers;
    }
    // Three numbers are a place; anything else names a player.
    std::array<float, 3> at{};
    std::size_t numbers{};
    auto rest = target;
    for (; numbers < 3 && !rest.empty(); ++numbers) {
        const auto [word, remaining] = split(rest);
        const auto parsed = std::from_chars(word.data(), word.data() + word.size(), at[numbers]);
        if (parsed.ptr != word.data() + word.size()) break;
        if (parsed.ec == std::errc::result_out_of_range) at[numbers] = INFINITY; // "1e99": a number, and too far
        else if (parsed.ec != std::errc{}) break;
        rest = remaining;
    }
    if (numbers == 3 && rest.empty()) {
        for (const auto value : at)
            if (!std::isfinite(value) || std::abs(value) > max_teleport_coordinate)
                return "Coordinates are x y z, each a number from -100000 to 100000.";
        out.point = at;
    } else {
        out.player = target;
    }
    return {};
}
} // namespace dingosdk::server
