#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// The game's own grade of each pop (EnumGS.PopHeight): low, medium or high, which the player's Minimum Pop Height
// setting moves. Record all tricks keeps a take only when the game graded it as asked, and playback chooses the clip
// for the grade the game gave the player's pop.
//
// The grades are not told apart by skater: any skater's graph is heard. Record all tricks refuses to start while other
// skaters are in the game, and playback uses the grade only in solo play with no AI skaters.
namespace dingosdk::anim_studio::pop_grades {
// 1 low, 2 medium, 3 high (0 none).
using PopHeight = std::int32_t;
// "low", "mid", "high", as the studio names its pops ("" for none).
[[nodiscard]] std::string_view pop_name(PopHeight height) noexcept;
[[nodiscard]] PopHeight pop_height_of(std::string_view name) noexcept;

// Hooks the game's setter for the graph's int values, once. False with `why` when this game build's setter is not the
// one this was written for. Client thread.
bool install(std::uintptr_t base, std::string& why);

// A take starts: for `lasts_ms` at most, the grade the game gives is kept as the take's. Any thread.
void begin_take(std::uint64_t lasts_ms) noexcept;
void end_take() noexcept;
// The grade of the pop in the take under way or just over (0 none yet). Any thread.
[[nodiscard]] PopHeight take_pop_height() noexcept;
// The grade of the latest pop the game made at or after `since_ms` (GetTickCount64), any skater's (0 none). Any thread.
[[nodiscard]] PopHeight pop_height_since(std::uint64_t since_ms) noexcept;
} // namespace dingosdk::anim_studio::pop_grades
