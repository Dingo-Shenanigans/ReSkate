#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// The game's own grade of each pop (EnumGS.PopHeight): low, medium or high, which the player's Minimum Pop Height
// setting moves. Playback chooses the clip for the grade the game gave the player's pop.
//
// The grades are not told apart by skater: any skater's graph is heard, so playback uses the grade only in solo play
// with no AI skaters.
namespace dingosdk::anim_studio::pop_grades {
// 1 low, 2 medium, 3 high (0 none).
using PopHeight = std::int32_t;

// Hooks the game's setter for the graph's int values, once. False with `why` when this game build's setter is not the
// one this was written for. Client thread.
bool install(std::uintptr_t base, std::string& why);

// The grade of the latest pop the game made at or after `since_ms` (GetTickCount64), any skater's (0 none). Any thread.
[[nodiscard]] PopHeight pop_height_since(std::uint64_t since_ms) noexcept;
} // namespace dingosdk::anim_studio::pop_grades
