#pragma once
#include "Extension/Style/style_model.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

// Which tricks a recording of the player's tricks takes, and what it tells the player.
namespace dingosdk::style {
// Measured in game: one trick on an open stage.
inline constexpr std::uint32_t ms_per_trick = 5000;
// A trick with no clip, a clip this version does not read (not on disk), or an outdated clip.
[[nodiscard]] inline TrickSet tricks_to_record(const TrickSet &on_disk, const TrickSet &outdated) noexcept {
    TrickSet result;
    for (std::uint8_t id = 1; id <= last_capturable; ++id) result.set(id, !on_disk.test(id) || outdated.test(id));
    return result;
}
[[nodiscard]] inline std::vector<std::uint8_t> in_order(const TrickSet &tricks) {
    std::vector<std::uint8_t> result;
    for (const auto id : flip_trick_ids())
        if (tricks.test(id)) result.push_back(id);
    return result;
}
[[nodiscard]] inline std::string record_text(std::size_t tricks, std::uint32_t ms_each) {
    const auto minutes = std::max<std::uint64_t>(1, (std::uint64_t{tricks} * ms_each + 59999) / 60000);
    return std::format("Your tricks need recording: {} {}, about {} min. Your skater goes to a practice stage.", tricks,
                       tricks == 1 ? "trick" : "tricks", minutes);
}
[[nodiscard]] inline std::string progress_text(std::size_t place, std::size_t total, std::uint8_t trick, bool stopping) {
    return std::format("Recording your tricks: {} of {} ({}). {}", place, total, flip_trick_titles[trick],
                       stopping ? "Stopping after this trick" : "Esc or Back: stop after this trick");
}
struct NotKept {
    std::uint8_t trick{};
    std::string reason;
};
[[nodiscard]] inline std::string summary_text(std::size_t total, std::size_t recorded, const std::vector<NotKept> &not_kept,
                                              bool stopped) {
    if (total == 1 && !stopped && !not_kept.empty())
        return std::format("the {} was not recorded: {}", flip_trick_titles[not_kept[0].trick], not_kept[0].reason);
    std::string text = stopped ? "stopped: " : "";
    if (!stopped && not_kept.empty())
        return total == 1 ? std::string("the trick is recorded") : std::format("your {} tricks are recorded", total);
    text += std::format("{} of {} tricks are recorded", recorded, total);
    for (std::size_t i = 0; i < not_kept.size(); ++i)
        text += std::format("{}the {} ({})", i ? ", " : "; not recorded: ", flip_trick_titles[not_kept[i].trick], not_kept[i].reason);
    return text;
}
} // namespace dingosdk::style
