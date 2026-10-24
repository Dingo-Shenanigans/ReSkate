#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// What the game shows and records of the local skater's tricks, as plugins ask (reskate.tricks.label
// and record_as; the hooks TheRealEJR found for Rock to Fakie, pull request 285):
//   - A label: the trick ticker shows `to` where the game's text is exactly `from`. Only the label the
//     ticker asks for on the one call route seen for it (Engine/Game/Build/20260929/trick_rewrite.h),
//     in the ticker's own buffer and only when it has room: the string database, other text and
//     other players' tickers are the game's.
//   - A record: the next trick the recorder gets whose id is one of `from`, within `within_ms`, is
//     recorded as `to`. The recorder is given a changed copy of the packet, never the game's.
// Each owner (a plugin) has its own labels and one pending record; both go with release(). With none
// asked for, the hooks pass everything on unchanged. Any thread.
namespace dingosdk::trick_rewrite {
// Installs the hooks when the game's code matches.
bool start(std::uintptr_t image_base) noexcept;
bool labels_available() noexcept;
bool records_available() noexcept;

inline constexpr std::size_t most_label_text = 63, most_labels = 16;
inline constexpr std::uint32_t most_trick_id = 0x150;
inline constexpr int most_within_ms = 2000;

// Empty when done, else why not. An empty `to` takes that owner's label for `from` away.
std::string label(std::string_view owner, std::string_view from, std::string_view to);
// Replaces that owner's pending record. Empty when done, else why not.
std::string record_as(std::string_view owner, std::vector<std::uint32_t> from, std::uint32_t to, int within_ms);
void release(std::string_view owner);

struct Counts {
    std::uint64_t labelled{}, recorded{}, no_room{};
};
Counts counts() noexcept;
}
