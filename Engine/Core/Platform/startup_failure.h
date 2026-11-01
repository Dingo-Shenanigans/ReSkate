#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// DingoSDKDebugInitialize runs inside Skate and answers the launcher with a bare
// TRUE or FALSE. On FALSE the runtime has already said why in ReSkate.log; it
// also keeps that reason in an exported buffer, which the launcher reads out of
// the game before closing it, so the STATUS tile and Copy details carry the
// cause instead of a FALSE that reads like an injection failure.
namespace dingosdk::startup_failure {

// ReSkate.dll exports a char[capacity] by this name: NUL-terminated UTF-8,
// empty until startup fails. The first reason written stays.
inline constexpr char export_name[] = "DingoSDKStartupFailure";
inline constexpr std::size_t capacity = 1024;

// The longest prefix of text, at most limit bytes, that does not end inside a
// UTF-8 sequence.
inline std::size_t utf8_prefix(std::string_view text, std::size_t limit) noexcept {
    if (text.size() <= limit) return text.size();
    auto length = limit;
    while (length && (static_cast<unsigned char>(text[length]) & 0xc0) == 0x80) --length;
    return length;
}

// One line for the STATUS tile: control characters become spaces and the
// spaces around the text go.
inline std::string printable(std::string_view text) {
    std::string line(text);
    for (auto& c : line)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = ' ';
    const auto first = line.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    return line.substr(first, line.find_last_not_of(' ') - first + 1);
}

// Runtimes without the export still logged why they stopped: the ERROR and
// CRITICAL messages process_id wrote, oldest first, at most limit of the newest.
// Lines look like "[time] [pid:tid] Context: [Channel] [LEVEL] message".
inline std::vector<std::string> logged_errors(std::string_view log, std::uint32_t process_id,
                                              std::size_t limit) {
    std::vector<std::string> found;
    const auto tag = "] [" + std::to_string(process_id) + ":";
    for (std::size_t start = 0; start < log.size();) {
        auto end = log.find('\n', start);
        if (end == std::string_view::npos) end = log.size();
        auto line = log.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        // The first "] [" closes the timestamp; the process id follows it.
        const auto stamp_end = line.find("] [");
        if (stamp_end == std::string_view::npos || !line.substr(stamp_end).starts_with(tag)) continue;
        const auto context = line.find(": [", stamp_end + tag.size());
        if (context == std::string_view::npos) continue;
        const auto channel_end = line.find("] [", context + 3);
        if (channel_end == std::string_view::npos) continue;
        const auto level_start = channel_end + 3;
        const auto level_end = line.find("] ", level_start);
        if (level_end == std::string_view::npos) continue;
        const auto level = line.substr(level_start, level_end - level_start);
        if (level != "ERROR" && level != "CRITICAL") continue;
        found.emplace_back(line.substr(level_end + 2));
        if (found.size() > limit) found.erase(found.begin());
    }
    return found;
}

}
